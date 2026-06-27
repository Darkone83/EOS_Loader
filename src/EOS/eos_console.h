#pragma once
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