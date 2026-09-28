/*
 * A small TCP/IP stack: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP client,
 * DNS resolver and a minimal TCP client (enough for HTTP/1.0 GET).
 * A single network thread polls the NIC and handles incoming frames.
 */
#include <kernel.h>
#include <net.h>
#include <sched.h>
#include <dev.h>
#include <mm.h>
#include <vfs.h>
#include <x86.h>
#include "netdev.h"

static struct netdev *nd;
static struct net_info info;
static spinlock_t net_lock = SPINLOCK_INIT("net");
static volatile uint64_t last_rx_t;

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
#define ntohs htons
#define ntohl htonl

struct PACKED eth { uint8_t dst[6], src[6]; uint16_t type; };
struct PACKED arp {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
};
struct PACKED ip4 {
    uint8_t vihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
};
struct PACKED icmp { uint8_t type, code; uint16_t csum, id, seq; };
struct PACKED udp { uint16_t sport, dport, len, csum; };
struct PACKED tcp {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
};

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ip_to_str(uint32_t ip, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool str_to_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(*s)) return false;
        int n = 0;
        while (isdigit(*s)) n = n * 10 + (*s++ - '0');
        if (n > 255) return false;
        v = (v << 8) | (uint32_t)n;
        if (i < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

static uint16_t csum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ------------------------------------------------------------------------
 * link layer
 * ---------------------------------------------------------------------- */

static bool eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    uint8_t frame[1600];
    if (len > 1500) return false;
    struct eth *e = (struct eth *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, nd->mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof(*e), payload, len);
    size_t total = sizeof(*e) + len;
    if (total < 60) { memset(frame + total, 0, 60 - total); total = 60; }
    bool ok = nd->send(nd, frame, total);
    if (ok) { info.tx_packets++; info.tx_bytes += total; }
    return ok;
}

/* ARP cache */
struct arp_entry { uint32_t ip; uint8_t mac[6]; uint64_t t; };
static struct arp_entry arp_cache[16];
static char arp_chan;

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    bool ok = false;
    spin_lock(&net_lock);
    for (int i = 0; i < 16; i++)
        if (arp_cache[i].ip == ip && arp_cache[i].t) { memcpy(mac, arp_cache[i].mac, 6); ok = true; }
    spin_unlock(&net_lock);
    return ok;
}

static void arp_store(uint32_t ip, const uint8_t *mac)
{
    spin_lock(&net_lock);
    int slot = 0;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < 16; i++) {
        if (arp_cache[i].ip == ip) { slot = i; break; }
        if (arp_cache[i].t < oldest) { oldest = arp_cache[i].t; slot = i; }
    }
    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].t = uptime_ms() | 1;
    spin_unlock(&net_lock);
    sched_wake(&arp_chan);
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct arp a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, nd->mac, 6);
    a.spa = htonl(info.ip);
    memcpy(a.tha, tha, 6);
    a.tpa = htonl(tpa);
    eth_send(op == 1 ? bcast : tha, ETH_ARP, &a, sizeof(a));
}

static bool arp_resolve(uint32_t ip, uint8_t *mac, int timeout_ms)
{
    if (ip == 0xFFFFFFFF) { memcpy(mac, bcast, 6); return true; }
    /* off-link destinations go through the gateway */
    if (info.netmask && (ip & info.netmask) != (info.ip & info.netmask)) ip = info.gateway;
    uint64_t end = uptime_ms() + (uint64_t)timeout_ms;
    while (uptime_ms() < end) {
        if (arp_lookup(ip, mac)) return true;
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", ip);
        sched_wait(&arp_chan, NULL, 200);
    }
    return arp_lookup(ip, mac);
}

/* ------------------------------------------------------------------------
 * IPv4
 * ---------------------------------------------------------------------- */

static uint16_t ip_id = 1;

