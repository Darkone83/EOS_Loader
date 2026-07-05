// eos_clock.h -- Read/set the console real-time clock.
//
// Wraps the kernel GetLocalTime/SetLocalTime (SYSTEMTIME). Setting the clock
// only touches the RTC -- nothing in the EEPROM -- so it carries no risk. The
// editor helpers keep the date valid (leap-aware month lengths) before a set.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#pragma once
#include <xtl.h>

typedef struct EosDateTime {
    int year;     // full year, e.g. 2026
    int month;    // 1..12
    int day;      // 1..31
    int hour;     // 0..23
    int minute;   // 0..59
    int second;   // 0..59
} EosDateTime;

LONGLONG    Clock_CivilToTicks(const EosDateTime* dt);  // ticks since 1601 (exposed for tests)
void        Clock_Get(EosDateTime* dt);
BOOL        Clock_Set(const EosDateTime* dt);
void        Clock_InitFromRtc(void);                 // boot: seed system clock from X-RTC if present

int         Clock_IsLeap(int year);
int         Clock_DaysInMonth(int year, int month);  // leap-aware
void        Clock_Clamp(EosDateTime* dt);            // valid ranges + day<=month length
const char* Clock_MonthName(int month);              // "Jan".."Dec"