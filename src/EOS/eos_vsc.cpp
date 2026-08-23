// eos_vsc.cpp -- vendor-specific HDD credential recovery for EOS Loader.
//
// The WD and Seagate service-mode flows in this file are adapted closely from
// PrometheOS hddVscUnlocker (Team Resurgent and contributors). PrometheOS is the
// behavioral source of truth for the command sequences, register signatures,
// timing, password offsets, and model dispatch used here.
//
// EOS-specific changes:
//   * primary-master only (the Xbox HDD path EOS actually uses)
//   * fixed buffers / no heap-backed class state
//   * compact C-style API for eos_hdd.cpp fallback integration
//   * recovered credentials are persisted to E:\\Eos\\unlock.txt

#include "eos_vsc.h"
#include "eos_gfx.h"       // <xtl.h>
#include "xboxinternals.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

// RXDK does not expose the desktop CRT _inp/_outp/_inpd/_outpd helpers used by
// PrometheOS. Keep the exact same port transactions, but issue them through the
// MSVC inline-assembler form already proven by eos_hdd.cpp/eos_flash.cpp.
#ifdef EOS_HOST_TEST
static void ioOutB(unsigned short, u8) {}
static u8   ioInB(unsigned short) { return 0; }
static void ioOutD(unsigned short, u32) {}
static u32  ioInD(unsigned short) { return 0; }
#else
static void ioOutB(unsigned short port, u8 val)
{
    __asm
    {
        mov dx, port
        mov al, val
        out dx, al
    }
}
static u8 ioInB(unsigned short port)
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
static void ioOutD(unsigned short port, u32 val)
{
    __asm
    {
        mov dx, port
        mov eax, val
        out dx, eax
    }
}
static u32 ioInD(unsigned short port)
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

#define IDE_PRIMARY      0x01F0
#define IDE_ALT_STATUS   (0x0200 + IDE_PRIMARY + 6)
#define IDE_MASTER       0xA0

#define ATA_IDENTIFY     0xEC
#define ATA_SEC_UNLOCK   0xF2
#define ATA_SEC_DISABLE  0xF6

#define SEC_SUPPORTED    0x0001
#define SEC_ENABLED      0x0002
#define SEC_LOCKED       0x0004
#define SEC_FROZEN       0x0008

#define WD_MASTER_OFF    0x398
#define WD_USER_OFF      0x3B8

#define VSC_WAIT_DEFAULT 2000
#define VSC_SPIN_DEFAULT 10000

// ATA IDENTIFY offsets.
#define ID_SERIAL_OFF    0x14
#define ID_FW_OFF        0x2E
#define ID_MODEL_OFF     0x36
#define ID_SEC_OFF       0x100

typedef struct IdeCmd {
    u8 feat;
    u8 nSect;
    u8 sLow;
    u8 sMed;
    u8 sHigh;
    u8 drv;
    u8 cmd;
} IdeCmd;

typedef struct VscState {
    EosVscResult r;
    u16 security;
} VscState;

static void memZero(void* p, int n)
{
    u8* b = (u8*)p;
    int i;
    for (i = 0; i < n; ++i) b[i] = 0;
}

static void memCopy(void* d, const void* s, int n)
{
    u8* dd = (u8*)d;
    const u8* ss = (const u8*)s;
    int i;
    for (i = 0; i < n; ++i) dd[i] = ss[i];
}

static int strLen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int strPrefix(const char* s, const char* pfx)
{
    int i = 0;
    while (pfx[i]) {
        if (s[i] != pfx[i]) return 0;
        ++i;
    }
    return 1;
}

static int memEq(const u8* a, const char* b, int n)
{
    int i;
    for (i = 0; i < n; ++i) if (a[i] != (u8)b[i]) return 0;
    return 1;
}

static void rawCmd(IdeCmd c)
{
    ioOutB(IDE_PRIMARY + 1, c.feat);
    ioOutB(IDE_PRIMARY + 2, c.nSect);
    ioOutB(IDE_PRIMARY + 3, c.sLow);
    ioOutB(IDE_PRIMARY + 4, c.sMed);
    ioOutB(IDE_PRIMARY + 5, c.sHigh);
    ioOutB(IDE_PRIMARY + 6, c.drv & ~0x10);  // primary master
    ioOutB(IDE_PRIMARY + 7, c.cmd);
}