static bool ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    uint8_t mac[6];
    if (!arp_resolve(dst, mac, 1500)) return false;
    uint8_t pkt[1500];
    if (len + sizeof(struct ip4) > sizeof(pkt)) return false;
    struct ip4 *h = (struct ip4 *)pkt;
    h->vihl = 0x45;
    h->tos = 0;
    h->len = htons((uint16_t)(sizeof(*h) + len));
    h->id = htons(ip_id++);
    h->frag = htons(0x4000);
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(info.ip);
    h->dst = htonl(dst);
    h->csum = csum(h, sizeof(*h), 0);
    memcpy(pkt + sizeof(*h), payload, len);
    return eth_send(mac, ETH_IP, pkt, sizeof(*h) + len);
}

/* ------------------------------------------------------------------------
 * ICMP
 * ---------------------------------------------------------------------- */

static volatile uint16_t ping_wait_seq;
static volatile bool ping_got;
static char ping_chan;

static void icmp_input(uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct icmp)) return;
    struct icmp *ic = (struct icmp *)data;
    if (ic->type == 8) {                     /* echo request -> reply */
        ic->type = 0;
        ic->csum = 0;
        ic->csum = csum(data, len, 0);
        ip_send(src, 1, data, len);
    } else if (ic->type == 0 && ntohs(ic->id) == 0x5A4E && ntohs(ic->seq) == ping_wait_seq) {
        ping_got = true;
        sched_wake(&ping_chan);
    }
}

int net_ping(uint32_t ip, uint16_t seq, int timeout_ms)
{
    if (!nd) return -1;
    uint8_t buf[sizeof(struct icmp) + 32];
    struct icmp *ic = (struct icmp *)buf;
    ic->type = 8;
    ic->code = 0;
    ic->id = htons(0x5A4E);
    ic->seq = htons(seq);
    for (int i = 0; i < 32; i++) buf[sizeof(*ic) + i] = (uint8_t)('a' + i % 26);
    ic->csum = 0;
    ic->csum = csum(buf, sizeof(buf), 0);
    ping_wait_seq = seq;
    ping_got = false;
    uint64_t t0 = uptime_ms();
    if (!ip_send(ip, 1, buf, sizeof(buf))) return -1;
    while (!ping_got && uptime_ms() - t0 < (uint64_t)timeout_ms) sched_wait(&ping_chan, NULL, 50);
    return ping_got ? (int)(uptime_ms() - t0) : -1;
}

/* ------------------------------------------------------------------------
 * UDP (one waiting receiver per port)
 * ---------------------------------------------------------------------- */

struct udp_sock {
    uint16_t port;
    uint8_t buf[1500];
    volatile int len;
    uint32_t from;
};
static struct udp_sock *udp_socks[4];
static char udp_chan;

static bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len)
{
    uint8_t pkt[1472];
    if (len + sizeof(struct udp) > sizeof(pkt)) return false;
    struct udp *u = (struct udp *)pkt;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(sizeof(*u) + len));
    u->csum = 0;
    memcpy(pkt + sizeof(*u), data, len);
    return ip_send(dst, 17, pkt, sizeof(*u) + len);
}

static void udp_input(uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct udp)) return;
    struct udp *u = (struct udp *)data;
    uint16_t port = ntohs(u->dport);
    size_t plen = MIN((size_t)ntohs(u->len), len) - sizeof(*u);
    spin_lock(&net_lock);
    for (int i = 0; i < 4; i++) {
        struct udp_sock *s = udp_socks[i];
        if (s && s->port == port && !s->len) {
            memcpy(s->buf, data + sizeof(*u), MIN(plen, sizeof(s->buf)));
            s->from = src;
            s->len = (int)MIN(plen, sizeof(s->buf));
        }
    }
    spin_unlock(&net_lock);
    sched_wake(&udp_chan);
}

static void udp_bind(struct udp_sock *s, uint16_t port)
{
    s->port = port;
    s->len = 0;
    spin_lock(&net_lock);
    for (int i = 0; i < 4; i++) if (!udp_socks[i]) { udp_socks[i] = s; break; }
    spin_unlock(&net_lock);
}

static void udp_unbind(struct udp_sock *s)
{
    spin_lock(&net_lock);
    for (int i = 0; i < 4; i++) if (udp_socks[i] == s) udp_socks[i] = NULL;
    spin_unlock(&net_lock);
}

static bool udp_wait(struct udp_sock *s, uint64_t deadline)
{
    while (!s->len && uptime_ms() < deadline) sched_wait(&udp_chan, NULL, 50);
    return s->len > 0;
}

