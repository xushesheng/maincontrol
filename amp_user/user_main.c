/***********************/
/*    主线程启动函数    */
/***********************/
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "user_declaration.h"

int amp_fd = -1;
int tun_fd = -1;

int main(void)
{
    pthread_t t1;
    pthread_t t2;
    pthread_t t3;

    amp_fd = open(AMP_DEV, O_RDWR);
    if (amp_fd < 0) {
        perror("open(" AMP_DEV ")");
        return 1;
    }

    tun_fd = tun_alloc("rf0");
    if (tun_fd < 0)
        return 1;

    setup_gateway_rules();

    if (pthread_create(&t1, NULL, tun_to_amp_thread, NULL) != 0) {
        perror("pthread_create(tun_to_amp)");
        return 1;
    }
    if (pthread_create(&t2, NULL, amp_to_tun_thread, NULL) != 0) {
        perror("pthread_create(amp_to_tun)");
        return 1;
    }
    if (pthread_create(&t3, NULL, pcap_control_thread, NULL) != 0) {
        perror("pthread_create(pcap_control)");
        return 1;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    return 0;
}
