// eos_eeprom_crypto.cpp -- RC4/SHA1 EEPROM security block + game-region write.
// RXDK / MSVC2003 / C89: declarations before statements, no CRT, no sprintf.
// Faithful port of PrometheOS XKSHA1 / XKRC4 / XKEEPROM crypto.
#include "eos_eeprom_crypto.h"
#include "eos_eeprom_io.h"     // Eeprom_ReadImage/WriteImage/BackupToHdd, EOS_EE_*

typedef unsigned long  u32;    // 32-bit on MSVC2003
typedef unsigned char  u8;

// EEPROM security-block offsets within the 256-byte image.
#define OFF_HASH    0x00       // HMAC-SHA1 hash / RC4 key seed (20 bytes)
#define OFF_CONF    0x14       // Confounder (8 bytes)
#define OFF_HDDK    0x1C       // HDD key (16) + XBERegion (4) = 20 bytes
#define OFF_REGION  0x2C       // region byte (HDDK[16])
#define OFF_CKSUM2  0x30
#define OFF_FACT    0x34
#define LEN_FACT    0x2C
#define OFF_CKSUM3  0x60
#define OFF_USER    0x64
#define LEN_USER    0x5C

// ===========================================================================
// SHA-1 (RFC 3174), matching XKSHA1 exactly.
// ===========================================================================
#define SHA1_ROL(b,w) (((w) << (b)) | ((w) >> (32 - (b))))

typedef struct {
    u32 h[5];
    u32 lenLow, lenHigh;
    int idx;
    u8  block[64];
} SHA1;

static void sha1Reset(SHA1* c)
{
    c->lenLow = 0; c->lenHigh = 0; c->idx = 0;
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
}

static void sha1Block(SHA1* c)
{
    static const u32 K[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    u32 W[80], A, B, C, D, E, temp; int t;
    for (t = 0; t < 16; t++)
        W[t] = ((u32)c->block[t * 4] << 24) | ((u32)c->block[t * 4 + 1] << 16)
        | ((u32)c->block[t * 4 + 2] << 8) | ((u32)c->block[t * 4 + 3]);
    for (t = 16; t < 80; t++)
        W[t] = SHA1_ROL(1, W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]);
    A = c->h[0]; B = c->h[1]; C = c->h[2]; D = c->h[3]; E = c->h[4];
    for (t = 0; t < 20; t++) { temp = SHA1_ROL(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0]; E = D; D = C; C = SHA1_ROL(30, B); B = A; A = temp; }
    for (t = 20; t < 40; t++) { temp = SHA1_ROL(5, A) + (B ^ C ^ D) + E + W[t] + K[1]; E = D; D = C; C = SHA1_ROL(30, B); B = A; A = temp; }
    for (t = 40; t < 60; t++) { temp = SHA1_ROL(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2]; E = D; D = C; C = SHA1_ROL(30, B); B = A; A = temp; }
    for (t = 60; t < 80; t++) { temp = SHA1_ROL(5, A) + (B ^ C ^ D) + E + W[t] + K[3]; E = D; D = C; C = SHA1_ROL(30, B); B = A; A = temp; }
    c->h[0] += A; c->h[1] += B; c->h[2] += C; c->h[3] += D; c->h[4] += E;
    c->idx = 0;
}

static void sha1Input(SHA1* c, const u8* msg, int len)
{
    while (len-- > 0) {
        c->block[c->idx++] = (u8)(*msg & 0xFF);
        c->lenLow += 8;
        if (c->lenLow == 0) c->lenHigh++;
        if (c->idx == 64) sha1Block(c);
        msg++;
    }
}

static void sha1Result(SHA1* c, u8 out[20])
{
    int i;
    if (c->idx > 55) {
        c->block[c->idx++] = 0x80;
        while (c->idx < 64) c->block[c->idx++] = 0;
        sha1Block(c);
        while (c->idx < 56) c->block[c->idx++] = 0;
    }
    else {
        c->block[c->idx++] = 0x80;
        while (c->idx < 56) c->block[c->idx++] = 0;
    }
    c->block[56] = (u8)(c->lenHigh >> 24); c->block[57] = (u8)(c->lenHigh >> 16);
    c->block[58] = (u8)(c->lenHigh >> 8);  c->block[59] = (u8)(c->lenHigh);
    c->block[60] = (u8)(c->lenLow >> 24); c->block[61] = (u8)(c->lenLow >> 16);
    c->block[62] = (u8)(c->lenLow >> 8);  c->block[63] = (u8)(c->lenLow);
    sha1Block(c);
    for (i = 0; i < 20; i++)
        out[i] = (u8)(c->h[i >> 2] >> (8 * (3 - (i & 3))));
}

