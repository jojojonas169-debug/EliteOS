#ifndef ZENITH_NETDEV_H
#define ZENITH_NETDEV_H

#include <kernel.h>

struct netdev {
    char name[48];
    uint8_t mac[6];
    bool (*send)(struct netdev *d, const void *frame, size_t len);
    int  (*poll)(struct netdev *d, void *buf, size_t cap);      /* bytes, 0 if nothing */
    bool (*link)(struct netdev *d);
};

struct netdev *e1000_init(void);

/* between the layers of the stack */
uint16_t net_csum(const void *data, size_t len, uint32_t sum);
bool     net_ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
uint32_t net_local_ip(void);
void     tcp_input(uint32_t src, uint8_t *data, size_t len);
void     tcp_timer(void);

#endif
