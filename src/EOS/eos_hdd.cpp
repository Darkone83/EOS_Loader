// eos_hdd.cpp -- raw ATA security tools (primary master).
// RXDK / MSVC2003 / C89: declarations before statements, no CRT, no sprintf.
// SendATACommand logic + HDD password derivation ported from PrometheOS XKHDD.
#include "eos_hdd.h"
#include "eos_gfx.h"           // <xtl.h>: BYTE/WORD, Sleep
#ifndef EOS_HOST_TEST
#include "xboxinternals.h"     // KeStallExecutionProcessor, XboxHDKey
#endif

typedef unsigned long  u32;
typedef unsigned char  u8;

// IDE ports / ATA constants.
#define IDE_PRIMARY        0x01F0
#define IDE_DEV_MASTER     0x00A0
#define ATA_IDENTIFY       0xEC
#define ATA_SEC_SETPASS    0xF1
#define ATA_SEC_UNLOCK     0xF2
#define ATA_SEC_DISABLE    0xF6
#define ATA_RD             0
#define ATA_WR             1

// IDENTIFY byte offsets.
#define OFF_SERIAL   0x14
#define OFF_MODEL    0x36
#define OFF_SECWORD  0x100

// ---- raw byte/dword port I/O (multi-line __asm, as in eos_flash) -----------
#ifdef EOS_HOST_TEST
static void outB(unsigned short, u8) {}
static u8   inB(unsigned short) { return 0; }
static void outD(unsigned short, u32) {}
static u32  inD(unsigned short) { return 0; }
#else
static void outB(unsigned short port, u8 val)
{
    __asm
    {
        mov dx, port
        mov al, val
        out dx, al
    }
}
static u8 inB(unsigned short port)
{
    u8 v;
    __asm
    {
        mov dx, port
        in  al, dx
        mov v, al
    }
    return v;
}
static void outD(unsigned short port, u32 val)
{
    __asm
    {
        mov dx, port
        mov eax, val
        out dx, eax
    }
}
static u32 inD(unsigned short port)
{
    u32 v;
    __asm
    {
        mov dx, port
        in  eax, dx
        mov v, eax
    }
    return v;
}
#endif

// ===========================================================================
// SHA-1 + standard HMAC-SHA1 (self-contained; matches XKSHA1).
// ===========================================================================
#define ROL(b,w) (((w) << (b)) | ((w) >> (32 - (b))))
typedef struct { u32 h[5]; u32 lo, hi; int idx; u8 blk[64]; } SHA1;