/* ------------------------------------------------------------------------
 * DHCP
 * ---------------------------------------------------------------------- */

struct PACKED dhcp {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t opts[312];
};

static size_t dhcp_build(struct dhcp *d, uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t server)
{
    memset(d, 0, sizeof(*d));
    d->op = 1;
    d->htype = 1;
    d->hlen = 6;
    d->xid = htonl(xid);
    d->flags = htons(0x8000);
    memcpy(d->chaddr, nd->mac, 6);
    d->magic = htonl(0x63825363);
    uint8_t *o = d->opts;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; uint32_t v = htonl(req_ip); memcpy(o, &v, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; uint32_t v = htonl(server); memcpy(o, &v, 4); o += 4; }
    *o++ = 12; *o++ = 6; memcpy(o, "zenith", 6); o += 6;
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    return (size_t)(o - (uint8_t *)d);
}

static void dhcp_parse(struct dhcp *d, int len, uint8_t *type, uint32_t *mask, uint32_t *router, uint32_t *dns, uint32_t *server)
{
    uint8_t *o = d->opts, *end = (uint8_t *)d + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        uint8_t code = o[0], l = o[1];
        uint8_t *v = o + 2;
        uint32_t x = 0;
        if (l >= 4) { memcpy(&x, v, 4); x = ntohl(x); }
        switch (code) {
        case 53: *type = v[0]; break;
        case 1: *mask = x; break;
        case 3: *router = x; break;
        case 6: *dns = x; break;
        case 54: *server = x; break;
        }
        o += 2 + l;
    }
}

bool net_dhcp(int timeout_ms)
{
    if (!nd) return false;
    static struct udp_sock s;
    struct dhcp d;
    uint32_t xid = (uint32_t)rdtsc();
    uint32_t saved_ip = info.ip;
    info.ip = 0;
    udp_bind(&s, 68);
    bool ok = false;
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    size_t n = dhcp_build(&d, 1, xid, 0, 0);                                /* DISCOVER */
    udp_send(0xFFFFFFFF, 68, 67, &d, n);
    uint32_t offer = 0, server = 0, mask = 0, router = 0, dns = 0;
    while (udp_wait(&s, deadline)) {
        struct dhcp *r = (struct dhcp *)s.buf;
        uint8_t type = 0;
        if (ntohl(r->xid) == xid) {
            dhcp_parse(r, s.len, &type, &mask, &router, &dns, &server);
            if (type == 2 && !offer) {                                       /* OFFER */
                offer = ntohl(r->yiaddr);
                n = dhcp_build(&d, 3, xid, offer, server);                   /* REQUEST */
                s.len = 0;
                udp_send(0xFFFFFFFF, 68, 67, &d, n);
                continue;
            }
            if (type == 5 && offer) {                                        /* ACK */
                info.ip = ntohl(r->yiaddr);
                info.netmask = mask ? mask : 0xFFFFFF00;
                info.gateway = router;
                info.dns = dns ? dns : router;
                ok = true;
                break;
            }
        }
        s.len = 0;
    }
    udp_unbind(&s);
    if (!ok) info.ip = saved_ip;
    else {
        char ip[20];
        ip_to_str(info.ip, ip);
        klog("net: DHCP lease %s", ip);
    }
    return ok;
}

/* ------------------------------------------------------------------------
 * DNS
 * ---------------------------------------------------------------------- */