static int waitBusy(int alt, u32 timeoutMs)
{
    u32 ticks;
    u16 reg;
    u8 st;

    // PrometheOS scales milliseconds into 100us polls (x10).
    ticks = timeoutMs * 10;
    reg = alt ? IDE_ALT_STATUS : (IDE_PRIMARY + 7);
    do {
        st = (u8)ioInB(reg);
        if ((st & 0x80) == 0) return 1;
        KeStallExecutionProcessor(100);
    } while (--ticks);
    return 0;
}

static int spinBusy(int alt, u32 ticks)
{
    u16 reg;
    u8 st;
    reg = alt ? IDE_ALT_STATUS : (IDE_PRIMARY + 7);
    do {
        st = (u8)ioInB(reg);
        if ((st & 0x80) == 0) return 1;
        KeStallExecutionProcessor(100);
    } while (--ticks);
    return 0;
}

static int sendCmd(IdeCmd c, int alt, u32 timeoutMs)
{
    if (!waitBusy(alt, timeoutMs)) return 0;
    rawCmd(c);
    if (!waitBusy(alt, timeoutMs)) return 0;
    return 1;
}

static int readData(u8* buf, u32 size)
{
    u32 count;
    u32* d;
    u32 i;

    if ((size & 3) != 0) return 0;
    if (!waitBusy(1, VSC_WAIT_DEFAULT)) return 0;
    count = size / 4;
    d = (u32*)buf;
    for (i = 0; i < count; ++i) {
        d[i] = (u32)ioInD(IDE_PRIMARY);
        KeStallExecutionProcessor(500);
    }
    return waitBusy(0, VSC_WAIT_DEFAULT);
}

static int writeData(const u8* buf, u32 size)
{
    u32 count;
    const u32* d;
    u32 i;

    if ((size & 3) != 0) return 0;
    if (!waitBusy(1, VSC_WAIT_DEFAULT)) return 0;
    count = size / 4;
    d = (const u32*)buf;
    for (i = 0; i < count; ++i) {
        ioOutD(IDE_PRIMARY, d[i]);
        KeStallExecutionProcessor(500);
    }
    return waitBusy(0, VSC_WAIT_DEFAULT);
}

static void resetIdeRaw(void)
{
    ioOutB(IDE_ALT_STATUS, 0);
    KeStallExecutionProcessor(50);
    ioOutB(IDE_ALT_STATUS, 4);
    KeStallExecutionProcessor(100);
    ioOutB(IDE_ALT_STATUS, 0);
    KeStallExecutionProcessor(50);
}

static int resetIde(void)
{
    if (!waitBusy(1, VSC_WAIT_DEFAULT)) return 0;
    resetIdeRaw();
    return waitBusy(0, VSC_WAIT_DEFAULT);
}

static int cleanAtaText(char* dst, int dstCap, const u8* src, int len)
{
    int i, n;
    if (dstCap <= 0) return 0;
    n = (len < dstCap - 1) ? len : dstCap - 1;
    for (i = 0; i < n; i += 2) {
        if (i + 1 < n) {
            dst[i] = (char)src[i + 1];
            dst[i + 1] = (char)src[i];
        }
        else dst[i] = (char)src[i];
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) --n;
    dst[n] = 0;
    return n;
}

static int identify(VscState* s)
{
    IdeCmd c;
    u8 id[512];

    c.feat = 0; c.nSect = 0; c.sLow = 0; c.sMed = 0; c.sHigh = 0;
    c.drv = IDE_MASTER; c.cmd = ATA_IDENTIFY;
    if (!sendCmd(c, 0, VSC_WAIT_DEFAULT)) return 0;
    if (!readData(id, sizeof(id))) return 0;

    cleanAtaText(s->r.serial, sizeof(s->r.serial), id + ID_SERIAL_OFF, 20);
    cleanAtaText(s->r.firmware, sizeof(s->r.firmware), id + ID_FW_OFF, 8);
    cleanAtaText(s->r.model, sizeof(s->r.model), id + ID_MODEL_OFF, 40);
    s->security = (u16)(id[ID_SEC_OFF] | ((u16)id[ID_SEC_OFF + 1] << 8));
    return 1;
}

