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

#endif
