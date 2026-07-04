// eos_ee_data.cpp -- see eos_ee_data.h. Ported from PrometheOS XKEEPROM (GPLv2).
#include "eos_ee_data.h"
#include "eos_ee_crypto.h"

// Kernel EEPROM image access (full 256-byte raw image, index 0xFFFF).
extern "C" ULONG __stdcall ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD* Type,
    PVOID Value, DWORD ValueLength,
    DWORD* ResultLength);
extern "C" ULONG __stdcall ExSaveNonVolatileSetting(DWORD ValueIndex, DWORD Type,
    PVOID Value, DWORD ValueLength);

#define EE_FULL_INDEX 0xFFFF
#define EE_SIZE       256

// ---- EEPROM layout offsets (Xbox EEPROMDATA struct) -------------------------
#define OFF_HMAC        0x00   // 20  HMAC-SHA1 hash
#define OFF_CONFOUNDER  0x14   //  8  RC4 encrypted confounder
#define OFF_HDDKEY      0x1C   // 16  RC4 encrypted HDD key
#define OFF_XBEREGION   0x2C   //  4  RC4 encrypted region code (in security block)
#define OFF_CHECKSUM2   0x30   //  4  CRC of next 0x2C bytes (0x34..0x5F)
#define OFF_SERIAL      0x34   // 12
#define OFF_MAC         0x40   //  6
#define OFF_VIDEOSTD    0x58   //  4
#define OFF_CHECKSUM3   0x60   //  4  CRC of next 0x5C bytes (0x64..0xBF)
#define OFF_LANGUAGE    0x90   //  4
#define OFF_VIDEOFLAGS  0x94   //  4
#define OFF_DVDZONE     0xBC   //  4

// The security block (confounder+hddkey) is validated against this known
// decrypted confounder to confirm the RC4 key generation matched.
// (Not needed for the hash-compare method, kept for reference.)

static DWORD rd32(const UCHAR* p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}
static void wr32(UCHAR* p, DWORD v) {
    p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
}

static int readImage(UCHAR* img)
{
    DWORD type = 0, got = 0, st; int i;
    for (i = 0; i < EE_SIZE; ++i) img[i] = 0;
    st = ExQueryNonVolatileSetting(EE_FULL_INDEX, &type, img, EE_SIZE, &got);
    return (st == 0 && got == EE_SIZE) ? 1 : 0;
}

static int writeImage(const UCHAR* img)
{
    UCHAR tmp[EE_SIZE]; int i;
    for (i = 0; i < EE_SIZE; ++i) tmp[i] = img[i];
    return (ExSaveNonVolatileSetting(EE_FULL_INDEX, 3, tmp, EE_SIZE) == 0) ? 1 : 0;
}

// Auto-detect the console key generation and decrypt the security block IN
// PLACE (confounder @0x14, hddkey @0x1C). Returns the matched version (EE_VER_*)
// or EE_VER_NONE. On success, img[0x14..0x2B] hold plaintext.
static int decryptSecurity(UCHAR* img)
{
    int ver;
    UCHAR orig[0x30];
    int i;
    for (i = 0; i < 0x30; ++i) orig[i] = img[i];

    for (ver = 10; ver < 13; ++ver) {           // 10=1.0, 11=1.1, 12=1.6
        UCHAR key_hash[20], confirm[20];
        EeRc4Key rc4;

        // key_hash = HMAC-SHA1(version, HMAC[0..19])
        Ee_XboxHmacSha1(ver, key_hash, img + OFF_HMAC, 20, (UCHAR*)0);

        // RC4-decrypt confounder(8) + hddkey(20) with key_hash
        Ee_Rc4Init(key_hash, 20, &rc4);
        Ee_Rc4Crypt(img + OFF_CONFOUNDER, 8, &rc4);
        Ee_Rc4Crypt(img + OFF_HDDKEY, 20, &rc4);

        // confirm = HMAC-SHA1(version, confounder(8), hddkey(20))
        Ee_XboxHmacSha1(ver, confirm,
            img + OFF_CONFOUNDER, 8,
            img + OFF_HDDKEY, 20, (UCHAR*)0);

        // matches stored HMAC -> this key generation is correct
        {
            int ok = 1;
            for (i = 0; i < 20; ++i) if (confirm[i] != img[OFF_HMAC + i]) { ok = 0; break; }
            if (ok) return (ver == 10) ? EE_VER_1_0 : (ver == 11) ? EE_VER_1_1 : EE_VER_1_6;
        }

        // wrong key -> restore the security block and try the next version
        for (i = 0; i < 0x30; ++i) img[i] = orig[i];
    }
    return EE_VER_NONE;
}