static int sendSecurity(const u8 pwd[EOS_VSC_PWD_SIZE], int disable)
{
    IdeCmd c;
    u8 buf[512];

    c.feat = 0; c.nSect = 0; c.sLow = 0; c.sMed = 0; c.sHigh = 0;
    c.drv = IDE_MASTER; c.cmd = disable ? ATA_SEC_DISABLE : ATA_SEC_UNLOCK;
    if (!sendCmd(c, 0, VSC_WAIT_DEFAULT)) return 0;

    memZero(buf, sizeof(buf));
    // byte 0 == 0 selects USER password, matching PrometheOS doAtaSecUnlock().
    memCopy(buf + 2, pwd, EOS_VSC_PWD_SIZE);
    return writeData(buf, sizeof(buf));
}

static int finishAtaUnlock(VscState* s)
{
    if (!sendSecurity(s->r.userPassword, 0)) return 0;
    if (!identify(s)) return 0;
    if (s->security & SEC_LOCKED) return 0;

    if (!sendSecurity(s->r.userPassword, 1)) return 0;
    if (!identify(s)) return 0;
    if (s->security & SEC_ENABLED) return 0;

    s->r.unlocked = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// Western Digital legacy / Mod42 path.
// ---------------------------------------------------------------------------
static int wdEnableService(void)
{
    IdeCmd c;
    c.feat = 0x57; c.nSect = 0x44; c.sLow = 0x43; c.sMed = 0;
    c.sHigh = 0; c.drv = 0xA0; c.cmd = 0x8A;
    return sendCmd(c, 0, VSC_WAIT_DEFAULT);
}

static int wdReadSecuritySectors(u8 n)
{
    IdeCmd c;
    c.feat = 0; c.nSect = n; c.sLow = 0; c.sMed = 0;
    c.sHigh = 0x0F; c.drv = 0xE0; c.cmd = 0x21;
    return sendCmd(c, 0, VSC_WAIT_DEFAULT);
}

static void wdExtractLegacy(VscState* s, const u8* buf)
{
    memCopy(s->r.masterPassword, buf + WD_MASTER_OFF, EOS_VSC_PWD_SIZE);
    memCopy(s->r.userPassword, buf + WD_USER_OFF, EOS_VSC_PWD_SIZE);
}

static int wdLegacyUnlock(VscState* s)
{
    u8 buf[8192];
    u8 nSectors;
    u32 nBytes;

    memZero(buf, sizeof(buf));
    if (!wdEnableService()) return 0;
    if (!wdReadSecuritySectors(1)) return 0;
    if (!readData(buf, 512)) return 0;

    nSectors = (u8)((u16*)buf)[11];
    nBytes = (u32)nSectors * 512;
    if (nSectors == 0 || nBytes > sizeof(buf)) return 0;

    if (!wdReadSecuritySectors(nSectors)) return 0;
    if (!readData(buf, nBytes)) return 0;
    if (!resetIde()) return 0;

    wdExtractLegacy(s, buf);
    return finishAtaUnlock(s);
}

// ---------------------------------------------------------------------------
// Western Digital ROYL path.
// ---------------------------------------------------------------------------
static int wdRoylMode(int disable)
{
    IdeCmd c;
    c.feat = disable ? 0x44 : 0x45;
    c.nSect = 0x0B; c.sLow = 0x00; c.sMed = 0x44; c.sHigh = 0x57;
    c.drv = 0xA0; c.cmd = 0x80;
    return sendCmd(c, 0, VSC_WAIT_DEFAULT);
}

static int wdRoylSeek02(void)
{
    IdeCmd c;
    u8 buf[512];
    c.feat = 0xD6; c.nSect = 0x01; c.sLow = 0xBE; c.sMed = 0x4F;
    c.sHigh = 0xC2; c.drv = 0xA0; c.cmd = 0xB0;
    if (!sendCmd(c, 0, VSC_WAIT_DEFAULT)) return 0;
    memZero(buf, sizeof(buf));
    buf[0] = 0x0B; buf[1] = 0x00; buf[2] = 0x04; buf[3] = 0x00; buf[4] = 0x02;
    return writeData(buf, sizeof(buf));
}

static int wdRoylRead02(u8 n)
{
    IdeCmd c;
    c.feat = 0xD5; c.nSect = n; c.sLow = 0xBF; c.sMed = 0x4F;
    c.sHigh = 0xC2; c.drv = 0xA0; c.cmd = 0xB0;
    return sendCmd(c, 0, VSC_WAIT_DEFAULT);
}

static int wdRoylExtract(VscState* s, const u8* buf, u32 passIdx, u32 bufSize)
{
    u16 off;
    off = ((const u16*)buf)[passIdx];
    if ((u32)off + 4 + (EOS_VSC_PWD_SIZE * 2) > bufSize) return 0;
    memCopy(s->r.userPassword, buf + off + 4, EOS_VSC_PWD_SIZE);
    memCopy(s->r.masterPassword, buf + off + 4 + EOS_VSC_PWD_SIZE, EOS_VSC_PWD_SIZE);
    return 1;
}

static int wdRoylUnlock(VscState* s)
{
    u8 buf[8192];
    u32 sectIdx, passIdx, nBytes, totalBytes;
    u8 nSectors;

    memZero(buf, sizeof(buf));
    sectIdx = 5;
    passIdx = 0x3D;

    if (!wdRoylMode(0)) return 0;
    if (!wdRoylSeek02()) return 0;
    if (!wdRoylRead02(1)) return 0;
    if (!readData(buf, 512)) return 0;

    if (!memEq(buf, "ROYL", 4)) {
        // PrometheOS handles 2005-ish transitional WD layouts here.
        if (buf[0x8C] == 'W' && buf[0x8D] == 'D' && buf[0x8E] == '-') {
            sectIdx = 11;
            passIdx = 0x31;
        }
        else {
            // Not ROYL: fall back to the legacy Mod42 path exactly as PrometheOS.
            return wdLegacyUnlock(s);
        }
    }

    nSectors = (u8)(((u16*)buf)[sectIdx] - 1);
    nBytes = (u32)nSectors * 512;
    totalBytes = nBytes + 512;
    if (nSectors == 0 || totalBytes > sizeof(buf)) return 0;

    if (!wdRoylRead02(nSectors)) return 0;
    if (!readData(buf + 512, nBytes)) return 0;
    if (!wdRoylMode(1)) return 0;
    if (!waitBusy(0, VSC_WAIT_DEFAULT)) return 0;
    if (!wdRoylExtract(s, buf, passIdx, totalBytes)) return 0;
    return finishAtaUnlock(s);
}

// ---------------------------------------------------------------------------
// Seagate diagnostic-terminal path.
// ---------------------------------------------------------------------------
static int sgEnableDiag(void)
{
    IdeCmd c;
    c.feat = 0; c.nSect = 0x44; c.sLow = 0x69; c.sMed = 0x61;
    c.sHigh = 0x47; c.drv = 0xE0; c.cmd = 0xFE;   // ".DiaG.."
    rawCmd(c);
    KeStallExecutionProcessor(10000);
    return spinBusy(0, VSC_SPIN_DEFAULT);
}

static int sgSendByte(u8 b)
{
    IdeCmd c;
    c.feat = 0; c.nSect = 1; c.sLow = 0; c.sMed = 0;
    c.sHigh = b; c.drv = 0xE0; c.cmd = 0xFE;
    rawCmd(c);
    KeStallExecutionProcessor(10000);
    return spinBusy(1, VSC_SPIN_DEFAULT);
}

static int sgSend(const char* text)
{
    u8 st;
    while (*text) {
        if (!sgSendByte((u8)*text)) return 0;
        st = (u8)ioInB(IDE_PRIMARY + 7);
        if (st & 1) return 0;
        KeStallExecutionProcessor(10000);
        ++text;
    }
    KeStallExecutionProcessor(10000);
    return 1;
}

static int sgReadReady(u8* status, u32 timeout)
{
    do {
        *status = (u8)ioInB(IDE_ALT_STATUS);
        if (((*status & 0x80) == 0) && ((*status & 0x40) != 0)) return 1;
        KeStallExecutionProcessor(100);
    } while (--timeout);
    return 0;
}

static int sgRead(char* buf, u32 cap, u32 expLen, u32 timeout)
{
    IdeCmd c;
    u8 status, flags;
    u32 retry, out;
    int hasExp;

    c.feat = 0; c.nSect = 1; c.sLow = 0; c.sMed = 0;
    c.sHigh = 0; c.drv = 0xE0; c.cmd = 0xFF;
    retry = 0;
    out = 0;
    hasExp = expLen ? 1 : 0;

    for (;;) {
        if (retry >= 10) break;
        if (hasExp && expLen > 0) {
            --expLen;
            if (expLen == 0) timeout = 1000;
        }

        rawCmd(c);
        KeStallExecutionProcessor(1000);
        if (!sgReadReady(&status, timeout)) {
            if (out < cap - 1) buf[out++] = (char)ioInB(IDE_PRIMARY + 4);
            break;
        }

        if ((status & 1) == 0) {
            if (out < cap - 1) buf[out++] = (char)ioInB(IDE_PRIMARY + 4);
        }
        else {
            flags = (u8)ioInB(IDE_PRIMARY + 2);
            if ((flags & 1) == 0) break;
            ++retry;
        }
        if (out >= cap - 1) break;
    }

    buf[out] = 0;
    KeStallExecutionProcessor(10000);
    return (out > 0) ? 1 : 0;
}

static int sgExec(const char* cmd, char* out, u32 outCap, u32 expLen, u32 timeout)
{
    char discard[128];
    if (!sgSend(cmd)) return 0;
    sgRead(discard, sizeof(discard), (u32)strLen(cmd), timeout);
    if (!sgSend("\r")) return 0;
    return sgRead(out, outCap, expLen, timeout);
}

static int hexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int sgExtractHex(const char* buf, u32 bufLen, u32 offset, u32 size, u8 out[EOS_VSC_PWD_SIZE])
{
    u32 i, end;
    int n, hi, lo;
    memZero(out, EOS_VSC_PWD_SIZE);
    if (offset >= bufLen) return 0;
    end = offset + size;
    if (end > bufLen) end = bufLen;
    n = 0;
    i = offset;
    while (i + 1 < end && n < EOS_VSC_PWD_SIZE) {
        if (buf[i] == ' ' || buf[i] == '\r') { ++i; continue; }
        if (buf[i] == '\n') { i += 8; continue; }
        hi = hexNibble(buf[i]); lo = hexNibble(buf[i + 1]);
        if (hi >= 0 && lo >= 0) {
            out[n++] = (u8)((hi << 4) | lo);
            i += 2;
        }
        else ++i;
    }
    return n == EOS_VSC_PWD_SIZE;
}

static int sgExtract(VscState* s, const char* buf, u32 len, u32 masterOff, u32 userOff)
{
    if (!sgExtractHex(buf, len, masterOff, 0x50, s->r.masterPassword)) return 0;
    if (!sgExtractHex(buf, len, userOff, 0x50, s->r.userPassword)) return 0;
    return 1;
}

static int sgUnlock310014A(VscState* s)
{
    char discard[512];
    char buffer[2048];
    u8 irql, oldIrql;
    int ok;

    memZero(discard, sizeof(discard));
    memZero(buffer, sizeof(buffer));
    ok = 1;

    HalGetInterruptVector(0x0E, &irql);
    oldIrql = KfRaiseIrql(irql);

    if (!sgEnableDiag()) ok = 0;
    if (ok && !sgSend("\x1A")) ok = 0;
    if (ok) sgRead(discard, sizeof(discard), 20, 10000);
    if (ok && !sgExec("/2", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("S006b", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("R20,01", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("C0,570", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("B570", buffer, sizeof(buffer), 0x5EA, 10000)) ok = 0;
    if (ok && !sgSend("\x03")) ok = 0;
    if (ok) sgRead(discard, sizeof(discard), 0x29, 30000);

    KeStallExecutionProcessor(100000);
    resetIdeRaw();
    KfLowerIrql(oldIrql);

    if (!ok) return 0;
    if (!sgExtract(s, buffer, sizeof(buffer), 0xA8, 0xF8)) return 0;
    if (!waitBusy(1, VSC_WAIT_DEFAULT)) return 0;
    return finishAtaUnlock(s);
}

static int sgUnlock310211A(VscState* s)
{
    char discard[512];
    char buffer[2048];
    u8 irql, oldIrql;
    int ok;

    memZero(discard, sizeof(discard));
    memZero(buffer, sizeof(buffer));
    ok = 1;

    HalGetInterruptVector(0x0E, &irql);
    oldIrql = KfRaiseIrql(irql);

    if (!sgEnableDiag()) ok = 0;
    if (ok && !sgSend("\x1A")) ok = 0;
    if (ok) sgRead(discard, sizeof(discard), 12, 10000);
    if (ok && !sgExec("GFFFFFFF3", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("/2", discard, sizeof(discard), 4, 10000)) ok = 0;
    if (ok && !sgExec("B0,0", buffer, sizeof(buffer), 0x5E2, 10000)) ok = 0;
    if (ok && !sgSend("\x03")) ok = 0;
    if (ok) sgRead(discard, sizeof(discard), 0x91, 30000);

    KeStallExecutionProcessor(100000);
    resetIdeRaw();
    KfLowerIrql(oldIrql);

    if (!ok) return 0;
    if (!sgExtract(s, buffer, sizeof(buffer), 0xA0, 0xF0)) return 0;
    if (!waitBusy(1, VSC_WAIT_DEFAULT)) return 0;
    return finishAtaUnlock(s);
}

int Vsc_TryUnlock(EosVscResult* out)
{
    VscState s;
    int ok;

    if (!out) return EOS_VSC_IO;
    memZero(&s, sizeof(s));
    memZero(out, sizeof(*out));

    if (!identify(&s)) return EOS_VSC_IO;
    if (!(s.security & SEC_SUPPORTED) || !(s.security & SEC_ENABLED)) return EOS_VSC_UNSUPPORTED;

    ok = 0;
    if (strPrefix(s.r.model, "WDC WD80EB")) {
        s.r.supported = 1;
        ok = wdLegacyUnlock(&s);
    }
    else if (strPrefix(s.r.model, "ST310014A")) {
        s.r.supported = 1;
        ok = sgUnlock310014A(&s);
    }
    else if (strPrefix(s.r.model, "ST310211A")) {
        s.r.supported = 1;
        ok = sgUnlock310211A(&s);
    }
    else if (strPrefix(s.r.model, "WDC ")) {
        s.r.supported = 1;
        ok = wdRoylUnlock(&s);
    }
    else {
        return EOS_VSC_UNSUPPORTED;
    }

    *out = s.r;
    if (!ok) return EOS_VSC_UNLOCK;
    return EOS_VSC_OK;
}

static int appendText(char* out, int cap, int p, const char* s)
{
    while (*s && p < cap - 1) out[p++] = *s++;
    out[p] = 0;
    return p;
}

static int appendHex(char* out, int cap, int p, const u8* data, int n)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < n && p < cap - 4; ++i) {
        out[p++] = hx[(data[i] >> 4) & 0xF];
        out[p++] = hx[data[i] & 0xF];
        if (i != n - 1) out[p++] = ' ';
    }
    out[p] = 0;
    return p;
}

int Vsc_SaveCredentials(const EosVscResult* r)
{
    HANDLE h;
    DWORD wrote;
    char text[1024];
    int p;

    if (!r || !r->unlocked) return EOS_VSC_SAVE;
    CreateDirectoryA("E:\\Eos", NULL);     // succeeds or already exists

    p = 0;
    p = appendText(text, sizeof(text), p, "EOS HDD VSC Recovery\r\n\r\nModel: ");
    p = appendText(text, sizeof(text), p, r->model);
    p = appendText(text, sizeof(text), p, "\r\nSerial: ");
    p = appendText(text, sizeof(text), p, r->serial);
    p = appendText(text, sizeof(text), p, "\r\nFirmware: ");
    p = appendText(text, sizeof(text), p, r->firmware);
    p = appendText(text, sizeof(text), p, "\r\n\r\nMaster Password (32-byte hex):\r\n");
    p = appendHex(text, sizeof(text), p, r->masterPassword, EOS_VSC_PWD_SIZE);
    p = appendText(text, sizeof(text), p, "\r\n\r\nUser Password (32-byte hex):\r\n");
    p = appendHex(text, sizeof(text), p, r->userPassword, EOS_VSC_PWD_SIZE);
    p = appendText(text, sizeof(text), p, "\r\n");

    h = CreateFileA("E:\\Eos\\unlock.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return EOS_VSC_SAVE;
    wrote = 0;
    if (!WriteFile(h, text, (DWORD)p, &wrote, NULL) || wrote != (DWORD)p) {
        CloseHandle(h);
        return EOS_VSC_SAVE;
    }
    CloseHandle(h);
    return EOS_VSC_OK;
}