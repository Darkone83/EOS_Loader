#pragma once
// eos_hdd.h -- ATA hard-disk tools for the OG Xbox (primary master).
//
// Raw ATA over the IDE ports (0x1F0), same in/out style as the flash driver.
// IDENTIFY reads model/serial/size/security; the lock/unlock password is
// HMAC-SHA1(XboxHDKey, model || serial) -- the kernel's decrypted HDD key, so
// no EEPROM crypto is needed here. Unlock removes security (unlock + disable);
// Lock enables it with this console's key. Both are guarded in the UI; Lock can
// lock a drive to this console, so it confirms first.
//
// ATA command-send logic ported from PrometheOS XKHDD (hardware-validated).
#pragma once

#define HDD_OK           0
#define HDD_ERR_NODISK  -1   // IDENTIFY failed (no master drive / timeout)
#define HDD_ERR_UNSUPP  -2   // drive doesn't support ATA security
#define HDD_ERR_STATE   -3   // wrong precondition (e.g. lock when already enabled)
#define HDD_ERR_ATA     -4   // a security command didn't take

// ATA IDENTIFY word-128 security-status bits.
#define HDD_SEC_SUPPORTED 0x0001
#define HDD_SEC_ENABLED   0x0002
#define HDD_SEC_LOCKED    0x0004
#define HDD_SEC_FROZEN    0x0008

typedef struct EosHddInfo {
    int            present;        // IDENTIFY succeeded
    char           model[44];      // trimmed, byte-swap corrected
    char           serial[24];     // trimmed
    unsigned short security;       // raw word 128
    unsigned long  sizeMB;         // capacity in MB
} EosHddInfo;

// ATA IDENTIFY on the primary master and parse it. Returns HDD_OK / HDD_ERR_*.
int Hdd_Identify(EosHddInfo* out);

// Remove security from the primary master: ATA UNLOCK then DISABLE using the
// console-derived password. No-op (HDD_OK) if already unlocked.
int Hdd_Unlock(void);

// Enable security on the primary master, locking it to THIS console's key
// (sets master + user passwords). Refuses if already enabled. DANGER.
int Hdd_Lock(void);