static void shReset(SHA1* c)
{
    c->lo = 0; c->hi = 0; c->idx = 0;
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
}
static void shBlock(SHA1* c)
{
    static const u32 K[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    u32 W[80], A, B, C, D, E, t; int i;
    for (i = 0; i < 16; i++)
        W[i] = ((u32)c->blk[i * 4] << 24) | ((u32)c->blk[i * 4 + 1] << 16) | ((u32)c->blk[i * 4 + 2] << 8) | ((u32)c->blk[i * 4 + 3]);
    for (i = 16; i < 80; i++) W[i] = ROL(1, W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16]);
    A = c->h[0]; B = c->h[1]; C = c->h[2]; D = c->h[3]; E = c->h[4];
    for (i = 0; i < 20; i++) { t = ROL(5, A) + ((B & C) | ((~B) & D)) + E + W[i] + K[0]; E = D; D = C; C = ROL(30, B); B = A; A = t; }
    for (i = 20; i < 40; i++) { t = ROL(5, A) + (B ^ C ^ D) + E + W[i] + K[1]; E = D; D = C; C = ROL(30, B); B = A; A = t; }
    for (i = 40; i < 60; i++) { t = ROL(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[i] + K[2]; E = D; D = C; C = ROL(30, B); B = A; A = t; }
    for (i = 60; i < 80; i++) { t = ROL(5, A) + (B ^ C ^ D) + E + W[i] + K[3]; E = D; D = C; C = ROL(30, B); B = A; A = t; }
    c->h[0] += A; c->h[1] += B; c->h[2] += C; c->h[3] += D; c->h[4] += E; c->idx = 0;
}
static void shInput(SHA1* c, const u8* m, int len)
{
    while (len-- > 0) {
        c->blk[c->idx++] = (u8)(*m & 0xFF);
        c->lo += 8; if (c->lo == 0) c->hi++;
        if (c->idx == 64) shBlock(c);
        m++;
    }
}
static void shResult(SHA1* c, u8 out[20])
{
    int i;
    if (c->idx > 55) { c->blk[c->idx++] = 0x80; while (c->idx < 64)c->blk[c->idx++] = 0; shBlock(c); while (c->idx < 56)c->blk[c->idx++] = 0; }
    else { c->blk[c->idx++] = 0x80; while (c->idx < 56)c->blk[c->idx++] = 0; }
    c->blk[56] = (u8)(c->hi >> 24); c->blk[57] = (u8)(c->hi >> 16); c->blk[58] = (u8)(c->hi >> 8); c->blk[59] = (u8)c->hi;
    c->blk[60] = (u8)(c->lo >> 24); c->blk[61] = (u8)(c->lo >> 16); c->blk[62] = (u8)(c->lo >> 8); c->blk[63] = (u8)c->lo;
    shBlock(c);
    for (i = 0; i < 20; i++) out[i] = (u8)(c->h[i >> 2] >> (8 * (3 - (i & 3))));
}
// HMAC-SHA1 over (text1 || text2) with key (key_length bytes).
static void hmacSha1(u8* result, const u8* key, int keyLen,
    const u8* t1, int t1len, const u8* t2, int t2len)
{
    u8 s1[0x40], s2[0x40 + 0x14]; int i; SHA1 c;
    for (i = 0x40 - 1; i >= keyLen; --i) s1[i] = 0x36;
    for (; i >= 0; --i) s1[i] = (u8)(key[i] ^ 0x36);
    shReset(&c); shInput(&c, s1, 0x40);
    if (t1len) shInput(&c, t1, t1len);
    if (t2len) shInput(&c, t2, t2len);
    shResult(&c, &s2[0x40]);
    for (i = 0x40 - 1; i >= keyLen; --i) s2[i] = 0x5C;
    for (; i >= 0; --i) s2[i] = (u8)(key[i] ^ 0x5C);
    shReset(&c); shInput(&c, s2, 0x40 + 0x14); shResult(&c, result);
}

// ===========================================================================
// ATA command object + SendATACommand (ported verbatim in spirit from XKHDD).
// ===========================================================================
typedef struct {
    u8  features, sectorCount, sectorNumber, cylLow, cylHigh, driveHead, command;
} IpReg;
typedef struct {
    u8  error, sectorCount, sectorNumber, cylLow, cylHigh, driveHead, status;
} OpReg;
typedef struct {
    IpReg ip; OpReg op; u8 data[512]; u32 dataSize;
} AtaCmd;

static void zeroCmd(AtaCmd* c) { int i; u8* p = (u8*)c; for (i = 0; i < (int)sizeof(AtaCmd); i++) p[i] = 0; }

static int sendATA(unsigned short port, AtaCmd* c, int rw)
{
    u32 defWait = 30000, wait = defWait; unsigned short inv = 0, succ = 0x58, bsy = 0x80;
    u32* pData = (u32*)c->data; int i;

    outB(port + 1, c->ip.features);
    outB(port + 2, c->ip.sectorCount);
    outB(port + 3, c->ip.sectorNumber);
    outB(port + 4, c->ip.cylLow);
    outB(port + 5, c->ip.cylHigh);
    outB(port + 6, c->ip.driveHead);
    outB(port + 7, c->ip.command);
    Sleep(50);

    inv = inB(port + 7);
    while (((inv & succ) != succ) && wait > 0) { inv = inB(port + 7); KeStallExecutionProcessor(100); wait--; }

    if (wait > 0 && rw == ATA_RD) {
        c->op.error = inB(port + 1); c->op.sectorCount = inB(port + 2); c->op.sectorNumber = inB(port + 3);
        c->op.cylLow = inB(port + 4); c->op.cylHigh = inB(port + 5); c->op.driveHead = inB(port + 6); c->op.status = inB(port + 7);
        c->dataSize = 512;
        for (i = 0; i < 512; i++) c->data[i] = 0;
        for (i = 0; i < 128; i++) pData[i] = inD(port);
        wait = defWait; inv = inB(0x200 + port + 6);
        while (((inv & bsy) == bsy) && wait > 0) { inv = inB(0x200 + port + 6); KeStallExecutionProcessor(100); wait--; }
        return wait > 0;
    }
    if (wait > 0 && c->dataSize > 0 && rw == ATA_WR) {
        c->op.error = inB(port + 1); c->op.sectorCount = inB(port + 2); c->op.sectorNumber = inB(port + 3);
        c->op.cylLow = inB(port + 4); c->op.cylHigh = inB(port + 5); c->op.driveHead = inB(port + 6); c->op.status = inB(port + 7);
        for (i = 0; i < 128; i++) outD(port, pData[i]);
        wait = defWait; inv = inB(0x200 + port + 6);
        while (((inv & bsy) == bsy) && wait > 0) { inv = inB(0x200 + port + 6); KeStallExecutionProcessor(100); wait--; }
        return wait > 0;
    }
    return 0;
}

// ===========================================================================
// IDENTIFY parsing + password derivation.
// ===========================================================================
// Byte-swap ATA word pairs into dst, trim trailing spaces, return trimmed length.
static int cleanATA(u8* dst, const u8* src, int len)
{
    u8 tmp; int i;
    for (i = 0; i < len; i += 2) { tmp = src[i]; dst[i] = src[i + 1]; dst[i + 1] = tmp; }
    --dst;
    for (i = len; i > 0; --i) if (dst[i] != ' ') break;
    return i;
}

static void genPwd(const u8* hddKey, const u8* ideData, u8 pass[20])
{
    u8 serial[0x14], model[0x28]; int sLen, mLen, i;
    for (i = 0; i < 0x14; i++) serial[i] = 0;
    for (i = 0; i < 0x28; i++) model[i] = 0;
    sLen = cleanATA(serial, ideData + OFF_SERIAL, 0x14);
    mLen = cleanATA(model, ideData + OFF_MODEL, 0x28);
    hmacSha1(pass, hddKey, 0x10, model, mLen, serial, sLen);
}

static void parseIdentify(const u8* d, EosHddInfo* out)
{
    u8 tmp[0x28]; int n, i; u32 sectors;

    n = cleanATA(tmp, d + OFF_MODEL, 0x28);
    if (n > 43) n = 43;
    for (i = 0; i < n; i++) out->model[i] = (char)tmp[i]; out->model[n] = 0;

    n = cleanATA(tmp, d + OFF_SERIAL, 0x14);
    if (n > 23) n = 23;
    for (i = 0; i < n; i++) out->serial[i] = (char)tmp[i]; out->serial[n] = 0;

    out->security = (unsigned short)(d[OFF_SECWORD] | (d[OFF_SECWORD + 1] << 8));

    // LBA28 total sectors = words 60..61 (bytes 0x78..0x7B), little-endian.
    sectors = (u32)d[0x78] | ((u32)d[0x79] << 8) | ((u32)d[0x7A] << 16) | ((u32)d[0x7B] << 24);
    // LBA48 (words 100..103) only if word 83 bit 10 says the drive supports it.
    if ((d[0xA6] | (d[0xA7] << 8)) & 0x0400) {
        u32 s48 = (u32)d[0xC8] | ((u32)d[0xC9] << 8) | ((u32)d[0xCA] << 16) | ((u32)d[0xCB] << 24);
        if (s48 > sectors) sectors = s48;
    }
    out->sizeMB = sectors / 2048;   // 512-byte sectors -> MB
}

// ===========================================================================
// Public API.
// ===========================================================================
static int identifyInto(AtaCmd* cmd)
{
    zeroCmd(cmd);
    cmd->dataSize = 0;
    cmd->ip.driveHead = IDE_DEV_MASTER;
    cmd->ip.command = ATA_IDENTIFY;
    return sendATA(IDE_PRIMARY, cmd, ATA_RD);
}

int Hdd_Identify(EosHddInfo* out)
{
    AtaCmd cmd;
    out->present = 0;
    if (!identifyInto(&cmd)) return HDD_ERR_NODISK;
    parseIdentify(cmd.data, out);
    out->present = 1;
    return HDD_OK;
}

int Hdd_Unlock(void)
{
    AtaCmd cmd; u8 pass[20]; unsigned short sec;

    if (!identifyInto(&cmd)) return HDD_ERR_NODISK;
    genPwd(XboxHDKey, cmd.data, pass);
    sec = (unsigned short)(cmd.data[OFF_SECWORD] | (cmd.data[OFF_SECWORD + 1] << 8));
    if (!(sec & HDD_SEC_SUPPORTED)) return HDD_ERR_UNSUPP;
    if (!(sec & HDD_SEC_ENABLED))   return HDD_OK;       // already unlocked

    // UNLOCK (user password)
    zeroCmd(&cmd); cmd.dataSize = 512; cmd.ip.driveHead = IDE_DEV_MASTER; cmd.ip.command = ATA_SEC_UNLOCK;
    { int i; for (i = 0; i < 20; i++) cmd.data[2 + i] = pass[i]; }
    sendATA(IDE_PRIMARY, &cmd, ATA_WR);

    // DISABLE (remove security)
    zeroCmd(&cmd); cmd.dataSize = 512; cmd.ip.driveHead = IDE_DEV_MASTER; cmd.ip.command = ATA_SEC_DISABLE;
    { int i; for (i = 0; i < 20; i++) cmd.data[2 + i] = pass[i]; }
    sendATA(IDE_PRIMARY, &cmd, ATA_WR);

    // verify
    if (!identifyInto(&cmd)) return HDD_ERR_ATA;
    sec = (unsigned short)(cmd.data[OFF_SECWORD] | (cmd.data[OFF_SECWORD + 1] << 8));
    return (sec & HDD_SEC_ENABLED) ? HDD_ERR_ATA : HDD_OK;
}

int Hdd_Lock(void)
{
    AtaCmd cmd; u8 pass[20]; unsigned short sec; int i;
    static const char master[12] = { 'T','E','A','M','A','S','S','E','M','B','L','Y' };

    if (!identifyInto(&cmd)) return HDD_ERR_NODISK;
    genPwd(XboxHDKey, cmd.data, pass);
    sec = (unsigned short)(cmd.data[OFF_SECWORD] | (cmd.data[OFF_SECWORD + 1] << 8));
    if (!(sec & HDD_SEC_SUPPORTED)) return HDD_ERR_UNSUPP;
    if (sec & HDD_SEC_ENABLED)      return HDD_ERR_STATE; // already locked

    // SET PASSWORD -- master (identifier word = 0x0001), recovery password
    zeroCmd(&cmd); cmd.dataSize = 512; cmd.ip.driveHead = IDE_DEV_MASTER; cmd.ip.command = ATA_SEC_SETPASS;
    *(u32*)cmd.data = 0x0001;
    for (i = 0; i < 12; i++) cmd.data[2 + i] = (u8)master[i];
    sendATA(IDE_PRIMARY, &cmd, ATA_WR);

    // SET PASSWORD -- user (identifier word = 0x0000), console-derived password
    zeroCmd(&cmd); cmd.dataSize = 512; cmd.ip.driveHead = IDE_DEV_MASTER; cmd.ip.command = ATA_SEC_SETPASS;
    for (i = 0; i < 20; i++) cmd.data[2 + i] = pass[i];
    sendATA(IDE_PRIMARY, &cmd, ATA_WR);

    // verify
    if (!identifyInto(&cmd)) return HDD_ERR_ATA;
    sec = (unsigned short)(cmd.data[OFF_SECWORD] | (cmd.data[OFF_SECWORD + 1] << 8));
    return (sec & HDD_SEC_ENABLED) ? HDD_OK : HDD_ERR_ATA;
}