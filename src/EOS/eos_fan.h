// eos_fan.h -- Original Xbox SMC fan control.
// Auto returns fan control to the SMC thermal algorithm. Manual writes a
// fixed fan duty request. UI uses 5% steps; SMC hardware resolution is 2%.
#pragma once
#include <xtl.h>

#define EOS_FAN_MIN_PERCENT 20
#define EOS_FAN_MAX_PERCENT 100
#define EOS_FAN_STEP_PERCENT 5

BOOL Fan_ReadMode(int* manualMode);       // 0=Auto, 1=Manual
BOOL Fan_ReadPercent(int* percent);       // SMC readback, 0..100 in 2% steps
BOOL Fan_SetAuto(void);
BOOL Fan_SetManual(int percent);          // clamps 20..100; nearest raw 2% duty
