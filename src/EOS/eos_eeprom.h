#pragma once
// eos_eeprom.h -- Read + decode the console EEPROM for display.
//
// We do NOT read the raw encrypted 256-byte blob and RC4-decrypt it ourselves
// (PrometheOS does that because it backs up / edits / restores the whole image).
// For a read-only info panel the kernel already decrypts the factory section:
// querying the individual XC_FACTORY_* / XC_* indices via ExQueryNonVolatileSetting
// returns plaintext fields with zero brick risk and no crypto to port.
//
// Reads only. Nothing here writes the EEPROM, so none of the brick-on-write
// fields (HMAC, online key, MAC, HDD key, checksums) are ever touched.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#pragma once
#include <xtl.h>

typedef struct EosEeprom {
    BOOL  valid;          // at least the core factory reads succeeded
    char  serial[16];     // factory serial (12 chars) + NUL
    BYTE  mac[6];         // factory MAC
    BOOL  macValid;
    DWORD avRegion;       // video standard dword (0x00400100 NTSC-M, ...)
    BOOL  avValid;
    DWORD gameRegion;     // 0x1 NA, 0x2 Japan, 0x4 Europe/RoW
    BOOL  gameValid;
    DWORD dvdRegion;      // 1..8, 0 = region-free
    BOOL  dvdValid;
    DWORD language;       // 1-based language id
    BOOL  langValid;
    DWORD videoFlags;     // XC_VIDEO bitmask (480p/720p/1080i/widescreen/...)
    BOOL  videoValid;
} EosEeprom;

// Populate e from the running console. Safe to call once at boot or on demand.
void        Eeprom_Read(EosEeprom* e);

// Human-readable decoders (return static strings; never NULL).
const char* Eeprom_VideoStandardStr(const EosEeprom* e); // "NTSC-M","NTSC-J","PAL-I", or hex
const char* Eeprom_GameRegionStr(const EosEeprom* e);    // "North America" / "Japan" / "Europe"
const char* Eeprom_LanguageStr(const EosEeprom* e);      // "English", ...
const char* Eeprom_DvdRegionStr(const EosEeprom* e);     // "Region 1" / "Region-free"
void        Eeprom_MacStr(const EosEeprom* e, char* out16); // "00:11:22:33:44:55" (needs >=18)

// Video capability flags decoded from videoFlags.
BOOL Eeprom_Has480p(const EosEeprom* e);
BOOL Eeprom_Has720p(const EosEeprom* e);
BOOL Eeprom_Has1080i(const EosEeprom* e);
BOOL Eeprom_IsWidescreen(const EosEeprom* e);