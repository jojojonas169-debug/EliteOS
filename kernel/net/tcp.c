/*
 * TCP (RFC 793/9293), client side: up to 32 connections at once, MSS
 * negotiation, send and receive windows, retransmission with exponential
 * back-off, orderly close. Segments that arrive ahead of a lost one are
 * kept and used once the gap is filled, so one lost packet costs one
 * retransmission rather than the rest of the window. Incoming segments
 * are handled on the network thread; tcp_timer() runs there too.
 */
#include <kernel.h>
#include <net.h>
#include <sched.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <crypto.h>
#include "netdev.h"

#define MAX_SOCKS 32
#define RX_CAP (256 * 1024)
#define TX_CAP (128 * 1024)
#define OUR_MSS 1460
#define OOO_MAX 24                  /* out-of-order segments kept per connection */

struct ooo_seg {
    uint32_t seq;
    uint16_t len;
    bool used;
    uint8_t data[OUR_MSS];
};

enum { S_FREE, S_INIT, S_SYN_SENT, S_ESTABLISHED, S_FIN_WAIT, S_CLOSE_WAIT, S_CLOSED };

struct tcp_sock {
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    /* sending */
    uint32_t iss, snd_una, snd_nxt;
    uint32_t snd_wnd;
    uint16_t mss;
    uint8_t *tx;
    size_t tx_len;                  /* bytes from snd_una on: sent-unacked + unsent */
    bool fin_queued, fin_sent, fin_acked;
    uint64_t rto_at;
    int rto_ms, retries;
    /* receiving */
    uint32_t rcv_nxt;
    uint8_t *rx;
    size_t rx_head, rx_len;
    struct ooo_seg *ooo;
    bool peer_fin, reset, window_closed;
    char chan;
};

static struct tcp_sock socks[MAX_SOCKS];
static spinlock_t tcp_lock = SPINLOCK_INIT("tcp");
static uint16_t next_port;

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int32_t)((a) - (b)) <= 0)

struct PACKED tcph {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
};

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

static uint16_t rx_window(struct tcp_sock *s)
{
    size_t free = RX_CAP - s->rx_len;
    return (uint16_t)MIN(MIN(free, (size_t)65535), (size_t)net_rx_window(s->rip));
}

/* build and send one segment; data is taken from the send buffer at offset off */
static void send_seg(struct tcp_sock *s, uint32_t seq, uint8_t flags, size_t off, size_t len)
{
    uint8_t pkt[sizeof(struct tcph) + 4 + OUR_MSS];
    struct tcph *t = (struct tcph *)pkt;
    size_t hl = sizeof(*t);
    if (flags & F_SYN) {
        uint8_t *o = pkt + hl;               /* MSS option */
        o[0] = 2; o[1] = 4; o[2] = OUR_MSS >> 8; o[3] = OUR_MSS & 0xFF;
        hl += 4;
    }
    t->sport = bswap16(s->lport);
    t->dport = bswap16(s->rport);
    t->seq = bswap32(seq);
    t->ack = (flags & F_ACK) ? bswap32(s->rcv_nxt) : 0;
    t->off = (uint8_t)((hl / 4) << 4);
    t->flags = flags;
    t->win = bswap16(rx_window(s));
    t->csum = 0;
    t->urg = 0;
    if (len) memcpy(pkt + hl, s->tx + off, len);
    uint32_t src = net_src_ip(s->rip), dst = s->rip;
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + (uint32_t)(hl + len);
    t->csum = net_csum(pkt, hl + len, sum);
    net_ip_send(s->rip, 6, pkt, hl + len);
}

static void arm_rto(struct tcp_sock *s)
{
    s->rto_at = uptime_ms() + (uint64_t)s->rto_ms;
}

