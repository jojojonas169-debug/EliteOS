/*
 * Time keeping: PIT-calibrated TSC and LAPIC timer. Every CPU gets a
 * 1 kHz periodic LAPIC interrupt that drives the scheduler. The global
 * millisecond counter is derived from the TSC, so it stays exact even if
 * timer interrupts are delayed.
 */
#include <kernel.h>
#include <dev.h>
#include <cpu.h>
#include <x86.h>
#include <sched.h>

volatile uint64_t timer_ticks_v;
static uint64_t tsc_base;
static uint64_t tsc_per_ms = 1000000;

void lapic_timer_begin_measure(void);
uint32_t lapic_timer_elapsed(void);
void lapic_timer_calibrate(void);

#define PIT_HZ 1193182u

/* Busy-wait for `ms` milliseconds using PIT channel 2 (speaker gate). */
static void pit_wait(uint32_t ms)
{
    uint32_t count = PIT_HZ * ms / 1000;
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));   /* gate on, speaker off */
    outb(0x43, 0xB0);                               /* ch2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    /* restart the count by toggling the gate */
    uint8_t g = inb(0x61) & (uint8_t)~0x01;
    outb(0x61, g);
    outb(0x61, g | 1);
    for (uint64_t i = 0; i < 100000000ull && !(inb(0x61) & 0x20); i++) {}
}

uint32_t pit_measure_lapic(void)
{
    const uint32_t ms = 50;
    lapic_timer_begin_measure();
    uint64_t t0 = rdtsc();
    pit_wait(ms);
    uint32_t el = lapic_timer_elapsed();
    uint64_t t1 = rdtsc();
    tsc_per_ms = (t1 - t0) / ms;
    if (tsc_per_ms < 1000) tsc_per_ms = 1000000;
    cpu_info.tsc_hz = tsc_per_ms * 1000;
    return el / ms;
}

void timer_calibrate(void)
{
    tsc_base = rdtsc();
    lapic_timer_calibrate();
    tsc_base = rdtsc();
    klog("timer: TSC %lu MHz", tsc_per_ms / 1000);
}

uint64_t tsc_hz(void) { return tsc_per_ms * 1000; }

uint64_t uptime_ms(void)
{
    return (rdtsc() - tsc_base) / tsc_per_ms;
}

void udelay(uint64_t us)
{
    uint64_t end = rdtsc() + tsc_per_ms * us / 1000;
    while (rdtsc() < end) cpu_relax();
}

void mdelay(uint64_t ms) { udelay(ms * 1000); }

static void timer_irq(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    uint64_t now = uptime_ms();
    uint64_t cur = timer_ticks_v;
    while (now > cur && !__atomic_compare_exchange_n(&timer_ticks_v, &cur, now, false,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    sched_tick();
}

void timer_init(void)
{
    irq_register(VEC_TIMER, timer_irq, NULL);
    lapic_timer_start(1000);
}

void timer_init_ap(void)
{
    lapic_timer_start(1000);
}
