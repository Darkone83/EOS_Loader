// eos_console.cpp -- hardware detection for the System Info "console" view.
//
// All probing is self-contained and linker-safe for this RXDK project:
//   - raw nForce SMBus reads (HalReadSMBusValue does not resolve here, the same
//     issue eos_bank.cpp hit, so we drive the 0xC000 controller directly)
//   - RDTSC over a GetTickCount window for the CPU clock
//   - GlobalMemoryStatus (kernel32-class, resolves like GetTickCount) for RAM
//
// MSVC2003 / no CRT: no strcmp/sprintf, integer-only, file-scope statics.
#include "eos_console.h"

// MEMORYSTATUS, GlobalMemoryStatus, GetTickCount come from <xtl.h> (pulled in by
// eos_gfx.h) -- do not redeclare them (GlobalMemoryStatus has C linkage already).

// ---- port I/O (file-local, same pattern as eos_bank.cpp) --------------------
static void io_out8(unsigned short port, unsigned char val)
{
    __asm { mov dx, port
    mov al, val
        out dx, al }
}
static unsigned char io_in8(unsigned short port)
{
    unsigned char v;
    __asm { mov dx, port
    in  al, dx
        mov v, al }
    return v;
}

// ---- raw nForce SMBus byte read (mirror of eos_bank's write, RW=1) ----------
// Returns TRUE on a completed cycle with no error; *val holds the data byte.
static BOOL smbus_read_byte(unsigned char addr7, unsigned char cmd, unsigned char* val)
{
    volatile int t;
    unsigned char st;

    for (t = 0; t < 100000; ++t) { if (!(io_in8(0xC000) & 0x08)) break; }

    io_out8(0xC000, io_in8(0xC000));                       // clear status (W1C)
    io_out8(0xC004, (unsigned char)((addr7 << 1) | 1));    // slave address, read (RW=1)
    io_out8(0xC008, cmd);                                  // command / register
    io_out8(0xC002, 0x0A);                                 // byte-data protocol + start

    for (t = 0; t < 100000; ++t) {
        st = io_in8(0xC000);
        if (st & 0x10) { *val = io_in8(0xC006); return TRUE; }   // complete
        if (st & 0x24) return FALSE;                             // error/abort
    }
    return FALSE;                                                 // timeout
}

static BOOL smbus_present(unsigned char addr7)
{
    unsigned char v;
    return smbus_read_byte(addr7, 0x00, &v);
}

// ---- RDTSC --------------------------------------------------------------------
static unsigned __int64 rdtsc64(void)
{
    unsigned long lo, hi;
    __asm { rdtsc
    mov lo, eax
        mov hi, edx }
    return ((unsigned __int64)hi << 32) | lo;
}

static int measureCpuMhz(void)
{
    unsigned __int64 t0, t1, dtsc;
    unsigned long    m0, dms;
    t0 = rdtsc64();
    m0 = GetTickCount();
    while ((GetTickCount() - m0) < 120) { /* busy-wait ~120 ms */ }
    dms = GetTickCount() - m0;
    t1 = rdtsc64();
    if (t1 <= t0 || dms == 0) return 0;
    dtsc = t1 - t0;
    return (int)(dtsc / ((unsigned __int64)dms * 1000));   // ticks/ms/1000 = MHz
}

// ---- Xbox hardware revision (SMC version reg, read 3 chars) -----------------
static const char* detectRev(void)
{
    unsigned char a, b, c;
    if (!smbus_read_byte(0x10, 0x01, &a)) return "Unknown";
    smbus_read_byte(0x10, 0x01, &b);
    smbus_read_byte(0x10, 0x01, &c);

    if (a == 'P' && b == '0' && c == '1') return "1.0";
    if (a == 'P' && b == '0' && c == '5') return "1.1";
    if (a == 'P' && b == '1' && c == '1')                         // 1.2/1.3 vs 1.4/1.5
        return smbus_present(0x6A) ? "1.4/1.5" : "1.2/1.3";
    if (a == 'P' && b == '2' && c == 'L') return "1.6";
    if ((a == '0' && b == '1' && c == 'D') || (a == 'D' && b == '0' && c == '1') ||
        (a == '1' && b == 'D' && c == '0') || (a == '0' && b == 'D' && c == '1')) return "DevKit";
    return "Unknown";
}

static const char* detectEncoder(void)
{
    if (smbus_present(0x45)) return "Conexant";
    if (smbus_present(0x6A)) return "Focus";
    return "Xcalibur";
}

void Console_Read(EosConsole* c)
{
    MEMORYSTATUS ms;
    if (!c) return;

    c->cpuMhz = measureCpuMhz();
    c->revStr = detectRev();
    c->encStr = detectEncoder();
    c->rtcExpansion = smbus_present(0x68);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    c->ramMB = (DWORD)(ms.dwTotalPhys / (1024 * 1024));    // 64 or 128 MB
}