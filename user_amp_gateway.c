#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>

#include "user_amp_runtime.h"

static int get_iface_ipv4(const char *ifname, struct in_addr *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0)
        return -1;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }

    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    close(fd);
    return 0;
}

static void run_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "[WARN] cmd failed (%d): %s\n", rc, cmd);
}

void setup_gateway_rules(void)
{
    struct in_addr local_ip;
    const char *remote_pc = NULL;
    const char *rf_ip = NULL;
    uint8_t *p;
    uint8_t last;

    if (get_iface_ipv4(CAP_IFACE, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 address\n", CAP_IFACE);
        return;
    }

    p = (uint8_t *)&local_ip.s_addr;
    last = p[3];

    if (last == 13) {
        remote_pc = "192.168.1.15";
        rf_ip = "10.255.0.1/30";
    } else if (last == 12) {
        remote_pc = "192.168.1.20";
        rf_ip = "10.255.0.2/30";
    } else {
        fprintf(stderr,
                "[WARN] unexpected %s IP=%s, default remote_pc=192.168.1.15, rf_ip=10.255.0.1/30\n",
                CAP_IFACE, inet_ntoa(local_ip));
        remote_pc = "192.168.1.15";
        rf_ip = "10.255.0.1/30";
    }

    remote_pc_addr = inet_addr(remote_pc);
    if (remote_pc_addr == INADDR_NONE) {
        fprintf(stderr, "[ERROR] bad remote_pc ip: %s\n", remote_pc);
        exit(1);
    }

    run_cmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null");
    run_cmd("sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null");
    run_cmd("sysctl -w net.ipv4.conf.eth1.rp_filter=0 >/dev/null");
    run_cmd("sysctl -w net.ipv4.conf.eth1.proxy_arp=1 >/dev/null");

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip neigh add proxy %s dev %s 2>/dev/null || true", remote_pc, CAP_IFACE);
        run_cmd(cmd);
    }

    run_cmd("ip link set rf0 up 2>/dev/null || true");

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip link set rf0 mtu %d 2>/dev/null || true", RF0_MTU);
        run_cmd(cmd);
    }

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip addr add %s dev rf0 2>/dev/null || true", rf_ip);
        run_cmd(cmd);
    }

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip route add %s/32 dev rf0 2>/dev/null || true", remote_pc);
        run_cmd(cmd);
    }

    printf("[INFO] gateway enabled on %s=%s, proxy_arp for %s, route %s/32 -> rf0\n",
           CAP_IFACE, inet_ntoa(local_ip), remote_pc, remote_pc);
}