/* send whatever the windows allow */
static void output(struct tcp_sock *s)
{
    if (s->state != S_ESTABLISHED && s->state != S_CLOSE_WAIT && s->state != S_FIN_WAIT) return;
    size_t wnd = MIN((size_t)s->snd_wnd, (size_t)(64 * 1024));
    for (;;) {
        size_t inflight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
        if (inflight >= s->tx_len) break;
        size_t avail = s->tx_len - inflight;
        if (inflight >= wnd) break;
        size_t n = MIN(MIN(avail, (size_t)s->mss), wnd - inflight);
        if (!n) break;
        bool was_idle = s->snd_nxt == s->snd_una;
        send_seg(s, s->snd_nxt, F_ACK | F_PSH, inflight, n);
        s->snd_nxt += (uint32_t)n;
        if (was_idle) arm_rto(s);
    }
    size_t inflight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
    if (s->fin_queued && !s->fin_sent && inflight == s->tx_len) {
        bool was_idle = s->snd_nxt == s->snd_una;
        send_seg(s, s->snd_nxt, F_FIN | F_ACK, 0, 0);
        s->snd_nxt++;
        s->fin_sent = true;
        if (was_idle) arm_rto(s);
    }
}

static void send_ack(struct tcp_sock *s) { send_seg(s, s->snd_nxt, F_ACK, 0, 0); }

static struct tcp_sock *lookup(uint32_t rip, uint16_t rport, uint16_t lport)
{
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct tcp_sock *s = &socks[i];
        if (s->state > S_INIT && s->rip == rip && s->rport == rport && s->lport == lport) return s;
    }
    return NULL;
}

/* append in-order data to the receive ring; returns how much fitted */
static size_t rx_append(struct tcp_sock *s, const uint8_t *p, size_t n)
{
    size_t take = MIN(n, RX_CAP - s->rx_len);
    for (size_t i = 0; i < take; i++) s->rx[(s->rx_head + s->rx_len + i) % RX_CAP] = p[i];
    s->rx_len += take;
    s->rcv_nxt += (uint32_t)take;
    return take;
}

void tcp_input(uint32_t src, uint8_t *data, size_t len)
{
    if (len < sizeof(struct tcph)) return;
    struct tcph *t = (struct tcph *)data;
    size_t hl = (size_t)(t->off >> 4) * 4;
    if (hl < sizeof(*t) || hl > len) return;
    spin_lock(&tcp_lock);
    struct tcp_sock *s = lookup(src, bswap16(t->sport), bswap16(t->dport));
    if (!s) { spin_unlock(&tcp_lock); return; }
    uint32_t seq = bswap32(t->seq), ack = bswap32(t->ack);
    uint8_t flags = t->flags;
    uint8_t *payload = data + hl;
    size_t plen = len - hl;
    bool wake = false;

    if (flags & F_RST) {
        s->reset = true;
        s->state = S_CLOSED;
        sched_wake(&s->chan);
        spin_unlock(&tcp_lock);
        return;
    }
    if (s->state == S_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->iss + 1) {
            s->rcv_nxt = seq + 1;
            s->snd_una = s->snd_nxt = ack;
            s->snd_wnd = bswap16(t->win);
            s->mss = 536;
            /* the peer's MSS option */
            for (size_t o = sizeof(*t); o + 1 < hl;) {
                uint8_t kind = data[o];
                if (kind == 0) break;
                if (kind == 1) { o++; continue; }
                uint8_t ol = data[o + 1];
                if (ol < 2) break;
                if (kind == 2 && ol == 4) s->mss = (uint16_t)MIN((data[o + 2] << 8) | data[o + 3], OUR_MSS);
                o += ol;
            }
            s->state = S_ESTABLISHED;
            s->retries = 0;
            s->rto_ms = 1000;
            send_ack(s);
            sched_wake(&s->chan);
        }
        spin_unlock(&tcp_lock);
        return;
    }

    /* acknowledgements */
    if (flags & F_ACK) {
        if (SEQ_LT(s->snd_una, ack) && SEQ_LE(ack, s->snd_nxt)) {
            uint32_t acked = ack - s->snd_una;
            if (s->fin_sent && ack == s->snd_nxt) {
                s->fin_acked = true;
                acked--;
            }
            size_t drop = MIN((size_t)acked, s->tx_len);
            memmove(s->tx, s->tx + drop, s->tx_len - drop);
            s->tx_len -= drop;
            s->snd_una = ack;
            s->retries = 0;
            s->rto_ms = 1000;
            if (s->snd_una != s->snd_nxt) arm_rto(s);
            wake = true;
        }
        s->snd_wnd = bswap16(t->win);
    }

    /* data in order; anything else is dropped and re-acknowledged */
    bool need_ack = false;
    if (plen) {
        if (SEQ_LT(seq, s->rcv_nxt)) {
            uint32_t old = s->rcv_nxt - seq;
            if (old >= plen) { plen = 0; }
            else { payload += old; plen -= old; seq = s->rcv_nxt; }
        }
        if (plen && seq == s->rcv_nxt) {
            size_t take = rx_append(s, payload, plen);
            if (take < plen) s->window_closed = true;
            /* segments that were waiting behind this one */
            for (bool more = take == plen; more && s->ooo;) {
                more = false;
                for (int i = 0; i < OOO_MAX; i++) {
                    struct ooo_seg *o = &s->ooo[i];
                    if (!o->used || SEQ_LT(s->rcv_nxt, o->seq)) continue;
                    o->used = false;
                    uint32_t skip = s->rcv_nxt - o->seq;
                    if (skip < o->len) {
                        size_t n = o->len - skip;
                        if (rx_append(s, o->data + skip, n) < n) { s->window_closed = true; break; }
                    }
                    more = true;
                }
            }
            wake = true;
        } else if (plen && s->ooo && plen <= OUR_MSS && SEQ_LE(seq + (uint32_t)plen, s->rcv_nxt + (uint32_t)(RX_CAP - s->rx_len))) {
            /* ahead of a gap: keep it (the duplicate ACK below asks for the gap) */
            int slot = -1;
            for (int i = 0; i < OOO_MAX; i++) {
                if (s->ooo[i].used && s->ooo[i].seq == seq) { slot = -2; break; }
                if (!s->ooo[i].used && slot == -1) slot = i;
            }
            if (slot >= 0) {
                s->ooo[slot].seq = seq;
                s->ooo[slot].len = (uint16_t)plen;
                memcpy(s->ooo[slot].data, payload, plen);
                s->ooo[slot].used = true;
            }
        }
        need_ack = true;
    }
    if ((flags & F_FIN) && seq + plen == s->rcv_nxt && !s->peer_fin) {
        s->rcv_nxt++;
        s->peer_fin = true;
        if (s->state == S_ESTABLISHED) s->state = S_CLOSE_WAIT;
        need_ack = wake = true;
    }
    if (need_ack) send_ack(s);
    output(s);
    if (wake) sched_wake(&s->chan);
    spin_unlock(&tcp_lock);
}

