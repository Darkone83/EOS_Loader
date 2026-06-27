// eos_eeprom_io.cpp -- full EEPROM image read/write + HDD backup.
// RXDK / MSVC2003 / C89: declarations before statements, no CRT, no sprintf.
#include "eos_eeprom_io.h"
#include "eos_gfx.h"        // pulls <xtl.h> for CreateFileA/WriteFile/etc.

// Kernel non-volatile setting access. Index 0xFFFF = the whole 256-byte image.
extern "C" ULONG __stdcall ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD* Type,
    PVOID Value, ULONG ValueLength,
    ULONG* ResultLength);
extern "C" ULONG __stdcall ExSaveNonVolatileSetting(DWORD ValueIndex, DWORD Type,
    PVOID Value, ULONG ValueLength);

#define EE_FULL_INDEX 0xFFFF

// SerialNumber lives at offset 0x34, 12 bytes (used for the backup filename).
#define EE_SERIAL_OFF 0x34
#define EE_SERIAL_LEN 12

int Eeprom_ReadImage(unsigned char* img256)
{
    DWORD type = 0; ULONG got = 0, st; int i;
    for (i = 0; i < EOS_EEPROM_SIZE; ++i) img256[i] = 0;
    st = ExQueryNonVolatileSetting(EE_FULL_INDEX, &type, img256, EOS_EEPROM_SIZE, &got);
    if (st != 0 || got != EOS_EEPROM_SIZE) return EOS_EE_ERR_READ;
    return EOS_EE_OK;
}

int Eeprom_WriteImage(const unsigned char* img256)
{
    DWORD type = 0; ULONG got = 0, st; unsigned char cur[EOS_EEPROM_SIZE];
    if (Eeprom_ImageValid(img256) != EOS_EE_OK) return EOS_EE_ERR_IMAGE;
    // Capture the exact REG type the kernel uses for the 0xFFFF slot by reading
    // first, then write the image back with that same type (matches PrometheOS).
    ExQueryNonVolatileSetting(EE_FULL_INDEX, &type, cur, EOS_EEPROM_SIZE, &got);
    st = ExSaveNonVolatileSetting(EE_FULL_INDEX, type, (PVOID)img256, EOS_EEPROM_SIZE);
    if (st != 0) return EOS_EE_ERR_WRITE;
    return EOS_EE_OK;
}

int Eeprom_ImageValid(const unsigned char* img256)
{
    int i, allZero = 1, allFF = 1;
    for (i = 0; i < EOS_EEPROM_SIZE; ++i) {
        if (img256[i] != 0x00) allZero = 0;
        if (img256[i] != 0xFF) allFF = 0;
    }
    if (allZero || allFF) return EOS_EE_ERR_IMAGE;
    return EOS_EE_OK;
}

// --- small no-CRT string helpers --------------------------------------------
static int strLen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

static char* appendStr(char* p, const char* s)
{
    while (*s) *p++ = *s++;
    return p;
}

// append a serial, replacing chars that are illegal in FATX names with '_'.
static char* appendSerial(char* p, const unsigned char* img)
{
    int i; unsigned char c;
    for (i = 0; i < EE_SERIAL_LEN; ++i) {
        c = img[EE_SERIAL_OFF + i];
        if (c == 0) break;
        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ' ')
            c = '_';
        *p++ = (char)c;
    }
    return p;
}

static char* append2(char* p, int n)   // two-digit zero-padded
{
    *p++ = (char)('0' + (n / 10) % 10);
    *p++ = (char)('0' + n % 10);
    return p;
}

static int fileExists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return (a != 0xFFFFFFFF);
}

static int writeFile(const char* path, const unsigned char* data, int len)
{
    HANDLE h; DWORD wrote = 0; BOOL ok;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return EOS_EE_ERR_FILE;
    ok = WriteFile(h, data, (DWORD)len, &wrote, NULL);
    CloseHandle(h);
    if (!ok || wrote != (DWORD)len) return EOS_EE_ERR_FILE;
    return EOS_EE_OK;
}

int Eeprom_BackupToHdd(char* outPath, int outPathLen)
{
    unsigned char img[EOS_EEPROM_SIZE];
    char path[128]; char* p; int rc, n;

    rc = Eeprom_ReadImage(img);
    if (rc != EOS_EE_OK) return rc;

    // Ensure E:\Eos\Backups exists (ignore "already exists").
    CreateDirectoryA("E:\\Eos", NULL);
    CreateDirectoryA("E:\\Eos\\Backups", NULL);

    // Find the first free eeprom_<serial>_NN.bin (NN = 00..99).
    for (n = 0; n < 100; ++n) {
        p = path;
        p = appendStr(p, "E:\\Eos\\Backups\\eeprom_");
        p = appendSerial(p, img);
        *p++ = '_';
        p = append2(p, n);
        p = appendStr(p, ".bin");
        *p = 0;
        if (!fileExists(path)) break;
    }
    if (n >= 100) return EOS_EE_ERR_FILE;

    rc = writeFile(path, img, EOS_EEPROM_SIZE);
    if (rc != EOS_EE_OK) return rc;

    // hand the path back to the caller (truncate safely)
    if (outPath && outPathLen > 0) {
        int i, L = strLen(path);
        if (L > outPathLen - 1) L = outPathLen - 1;
        for (i = 0; i < L; ++i) outPath[i] = path[i];
        outPath[L] = 0;
    }
    return EOS_EE_OK;
}

