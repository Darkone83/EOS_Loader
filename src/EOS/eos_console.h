// eos_console.h -- live console/hardware facts for the System Info panel,
// styled after PrometheOS's "Console" category (CPU, revision, RAM, encoder).
#pragma once
#include "eos_gfx.h"

typedef struct EosConsole {
    int         cpuMhz;        // measured CPU clock (0 = unknown)
    const char* revStr;        // "1.0".."1.6", "DevKit", "Unknown"
    const char* encStr;        // "Conexant" / "Focus" / "Xcalibur"
    BOOL        rtcExpansion;  // TR1865-class RTC expansion present
    DWORD       ramMB;         // total physical RAM in MB
} EosConsole;

void Console_Read(EosConsole* c);   // detects + fills (call once on screen enter)

// SMBus access for other modules (kernel-HAL path, same as the probes). Reset
// once per burst, then read/write. addr is the 8-bit shifted address (X-RTC 0xD0).
void Con_SmbReset(void);
BOOL Con_SmbRead8(unsigned char addr, unsigned char reg, unsigned char* val);
BOOL Con_SmbWrite8(unsigned char addr, unsigned char reg, unsigned char val);

// Live telemetry for the persistent HUD (cheap enough to poll periodically).
typedef struct EosLive {
    int  cpuTempC;      // CPU die temperature, C   (-1 = unavailable)
    int  mbTempC;       // board ambient temp,  C   (-1 = unavailable)
    int  ramTotalMB;    // total physical RAM (64 or 128)
    int  ramUsedMB;     // physical RAM in use
    int  ramFreeMB;     // physical RAM available
    BOOL tempOK;        // temperatures were read
} EosLive;

// isRev16: pass 1 on rev 1.6 boards (ADM1032 absent -> read PIC 0x09/0x0A).
// Auto-detects if the ADM1032 doesn't answer, so passing 0 is safe.
void Console_ReadLive(EosLive* v, int isRev16);