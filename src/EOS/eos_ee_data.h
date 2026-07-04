// eos_ee_data.h -- decrypted EEPROM data access for Eos.
//
// Reads the 256-byte EEPROM via ExQueryNonVolatileSetting(0xFFFF), decrypts the
// factory security section (RC4 + Xbox HMAC-SHA1 key auto-detect), and exposes
// the video standard, game region, DVD region, language, serial and MAC. Also
// writes them back with correct checksums (video/DVD are plaintext + CRC; game
// region lives in the encrypted block and is re-encrypted on write).
//
// Ported from PrometheOS XKEEPROM (Team-Assembly / Xbox-Linux, GPLv2).
#pragma once
#include <xtl.h>

// Video standard dwords (EEPROM offset 0x58).
#define EE_VS_NTSC_M   0x00400100
#define EE_VS_NTSC_J   0x00400200
#define EE_VS_PAL_I    0x00800300
#define EE_VS_PAL_M    0x00400400

// Game/XBE region byte (EEPROM offset 0x2C, encrypted).
#define EE_REGION_NA     0x01
#define EE_REGION_JAPAN  0x02
#define EE_REGION_EURO   0x04

// Detected console key generation (for re-encryption on write).
#define EE_VER_NONE  0x00
#define EE_VER_1_0   0x0A
#define EE_VER_1_1   0x0B
#define EE_VER_1_6   0x0C

typedef struct {
    int   valid;         // image read + parsed
    int   decrypted;     // security section decrypted OK (key found)
    int   version;       // EE_VER_* console key generation (0 if decrypt failed)

    DWORD videoStd;      // 0x58 dword (EE_VS_*)
    DWORD videoFlags;    // 0x94 flags bitmask
    DWORD gameRegion;    // 0x2C low byte (EE_REGION_*), decrypted
    DWORD dvdRegion;     // 0xBC zone
    DWORD language;      // 0x90 language id

    char  serial[13];    // 0x34, 12 chars + NUL
    UCHAR mac[6];        // 0x40
    int   serialValid;
    int   macValid;
} EeData;

// Read + decrypt the live EEPROM. Returns 1 on a valid read (decrypted may be 0
// if the console key wasn't matched -- plaintext fields are still valid).
int EeData_Read(EeData* d);

// Human-readable strings.
const char* EeData_VideoStdStr(const EeData* d);
const char* EeData_GameRegionStr(const EeData* d);
const char* EeData_DvdRegionStr(const EeData* d);

// --- Writers (persist to the live EEPROM) -----------------------------------
// Each re-reads, applies the change with correct checksums, and writes back.
// Return 1 on success. Video standard / DVD region are plaintext + CRC; game
// region is re-encrypted. Callers should have backed up the EEPROM first.
int EeData_SetVideoStd(DWORD videoStd);      // EE_VS_*
int EeData_SetGameRegion(DWORD region);      // EE_REGION_*
int EeData_SetDvdRegion(DWORD zone);         // 0..6 (0 = region-free)