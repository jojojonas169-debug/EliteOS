/*
 * Realtek RTL8169/8110 and the PCI Express RTL8111/8168 and RTL8101/8102
 * family, in "C+" descriptor mode: rings of 16-byte descriptors with an
 * OWN bit, the end of each ring marked by EOR.
 *
 * No emulator models these chips, so this driver has not run on hardware
 * yet; it follows the documented register interface and does no per-chip
 * PHY tuning.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "../netdev.h"

#define IDR0      0x00
#define MAR0      0x08
#define TNPDS     0x20
#define CHIPCMD   0x37
#define TPPOLL    0x38
#define INTRMASK  0x3C
#define INTRSTAT  0x3E
#define TXCONFIG  0x40
#define RXCONFIG  0x44
#define CFG9346   0x50
#define PHYSTATUS 0x6C
#define RXMAXSIZE 0xDA
#define CPLUSCMD  0xE0
#define RDSAR     0xE4
#define MAXTXPKT  0xEC

#define CMD_RST 0x10
#define CMD_RE  0x08
#define CMD_TE  0x04

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS  (1u << 29)
#define LS  (1u << 28)
#define RES (1u << 21)      /* receive error summary */

#define NRX 128
#define NTX 64
#define BUFSZ 2048

struct desc { uint32_t opts1, opts2; uint64_t addr; };

struct rtl {
    struct netdev nd;
    volatile uint8_t *mmio;
    struct desc *rx, *tx;
    uint8_t *rxbuf, *txbuf;
    unsigned rx_cur, tx_cur;
    spinlock_t lock;
};

static bool r_send(struct netdev *nd, const void *data, size_t len)
{
    struct rtl *r = nd->priv;
    if (len > BUFSZ) return false;
    spin_lock(&r->lock);
    struct desc *d = &r->tx[r->tx_cur];
    for (int i = 0; i < 100000 && (((volatile struct desc *)d)->opts1 & OWN); i++) cpu_relax();
    if (d->opts1 & OWN) { spin_unlock(&r->lock); return false; }
    memcpy(r->txbuf + (size_t)r->tx_cur * BUFSZ, data, len);
    if (len < 60) { memset(r->txbuf + (size_t)r->tx_cur * BUFSZ + len, 0, 60 - len); len = 60; }
    d->opts2 = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d->opts1 = OWN | FS | LS | (uint32_t)len | (r->tx_cur == NTX - 1 ? EOR : 0);
    r->tx_cur = (r->tx_cur + 1) % NTX;
    mmio_w8(r->mmio, TPPOLL, 0x40);                     /* normal priority queue */
    spin_unlock(&r->lock);
    return true;
}

static int r_poll(struct netdev *nd, void *buf, size_t cap)
{
    struct rtl *r = nd->priv;
    for (;;) {
        struct desc *d = &r->rx[r->rx_cur];
        uint32_t o = ((volatile struct desc *)d)->opts1;
        if (o & OWN) return 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        int len = 0;
        if (!(o & RES) && (o & FS) && (o & LS)) {
            int n = (int)(o & 0x3FFF) - 4;                  /* includes the CRC */
            if (n > 0) {
                len = MIN(n, (int)cap);
                memcpy(buf, r->rxbuf + (size_t)r->rx_cur * BUFSZ, (size_t)len);
            }
        }
        d->opts2 = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        d->opts1 = OWN | BUFSZ | (r->rx_cur == NRX - 1 ? EOR : 0);
        r->rx_cur = (r->rx_cur + 1) % NRX;
        mmio_w16(r->mmio, INTRSTAT, 0xFFFF);
        if (len) return len;
    }
}

static bool r_link(struct netdev *nd)
{
    struct rtl *r = nd->priv;
    return mmio_r8(r->mmio, PHYSTATUS) & 0x02;
}

static struct netdev *r_attach(struct pci_dev *p, const struct nic_id *id)
{
    /* the register window is the first memory BAR (BAR1 on 8169, BAR2 on 8168) */
    uint64_t bar = 0;
    for (int i = 0; i < 6 && !bar; i++)
        if (p->bar[i] && !pci_bar_is_io(p, i)) bar = pci_bar_addr(p, i);
    if (!bar) return NULL;
    struct rtl *r = kzalloc(sizeof(*r));
    r->lock = (spinlock_t)SPINLOCK_INIT("r8169");
    r->mmio = vmm_map_mmio(bar, 256);
    pci_enable_busmaster(p);

