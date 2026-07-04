// eos_ee_crypto.cpp -- see eos_ee_crypto.h.
// Verbatim algorithms from XKUtils (GPLv2): SHA1, Xbox HMAC-SHA1, RC4, QuickCRC.
#include "eos_ee_crypto.h"
#include <stdarg.h>

// ============================================================================
// SHA-1 core (RFC 3174 reference, as used by XKSHA1)
// ============================================================================
#define SHA1_OK    0
#define SHA1_NULL  1
#define SHA1CircularShift(bits,word) (((word) << (bits)) | ((word) >> (32-(bits))))

typedef struct {
    UINT32 Intermediate_Hash[5];
    UINT32 Length_Low;
    UINT32 Length_High;
    DWORD  Message_Block_Index;
    UCHAR  Message_Block[64];
    int    Computed;
    int    Corrupted;
} Sha1Ctx;

static void Sha1ProcessBlock(Sha1Ctx* c)
{
    const UINT32 K[] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    int    t;
    UINT32 temp, W[80], A, B, C, D, E;

    for (t = 0; t < 16; t++) {
        W[t] = c->Message_Block[t * 4] << 24;
        W[t] |= c->Message_Block[t * 4 + 1] << 16;
        W[t] |= c->Message_Block[t * 4 + 2] << 8;
        W[t] |= c->Message_Block[t * 4 + 3];
    }
    for (t = 16; t < 80; t++)
        W[t] = SHA1CircularShift(1, W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]);

    A = c->Intermediate_Hash[0]; B = c->Intermediate_Hash[1];
    C = c->Intermediate_Hash[2]; D = c->Intermediate_Hash[3];
    E = c->Intermediate_Hash[4];

    for (t = 0; t < 20; t++) {
        temp = SHA1CircularShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        E = D; D = C; C = SHA1CircularShift(30, B); B = A; A = temp;
    }
    for (t = 20; t < 40; t++) {
        temp = SHA1CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
        E = D; D = C; C = SHA1CircularShift(30, B); B = A; A = temp;
    }
    for (t = 40; t < 60; t++) {
        temp = SHA1CircularShift(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        E = D; D = C; C = SHA1CircularShift(30, B); B = A; A = temp;
    }
    for (t = 60; t < 80; t++) {
        temp = SHA1CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
        E = D; D = C; C = SHA1CircularShift(30, B); B = A; A = temp;
    }

    c->Intermediate_Hash[0] += A; c->Intermediate_Hash[1] += B;
    c->Intermediate_Hash[2] += C; c->Intermediate_Hash[3] += D;
    c->Intermediate_Hash[4] += E;
    c->Message_Block_Index = 0;
}

static int Sha1Reset(Sha1Ctx* c)
{
    if (!c) return SHA1_NULL;
    c->Length_Low = 0; c->Length_High = 0; c->Message_Block_Index = 0;
    c->Intermediate_Hash[0] = 0x67452301; c->Intermediate_Hash[1] = 0xEFCDAB89;
    c->Intermediate_Hash[2] = 0x98BADCFE; c->Intermediate_Hash[3] = 0x10325476;
    c->Intermediate_Hash[4] = 0xC3D2E1F0;
    c->Computed = 0; c->Corrupted = 0;
    return SHA1_OK;
}

static int Sha1Input(Sha1Ctx* c, const UCHAR* msg, unsigned int length)
{
    if (!length) return SHA1_OK;
    if (!c || !msg) return SHA1_NULL;
    if (c->Computed) { c->Corrupted = 1; return 1; }
    if (c->Corrupted) return c->Corrupted;

    while (length-- && !c->Corrupted) {
        c->Message_Block[c->Message_Block_Index++] = (UCHAR)(*msg & 0xFF);
        c->Length_Low += 8;
        if (c->Length_Low == 0) {
            c->Length_High++;
            if (c->Length_High == 0) c->Corrupted = 1;
        }
        if (c->Message_Block_Index == 64) Sha1ProcessBlock(c);
        msg++;
    }
    return SHA1_OK;
}

