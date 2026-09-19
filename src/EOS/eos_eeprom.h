#pragma once
// eos_eeprom.h -- Read + decode the console EEPROM for display.
//
// The active EEPROM path reads the live 256-byte image through eos_ee_data,
// decrypts the security block with the Xbox EEPROM key set, and exposes the
// fields used by Settings/System Info. Writes are handled by eos_ee_data and
// eos_eeprom_io, which also maintain the required checksums/encryption.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include <xtl.h>

typedef struct EosEeprom {
    BOOL  valid;
    char  serial[16];
    BYTE  mac[6];
    BOOL  macValid;
    DWORD avRegion;
    BOOL  avValid;
    DWORD gameRegion;
    BOOL  gameValid;
    DWORD dvdRegion;
    BOOL  dvdValid;
    DWORD language;
    BOOL  langValid;
    DWORD videoFlags;
    BOOL  videoValid;
} EosEeprom;

void        Eeprom_Read(EosEeprom* e);
const char* Eeprom_VideoStandardStr(const EosEeprom* e);
const char* Eeprom_GameRegionStr(const EosEeprom* e);
const char* Eeprom_LanguageStr(const EosEeprom* e);
const char* Eeprom_DvdRegionStr(const EosEeprom* e);
void        Eeprom_MacStr(const EosEeprom* e, char* out16);
