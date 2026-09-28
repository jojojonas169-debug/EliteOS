#ifndef ZENITH_NET_H
#define ZENITH_NET_H

#include <kernel.h>

struct net_info {
    char ifname[8];
    char driver[48];
    uint8_t mac[6];
    bool link;
    uint32_t ip, netmask, gateway, dns;     /* host byte order */
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};

bool net_get_info(struct net_info *ni);
bool net_dhcp(int timeout_ms);
bool net_resolve(const char *host, uint32_t *ip, int timeout_ms);
int  net_ping(uint32_t ip, uint16_t seq, int timeout_ms);          /* rtt ms, -1 on timeout */
int  net_http_get(const char *url, char **body, size_t *len, int timeout_ms);  /* status or -1 */
void ip_to_str(uint32_t ip, char *out);
bool str_to_ip(const char *s, uint32_t *ip);

#endif