int Eeprom_LoadBin(const char* path, unsigned char* img256)
{
    HANDLE h; DWORD got = 0; BOOL ok; DWORD sz;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return EOS_EE_ERR_FILE;
    sz = GetFileSize(h, NULL);
    if (sz != EOS_EEPROM_SIZE) { CloseHandle(h); return EOS_EE_ERR_SIZE; }
    ok = ReadFile(h, img256, EOS_EEPROM_SIZE, &got, NULL);
    CloseHandle(h);
    if (!ok || got != EOS_EEPROM_SIZE) return EOS_EE_ERR_FILE;
    return EOS_EE_OK;
}

// Enumerate up to maxN backup filenames (basename only) from E:\Eos\Backups.
int Eeprom_ListBackups(char names[][64], int maxN)
{
    WIN32_FIND_DATA fd; HANDLE h; int n = 0, i;
    h = FindFirstFileA("E:\\Eos\\Backups\\eeprom_*.bin", &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n >= maxN) break;
        for (i = 0; i < 63 && fd.cFileName[i]; ++i) names[n][i] = fd.cFileName[i];
        names[n][i] = 0;
        ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// Pull the printable 12-char serial out of an image (offset 0x34) into out13.
void Eeprom_ImageSerial(const unsigned char* img256, char* out13)
{
    int i; unsigned char c;
    for (i = 0; i < EE_SERIAL_LEN; ++i) {
        c = img256[EE_SERIAL_OFF + i];
        out13[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out13[EE_SERIAL_LEN] = 0;
}


// --- video standard (factory region, Checksum2-protected) -------------------
#define EE_VSTD_OFF    0x58
#define EE_CKSUM2_OFF  0x30
#define EE_FACTORY_OFF 0x34
#define EE_FACTORY_LEN 0x2C   /* 44 bytes 0x34..0x5F covered by Checksum2 */

// PrometheOS QuickCRC (carry-folding sum), 64-bit math avoided for no-CRT.
static void quickCRC(unsigned char* crc4, const unsigned char* data, int len)
{
    unsigned long high = 0, low = 0, val, newlow; int i, n = len / 4;
    for (i = 0; i < n; ++i) {
        val = ((const unsigned long*)data)[i];
        newlow = low + val;
        if (newlow < low) high += 1;          /* 32-bit carry into high */
        low = newlow;
    }
    *(unsigned long*)crc4 = ~(high + low);
}

unsigned int Eeprom_GetVideoStandardRaw(void)
{
    unsigned char img[EOS_EEPROM_SIZE];
    if (Eeprom_ReadImage(img) != EOS_EE_OK) return 0;
    return (unsigned int)img[EE_VSTD_OFF]
        | ((unsigned int)img[EE_VSTD_OFF + 1] << 8)
        | ((unsigned int)img[EE_VSTD_OFF + 2] << 16)
        | ((unsigned int)img[EE_VSTD_OFF + 3] << 24);
}

int Eeprom_SetVideoStandard(unsigned int rawVal, char* backupOut, int backupLen)
{
    unsigned char img[EOS_EEPROM_SIZE], rb[EOS_EEPROM_SIZE];
    int rc, i;

    rc = Eeprom_ReadImage(img);
    if (rc != EOS_EE_OK) return rc;

    // mandatory safety backup of the current EEPROM before any factory write
    rc = Eeprom_BackupToHdd(backupOut, backupLen);
    if (rc != EOS_EE_OK) return rc;

    img[EE_VSTD_OFF + 0] = (unsigned char)(rawVal & 0xFF);
    img[EE_VSTD_OFF + 1] = (unsigned char)((rawVal >> 8) & 0xFF);
    img[EE_VSTD_OFF + 2] = (unsigned char)((rawVal >> 16) & 0xFF);
    img[EE_VSTD_OFF + 3] = (unsigned char)((rawVal >> 24) & 0xFF);
    quickCRC(&img[EE_CKSUM2_OFF], &img[EE_FACTORY_OFF], EE_FACTORY_LEN);

    rc = Eeprom_WriteImage(img);
    if (rc != EOS_EE_OK) return rc;

    rc = Eeprom_ReadImage(rb);
    if (rc != EOS_EE_OK) return rc;
    for (i = EE_VSTD_OFF; i < EE_VSTD_OFF + 4; ++i)
        if (rb[i] != img[i]) return EOS_EE_ERR_WRITE;
    return EOS_EE_OK;
}