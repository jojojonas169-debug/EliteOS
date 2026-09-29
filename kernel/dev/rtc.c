/* CMOS real-time clock. Read once at boot, then advanced by the TSC. */
#include <kernel.h>
#include <dev.h>
#include <x86.h>

static int64_t boot_epoch;
static uint64_t boot_ms;
int rtc_utc_offset_min = 0;    /* set from Settings */

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg | 0x80);
    return inb(0x71);
}

static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void epoch_to_datetime(int64_t t, struct datetime *dt)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    dt->hour = (int)(rem / 3600);
    dt->minute = (int)(rem / 60 % 60);
    dt->second = (int)(rem % 60);
    dt->weekday = (int)((days + 4) % 7);       /* 1970-01-01 was a Thursday */
    if (dt->weekday < 0) dt->weekday += 7;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    dt->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    dt->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    dt->year = (int)(y + (dt->month <= 2));
}

void rtc_init(void)
{
    /* wait for an update to finish, then read twice until stable */
    int s, mi, h, d, mo, y, c, s2, mi2, h2;
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) {}
    do {
        s = cmos(0x00); mi = cmos(0x02); h = cmos(0x04);
        d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
        c = acpi.century_reg ? cmos((uint8_t)acpi.century_reg) : 0;
        s2 = cmos(0x00); mi2 = cmos(0x02); h2 = cmos(0x04);
    } while (s != s2 || mi != mi2 || h != h2);
    uint8_t b = cmos(0x0B);
    bool pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 0x04)) {
        s = bcd(s); mi = bcd(mi); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y);
        if (c) c = bcd(c);
    }
    if (!(b & 0x02) && pm) h = (h + 12) % 24;
    int year = c ? c * 100 + y : 2000 + y;
    boot_epoch = days_from_civil(year, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    boot_ms = uptime_ms();
    klog("rtc: %04d-%02d-%02d %02d:%02d:%02d UTC", year, mo, d, h, mi, s);
}

int64_t rtc_epoch(void)
{
    return boot_epoch + (int64_t)((uptime_ms() - boot_ms) / 1000);
}

void rtc_now(struct datetime *dt)
{
    epoch_to_datetime(rtc_epoch() + rtc_utc_offset_min * 60, dt);
}
