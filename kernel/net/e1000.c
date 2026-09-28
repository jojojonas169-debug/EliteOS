/*
 * Intel 8254x (e1000) network driver: legacy descriptors, polled by the
 * network thread (no dependency on PCI interrupt routing).
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <spinlock.h>
#include <x86.h>
#include "netdev.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define NRX 32
#define NTX 32
#define BUFSZ 2048

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } PACKED;
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } PACKED;

static volatile uint8_t *mmio;
static struct rx_desc *rx;
static struct tx_desc *tx;
static uint8_t *rxbuf[NRX], *txbuf[NTX];
static unsigned rx_cur, tx_cur;
static bool present;
static spinlock_t tx_lock = SPINLOCK_INIT("e1000-tx");

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }

static bool e1000_send(struct netdev *nd, const void *data, size_t len)
{
    UNUSED(nd);
    if (!present || len > BUFSZ) return false;
    spin_lock(&tx_lock);
    struct tx_desc *d = &tx[tx_cur];
    /* wait for the slot to be free (descriptor done) */
    for (int i = 0; i < 100000 && d->cmd && !(d->status & 1); i++) cpu_relax();
    memcpy(txbuf[tx_cur], data, len);
    d->addr = V2P(txbuf[tx_cur]);
    d->len = (uint16_t)len;
    d->status = 0;
    d->cmd = (1 << 0) | (1 << 1) | (1 << 3);    /* EOP, IFCS, RS */
    tx_cur = (tx_cur + 1) % NTX;
    wr(REG_TDT, tx_cur);
    spin_unlock(&tx_lock);
    return true;
}

static int e1000_poll(struct netdev *nd, void *buf, size_t cap)
{
    UNUSED(nd);
    if (!present) return 0;
    struct rx_desc *d = &rx[rx_cur];
    if (!(d->status & 1)) return 0;
    int len = MIN((int)d->len, (int)cap);
    memcpy(buf, rxbuf[rx_cur], (size_t)len);
    d->status = 0;
    unsigned old = rx_cur;
    rx_cur = (rx_cur + 1) % NRX;
    wr(REG_RDT, old);
    return len;
}

static bool e1000_link(struct netdev *nd)
{
    UNUSED(nd);
    return present && (rd(REG_STATUS) & 2);
}

static uint16_t eeprom_read(uint8_t addr)
{
    wr(REG_EERD, 1 | ((uint32_t)addr << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1 << 4)) return (uint16_t)(v >> 16);
    }
    return 0;
}

static struct netdev dev = { .send = e1000_send, .poll = e1000_poll, .link = e1000_link };

struct netdev *e1000_init(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x10D3, 0x100C, 0x107C };
    struct pci_dev *p = NULL;
    for (unsigned i = 0; i < ARRAY_SIZE(ids) && !p; i++) p = pci_find(0x8086, ids[i]);
    if (!p) return NULL;
    uint64_t bar = pci_bar_addr(p, 0);
    mmio = vmm_map_mmio(bar, 128 * 1024);
    pci_enable_busmaster(p);

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));          /* reset */
    mdelay(10);
    wr(REG_IMC, 0xFFFFFFFF);
    rd(REG_ICR);
    wr(REG_CTRL, (rd(REG_CTRL) | (1u << 6) | (1u << 5)) & ~(1u << 3));   /* SLU, ASDE, clear LRST */

    uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
    if (ral || (rah & 0xFFFF)) {
        for (int i = 0; i < 4; i++) dev.mac[i] = (uint8_t)(ral >> (i * 8));
        dev.mac[4] = (uint8_t)rah;
        dev.mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read((uint8_t)i);
            dev.mac[i * 2] = (uint8_t)w;
            dev.mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
        wr(REG_RAL, (uint32_t)dev.mac[0] | ((uint32_t)dev.mac[1] << 8) | ((uint32_t)dev.mac[2] << 16) | ((uint32_t)dev.mac[3] << 24));
        wr(REG_RAH, (uint32_t)dev.mac[4] | ((uint32_t)dev.mac[5] << 8) | (1u << 31));
    }
    for (int i = 0; i < 128; i++) wr(REG_MTA + (uint32_t)i * 4, 0);

    /* receive ring */
    rx = P2V(pmm_alloc());
    for (int i = 0; i < NRX; i++) {
        if (i % 2 == 0) {
            uint8_t *pg = P2V(pmm_alloc());
            rxbuf[i] = pg;
            rxbuf[i + 1] = pg + BUFSZ;
        }
        rx[i].addr = V2P(rxbuf[i]);
        rx[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)V2P(rx));
    wr(REG_RDBAH, (uint32_t)(V2P(rx) >> 32));
    wr(REG_RDLEN, NRX * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));      /* EN, BAM, SECRC, 2048-byte buffers */

    /* transmit ring */
    tx = P2V(pmm_alloc());
    for (int i = 0; i < NTX; i++) {
        if (i % 2 == 0) {
            uint8_t *pg = P2V(pmm_alloc());
            txbuf[i] = pg;
            txbuf[i + 1] = pg + BUFSZ;
        }
        tx[i].addr = V2P(txbuf[i]);
        tx[i].cmd = 0;
        tx[i].status = 1;
    }
    wr(REG_TDBAL, (uint32_t)V2P(tx));
    wr(REG_TDBAH, (uint32_t)(V2P(tx) >> 32));
    wr(REG_TDLEN, NTX * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));   /* EN, PSP, CT, COLD */
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));

    present = true;
    const char *name = pci_device_name(p->vendor, p->device);
    snprintf(dev.name, sizeof(dev.name), "Intel %s", name ? name : "e1000");
    klog("e1000: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", dev.name, dev.mac[0], dev.mac[1], dev.mac[2],
         dev.mac[3], dev.mac[4], dev.mac[5], e1000_link(&dev) ? "up" : "down");
    return &dev;
}