void tcp_timer(void)
{
    uint64_t now = uptime_ms();
    spin_lock(&tcp_lock);
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct tcp_sock *s = &socks[i];
        if (s->state <= S_INIT || s->state == S_CLOSED) continue;
        bool outstanding = s->state == S_SYN_SENT || s->snd_una != s->snd_nxt;
        if (!outstanding || now < s->rto_at) continue;
        if (++s->retries > 8) {
            s->reset = true;
            s->state = S_CLOSED;
            sched_wake(&s->chan);
            continue;
        }
        s->rto_ms = MIN(s->rto_ms * 2, 16000);
        if (s->state == S_SYN_SENT) {
            send_seg(s, s->iss, F_SYN, 0, 0);
        } else {
            /* go back to the first unacknowledged byte */
            s->snd_nxt = s->snd_una;
            s->fin_sent = false;
            output(s);
        }
        arm_rto(s);
    }
    spin_unlock(&tcp_lock);
}

/* ------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

tcp_sock_t *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms)
{
    if (!net_src_ip(ip)) return NULL;
    spin_lock(&tcp_lock);
    struct tcp_sock *s = NULL;
    for (int i = 0; i < MAX_SOCKS && !s; i++)
        if (socks[i].state == S_FREE) s = &socks[i];
    if (!s) { spin_unlock(&tcp_lock); return NULL; }
    uint8_t *rx = s->rx, *tx = s->tx;
    struct ooo_seg *ooo = s->ooo;
    memset(s, 0, sizeof(*s));
    s->rx = rx;
    s->tx = tx;
    s->ooo = ooo;
    if (ooo) for (int i = 0; i < OOO_MAX; i++) ooo[i].used = false;
    s->state = S_INIT;                          /* claims the slot; the timer leaves it alone */
    spin_unlock(&tcp_lock);
    if (!s->rx) s->rx = kmalloc(RX_CAP);
    if (!s->tx) s->tx = kmalloc(TX_CAP);
    if (!s->ooo) s->ooo = kzalloc(OOO_MAX * sizeof(struct ooo_seg));
    if (!s->rx || !s->tx || !net_route_ready(ip, 2000)) {
        klog("tcp: no route to the destination (ARP failed)");
        spin_lock(&tcp_lock);
        s->state = S_FREE;
        spin_unlock(&tcp_lock);
        return NULL;
    }
    uint32_t iss;
    random_bytes(&iss, sizeof(iss));
    if (!next_port) random_bytes(&next_port, sizeof(next_port));
    spin_lock(&tcp_lock);
    s->rip = ip;
    s->rport = port;
    s->lport = (uint16_t)(49152 + (next_port++ % 16000));
    s->iss = iss;
    s->snd_una = s->snd_nxt = iss;
    s->mss = 536;
    s->rto_ms = 1000;
    s->state = S_SYN_SENT;
    send_seg(s, iss, F_SYN, 0, 0);
    arm_rto(s);
    spin_unlock(&tcp_lock);
    uint64_t deadline = uptime_ms() + (uint64_t)timeout_ms;
    while (s->state == S_SYN_SENT && uptime_ms() < deadline) sched_wait(&s->chan, NULL, 50);
    if (s->state != S_ESTABLISHED) {
        klog("tcp: connect failed: state %d reset %d retries %d", s->state, s->reset, s->retries);
        spin_lock(&tcp_lock);
        s->state = S_FREE;
        spin_unlock(&tcp_lock);
        return NULL;
    }
    return s;
}

