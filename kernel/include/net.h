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
int  net_http_get_ex(const char *url, char **body, size_t *len, char *location, size_t locn, int timeout_ms);
void ip_to_str(uint32_t ip, char *out);
bool net_route_ready(uint32_t dst, int timeout_ms);

/* TCP client sockets */
typedef struct tcp_sock tcp_sock_t;
tcp_sock_t *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
long tcp_send(tcp_sock_t *s, const void *data, size_t len);
long tcp_recv(tcp_sock_t *s, void *buf, size_t len, int timeout_ms);   /* 0 = closed, -1 error, -2 timeout */
void tcp_close(tcp_sock_t *s);
int  tcp_open_count(void);

/* HTTP(S) requests */
struct http_result {
    int status;                 /* -1 when there was no answer */
    char *body;                 /* kmalloc'd, NUL terminated */
    size_t len;
    char location[512];         /* redirect target */
    char content_type[96];
    char error[160];            /* human readable reason on failure */
    char tls[96];               /* e.g. "TLS 1.3, AES-128-GCM, X25519" when secure */
};
int  net_http_request(const char *url, struct http_result *r, int timeout_ms);
bool str_to_ip(const char *s, uint32_t *ip);

#endif