bool net_resolve(const char *host, uint32_t *ip, int timeout_ms)
{
    if (str_to_ip(host, ip)) return true;
    if (!strcmp(host, "localhost")) { *ip = 0x7F000001; return true; }
    if (!nd || !info.dns) return false;
    uint8_t q[512];
    memset(q, 0, 12);
    uint16_t id = (uint16_t)rdtsc();
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01;                /* recursion desired */
    q[5] = 1;                   /* one question */
    size_t p = 12;
    const char *s = host;
    while (*s && p < 400) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[p++] = (uint8_t)l;
        memcpy(q + p, s, l);
        p += l;
        s += l;
        if (*s == '.') s++;
    }
    q[p++] = 0;
    q[p++] = 0; q[p++] = 1;     /* type A */
    q[p++] = 0; q[p++] = 1;     /* class IN */
    static struct udp_sock sock;
    uint16_t port = (uint16_t)(40000 + (rdtsc() & 0x3FFF));
    udp_bind(&sock, port);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        udp_send(info.dns, port, 53, q, p);
        uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms / 2;
        while (udp_wait(&sock, deadline)) {
            uint8_t *r = sock.buf;
            int len = sock.len;
            if (len > 12 && r[0] == q[0] && r[1] == q[1]) {
                int an = r[6] << 8 | r[7];
                int off = 12;
                /* skip the question */
                while (off < len && r[off]) off += r[off] + 1;
                off += 5;
                for (int i = 0; i < an && off + 12 <= len; i++) {
                    if ((r[off] & 0xC0) == 0xC0) off += 2;
                    else { while (off < len && r[off]) off += r[off] + 1; off++; }
                    int type = r[off] << 8 | r[off + 1];
                    int rdlen = r[off + 8] << 8 | r[off + 9];
                    off += 10;
                    if (type == 1 && rdlen == 4 && off + 4 <= len) {
                        *ip = (uint32_t)r[off] << 24 | (uint32_t)r[off + 1] << 16 | (uint32_t)r[off + 2] << 8 | r[off + 3];
                        ok = true;
                        break;
                    }
                    off += rdlen;
                }
                break;
            }
            sock.len = 0;
        }
        sock.len = 0;
    }
    udp_unbind(&sock);
    return ok;
}

/* ------------------------------------------------------------------------
 * TCP: one active client connection at a time
 * ---------------------------------------------------------------------- */

enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_DONE };

static struct {
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t snd_nxt, rcv_nxt;
    uint8_t *rx;
    size_t rx_len, rx_cap;
    bool rst;
} tc;
static char tcp_chan;
static mutex_t tcp_mtx = MUTEX_INIT("tcp");

static bool tcp_send_seg(uint8_t flags, const void *data, size_t len)
{
    uint8_t pkt[1460 + sizeof(struct tcp)];
    struct tcp *t = (struct tcp *)pkt;
    t->sport = htons(tc.lport);
    t->dport = htons(tc.rport);
    t->seq = htonl(tc.snd_nxt);
    t->ack = htonl(tc.rcv_nxt);
    t->off = (sizeof(struct tcp) / 4) << 4;
    t->flags = flags;
    t->win = htons(32768);
    t->csum = 0;
    t->urg = 0;
    memcpy(pkt + sizeof(*t), data, len);
    /* pseudo header checksum */
    uint32_t sum = 0;
    uint32_t s = info.ip, d = tc.rip;
    sum += (s >> 16) + (s & 0xFFFF) + (d >> 16) + (d & 0xFFFF);
    sum += 6 + (uint32_t)(sizeof(*t) + len);
    t->csum = csum(pkt, sizeof(*t) + len, sum);
    return ip_send(tc.rip, 6, pkt, sizeof(*t) + len);
}

static void tcp_input(uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct tcp)) return;
    struct tcp *t = (struct tcp *)data;
    if (tc.state == TCP_CLOSED || src != tc.rip || ntohs(t->dport) != tc.lport || ntohs(t->sport) != tc.rport) return;
    size_t hl = (size_t)(t->off >> 4) * 4;
    if (hl > len) return;
    uint8_t *payload = data + hl;
    size_t plen = len - hl;
    uint32_t seq = ntohl(t->seq);
    if (t->flags & TCP_RST) { tc.rst = true; tc.state = TCP_DONE; sched_wake(&tcp_chan); return; }
    if (tc.state == TCP_SYN_SENT) {
        if ((t->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            tc.rcv_nxt = seq + 1;
            tc.snd_nxt = ntohl(t->ack);
            tc.state = TCP_ESTABLISHED;
            tcp_send_seg(TCP_ACK, NULL, 0);
            sched_wake(&tcp_chan);
        }
        return;
    }
    if (plen && seq == tc.rcv_nxt) {
        if (tc.rx_len + plen > tc.rx_cap) {
            size_t cap = MAX(tc.rx_cap * 2, tc.rx_len + plen + 4096);
            if (cap > MiB(8)) cap = MiB(8);
            uint8_t *n = krealloc(tc.rx, cap);
            if (n) { tc.rx = n; tc.rx_cap = cap; }
        }
        if (tc.rx_len + plen <= tc.rx_cap) {
            memcpy(tc.rx + tc.rx_len, payload, plen);
            tc.rx_len += plen;
        }
        tc.rcv_nxt += (uint32_t)plen;
    }
    if (t->flags & TCP_FIN && seq + plen == tc.rcv_nxt) {
        tc.rcv_nxt++;
        tc.state = TCP_DONE;
    }
    if (plen || (t->flags & TCP_FIN)) tcp_send_seg(TCP_ACK, NULL, 0);
    sched_wake(&tcp_chan);
}

