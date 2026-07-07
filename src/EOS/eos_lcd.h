#ifndef EOS_LCD_H
#define EOS_LCD_H
/*---------------------------------------------------------------------------
    eos_lcd.h -- optional character-LCD status display (SMBus).

    Light, status-only 20x4 screen modeled on PrometheOS's LCD render: a fixed
    layout whose lines are shadow-diffed, so an idle loader writes nothing to
    the bus. Pumped from the main loop (NOT a thread -- the loader's SMBus isn't
    thread-safe and is shared with the temp reads). Two controller families
    behind a small driver vtable, SMBus only:

        US2066  (native I2C OLED,     0x3C / 0x3D)  -- contrast supported
        HD44780 (via PCF8574 backpack, 0x27 / 0x3F) -- Phase 2

    All bus access goes through eos_console's Con_Smb* (kernel HAL + reset),
    the same path the X-RTC and temp probes use. See eos_lcd_spec.md.

    Layout:
        row 0  <current menu / selected item>   (Lcd_SetContext top)
        row 1  IP: <addr>
        row 2  CPU:<t>c   MB:<t>c
        row 3  RAM:<n>M free           <bank>   (bank appended if set + fits)

    Usage:  Lcd_Init() once at boot (after the SMBus is warm);
            Lcd_Tick(&live) every frame; Lcd_HandOff() before a bank launch.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "eos_console.h"   /* EosLive */

#ifdef __cplusplus
extern "C" {
#endif

    enum { LCD_DRV_NONE = 0, LCD_DRV_HD44780 = 1, LCD_DRV_US2066 = 2 };

    void Lcd_Init(void);                       /* pick driver, probe, init if present   */
    void Lcd_Tick(const EosLive* live);        /* pump from the main loop (throttled)    */

    void Lcd_SetContext(const char* top,       /* row-0 text (menu / selected item)      */
        const char* bank);     /* row-3 bank tag, NULL = none            */

    void Lcd_HandOff(const char* bankName);    /* freeze "Booting <bank>" before launch  */
    void Lcd_Resume(void);                     /* unfreeze -> status repaints            */

    /* config -- the Settings LCD screen drives these (and persists them) */
    void Lcd_SetDriver(int drv);               /* LCD_DRV_* ; re-probes + re-inits       */
    int  Lcd_Driver(void);
    void Lcd_SetAddress(unsigned char addr7);  /* 7-bit address; re-probes + re-inits    */
    unsigned char Lcd_Address(void);           /* current 7-bit address                  */
    void Lcd_SetBrightness(int v);             /* 0..255 (US2066 only); applies live     */
    int  Lcd_Brightness(void);
    int  Lcd_Detected(void);                   /* live probe result at driver+address    */
    int  Lcd_HasContrast(void);                /* active driver supports brightness      */

#ifdef __cplusplus
}
#endif
#endif /* EOS_LCD_H */