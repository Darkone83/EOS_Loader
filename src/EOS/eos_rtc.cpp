/*---------------------------------------------------------------------------
    eos_rtc.cpp -- see eos_rtc.h. DS1307-class register logic (from dd_rtc),
    wired to the loader's SMBus owner (eos_console) instead of a second broker.

    DS1307 register map (BCD):
       0 seconds (bit7 = CH clock-halt)  1 minutes   2 hours (bit6 = 12/24)
       3 day-of-week (1..7)              4 day       5 month   6 year (+2000)

    RXDK / MSVC2003 / C89: declarations before statements, no CRT.
---------------------------------------------------------------------------*/
#include "eos_rtc.h"
#include "eos_console.h"   /* Con_SmbReset / Con_SmbRead8 / Con_SmbWrite8 */

#define RTC_ADDR8   0xD0       /* 7-bit 0x68 */
#define RTC_SEC     0
#define RTC_MIN     1
#define RTC_HOUR    2
#define RTC_DOW     3
#define RTC_DAY     4
#define RTC_MONTH   5
#define RTC_YEAR    6

static int s_probed = 0;
static int s_present = 0;

static unsigned char Bcd2Bin(unsigned char v) { return (unsigned char)(((v >> 4) * 10) + (v & 0x0f)); }
static unsigned char Bin2Bcd(unsigned char v) { return (unsigned char)(((v / 10) << 4) | (v % 10)); }

/* DS1307 day-of-week is a plain 1..7 counter (nothing reads it here); keep it
   sane by deriving it so a set date looks right on other tools. Sakamoto. */
static unsigned char DowOf(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int w;
    if (m < 3) y -= 1;
    w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */
    return (unsigned char)(w + 1);
}

/* One-shot: read the time registers and sanity-check the BCD ranges, so a
   device that merely ACKs at 0xD0 but returns garbage is NOT mistaken for an
   RTC (and we never write to a non-RTC responder). */
void Rtc_Probe(void)
{
    unsigned char sec, mins, hr, day, mon;
    if (s_probed) return;
    s_probed = 1;
    s_present = 0;
    Con_SmbReset();
    if (!Con_SmbRead8(RTC_ADDR8, RTC_SEC, &sec))  return;
    if (!Con_SmbRead8(RTC_ADDR8, RTC_MIN, &mins)) return;
    if (!Con_SmbRead8(RTC_ADDR8, RTC_HOUR, &hr))   return;
    if (!Con_SmbRead8(RTC_ADDR8, RTC_DAY, &day))  return;
    if (!Con_SmbRead8(RTC_ADDR8, RTC_MONTH, &mon))  return;
    if (Bcd2Bin((unsigned char)(sec & 0x7f)) > 59) return;
    if (Bcd2Bin(mins) > 59)                        return;
    if (Bcd2Bin((unsigned char)(hr & 0x3f)) > 23)  return;
    {
        unsigned char d = Bcd2Bin(day), m = Bcd2Bin(mon);
        if (d < 1 || d > 31 || m < 1 || m > 12)    return;
    }
    s_present = 1;
}

int Rtc_Present(void) { return s_present; }

int Rtc_Read(EosDateTime* dt)
{
    unsigned char v;
    if (!s_present || !dt) return 0;
    Con_SmbReset();
    if (!Con_SmbRead8(RTC_ADDR8, RTC_SEC, &v)) return 0;  dt->second = Bcd2Bin((unsigned char)(v & 0x7f));
    if (!Con_SmbRead8(RTC_ADDR8, RTC_MIN, &v)) return 0;  dt->minute = Bcd2Bin(v);
    if (!Con_SmbRead8(RTC_ADDR8, RTC_HOUR, &v)) return 0;  dt->hour = Bcd2Bin((unsigned char)(v & 0x3f));
    if (!Con_SmbRead8(RTC_ADDR8, RTC_DAY, &v)) return 0;  dt->day = Bcd2Bin(v);
    if (!Con_SmbRead8(RTC_ADDR8, RTC_MONTH, &v)) return 0;  dt->month = Bcd2Bin(v);
    if (!Con_SmbRead8(RTC_ADDR8, RTC_YEAR, &v)) return 0;  dt->year = 2000 + Bcd2Bin(v);
    if (dt->month < 1 || dt->month > 12 || dt->day < 1 || dt->day > 31) return 0;
    return 1;
}

void Rtc_Write(const EosDateTime* dt)
{
    if (!s_present || !dt) return;
    Con_SmbReset();
    /* writing valid BCD seconds also clears CH (bit7 = 0), so the oscillator runs */
    Con_SmbWrite8(RTC_ADDR8, RTC_SEC, Bin2Bcd((unsigned char)dt->second));
    Con_SmbWrite8(RTC_ADDR8, RTC_MIN, Bin2Bcd((unsigned char)dt->minute));
    Con_SmbWrite8(RTC_ADDR8, RTC_HOUR, Bin2Bcd((unsigned char)dt->hour));   /* 24h: bit6 = 0 */
    Con_SmbWrite8(RTC_ADDR8, RTC_DOW, DowOf(dt->year, dt->month, dt->day));
    Con_SmbWrite8(RTC_ADDR8, RTC_DAY, Bin2Bcd((unsigned char)dt->day));
    Con_SmbWrite8(RTC_ADDR8, RTC_MONTH, Bin2Bcd((unsigned char)dt->month));
    Con_SmbWrite8(RTC_ADDR8, RTC_YEAR, Bin2Bcd((unsigned char)(dt->year % 100)));
}