static void Sha1Pad(Sha1Ctx* c)
{
    if (c->Message_Block_Index > 55) {
        c->Message_Block[c->Message_Block_Index++] = 0x80;
        while (c->Message_Block_Index < 64) c->Message_Block[c->Message_Block_Index++] = 0;
        Sha1ProcessBlock(c);
        while (c->Message_Block_Index < 56) c->Message_Block[c->Message_Block_Index++] = 0;
    }
    else {
        c->Message_Block[c->Message_Block_Index++] = 0x80;
        while (c->Message_Block_Index < 56) c->Message_Block[c->Message_Block_Index++] = 0;
    }
    c->Message_Block[56] = (UCHAR)(c->Length_High >> 24);
    c->Message_Block[57] = (UCHAR)(c->Length_High >> 16);
    c->Message_Block[58] = (UCHAR)(c->Length_High >> 8);
    c->Message_Block[59] = (UCHAR)(c->Length_High);
    c->Message_Block[60] = (UCHAR)(c->Length_Low >> 24);
    c->Message_Block[61] = (UCHAR)(c->Length_Low >> 16);
    c->Message_Block[62] = (UCHAR)(c->Length_Low >> 8);
    c->Message_Block[63] = (UCHAR)(c->Length_Low);
    Sha1ProcessBlock(c);
}

static int Sha1Result(Sha1Ctx* c, UCHAR digest[20])
{
    int i;
    if (!c || !digest) return SHA1_NULL;
    if (c->Corrupted) return c->Corrupted;
    if (!c->Computed) {
        Sha1Pad(c);
        for (i = 0; i < 64; ++i) c->Message_Block[i] = 0;
        c->Length_Low = 0; c->Length_High = 0; c->Computed = 1;
    }
    for (i = 0; i < 20; ++i)
        digest[i] = (UCHAR)(c->Intermediate_Hash[i >> 2] >> (8 * (3 - (i & 3))));
    return SHA1_OK;
}

// ---- Xbox HMAC-SHA1 per-version intermediate-hash keys ----------------------
// version: 9=1.0, 10=1.1, 11=1.4, 12=1.6 (matches XKSHA1 HMAC1/2Reset).
static void Hmac1Reset(int version, Sha1Ctx* c)
{
    Sha1Reset(c);
    switch (version) {
    case 9:
        c->Intermediate_Hash[0] = 0x85F9E51A; c->Intermediate_Hash[1] = 0xE04613D2;
        c->Intermediate_Hash[2] = 0x6D86A50C; c->Intermediate_Hash[3] = 0x77C32E3C;
        c->Intermediate_Hash[4] = 0x4BD717A4; break;
    case 10:
        c->Intermediate_Hash[0] = 0x72127625; c->Intermediate_Hash[1] = 0x336472B9;
        c->Intermediate_Hash[2] = 0xBE609BEA; c->Intermediate_Hash[3] = 0xF55E226B;
        c->Intermediate_Hash[4] = 0x99958DAC; break;
    case 11:
        c->Intermediate_Hash[0] = 0x39B06E79; c->Intermediate_Hash[1] = 0xC9BD25E8;
        c->Intermediate_Hash[2] = 0xDBC6B498; c->Intermediate_Hash[3] = 0x40B4389D;
        c->Intermediate_Hash[4] = 0x86BBD7ED; break;
    case 12:
        c->Intermediate_Hash[0] = 0x8058763A; c->Intermediate_Hash[1] = 0xF97D4E0E;
        c->Intermediate_Hash[2] = 0x865A9762; c->Intermediate_Hash[3] = 0x8A3D920D;
        c->Intermediate_Hash[4] = 0x08995B2C; break;
    }
    c->Length_Low = 512;
}

