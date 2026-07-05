// eos_clock.cpp -- Console RTC read/set. See eos_clock.h.
//
// RXDK exports GetLocalTime (read) but NOT SetLocalTime, so the clock is set
// through the kernel: NtSetSystemTime takes a 100ns-since-1601 system (UTC)
// time. We read local via GetLocalTime for a friendly display, and to make the
// value the user enters be the value that sticks, we measure the kernel's own
// local<->system offset (its effective timezone bias) at set time and subtract
// it -- no timezone-rule parsing, no missing Win32 APIs.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_clock.h"
#include "eos_rtc.h"    // X-RTC mirror (source of truth when present)

extern "C" void __stdcall KeQuerySystemTime(LARGE_INTEGER* CurrentTime);
extern "C" long __stdcall NtSetSystemTime(LARGE_INTEGER* SystemTime, LARGE_INTEGER* PreviousTime);

#define TICKS_PER_SEC   10000000      /* 100ns units per second                */
#define DAYS_1601_1970  134774        /* days from 1601-01-01 to 1970-01-01    */

// Days from 1601-01-01 to a civil (proleptic Gregorian) date. Integer only.
static LONGLONG daysFromCivil(int y, int m, int d)
{
    LONGLONG yy = y, era, yoe, doy, doe, days1970;
    yy -= (m <= 2);
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = yy - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days1970 = era * 146097 + doe - 719468;
    return days1970 + DAYS_1601_1970;
}

// Civil local date/time -> 100ns ticks since 1601 (treated as if a system time).
LONGLONG Clock_CivilToTicks(const EosDateTime* dt)
{
    LONGLONG days = daysFromCivil(dt->year, dt->month, dt->day);
    LONGLONG secs = days * 86400 + (LONGLONG)dt->hour * 3600
        + (LONGLONG)dt->minute * 60 + dt->second;
    return secs * (LONGLONG)TICKS_PER_SEC;
}

void Clock_Get(EosDateTime* dt)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    dt->year = st.wYear;  dt->month = st.wMonth;  dt->day = st.wDay;
    dt->hour = st.wHour;  dt->minute = st.wMinute; dt->second = st.wSecond;
}

BOOL Clock_Set(const EosDateTime* dt)
{
    SYSTEMTIME    lt;
    EosDateTime   localNow;
    LARGE_INTEGER sysNow, toSet;
    LONGLONG      offset;

    // Effective local offset = local-now - system-now (the kernel's TZ bias).
    GetLocalTime(&lt);
    localNow.year = lt.wYear;  localNow.month = lt.wMonth;  localNow.day = lt.wDay;
    localNow.hour = lt.wHour;  localNow.minute = lt.wMinute; localNow.second = lt.wSecond;
    KeQuerySystemTime(&sysNow);
    offset = Clock_CivilToTicks(&localNow) - sysNow.QuadPart;

    // System time to set so that the displayed LOCAL time equals what was entered.
    toSet.QuadPart = Clock_CivilToTicks(dt) - offset;
    {
        BOOL ok = (NtSetSystemTime(&toSet, NULL) == 0);
        if (ok && Rtc_Present()) Rtc_Write(dt);   // X-RTC is source of truth: mirror the set
        return ok;
    }
}

int Clock_IsLeap(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

int Clock_DaysInMonth(int year, int month)
{
    static const int dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2 && Clock_IsLeap(year)) return 29;
    return dim[month - 1];
}

void Clock_Clamp(EosDateTime* dt)
{
    int dim;
    if (dt->year < 2000) dt->year = 2000;
    if (dt->year > 2099) dt->year = 2099;
    if (dt->month < 1)    dt->month = 1;
    if (dt->month > 12)   dt->month = 12;
    dim = Clock_DaysInMonth(dt->year, dt->month);
    if (dt->day < 1)   dt->day = 1;
    if (dt->day > dim) dt->day = dim;
    if (dt->hour < 0)  dt->hour = 0;
    if (dt->hour > 23) dt->hour = 23;
    if (dt->minute < 0)  dt->minute = 0;
    if (dt->minute > 59) dt->minute = 59;
    if (dt->second < 0)  dt->second = 0;
    if (dt->second > 59) dt->second = 59;
}

const char* Clock_MonthName(int month)
{
    static const char* k[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    if (month < 1 || month > 12) return "---";
    return k[month - 1];
}

// Boot: an X-RTC, when present, is the source of truth. Probe it and seed the
// system clock from it. No-op when absent (system time-only, as before). The
// Clock_Set re-mirror is harmless (writes back what we read) and ensures the
// oscillator is running (CH cleared).
void Clock_InitFromRtc(void)
{
    EosDateTime dt;
    Rtc_Probe();
    if (Rtc_Present() && Rtc_Read(&dt)) Clock_Set(&dt);
}