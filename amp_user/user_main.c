/***********************/
/*    主线程启动函数    */
/***********************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "user_declaration.h"

int amp_fd = -1;
int ctrl_fd = -1;
int tun_fd = -1;

volatile int g_running = 1;

int main(void)
{
#define AMP_THREAD_COUNT 5
    pthread_t threads[AMP_THREAD_COUNT];
    int thread_created[AMP_THREAD_COUNT];
    int i;

    memset(threads, 0, sizeof(threads));
    memset(thread_created, 0, sizeof(thread_created));

    /*    打开业务数据传输接口设备    */
    amp_fd = open(AMP_DATA_DEV, O_RDWR);
    if (amp_fd < 0) {
        perror("open(" AMP_DATA_DEV ")");
        return 1;
    }

    /*    打开控制数据传输接口设备    */
    ctrl_fd = open(AMP_CTRL_DEV, O_RDWR);
    if (ctrl_fd < 0) {
        perror("open(" AMP_CTRL_DEV ")");
        goto err_amp_fd;
    }

    /*    创建虚拟网络TUN设备"rf0"    */
    tun_fd = tun_alloc("rf0");
    if (tun_fd < 0)
        goto err_ctrl_fd;

    /*    配置网关规则    */
    setup_gateway_rules();

    /*    初始化控制socket    */
    if (control_socket_init() != 0)
        goto err_tun_fd;

    /*    业务数据写入线程    */
    if (pthread_create(&threads[0], NULL, amp_tx_thread, NULL) != 0) {
        perror("pthread_create(amp_tx)");
        goto err_threads;
    }
    thread_created[0] = 1;

    /*    TUN设备 -> amp_ipi线程    */
    if (pthread_create(&threads[1], NULL, tun_to_amp_thread, NULL) != 0) {
        perror("pthread_create(tun_to_amp)");
        goto err_threads;
    }
    thread_created[1] = 1;

    /*    TUN设备 <- amp_ipi线程    */
    if (pthread_create(&threads[2], NULL, amp_to_tun_thread, NULL) != 0) {
        perror("pthread_create(amp_to_tun)");
        goto err_threads;
    }
    thread_created[2] = 1;

    /*    控制数据 -> amp_ctrl线程    */
    if (pthread_create(&threads[3], NULL, control_rx_to_amp_thread, NULL) != 0) {
        perror("pthread_create(control_rx_to_amp)");
        goto err_threads;
    }
    thread_created[3] = 1;

    /*    控制数据 <- amp_ctrl线程    */
    if (pthread_create(&threads[4], NULL, control_amp_to_udp_thread, NULL) != 0) {
        perror("pthread_create(control_amp_to_udp)");
        goto err_threads;
    }
    thread_created[4] = 1;

    /* 等待所有线程结束 */
    for (i = 0; i < AMP_THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    control_socket_close();
    close(tun_fd);
    close(ctrl_fd);
    close(amp_fd);
    return 0;

err_threads:
    /* 通知所有线程退出 */
    g_running = 0;
    /* 唤醒可能阻塞在条件变量上的 amp_tx_thread */
    pthread_cond_broadcast(&amp_tx_runtime.not_empty);
    pthread_cond_broadcast(&amp_tx_runtime.data_not_full);

    /* cancel + join 所有已创建的线程 */
    for (i = 0; i < AMP_THREAD_COUNT; i++) {
        if (thread_created[i]) {
            pthread_cancel(threads[i]);
            pthread_join(threads[i], NULL);
        }
    }

    control_socket_close();

err_tun_fd:
    close(tun_fd);
err_ctrl_fd:
    close(ctrl_fd);
err_amp_fd:
    close(amp_fd);
    return 1;
}