// HMAC inner/outer states pre-seeded with (key XOR pad) per EEPROM key version.
static void hmac1Reset(int version, SHA1* c)   // inner
{
    sha1Reset(c);
    if (version == 10) { c->h[0] = 0x72127625; c->h[1] = 0x336472B9; c->h[2] = 0xBE609BEA; c->h[3] = 0xF55E226B; c->h[4] = 0x99958DAC; }
    else if (version == 11) { c->h[0] = 0x39B06E79; c->h[1] = 0xC9BD25E8; c->h[2] = 0xDBC6B498; c->h[3] = 0x40B4389D; c->h[4] = 0x86BBD7ED; }
    else if (version == 12) { c->h[0] = 0x8058763A; c->h[1] = 0xF97D4E0E; c->h[2] = 0x865A9762; c->h[3] = 0x8A3D920D; c->h[4] = 0x08995B2C; }
    c->lenLow = 512;   // one 64-byte (key^ipad) block already counted
}

static void hmac2Reset(int version, SHA1* c)   // outer
{
    sha1Reset(c);
    if (version == 10) { c->h[0] = 0x76441D41; c->h[1] = 0x4DE82659; c->h[2] = 0x2E8EF85E; c->h[3] = 0xB256FACA; c->h[4] = 0xC4FE2DE8; }
    else if (version == 11) { c->h[0] = 0x9B49BED3; c->h[1] = 0x84B430FC; c->h[2] = 0x6B8749CD; c->h[3] = 0xEBFE5FE5; c->h[4] = 0xD96E7393; }
    else if (version == 12) { c->h[0] = 0x01075307; c->h[1] = 0xA2F1E037; c->h[2] = 0x1186EEEA; c->h[3] = 0x88DA9992; c->h[4] = 0x168A5609; }
    c->lenLow = 512;
}

// XBOX_HMAC_SHA1 over one buffer (the stored hash) -> RC4 key seed.
static void xbHmac1(int version, u8 out[20], const u8* buf, int len)
{
    SHA1 c; u8 inner[20];
    hmac1Reset(version, &c); sha1Input(&c, buf, len); sha1Result(&c, inner);
    hmac2Reset(version, &c); sha1Input(&c, inner, 20); sha1Result(&c, out);
}

// XBOX_HMAC_SHA1 over (confounder[8] + hddkey[20]) -> data hash.
static void xbHmac2(int version, u8 out[20], const u8* conf, const u8* hddk)
{
    SHA1 c; u8 inner[20];
    hmac1Reset(version, &c); sha1Input(&c, conf, 8); sha1Input(&c, hddk, 20); sha1Result(&c, inner);
    hmac2Reset(version, &c); sha1Input(&c, inner, 20); sha1Result(&c, out);
}

// ===========================================================================
// RC4 (XKRC4).
// ===========================================================================
typedef struct { u8 state[256]; u8 x, y; } RC4;

static void rc4Init(RC4* k, const u8* key, int keyLen)
{
    int i; u8 i1 = 0, i2 = 0, t;
    for (i = 0; i < 256; i++) k->state[i] = (u8)i;
    k->x = 0; k->y = 0;
    for (i = 0; i < 256; i++) {
        i2 = (u8)(key[i1] + k->state[i] + i2);
        t = k->state[i]; k->state[i] = k->state[i2]; k->state[i2] = t;
        i1 = (u8)((i1 + 1) % keyLen);
    }
}

static void rc4Crypt(RC4* k, u8* data, int len)
{
    int n; u8 x = k->x, y = k->y, t, xi;
    for (n = 0; n < len; n++) {
        x = (u8)(x + 1);
        y = (u8)(k->state[x] + y);
        t = k->state[x]; k->state[x] = k->state[y]; k->state[y] = t;
        xi = (u8)(k->state[x] + k->state[y]);
        data[n] ^= k->state[xi];
    }
    k->x = x; k->y = y;
}

// QuickCRC (carry-folding sum), no 64-bit -- same as eos_eeprom_io.
static void quickCRC(u8* crc4, const u8* data, int len)
{
    u32 high = 0, low = 0, val, nl; int i, n = len / 4;
    for (i = 0; i < n; i++) {
        val = ((const u32*)data)[i];
        nl = low + val;
        if (nl < low) high += 1;
        low = nl;
    }
    *(u32*)crc4 = ~(high + low);
}

// ===========================================================================
// EEPROM security block: decrypt (auto-detect) / encrypt.
// img is the full 256-byte image; decrypt rewrites 0x14..0x2F in place.
// ===========================================================================
static int eqv(const u8* a, const u8* b, int n)
{
    int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1;
}