static void split_url(const char *url, char *host, size_t hn, uint16_t *port, char *path, size_t pn)
{
    if (!strncmp(url, "http://", 7)) url += 7;
    const char *slash = strchr(url, '/');
    size_t hl = slash ? (size_t)(slash - url) : strlen(url);
    char hp[128];
    strlcpy(hp, url, MIN(hl + 1, sizeof(hp)));
    char *colon = strchr(hp, ':');
    *port = 80;
    if (colon) { *colon = 0; *port = (uint16_t)atoi(colon + 1); }
    strlcpy(host, hp, hn);
    strlcpy(path, slash ? slash : "/", pn);
}

/* find a header value (case-insensitive name) in a raw response */
static bool header_value(const uint8_t *rx, size_t hdr_end, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < hdr_end; i++) {
        if ((i == 0 || rx[i - 1] == '\n') && !strncasecmp((const char *)rx + i, name, nl) && rx[i + nl] == ':') {
            size_t j = i + nl + 1, k = 0;
            while (j < hdr_end && rx[j] == ' ') j++;
            while (j < hdr_end && rx[j] != '\r' && rx[j] != '\n' && k + 1 < n) out[k++] = (char)rx[j++];
            out[k] = 0;
            return true;
        }
    }
    return false;
}

static size_t find_header_end(void)
{
    for (size_t i = 0; i + 3 < tc.rx_len; i++)
        if (!memcmp(tc.rx + i, "\r\n\r\n", 4)) return i + 4;
    return 0;
}

int net_http_get_ex(const char *url, char **body, size_t *blen, char *location, size_t locn, int timeout_ms)
{
    if (location && locn) location[0] = 0;
    if (!nd || !info.ip) return -1;
    char host[128], path[512];
    uint16_t port;
    split_url(url, host, sizeof(host), &port, path, sizeof(path));
    uint32_t ip;
    if (!net_resolve(host, &ip, 3000)) return -1;

    mutex_lock(&tcp_mtx);
    memset(&tc, 0, sizeof(tc));
    tc.rip = ip;
    tc.rport = port;
    tc.lport = (uint16_t)(49152 + (rdtsc() & 0x3FFF));
    tc.snd_nxt = (uint32_t)rdtsc();
    tc.state = TCP_SYN_SENT;
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    int status = -1;
    for (int tries = 0; tries < 3 && tc.state == TCP_SYN_SENT; tries++) {
        tcp_send_seg(TCP_SYN, NULL, 0);
        uint64_t t = uptime_ms() + 1000;
        while (tc.state == TCP_SYN_SENT && uptime_ms() < t) sched_wait(&tcp_chan, NULL, 50);
    }
    if (tc.state == TCP_ESTABLISHED) {
        char req[768];
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ZenithOS/" ZENITH_VERSION
                         "\r\nAccept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n",
                         path, host);
        tcp_send_seg(TCP_PSH | TCP_ACK, req, (size_t)n);
        tc.snd_nxt += (uint32_t)n;
        size_t want = 0, hdr_end = 0;
        while (tc.state != TCP_DONE && uptime_ms() < deadline) {
            sched_wait(&tcp_chan, NULL, 50);
            if (!hdr_end && (hdr_end = find_header_end())) {
                char cl[24];
                if (header_value(tc.rx, hdr_end, "Content-Length", cl, sizeof(cl))) want = (size_t)atoi(cl);
            }
            if (hdr_end && want && tc.rx_len >= hdr_end + want) break;
        }
        tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
        /* parse the response */
        if (tc.rx_len > 12 && !memcmp(tc.rx, "HTTP/", 5)) {
            status = atoi((char *)tc.rx + 9);
            if (!hdr_end) hdr_end = find_header_end();
            if (!hdr_end) hdr_end = tc.rx_len;
            if (location && locn) header_value(tc.rx, hdr_end, "Location", location, locn);
            size_t bl = tc.rx_len - hdr_end;
            char *b = kmalloc(bl + 1);
            memcpy(b, tc.rx + hdr_end, bl);
            b[bl] = 0;
            *body = b;
            *blen = bl;
        }
    }
    kfree(tc.rx);
    tc.rx = NULL;
    tc.state = TCP_CLOSED;
    mutex_unlock(&tcp_mtx);
    return status;
}