static void Hmac2Reset(int version, Sha1Ctx* c)
{
    Sha1Reset(c);
    switch (version) {
    case 9:
        c->Intermediate_Hash[0] = 0x5D7A9C6B; c->Intermediate_Hash[1] = 0xE1922BEB;
        c->Intermediate_Hash[2] = 0xB82CCDBC; c->Intermediate_Hash[3] = 0x3137AB34;
        c->Intermediate_Hash[4] = 0x486B52B3; break;
    case 10:
        c->Intermediate_Hash[0] = 0x76441D41; c->Intermediate_Hash[1] = 0x4DE82659;
        c->Intermediate_Hash[2] = 0x2E8EF85E; c->Intermediate_Hash[3] = 0xB256FACA;
        c->Intermediate_Hash[4] = 0xC4FE2DE8; break;
    case 11:
        c->Intermediate_Hash[0] = 0x9B49BED3; c->Intermediate_Hash[1] = 0x84B430FC;
        c->Intermediate_Hash[2] = 0x6B8749CD; c->Intermediate_Hash[3] = 0xEBFE5FE5;
        c->Intermediate_Hash[4] = 0xD96E7393; break;
    case 12:
        c->Intermediate_Hash[0] = 0x01075307; c->Intermediate_Hash[1] = 0xA2F1E037;
        c->Intermediate_Hash[2] = 0x1186EEEA; c->Intermediate_Hash[3] = 0x88DA9992;
        c->Intermediate_Hash[4] = 0x168A5609; break;
    }
    c->Length_Low = 512;
}

void Ee_XboxHmacSha1(int version, UCHAR* result, ...)
{
    va_list args;
    Sha1Ctx ctx;
    va_start(args, result);

    Hmac1Reset(version, &ctx);
    for (;;) {
        UCHAR* buffer = va_arg(args, UCHAR*);
        int length;
        if (buffer == 0) break;
        length = va_arg(args, int);
        Sha1Input(&ctx, buffer, (unsigned int)length);
    }
    va_end(args);

    Sha1Result(&ctx, &ctx.Message_Block[0]);
    Hmac2Reset(version, &ctx);
    Sha1Input(&ctx, &ctx.Message_Block[0], 0x14);
    Sha1Result(&ctx, result);
}

// ============================================================================
// RC4
// ============================================================================
static void rc4_swap(UCHAR* a, UCHAR* b) { UCHAR t = *a; *a = *b; *b = t; }

void Ee_Rc4Init(const UCHAR* keyData, int keyLen, EeRc4Key* key)
{
    UCHAR index1 = 0, index2 = 0, * state;
    short counter;
    state = &key->state[0];
    for (counter = 0; counter < 256; counter++) state[counter] = (UCHAR)counter;
    key->x = 0; key->y = 0;
    for (counter = 0; counter < 256; counter++) {
        index2 = (UCHAR)((keyData[index1] + state[counter] + index2) % 256);
        rc4_swap(&state[counter], &state[index2]);
        index1 = (UCHAR)((index1 + 1) % keyLen);
    }
}

void Ee_Rc4Crypt(UCHAR* data, int dataLen, EeRc4Key* key)
{
    UCHAR x = key->x, y = key->y, * state = &key->state[0], xorIndex;
    long counter;
    for (counter = 0; counter < dataLen; counter++) {
        x = (UCHAR)((x + 1) % 256);
        y = (UCHAR)((state[x] + y) % 256);
        rc4_swap(&state[x], &state[y]);
        xorIndex = (UCHAR)((state[x] + state[y]) % 256);
        data[counter] ^= state[xorIndex];
    }
    key->x = x; key->y = y;
}

// ============================================================================
// EEPROM QuickCRC (sum-with-carry, complemented) -- XKCRC::QuickCRC
// ============================================================================
void Ee_QuickCRC(UCHAR* crcOut4, const UCHAR* inData, DWORD dataLen)
{
    unsigned long high = 0, low = 0;
    DWORD i;
    for (i = 0; i < dataLen / sizeof(unsigned long); i++) {
        unsigned long val = ((const unsigned long*)inData)[i];
        unsigned __int64 sum = ((unsigned __int64)high << 32) | low;
        high = (unsigned long)((sum + val) >> 32);
        low += val;
    }
    *(unsigned long*)crcOut4 = ~(high + low);
}