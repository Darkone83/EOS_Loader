// eos_console.cpp -- hardware detection for the System Info "console" view.
//
// SMBus access uses the kernel HAL (HalReadSMBusValue) with a controller reset
// first -- the XbDiag-proven method. The kernel arbitrates multi-master bus
// contention (the Eos FPGA shares this bus) correctly; raw port I/O did not,
// which is why console-version / encoder probes came back "Unknown".
//   - CPU clock from the MCPX CPUMPLL register + MSR 0x2A ratio (no timing window)
//   - GlobalMemoryStatus for RAM
//
// MSVC2003 / no CRT: no strcmp/sprintf, integer-only, file-scope statics.
#include "eos_console.h"
#include "xboxinternals.h"   // HalReadSMBusValue, KeStallExecutionProcessor

// Kernel-arbitrated PCI config access (safe path, avoids raw 0xCF8/0xCFC).
// Declared locally exactly as XbDiag does -- not in this project's xboxinternals.h.
extern "C" VOID __stdcall HalReadWritePCISpace(
    ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
    PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

// MEMORYSTATUS, GlobalMemoryStatus, GetTickCount come from <xtl.h> (pulled in by
// eos_gfx.h) -- do not redeclare them (GlobalMemoryStatus has C linkage already).

// ---- SMBus (XbDiag method: controller reset + kernel HAL) -------------------
// XbDiag proved the reliable path is the kernel HAL (HalReadSMBusValue), NOT raw
// port I/O -- the kernel arbitrates multi-master contention (the Eos FPGA shares
// this bus) correctly. The one required prep is clearing a stuck controller
// first: HalReadSMBusValue's internal retry loop has no hard timeout and spins
// forever if a prior transaction left the controller busy (the softmod/contention
// "reading data" hang). SMBusControllerReset() does the raw W1C + settle.
//
// Addresses here are SOFTWARE-SHIFTED 8-bit (7-bit hw << 1), matching the HAL
// convention: SMC/PIC 0x10 -> 0x20, ADM1032 0x4C -> 0x98, encoders as below.

#define SMB_PIC        0x20   // PIC16L / SMC        (hw 0x10)
#define SMB_ENC_CNXT   0x8A   // Conexant encoder    (hw 0x45)
#define SMB_ENC_FOCUS  0xD4   // Focus FS454 encoder (hw 0x6A)
#define SMB_ENC_XCAL   0xE0   // Xcalibur (1.6)      (hw 0x70)
#define SMB_ADM1032    0x98   // ADM1032 temp        (hw 0x4C)
#define SMB_RTC        0xD0   // X-RTC expansion     (hw 0x68)

static void io_out8(unsigned short port, unsigned char val)
{
    __asm { mov dx, port
    mov al, val
        out dx, al }
}

// Clear any stuck state in the nForce SMBus controller (W1C 0xFF at 0xC000),
// then let the bus settle. Call once before a probe sequence.
static void SMBusControllerReset(void)
{
    io_out8(0xC000, 0xFF);              // W1C: clear all status/busy bits
    KeStallExecutionProcessor(2000);    // 2ms settle (PIC/ADM1032 need <100us)
}

// Read one byte via the kernel HAL. addr is the 8-bit shifted address.
// Returns TRUE on ACK. The HAL handles bus arbitration; we just reset first.
static BOOL smbus_read_byte(unsigned char addr, unsigned char reg, unsigned char* val)
{
    DWORD v = 0;
    if (HalReadSMBusValue(addr, reg, FALSE, &v) != 0) return FALSE;
    *val = (unsigned char)(v & 0xFF);
    return TRUE;
}

static BOOL smbus_present(unsigned char addr)
{
    unsigned char v;
    return smbus_read_byte(addr, 0x00, &v);
}

static BOOL smbus_write_byte(unsigned char addr, unsigned char reg, unsigned char val)
{
    return HalWriteSMBusValue(addr, reg, FALSE, (DWORD)val) == 0;
}

// Exposed SMBus for other modules (e.g. eos_rtc). Same kernel-HAL path as the
// probes: Con_SmbReset() once per burst, then Con_SmbRead8/Write8 per byte.
void Con_SmbReset(void) { SMBusControllerReset(); }
BOOL Con_SmbRead8(unsigned char addr, unsigned char reg, unsigned char* val) { return smbus_read_byte(addr, reg, val); }
BOOL Con_SmbWrite8(unsigned char addr, unsigned char reg, unsigned char val) { return smbus_write_byte(addr, reg, val); }

// ---- PCI config read (kernel HAL, XbDiag method) ----------------------------
static DWORD PciRead32(BYTE bus, BYTE dev, BYTE func, BYTE reg)
{
    DWORD val = 0;
    ULONG slot = (((ULONG)dev & 0x1F) << 5) | ((ULONG)func & 0x07);
    HalReadWritePCISpace(bus, slot, reg, &val, sizeof(val), FALSE);
    return val;
}

// Xbox crystal reference: 16.666... MHz (explicit repeating decimal to match
// Haguero's reference implementation rather than 50000000.0/3.0 FPU rounding).
static const double XTAL_HZ = 16666666.6667;

// ---- CPU clock: pure PLL, no timing window (verbatim from XbDiag SysInfo) ----
// CPUMPLL offset 0x6C: FSB divider (byte 0) + multiplier (byte 1).
// MSR 0x2A bits [27:22] masked 0x2F: CPU ratio index the CPU writes during init
//   -- authoritative on Tualatin upgrades where CPUCTL (bootloader-written) may
//   not reflect the correct multiplier. Matches StressTestCPU ReadCPUMHz exactly.
static DWORD CpuRatioX10FromMsr(DWORD msr_lo)
{
    BYTE pat = (BYTE)((msr_lo >> 22) & 0x2F);
    switch (pat)
    {
    case 0x01: return 30;
    case 0x05: return 35;
    case 0x02: return 40;
    case 0x06: return 45;
    case 0x00: return 50;
    case 0x04: return 55;
    case 0x0B: return 60;
    case 0x0F: return 65;
    case 0x09: return 70;
    case 0x0D: return 75;
    case 0x0A: return 80;
    case 0x26: return 85;
    case 0x20: return 90;
    case 0x24: return 95;
    case 0x2B: return 100;
    case 0x2F: return 105;
    case 0x2A: return 130;
    case 0x2C: return 140;
    default:   return 0;
    }
}

static int measureCpuMhz(void)
{
    DWORD cpumpll = PciRead32(0, 3, 0, 0x6C);

    DWORD fsb_div = cpumpll & 0xFF;
    DWORD fsb_mult = (cpumpll >> 8) & 0xFF;
    DWORD msr_lo = 0, ratio, result;
    double fsb_hz, cpu_mhz;

    if (fsb_div == 0 || fsb_mult == 0) return 733;

    fsb_hz = XTAL_HZ * ((double)fsb_mult / (double)fsb_div);

    __asm
    {
        mov  ecx, 0x2A
        rdmsr
        mov  msr_lo, eax
    }

    ratio = CpuRatioX10FromMsr(msr_lo);
    if (ratio == 0) return 733;

    cpu_mhz = (fsb_hz * ((double)ratio / 10.0)) / 1.0e6;
    result = (DWORD)(cpu_mhz + 0.5);

    if (result < 400 || result > 1600) return 733;
    return (int)result;
}

// ---- Xbox hardware revision (XbDiag DetectBoardRevision, verbatim logic) ----
// The PIC returns a 3-char board string via three consecutive reads of reg 0x01
// (auto-incrementing internal pointer). XbDiag reads them BACK-TO-BACK through
// the kernel HAL with no delay between -- the HAL serialises the transactions,
// and the SMC's auto-increment expects consecutive reads. Adding stalls between
// them was wrong. If any of the three NAKs, the string is unknown.
static const char* detectRev(void)
{
    unsigned char b0 = 0, b1 = 0, b2 = 0;
    BOOL ok = smbus_read_byte(SMB_PIC, 0x01, &b0) &&
        smbus_read_byte(SMB_PIC, 0x01, &b1) &&
        smbus_read_byte(SMB_PIC, 0x01, &b2);
    if (!ok) return "Unknown";

    // Dev kit variants (D01 / 01D / 1D0 in any order)
    if ((b0 == 'D' || b1 == 'D' || b2 == 'D') &&
        (b0 == '0' || b1 == '0' || b2 == '0') &&
        (b0 == '1' || b1 == '1' || b2 == '1')) return "DevKit";

    // Debug / Alpha kit
    if ((b0 == 'D' && b1 == 'B' && b2 == 'G') || (b0 == 'B' && b1 == '1' && b2 == '1')) return "Debug Kit";

    if (b0 == 'P' && b1 == '0' && b2 == '1') return "1.0";
    if (b0 == 'P' && b1 == '0' && b2 == '5') return "1.1";

    // 1.2-1.5 all report P11 from the PIC; disambiguate by encoder.
    if ((b0 == 'P' && b1 == '1' && b2 == '1') ||
        (b0 == '1' && b1 == 'P' && b2 == '1') ||
        (b0 == '1' && b1 == '1' && b2 == 'P')) {
        unsigned char encId = 0;
        // Conexant (0x8A) present on 1.2/1.3
        if (smbus_read_byte(SMB_ENC_CNXT, 0x00, &encId)) return "1.2/1.3";
        // Focus (0xD4) present on 1.4/1.5: FS454(0x54)->1.4, FS455(0x09)->1.5
        if (smbus_read_byte(SMB_ENC_FOCUS, 0x00, &encId)) {
            if (encId == 0x54) return "1.4";
            if (encId == 0x09) return "1.5";
            return "1.4/1.5";
        }
        return "1.2/1.3";
    }

    if (b0 == 'P' && b1 == '2' && b2 == 'L') return "1.6";   // 1.6/1.6b (strap read omitted: crashes on 1.6)
    return "Unknown";
}

static const char* detectEncoder(void)
{
    if (smbus_present(SMB_ENC_CNXT)) return "Conexant";
    if (smbus_present(SMB_ENC_FOCUS)) return "Focus";
    return "Xcalibur";
}

// ---- live telemetry: temps (XbDiag method) + RAM usage ----------------------
// ADM1032 (0x98) on rev 1.0-1.5: reg 0x00 = ambient/local, 0x01 = CPU/remote.
// Rev 1.6 has no ADM1032 -> PIC (0x20) reg 0x09 = CPU, 0x0A = ambient, averaged
// over 3 samples with a 0.8x ambient correction (matches PrometheOS/TempMonitor).
void Console_ReadLive(EosLive* v, int isRev16)
{
    unsigned char amb = 0, cpu = 0;
    BOOL useADM;
    MEMORYSTATUS ms;
    if (!v) return;

    v->cpuTempC = -1; v->mbTempC = -1; v->tempOK = FALSE;

    SMBusControllerReset();   // clear stuck state before probing the shared bus

    // Prefer the ADM1032 unless we know this is a 1.6 (no ADM1032 there).
    useADM = FALSE;
    if (!isRev16) {
        if (smbus_read_byte(SMB_ADM1032, 0x00, &amb) &&
            smbus_read_byte(SMB_ADM1032, 0x01, &cpu)) {
            v->mbTempC = (int)amb;
            v->cpuTempC = (int)cpu;
            v->tempOK = TRUE;
            useADM = TRUE;
        }
    }

    if (!useADM) {
        // PIC path (rev 1.6, or ADM1032 didn't answer): average 3 samples.
        int accAmb = 0, accCpu = 0, good = 0, si;
        for (si = 0; si < 3; ++si) {
            unsigned char pc = 0, pa = 0;
            if (smbus_read_byte(SMB_PIC, 0x09, &pc) &&
                smbus_read_byte(SMB_PIC, 0x0A, &pa)) {
                accCpu += (int)pc; accAmb += (int)pa; ++good;
            }
        }
        if (good > 0) {
            int ra = accAmb / good, rc = accCpu / good;
            ra = ra * 4 / 5;                 // 0.8x ambient correction
            v->mbTempC = ra;
            v->cpuTempC = rc;
            v->tempOK = TRUE;
        }
    }

    // RAM usage.
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    v->ramTotalMB = (int)(ms.dwTotalPhys / (1024 * 1024));   // 64 or 128
    v->ramUsedMB = (int)((ms.dwTotalPhys - ms.dwAvailPhys) / (1024 * 1024));
    v->ramFreeMB = (int)(ms.dwAvailPhys / (1024 * 1024));
}

void Console_Read(EosConsole* c)
{
    MEMORYSTATUS ms;
    if (!c) return;

    SMBusControllerReset();   // clear any stuck transaction before HAL probing

    c->cpuMhz = measureCpuMhz();
    c->revStr = detectRev();
    c->encStr = detectEncoder();
    c->rtcExpansion = smbus_present(SMB_RTC);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    c->ramMB = (DWORD)(ms.dwTotalPhys / (1024 * 1024));    // 64 or 128 MB
}