long tcp_send(tcp_sock_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        spin_lock(&tcp_lock);
        if (s->state == S_CLOSED || s->reset || s->fin_queued) {
            spin_unlock(&tcp_lock);
            return done ? (long)done : -1;
        }
        size_t room = TX_CAP - s->tx_len;
        size_t k = MIN(room, len - done);
        if (k) {
            memcpy(s->tx + s->tx_len, p + done, k);
            s->tx_len += k;
            done += k;
            output(s);
        }
        spin_unlock(&tcp_lock);
        if (!k) sched_wait(&s->chan, NULL, 50);
    }
    return (long)done;
}

long tcp_recv(tcp_sock_t *s, void *buf, size_t len, int timeout_ms)
{
    uint64_t deadline = uptime_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        spin_lock(&tcp_lock);
        if (s->rx_len) {
            size_t k = MIN(len, s->rx_len);
            uint8_t *o = buf;
            for (size_t i = 0; i < k; i++) o[i] = s->rx[(s->rx_head + i) % RX_CAP];
            s->rx_head = (s->rx_head + k) % RX_CAP;
            s->rx_len -= k;
            /* tell the peer once the window has opened up again */
            if (s->window_closed || RX_CAP - s->rx_len - k < OUR_MSS * 4) {
                s->window_closed = false;
                if (s->state != S_CLOSED) send_ack(s);
            }
            spin_unlock(&tcp_lock);
            return (long)k;
        }
        bool eof = s->peer_fin || s->state == S_CLOSED;
        bool err = s->reset;
        spin_unlock(&tcp_lock);
        if (err) return -1;
        if (eof) return 0;
        if (timeout_ms >= 0 && uptime_ms() >= deadline) return -2;
        sched_wait(&s->chan, NULL, 50);
    }
}

void tcp_close(tcp_sock_t *s)
{
    if (!s) return;
    spin_lock(&tcp_lock);
    if (s->state == S_ESTABLISHED || s->state == S_CLOSE_WAIT) {
        s->fin_queued = true;
        if (s->state == S_ESTABLISHED) s->state = S_FIN_WAIT;
        output(s);
    }
    spin_unlock(&tcp_lock);
    /* give the FIN a moment to be acknowledged, but do not hang around */
    uint64_t deadline = uptime_ms() + 1500;
    while (!s->fin_acked && !s->reset && s->state != S_CLOSED && uptime_ms() < deadline) sched_wait(&s->chan, NULL, 50);
    spin_lock(&tcp_lock);
    if (!s->fin_acked && !s->reset && s->state != S_CLOSED) send_seg(s, s->snd_nxt, F_RST | F_ACK, 0, 0);
    s->state = S_FREE;
    spin_unlock(&tcp_lock);
}

int tcp_open_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; i++) if (socks[i].state != S_FREE) n++;
    return n;
}
