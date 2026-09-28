/* PCI bus enumeration via configuration mechanism #1. */
#include <kernel.h>
#include <dev.h>
#include <x86.h>
#include <mm.h>

#define MAX_PCI 128

static struct pci_dev devs[MAX_PCI];
static int ndevs;

static uint32_t addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(0xCF8, addr(bus, dev, fn, off));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(0xCF8, addr(bus, dev, fn, off));
    outl(0xCFC, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return (uint16_t)(pci_read32(bus, dev, fn, off) >> ((off & 2) * 8));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    uint32_t cur = pci_read32(bus, dev, fn, off);
    int sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(bus, dev, fn, off, cur);
}

void pci_enable_busmaster(struct pci_dev *d)
{
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0006;          /* memory space + bus master */
    cmd &= (uint16_t)~0x0400;  /* interrupts enabled */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}

uint64_t pci_bar_addr(struct pci_dev *d, int bar)
{
    uint32_t v = d->bar[bar];
    if (v & 1) return v & ~3u;                      /* I/O */
    uint64_t a = v & ~0xFu;
    if (((v >> 1) & 3) == 2 && bar < 5) a |= (uint64_t)d->bar[bar + 1] << 32;
    return a;
}

static void probe(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = pci_read32(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs >= MAX_PCI) return;
    struct pci_dev *d = &devs[ndevs++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    d->rev = cls & 0xFF;
    d->progif = (cls >> 8) & 0xFF;
    d->subcls = (cls >> 16) & 0xFF;
    d->cls = cls >> 24;
    uint32_t irq = pci_read32(bus, dev, fn, 0x3C);
    d->irq_line = irq & 0xFF;
    d->irq_pin = (irq >> 8) & 0xFF;
    uint8_t htype = (pci_read32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
    if (htype == 0)
        for (int i = 0; i < 6; i++) d->bar[i] = pci_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4));
}

void pci_init(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            probe((uint8_t)bus, (uint8_t)dev, 0);
            uint8_t ht = (pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C) >> 16) & 0xFF;
            if (ht & 0x80)
                for (int fn = 1; fn < 8; fn++) probe((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
        }
    }
    klog("pci: %d devices", ndevs);
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        const char *dn = pci_device_name(d->vendor, d->device);
        klog("pci: %02x:%02x.%x %04x:%04x %s%s%s", d->bus, d->dev, d->fn, d->vendor, d->device,
             pci_class_name(d->cls, d->subcls), dn ? " - " : "", dn ? dn : "");
    }
}

int pci_count(void) { return ndevs; }
struct pci_dev *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : NULL; }

struct pci_dev *pci_find_class(uint8_t cls, uint8_t sub)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].cls == cls && devs[i].subcls == sub) return &devs[i];
    return NULL;
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) return &devs[i];
    return NULL;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default: return "Storage controller";
        }
    case 0x02: return s == 0x80 ? "Network controller" : "Ethernet controller";
    case 0x03: return "Display controller";
    case 0x04: return s == 0x03 ? "Audio device" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default: return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default: return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    default: return "Device";
    }
}

const char *pci_vendor_name(uint16_t v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x1B21: return "ASMedia";
    case 0x144D: return "Samsung";
    case 0x1912: return "Renesas";
    default: return "Unknown vendor";
    }
}

const char *pci_device_name(uint16_t v, uint16_t d)
{
    if (v == 0x8086) {
        switch (d) {
        case 0x29C0: return "82G33 DRAM controller (Q35)";
        case 0x2918: return "ICH9 LPC bridge";
        case 0x2922: return "ICH9 AHCI SATA";
        case 0x2930: return "ICH9 SMBus";
        case 0x100E: return "82540EM Gigabit Ethernet (e1000)";
        case 0x100F: return "82545EM Gigabit Ethernet";
        case 0x10D3: return "82574L Gigabit Ethernet (e1000e)";
        case 0x1237: return "440FX host bridge";
        case 0x7000: return "PIIX3 ISA bridge";
        case 0x7010: return "PIIX3 IDE";
        case 0x7113: return "PIIX4 ACPI";
        case 0x2668: return "ICH6 HD Audio";
        case 0x293E: return "ICH9 HD Audio";
        case 0x2415: return "82801AA AC'97 Audio";
        }
    }
    if (v == 0x1234 && d == 0x1111) return "Standard VGA";
    if (v == 0x1B36 && d == 0x000D) return "xHCI USB controller";
    if (v == 0x1B36 && d == 0x0100) return "QXL display";
    if (v == 0x1AF4) {
        switch (d) {
        case 0x1000: case 0x1041: return "virtio network";
        case 0x1001: case 0x1042: return "virtio block";
        case 0x1050: return "virtio GPU";
        case 0x1052: return "virtio input";
        }
    }
    if (v == 0x10EC && (d == 0x8139)) return "RTL8139 Fast Ethernet";
    if (v == 0x10EC && (d == 0x8168)) return "RTL8111 Gigabit Ethernet";
    return NULL;
}