    mmio_w16(r->mmio, INTRMASK, 0);
    mmio_w8(r->mmio, CHIPCMD, CMD_RST);
    for (int i = 0; i < 1000 && (mmio_r8(r->mmio, CHIPCMD) & CMD_RST); i++) udelay(10);
    for (int i = 0; i < 6; i++) r->nd.mac[i] = mmio_r8(r->mmio, IDR0 + i);

    uint64_t rx_phys, tx_phys;
    r->rx = dma_alloc(NRX * sizeof(struct desc), &rx_phys);      /* page aligned (needs 256) */
    r->tx = dma_alloc(NTX * sizeof(struct desc), &tx_phys);
    r->rxbuf = dma_alloc(NRX * BUFSZ, NULL);
    r->txbuf = dma_alloc(NTX * BUFSZ, NULL);
    if (!r->rx || !r->tx || !r->rxbuf || !r->txbuf) return NULL;
    for (int i = 0; i < NRX; i++) {
        r->rx[i].addr = V2P(r->rxbuf + (size_t)i * BUFSZ);
        r->rx[i].opts1 = OWN | BUFSZ | (i == NRX - 1 ? EOR : 0);
    }
    for (int i = 0; i < NTX; i++) {
        r->tx[i].addr = V2P(r->txbuf + (size_t)i * BUFSZ);
        r->tx[i].opts1 = i == NTX - 1 ? EOR : 0;
    }

    mmio_w8(r->mmio, CFG9346, 0xC0);                    /* unlock configuration registers */
    mmio_w16(r->mmio, CPLUSCMD, (uint16_t)(mmio_r16(r->mmio, CPLUSCMD) | 0x0008));   /* PCI multiple read/write */
    mmio_w8(r->mmio, CHIPCMD, CMD_TE | CMD_RE);         /* the 8169 wants these on before its config */
    mmio_w16(r->mmio, RXMAXSIZE, 1536);
    mmio_w8(r->mmio, MAXTXPKT, 0x3B);
    /* accept broadcast, multicast, our address; no rx threshold; unlimited DMA bursts */
    mmio_w32(r->mmio, RXCONFIG, 0x0E | (7u << 8) | (7u << 13));
    mmio_w32(r->mmio, TXCONFIG, (3u << 24) | (7u << 8));  /* standard IFG, unlimited DMA bursts */
    mmio_w32(r->mmio, MAR0, 0xFFFFFFFF);
    mmio_w32(r->mmio, MAR0 + 4, 0xFFFFFFFF);
    mmio_w32(r->mmio, TNPDS, (uint32_t)tx_phys);
    mmio_w32(r->mmio, TNPDS + 4, (uint32_t)(tx_phys >> 32));
    mmio_w32(r->mmio, RDSAR, (uint32_t)rx_phys);
    mmio_w32(r->mmio, RDSAR + 4, (uint32_t)(rx_phys >> 32));
    mmio_w8(r->mmio, CHIPCMD, CMD_TE | CMD_RE);
    mmio_w8(r->mmio, CFG9346, 0x00);                    /* lock */
    mmio_w16(r->mmio, INTRSTAT, 0xFFFF);

    snprintf(r->nd.name, sizeof(r->nd.name), "%s", id->model);
    r->nd.send = r_send;
    r->nd.poll = r_poll;
    r->nd.link = r_link;
    r->nd.priv = r;
    return &r->nd;
}

static const struct nic_id ids[] = {
    { 0x10EC, 0x8169, "Realtek RTL8169/8110 Gigabit Ethernet" },
    { 0x10EC, 0x8167, "Realtek RTL8169SC/8110SC Gigabit Ethernet" },
    { 0x10EC, 0x8168, "Realtek RTL8111/8168 PCIe Gigabit Ethernet" },
    { 0x10EC, 0x8161, "Realtek RTL8111/8168 PCIe Gigabit Ethernet" },
    { 0x10EC, 0x8136, "Realtek RTL8101/8102/8103 PCIe Fast Ethernet" },
    { 0x1186, 0x4300, "D-Link DGE-528T (RTL8169)" },
    { 0x1186, 0x4302, "D-Link DGE-530T rev C (RTL8169)" },
    { 0x1259, 0xC107, "Allied Telesyn AT-2450 (RTL8169)" },
    { 0x16EC, 0x0116, "US Robotics USR997902 (RTL8169)" },
    { 0x1737, 0x1032, "Linksys EG1032 v3 (RTL8169)" },
    { 0 },
};

const struct nic_driver drv_r8169 = { "r8169", "Realtek RTL8169/8111/8168/8101 (untested: no emulator)", ids, r_attach };
