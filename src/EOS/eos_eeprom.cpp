// eos_eeprom.cpp -- Read + decode the console EEPROM. See eos_eeprom.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_eeprom.h"
#include "eos_ee_data.h"   // decrypted EEPROM layer (raw image + crypto)

// xboxkrnl ExQueryNonVolatileSetting value indices (XAPI.H / PrometheOS /
// XbDiag). The kernel decrypts the factory section, so these individual queries
// return plaintext. NOTE: the previous map was WRONG (language/video/dvd indices
// were mis-assigned), which is why video/region read garbage. Corrected here.
#define XC_VIDEO_STANDARD          0x04   // NTSC-M/J, PAL-I/M dword
#define XC_VIDEO_FLAGS             0x05   // HDTV/widescreen/50-60hz bitmask
#define XC_AUDIO_FLAGS             0x06
#define XC_GAME_REGION             0x07   // 1=NA, 2=Japan, 4=Europe
#define XC_DVD_REGION              0x08   // DVD playback zone
#define XC_LANGUAGE                0x11   // menu language ID
#define XC_FACTORY_SERIAL_NUMBER   0x100
#define XC_FACTORY_ETHERNET_ADDR   0x101
#define XC_FACTORY_AV_REGION       0x102
#define XC_FACTORY_GAME_REGION     0x103

// Video standard dwords (from xboxConfig): NTSC-M 0x00400100, NTSC-J 0x00400200,
// PAL-I 0x00800300. High byte 0x40 = NTSC family, 0x80 = PAL family.
#define VS_NTSC_M                  0x00400100
#define VS_NTSC_J                  0x00400200
#define VS_PAL_I                   0x00800300

// XC_VIDEO flag bits (from xboxinternals.h).
#define VF_WIDESCREEN              0x00010000
#define VF_HDTV_720p               0x00020000
#define VF_HDTV_1080i              0x00040000
#define VF_HDTV_480p               0x00080000

// The kernel export. RXDK's xtl.h declares it; PrometheOS calls it the same way.
extern "C" ULONG __stdcall ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD* Type,
    PVOID Value, DWORD ValueLength,
    DWORD* ResultLength);


void Eeprom_Read(EosEeprom* e)
{
    EeData d;
    int i;

    for (i = 0; i < (int)sizeof(EosEeprom); ++i) ((unsigned char*)e)[i] = 0;

    // Read + decrypt the raw EEPROM image ourselves (the kernel setting indices
    // returned wrong data). EeData_Read handles the RC4/SHA1 security block.
    if (!EeData_Read(&d)) { e->valid = FALSE; return; }
    e->valid = TRUE;

    for (i = 0; i < 12; ++i) e->serial[i] = d.serial[i];
    e->serial[12] = 0;
    for (i = 0; i < 6; ++i)  e->mac[i] = d.mac[i];
    e->macValid = d.macValid ? TRUE : FALSE;

    e->avRegion = d.videoStd;    e->avValid = TRUE;
    e->gameRegion = d.gameRegion;  e->gameValid = d.decrypted ? TRUE : FALSE;
    e->dvdRegion = d.dvdRegion;   e->dvdValid = TRUE;
    e->language = d.language;    e->langValid = TRUE;
    e->videoFlags = d.videoFlags;  e->videoValid = TRUE;
}

const char* Eeprom_VideoStandardStr(const EosEeprom* e)
{
    static char hex[12];
    DWORD v;
    int   i, sh;
    if (!e->avValid) return "Unknown";
    v = e->avRegion;
    // XC_VIDEO_STANDARD dword values (same set XbDiag decodes at EEPROM 0x58).
    if (v == 0x00400100) return "NTSC-M (N. America)";
    if (v == 0x00400200) return "NTSC-J (Japan)";
    if (v == 0x00800300) return "PAL-I (Europe/AUS)";
    if (v == 0x00400400) return "PAL-M (Brazil)";
    // family fallback by high byte, else raw hex
    if ((v & 0x00FF0000) == 0x00400000) return "NTSC";
    if ((v & 0x00FF0000) == 0x00800000) return "PAL";
    hex[0] = '0'; hex[1] = 'x';
    for (i = 0; i < 8; ++i) {
        sh = (7 - i) * 4;
        hex[2 + i] = "0123456789ABCDEF"[(v >> sh) & 0xF];
    }
    hex[10] = 0;
    return hex;
}

const char* Eeprom_GameRegionStr(const EosEeprom* e)
{
    if (!e->gameValid) return "Unknown";
    switch (e->gameRegion) {
    case 0x1: return "North America";
    case 0x2: return "Japan";
    case 0x4: return "Europe / RoW";
    }
    return "Multi / Other";
}

const char* Eeprom_LanguageStr(const EosEeprom* e)
{
    if (!e->langValid) return "Unknown";
    switch (e->language) {
    case 1:  return "English";
    case 2:  return "Japanese";
    case 3:  return "German";
    case 4:  return "French";
    case 5:  return "Spanish";
    case 6:  return "Italian";
    case 7:  return "Korean";
    case 8:  return "Chinese";
    case 9:  return "Portuguese";
    }
    return "Not set";
}

const char* Eeprom_DvdRegionStr(const EosEeprom* e)
{
    static char s[12];
    if (!e->dvdValid) return "Unknown";
    if (e->dvdRegion == 0 || e->dvdRegion > 8) return "Region-free";
    s[0] = 'R'; s[1] = 'e'; s[2] = 'g'; s[3] = 'i'; s[4] = 'o'; s[5] = 'n'; s[6] = ' ';
    s[7] = (char)('0' + (int)e->dvdRegion);
    s[8] = 0;
    return s;
}

void Eeprom_MacStr(const EosEeprom* e, char* out16)
{
    const char* hx = "0123456789ABCDEF";
    int i, p = 0;
    if (!e->macValid) { out16[0] = '-'; out16[1] = '-'; out16[2] = 0; return; }
    for (i = 0; i < 6; ++i) {
        out16[p++] = hx[(e->mac[i] >> 4) & 0xF];
        out16[p++] = hx[e->mac[i] & 0xF];
        if (i < 5) out16[p++] = ':';
    }
    out16[p] = 0;
}

BOOL Eeprom_Has480p(const EosEeprom* e) { return e->videoValid && (e->videoFlags & VF_HDTV_480p) != 0; }
BOOL Eeprom_Has720p(const EosEeprom* e) { return e->videoValid && (e->videoFlags & VF_HDTV_720p) != 0; }
BOOL Eeprom_Has1080i(const EosEeprom* e) { return e->videoValid && (e->videoFlags & VF_HDTV_1080i) != 0; }
BOOL Eeprom_IsWidescreen(const EosEeprom* e) { return e->videoValid && (e->videoFlags & VF_WIDESCREEN) != 0; }