// Re-encrypt the security block for the given version and recompute both CRCs.
static void encryptAndCrc(UCHAR* img, int eeVer)
{
    int ver = (eeVer == EE_VER_1_0) ? 10 : (eeVer == EE_VER_1_1) ? 11 : 12;
    UCHAR key_hash[20];
    EeRc4Key rc4;

    // rebuild HMAC over the plaintext confounder+hddkey
    Ee_XboxHmacSha1(ver, img + OFF_HMAC,
        img + OFF_CONFOUNDER, 8,
        img + OFF_HDDKEY, 20, (UCHAR*)0);
    // key_hash from the new HMAC
    Ee_XboxHmacSha1(ver, key_hash, img + OFF_HMAC, 20, (UCHAR*)0);
    // RC4-encrypt confounder+hddkey
    Ee_Rc4Init(key_hash, 20, &rc4);
    Ee_Rc4Crypt(img + OFF_CONFOUNDER, 8, &rc4);
    Ee_Rc4Crypt(img + OFF_HDDKEY, 20, &rc4);
    // checksum2 over 0x34..0x5F (0x2C bytes), checksum3 over 0x64..0xBF (0x5C)
    Ee_QuickCRC(img + OFF_CHECKSUM2, img + OFF_SERIAL, 0x2C);
    Ee_QuickCRC(img + OFF_CHECKSUM3, img + (OFF_CHECKSUM3 + 4), 0x5C);
}

// ============================================================================
// Public: read + parse
// ============================================================================
int EeData_Read(EeData* d)
{
    UCHAR img[EE_SIZE];
    int i, v;
    for (i = 0; i < (int)sizeof(EeData); ++i) ((UCHAR*)d)[i] = 0;

    if (!readImage(img)) return 0;
    d->valid = 1;

    // Plaintext fields (no decryption needed).
    d->videoStd = rd32(img + OFF_VIDEOSTD);
    d->videoFlags = rd32(img + OFF_VIDEOFLAGS);
    d->dvdRegion = rd32(img + OFF_DVDZONE);
    d->language = rd32(img + OFF_LANGUAGE);

    for (i = 0; i < 12; ++i) d->serial[i] = (char)img[OFF_SERIAL + i];
    d->serial[12] = 0;
    d->serialValid = (d->serial[0] != 0) ? 1 : 0;
    for (i = 0; i < 6; ++i) d->mac[i] = img[OFF_MAC + i];
    d->macValid = 1;

    // Region is in the encrypted security block -> decrypt to read it.
    v = decryptSecurity(img);
    d->version = v;
    d->decrypted = (v != EE_VER_NONE) ? 1 : 0;
    if (d->decrypted) {
        d->gameRegion = (DWORD)img[OFF_XBEREGION];   // low byte holds the region
    }
    else {
        d->gameRegion = 0;
    }
    return 1;
}

// ============================================================================
// Public: strings
// ============================================================================
const char* EeData_VideoStdStr(const EeData* d)
{
    if (!d->valid) return "Unknown";
    switch (d->videoStd) {
    case EE_VS_NTSC_M: return "NTSC-M (N. America)";
    case EE_VS_NTSC_J: return "NTSC-J (Japan)";
    case EE_VS_PAL_I:  return "PAL-I (Europe/AUS)";
    case EE_VS_PAL_M:  return "PAL-M (Brazil)";
    }
    if ((d->videoStd & 0x00FF0000) == 0x00400000) return "NTSC";
    if ((d->videoStd & 0x00FF0000) == 0x00800000) return "PAL";
    return "Unknown";
}

const char* EeData_GameRegionStr(const EeData* d)
{
    if (!d->decrypted) return "Unknown";
    switch (d->gameRegion) {
    case EE_REGION_NA:    return "North America";
    case EE_REGION_JAPAN: return "Japan";
    case EE_REGION_EURO:  return "Europe / RoW";
    case 0x80000000:      return "Manufacturing";
    }
    return "Unknown";
}

const char* EeData_DvdRegionStr(const EeData* d)
{
    static char s[12];
    if (!d->valid) return "Unknown";
    if (d->dvdRegion == 0 || d->dvdRegion > 8) return "Region-free";
    s[0] = 'R'; s[1] = 'e'; s[2] = 'g'; s[3] = 'i'; s[4] = 'o'; s[5] = 'n'; s[6] = ' ';
    s[7] = (char)('0' + (int)d->dvdRegion); s[8] = 0;
    return s;
}

// ============================================================================
// Public: writers
// ============================================================================
int EeData_SetVideoStd(DWORD videoStd)
{
    UCHAR img[EE_SIZE];
    if (!readImage(img)) return 0;
    wr32(img + OFF_VIDEOSTD, videoStd);
    // video standard lives in the checksum2 span (plaintext) -> recompute CRC2.
    Ee_QuickCRC(img + OFF_CHECKSUM2, img + OFF_SERIAL, 0x2C);
    return writeImage(img);
}

int EeData_SetDvdRegion(DWORD zone)
{
    UCHAR img[EE_SIZE];
    if (!readImage(img)) return 0;
    wr32(img + OFF_DVDZONE, zone);
    // DVD zone is in the checksum3 span (plaintext) -> recompute CRC3.
    Ee_QuickCRC(img + OFF_CHECKSUM3, img + (OFF_CHECKSUM3 + 4), 0x5C);
    return writeImage(img);
}

int EeData_SetGameRegion(DWORD region)
{
    UCHAR img[EE_SIZE];
    int ver;
    if (!readImage(img)) return 0;
    ver = decryptSecurity(img);              // security block now plaintext
    if (ver == EE_VER_NONE) return 0;        // can't safely re-encrypt
    img[OFF_XBEREGION + 0] = (UCHAR)region;  // first byte only, per PrometheOS
    img[OFF_XBEREGION + 1] = 0;
    img[OFF_XBEREGION + 2] = 0;
    img[OFF_XBEREGION + 3] = 0;
    encryptAndCrc(img, ver);                 // re-encrypt + both CRCs
    return writeImage(img);
}