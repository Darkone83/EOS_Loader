#pragma once
#pragma once
// eos_vsc.h -- vendor-specific HDD credential recovery for EOS Loader.
//
// Behavioral reference: PrometheOS hddVscUnlocker by Team Resurgent and
// contributors. EOS keeps the proven WD / Seagate command flows separate from
// eos_hdd.cpp so normal ATA lock/unlock remains small and readable.

#define EOS_VSC_OK            0
#define EOS_VSC_UNSUPPORTED  -1
#define EOS_VSC_IO           -2
#define EOS_VSC_PARSE        -3
#define EOS_VSC_UNLOCK       -4
#define EOS_VSC_SAVE         -5

#define EOS_VSC_PWD_SIZE 32

typedef struct EosVscResult {
    int supported;
    int unlocked;
    char model[44];
    char serial[24];
    char firmware[12];
    unsigned char masterPassword[EOS_VSC_PWD_SIZE];
    unsigned char userPassword[EOS_VSC_PWD_SIZE];
} EosVscResult;

// Attempt the PrometheOS-derived vendor recovery path for the primary master.
// Safe to call only after the normal EOS ATA unlock path has failed and the
// drive still reports security enabled/locked.
int Vsc_TryUnlock(EosVscResult* out);

// Save recovered model/serial + master/user passwords as hex.
// Path: E:\\Eos\\unlock.txt
int Vsc_SaveCredentials(const EosVscResult* result);