// Returns detected version (10/11/12) on success, or EOS_XV_NONE.
static int eepDecrypt(u8* img)
{
    int ver; u8 keyHash[20], dataHash[20], conf[8], hddk[20]; RC4 rk; int j;
    for (ver = 10; ver <= 12; ver++) {
        for (j = 0; j < 8; j++) conf[j] = img[OFF_CONF + j];
        for (j = 0; j < 20; j++) hddk[j] = img[OFF_HDDK + j];
        xbHmac1(ver, keyHash, &img[OFF_HASH], 20);
        rc4Init(&rk, keyHash, 20);
        rc4Crypt(&rk, conf, 8);
        rc4Crypt(&rk, hddk, 20);
        xbHmac2(ver, dataHash, conf, hddk);
        if (eqv(&img[OFF_HASH], dataHash, 20)) {     // key/version correct
            for (j = 0; j < 8; j++) img[OFF_CONF + j] = conf[j];
            for (j = 0; j < 20; j++) img[OFF_HDDK + j] = hddk[j];
            return ver;
        }
    }
    return EOS_XV_NONE;
}

// img holds DECRYPTED conf/hddk; re-encrypt with `ver`, refresh hash + CRCs.
static void eepEncrypt(u8* img, int ver)
{
    u8 keyHash[20]; RC4 rk;
    xbHmac2(ver, &img[OFF_HASH], &img[OFF_CONF], &img[OFF_HDDK]);  // new hash
    xbHmac1(ver, keyHash, &img[OFF_HASH], 20);                    // RC4 seed
    rc4Init(&rk, keyHash, 20);
    rc4Crypt(&rk, &img[OFF_CONF], 8);
    rc4Crypt(&rk, &img[OFF_HDDK], 20);
    quickCRC(&img[OFF_CKSUM2], &img[OFF_FACT], LEN_FACT);
    quickCRC(&img[OFF_CKSUM3], &img[OFF_USER], LEN_USER);
}

// ===========================================================================
// Public API.
// ===========================================================================
static int s_lastVersion = EOS_XV_NONE;

int Eeprom_GameRegionVersion(void) { return s_lastVersion; }

int Eeprom_GetGameRegion(void)
{
    u8 img[EOS_EEPROM_SIZE]; int ver;
    if (Eeprom_ReadImage(img) != EOS_EE_OK) return -1;
    ver = eepDecrypt(img);
    s_lastVersion = ver;
    if (ver == EOS_XV_NONE) return -1;
    return (int)img[OFF_REGION];
}

int Eeprom_SetGameRegion(int region, char* backupOut, int backupLen)
{
    u8 img[EOS_EEPROM_SIZE], chk[EOS_EEPROM_SIZE], hddSave[16], rb[EOS_EEPROM_SIZE];
    int ver, vchk, j, rc;

    if (region != EOS_XBE_NA && region != EOS_XBE_JP && region != EOS_XBE_EU)
        return EOS_RGN_ERR_ARG;

    if (Eeprom_ReadImage(img) != EOS_EE_OK) return EOS_RGN_ERR_READ;

    // 1) decrypt + capture the HDD key (must survive the round-trip)
    ver = eepDecrypt(img);
    s_lastVersion = ver;
    if (ver == EOS_XV_NONE) return EOS_RGN_ERR_CRYPTO;
    for (j = 0; j < 16; j++) hddSave[j] = img[OFF_HDDK + j];

    // 2) change only the region (4-byte LE field at 0x2C)
    img[OFF_REGION + 0] = (u8)region;
    img[OFF_REGION + 1] = 0;
    img[OFF_REGION + 2] = 0;
    img[OFF_REGION + 3] = 0;

    // 3) re-encrypt, then PROVE the round-trip on a copy before writing anything
    eepEncrypt(img, ver);
    for (j = 0; j < EOS_EEPROM_SIZE; j++) chk[j] = img[j];
    vchk = eepDecrypt(chk);
    if (vchk != ver) return EOS_RGN_ERR_CRYPTO;
    if (chk[OFF_REGION] != (u8)region) return EOS_RGN_ERR_CRYPTO;
    for (j = 0; j < 16; j++) if (chk[OFF_HDDK + j] != hddSave[j]) return EOS_RGN_ERR_CRYPTO;

    // 4) mandatory backup of the CURRENT EEPROM, then write the new image
    rc = Eeprom_BackupToHdd(backupOut, backupLen);
    if (rc != EOS_EE_OK) return EOS_RGN_ERR_BACKUP;

    if (Eeprom_WriteImage(img) != EOS_EE_OK) return EOS_RGN_ERR_WRITE;

    // 5) read back + decrypt + verify region and HDD key landed correctly
    if (Eeprom_ReadImage(rb) != EOS_EE_OK) return EOS_RGN_ERR_WRITE;
    if (eepDecrypt(rb) != ver) return EOS_RGN_ERR_WRITE;
    if (rb[OFF_REGION] != (u8)region) return EOS_RGN_ERR_WRITE;
    for (j = 0; j < 16; j++) if (rb[OFF_HDDK + j] != hddSave[j]) return EOS_RGN_ERR_WRITE;

    return EOS_RGN_OK;
}