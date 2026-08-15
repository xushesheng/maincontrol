/***********************/
/*    主线程启动函数    */
/***********************/
#include <stdio.h>                 /* perror / fprintf */
#include <string.h>                /* memset */
#include <unistd.h>                /* close */
#include <fcntl.h>                 /* open / O_RDWR */
#include <pthread.h>               /* POSIX 线程创建/取消/等待/条件变量 */

#include "user_declaration.h"      /* 全局变量声明与函数声明 */

int amp_fd = -1;    /* /dev/amp_ipi 文件描述符，初始 -1（未打开） */
int ctrl_fd = -1;   /* /dev/amp_ctrl 文件描述符 */
int tun_fd = -1;    /* TUN 设备 rf0 文件描述符 */

/* ============================================================
 * 主控用户态线程模型（主线程 + 5 个工作线程）
 *   amp_tx_thread            统一写 /dev/amp_ipi（业务），串行化避免并发踩 TX 单槽
 *   tun_to_amp_thread        TUN(rf0) 读 IP 包 -> 聚合/单发 -> /dev/amp_ipi（业务上行）
 *   amp_to_tun_thread        /dev/amp_ipi 读 -> 拆 AMPB/原始 IP -> TUN(rf0)（业务下行）
 *   control_rx_to_amp_thread 网管 UDP 3409 -> /dev/amp_ctrl（控制指令上行）
 *   control_amp_to_udp_thread /dev/amp_ctrl -> 网管 UDP 3419（控制回执/上报下行）
 * 三大设备 fd：amp_fd(/dev/amp_ipi)、ctrl_fd(/dev/amp_ctrl)、tun_fd(rf0)
 * ============================================================ */

volatile int g_running = 1;     /* 全局运行标志：驱动加载后初始为 1，退出时置 0 */

int main(void)
{
#define AMP_THREAD_COUNT 5                  /* 共 5 个工作线程 */
    pthread_t threads[AMP_THREAD_COUNT];    /* 线程 ID 数组 */
    int thread_created[AMP_THREAD_COUNT];   /* 标记哪些线程已创建成功（用于回滚） */
    int i;                                  /* 循环索引 */

    memset(threads, 0, sizeof(threads));                /* 清零线程 ID 数组 */
    memset(thread_created, 0, sizeof(thread_created));  /* 清零创建标记数组 */

    /* 步骤1：打开业务数据传输接口设备 /dev/amp_ipi */
    amp_fd = open(AMP_DATA_DEV, O_RDWR);
    if (amp_fd < 0) {
        perror("open(" AMP_DATA_DEV ")");
        return 1;
    }

    /* 步骤2：打开控制数据传输接口设备 /dev/amp_ctrl */
    ctrl_fd = open(AMP_CTRL_DEV, O_RDWR);
    if (ctrl_fd < 0) {
        perror("open(" AMP_CTRL_DEV ")");
        goto err_amp_fd;    /* 回滚：跳转到关闭 amp_fd */
    }

    /* 步骤3：创建虚拟网络 TUN 设备 "rf0" */
    tun_fd = tun_alloc("rf0");
    if (tun_fd < 0)
        goto err_ctrl_fd;   /* 回滚：跳转到关闭 ctrl_fd 和 amp_fd */

    /* 步骤4：配置网关规则（路由、proxy ARP、sysctl） */
    setup_gateway_rules();

    /* 步骤5：初始化控制 UDP socket（绑定 3409 端口） */
    if (control_socket_init() != 0)
        goto err_tun_fd;    /* 回滚：跳转到关闭 tun_fd/ctrl_fd/amp_fd */

    /* 步骤5.5：开机一次性下发 0x19 时钟帧给路由（带初始延时+重试，失败不致命） */
    clock_send_on_boot();

    /* 步骤6：创建 5 个工作线程 */

    /* 线程0：统一业务发送线程（从队列取数据 -> write /dev/amp_ipi） */
    if (pthread_create(&threads[0], NULL, amp_tx_thread, NULL) != 0) {
        perror("pthread_create(amp_tx)");
        goto err_threads;
    }
    thread_created[0] = 1;

    /* 线程1：TUN 设备 -> AMP 通道（从 rf0 读 IP 包，聚合后通过 /dev/amp_ipi 发出） */
    if (pthread_create(&threads[1], NULL, tun_to_amp_thread, NULL) != 0) {
        perror("pthread_create(tun_to_amp)");
        goto err_threads;
    }
    thread_created[1] = 1;

    /* 线程2：AMP 通道 -> TUN 设备（从 /dev/amp_ipi 读数据，拆包后写回 rf0） */
    if (pthread_create(&threads[2], NULL, amp_to_tun_thread, NULL) != 0) {
        perror("pthread_create(amp_to_tun)");
        goto err_threads;
    }
    thread_created[2] = 1;

    /* 线程3：控制数据写入（UDP 3409 -> /dev/amp_ctrl） */
    if (pthread_create(&threads[3], NULL, control_rx_to_amp_thread, NULL) != 0) {
        perror("pthread_create(control_rx_to_amp)");
        goto err_threads;
    }
    thread_created[3] = 1;

    /* 线程4：控制数据读取（/dev/amp_ctrl -> UDP 3419） */
    if (pthread_create(&threads[4], NULL, control_amp_to_udp_thread, NULL) != 0) {
        perror("pthread_create(control_amp_to_udp)");
        goto err_threads;
    }
    thread_created[4] = 1;

    /* 等待所有线程结束 */
    for (i = 0; i < AMP_THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    /* 正常退出：清理资源 */
    control_socket_close();     /* 关闭控制 UDP socket */
    close(tun_fd);              /* 关闭 TUN 设备 */
    close(ctrl_fd);             /* 关闭控制设备 */
    close(amp_fd);              /* 关闭业务设备 */
    return 0;

err_threads:
    /* 通知所有线程退出 */
    g_running = 0;

    /* 唤醒可能阻塞在条件变量上的 amp_tx_thread（等待队列非空或队列非满） */
    pthread_cond_broadcast(&amp_tx_runtime.not_empty);
    pthread_cond_broadcast(&amp_tx_runtime.data_not_full);

    /* cancel + join 所有已创建的线程（逆序清理） */
    for (i = 0; i < AMP_THREAD_COUNT; i++) {
        if (thread_created[i]) {        /* 只处理确实创建成功的线程 */
            pthread_cancel(threads[i]); /* 发送取消请求 */
            pthread_join(threads[i], NULL); /* 等待线程退出 */
        }
    }

    control_socket_close();     /* 关闭控制 socket */

err_tun_fd:
    close(tun_fd);              /* 关闭 TUN */
err_ctrl_fd:
    close(ctrl_fd);             /* 关闭控制设备 */
err_amp_fd:
    close(amp_fd);              /* 关闭业务设备 */
    return 1;                   /* 返回非零表示异常退出 */
}
