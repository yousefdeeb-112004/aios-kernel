/* RTC — Real-Time Clock via CMOS
 * Reads time from CMOS RTC at ports 0x70/0x71.
 * Handles BCD→binary conversion. */

#include <drivers/rtc.h>
#include <kernel/ports.h>
#include <drivers/vga.h>

rtc_time_t g_rtc_time;

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static bool rtc_updating(void) {
    outb(0x70, 0x0A);
    return (inb(0x71) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return ((v >> 4) * 10) + (v & 0x0F);
}

void rtc_read(rtc_time_t* t) {
    /* Wait for update-in-progress to clear */
    while (rtc_updating());

    t->second  = cmos_read(0x00);
    t->minute  = cmos_read(0x02);
    t->hour    = cmos_read(0x04);
    t->day     = cmos_read(0x07);
    t->month   = cmos_read(0x08);
    t->year    = cmos_read(0x09);
    t->weekday = cmos_read(0x06);

    /* Check register B for BCD vs binary mode */
    uint8_t regB = cmos_read(0x0B);
    if (!(regB & 0x04)) {
        /* BCD mode — convert to binary */
        t->second = bcd_to_bin(t->second);
        t->minute = bcd_to_bin(t->minute);
        t->hour   = bcd_to_bin(t->hour & 0x7F) | (t->hour & 0x80);
        t->day    = bcd_to_bin(t->day);
        t->month  = bcd_to_bin(t->month);
        t->year   = bcd_to_bin((uint8_t)t->year);
    }

    /* Handle 12-hour mode */
    if (!(regB & 0x02) && (t->hour & 0x80)) {
        t->hour = ((t->hour & 0x7F) + 12) % 24;
    }

    /* Century: CMOS year is 0-99, assume 20xx */
    t->year += 2000;
}

void rtc_init(void) {
    rtc_read(&g_rtc_time);
}

/* Approximate unix timestamp (not leap-second accurate, good enough for file timestamps) */
uint32_t rtc_unix_approx(void) {
    rtc_time_t t;
    rtc_read(&t);

    /* Days from months (non-leap approximation) */
    static const uint16_t mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t y = t.year - 1970;
    uint32_t days = y * 365 + (y + 1) / 4; /* Rough leap year count */
    if (t.month >= 1 && t.month <= 12)
        days += mdays[t.month - 1];
    days += t.day - 1;

    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}

void rtc_format(rtc_time_t* t, char* buf, uint32_t len) {
    if (len < 20) { buf[0] = '\0'; return; }
    /* "YYYY-MM-DD HH:MM:SS" */
    buf[0]  = '0' + (t->year / 1000) % 10;
    buf[1]  = '0' + (t->year / 100) % 10;
    buf[2]  = '0' + (t->year / 10) % 10;
    buf[3]  = '0' + t->year % 10;
    buf[4]  = '-';
    buf[5]  = '0' + t->month / 10;
    buf[6]  = '0' + t->month % 10;
    buf[7]  = '-';
    buf[8]  = '0' + t->day / 10;
    buf[9]  = '0' + t->day % 10;
    buf[10] = ' ';
    buf[11] = '0' + t->hour / 10;
    buf[12] = '0' + t->hour % 10;
    buf[13] = ':';
    buf[14] = '0' + t->minute / 10;
    buf[15] = '0' + t->minute % 10;
    buf[16] = ':';
    buf[17] = '0' + t->second / 10;
    buf[18] = '0' + t->second % 10;
    buf[19] = '\0';
}

void rtc_dump(void) {
    rtc_time_t t;
    rtc_read(&t);
    char buf[24];
    rtc_format(&t, buf, sizeof(buf));
    vga_puts_color("  Time: ", VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_color(buf, VGA_WHITE, VGA_BLACK);
    vga_puts("\n");
}
