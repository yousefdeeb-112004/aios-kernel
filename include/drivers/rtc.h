/* RTC — Real-Time Clock (CMOS)
 * Reads date/time from CMOS ports 0x70/0x71 */
#ifndef _DRIVERS_RTC_H
#define _DRIVERS_RTC_H
#include <kernel/types.h>

typedef struct {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
    uint8_t  weekday;
} rtc_time_t;

extern rtc_time_t g_rtc_time;

void        rtc_init(void);
void        rtc_read(rtc_time_t* t);
uint32_t    rtc_unix_approx(void);  /* Rough unix timestamp for file metadata */
void        rtc_format(rtc_time_t* t, char* buf, uint32_t len); /* "YYYY-MM-DD HH:MM:SS" */
void        rtc_dump(void);

#endif
