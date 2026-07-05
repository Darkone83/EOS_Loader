#ifndef EOS_RTC_H
#define EOS_RTC_H
/*---------------------------------------------------------------------------
    eos_rtc.h -- X-RTC support (optional battery-backed clock on the SMBus).

    A DS1307-class RTC at SMBus 0xD0 (7-bit 0x68), the Darkone Customs X-RTC.
    The stock Xbox has no battery clock, so time is lost on power-off; an X-RTC
    persists it. When present it is the SOURCE OF TRUTH: the loader seeds the
    system clock from it at boot (Clock_InitFromRtc) and mirrors every clock
    set back to it (Clock_Set). When absent, every entry point is a safe no-op.

    Bus access goes through the loader's existing SMBus owner (eos_console:
    Con_SmbReset/Read8/Write8 -- kernel HAL + controller reset), NOT a second
    bus master. Stored time is LOCAL wall time (the loader's clock is local),
    so there is no timezone conversion.

    RXDK / MSVC2003 / C89: declarations before statements, no CRT.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "eos_clock.h"   /* EosDateTime */

#ifdef __cplusplus
extern "C" {
#endif

    void Rtc_Probe(void);                 /* one-shot validated presence probe */
    int  Rtc_Present(void);               /* 1 if a valid X-RTC was detected    */
    int  Rtc_Read(EosDateTime* dt);       /* read local time; 1 ok, 0 absent/bad */
    void Rtc_Write(const EosDateTime* dt);/* write local time; no-op if absent   */

#ifdef __cplusplus
}
#endif
#endif /* EOS_RTC_H */