int net_http_get(const char *url, char **body, size_t *blen, int timeout_ms)
{
    return net_http_get_ex(url, body, blen, NULL, 0, timeout_ms);
}

/* ------------------------------------------------------------------------
 * receive path
 * ---------------------------------------------------------------------- */

static void handle_frame(uint8_t *f, size_t len)
{
    if (len < sizeof(struct eth)) return;
    struct eth *e = (struct eth *)f;
    uint16_t type = ntohs(e->type);
    uint8_t *p = f + sizeof(*e);
    size_t pl = len - sizeof(*e);
    if (type == ETH_ARP && pl >= sizeof(struct arp)) {
        struct arp *a = (struct arp *)p;
        uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
        if (spa) arp_store(spa, a->sha);
        if (ntohs(a->op) == 1 && info.ip && tpa == info.ip) arp_send(2, a->sha, spa);
        return;
    }
    if (type != ETH_IP || pl < sizeof(struct ip4)) return;
    struct ip4 *h = (struct ip4 *)p;
    size_t hl = (size_t)(h->vihl & 15) * 4;
    size_t tl = MIN((size_t)ntohs(h->len), pl);
    if (hl < 20 || hl > tl) return;
    uint32_t dst = ntohl(h->dst), src = ntohl(h->src);
    if (info.ip && dst != info.ip && dst != 0xFFFFFFFF && (dst | info.netmask) != 0xFFFFFFFF) return;
    uint8_t *payload = p + hl;
    size_t plen = tl - hl;
    switch (h->proto) {
    case 1: icmp_input(src, payload, plen); break;
    case 17: udp_input(src, payload, plen); break;
    case 6: tcp_input(src, payload, plen); break;
    }
}

static int net_thread(void *arg)
{
    UNUSED(arg);
    static uint8_t frame[2048];
    for (;;) {
        int n = 0, got = 0;
        while ((n = nd->poll(nd, frame, sizeof(frame))) > 0) {
            info.rx_packets++;
            info.rx_bytes += (uint64_t)n;
            handle_frame(frame, (size_t)n);
            got++;
            last_rx_t = uptime_ms();
        }
        info.link = nd->link(nd);
        /* poll fast while traffic is flowing, relax when idle */
        sched_sleep(uptime_ms() - last_rx_t < 2000 ? 1 : 10);
    }
    return 0;
}

static int dhcp_boot(void *arg)
{
    UNUSED(arg);
    sched_sleep(300);
    for (int i = 0; i < 3 && !info.ip; i++) net_dhcp(3000);
    return 0;
}

bool net_get_info(struct net_info *out)
{
    if (!nd) return false;
    *out = info;
    return true;
}

void net_init(void)
{
    nd = e1000_init();
    if (!nd) {
        klog("net: no supported network adapter");
        return;
    }
    strcpy(info.ifname, "eth0");
    strlcpy(info.driver, nd->name, sizeof(info.driver));
    memcpy(info.mac, nd->mac, 6);
    info.link = nd->link(nd);
    thread_create_ex("net", net_thread, NULL, 1, -1, NULL, 32 * 1024);
    thread_create("dhcp", dhcp_boot, NULL);
}
