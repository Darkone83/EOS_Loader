// eos_fan.cpp -- Original Xbox SMC/PIC fan control.
//
// PIC/SMC registers (software-shifted SMBus address 0x20):
//   0x05 FAN_MODE      0 = SMC automatic, 1 = manual override
//   0x06 FAN_REGISTER  requested duty, raw 0..50 => 0..100%
//   0x10 FAN_READBACK  current duty,   raw 0..50 => 0..100%
//
// PrometheOS writes the manual mode/speed pair twice with a short delay; keep
// that reliability behavior here. All access goes through eos_console's shared
// kernel-HAL SMBus path so EOS remains safe with its FPGA bus participant.
#include "eos_fan.h"
#include "eos_console.h"
#include "xboxinternals.h"

#define FAN_SMB_ADDR  PIC_ADDRESS
#define FAN_AUTO_MODE 0
#define FAN_MANUAL_MODE 1

static int clampPercent(int percent)
{
    if (percent < EOS_FAN_MIN_PERCENT) return EOS_FAN_MIN_PERCENT;
    if (percent > EOS_FAN_MAX_PERCENT) return EOS_FAN_MAX_PERCENT;
    return percent;
}

BOOL Fan_ReadMode(int* manualMode)
{
    unsigned char mode = 0;
    if (!manualMode) return FALSE;
    Con_SmbReset();
    if (!Con_SmbRead8(FAN_SMB_ADDR, FAN_MODE, &mode)) return FALSE;
    *manualMode = (mode != FAN_AUTO_MODE) ? 1 : 0;
    return TRUE;
}

BOOL Fan_ReadPercent(int* percent)
{
    unsigned char raw = 0;
    if (!percent) return FALSE;
    Con_SmbReset();
    if (!Con_SmbRead8(FAN_SMB_ADDR, FAN_READBACK, &raw)) return FALSE;
    if (raw > 50) raw = 50;
    *percent = (int)raw * 2;
    return TRUE;
}

BOOL Fan_SetAuto(void)
{
    BOOL ok = TRUE;
    Con_SmbReset();
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_MODE, FAN_AUTO_MODE)) ok = FALSE;
    Sleep(10);
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_MODE, FAN_AUTO_MODE)) ok = FALSE;
    return ok;
}

BOOL Fan_SetManual(int percent)
{
    unsigned char raw;
    int p = clampPercent(percent);
    BOOL ok = TRUE;

    // The SMC command has 2% granularity. Round requested 5% UI steps to the
    // nearest representable duty (odd 5% values become +1%, e.g. 25 -> 26%).
    raw = (unsigned char)((p + 1) / 2);
    if (raw > 50) raw = 50;

    Con_SmbReset();
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_MODE, FAN_MANUAL_MODE)) ok = FALSE;
    Sleep(10);
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_REGISTER, raw)) ok = FALSE;
    Sleep(10);
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_MODE, FAN_MANUAL_MODE)) ok = FALSE;
    Sleep(10);
    if (!Con_SmbWrite8(FAN_SMB_ADDR, FAN_REGISTER, raw)) ok = FALSE;
    return ok;
}
