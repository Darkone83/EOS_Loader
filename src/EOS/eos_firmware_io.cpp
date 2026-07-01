// eos_firmware_io.cpp -- per-bank firmware backup/restore (streamed).
// RXDK / MSVC2003 / C89: declarations before statements, no CRT, no sprintf.
#include "eos_firmware_io.h"
#include "eos_bank.h"
#include "eos_flash.h"
#include "eos_gfx.h"        // <xtl.h>: CreateFileA/ReadFile/WriteFile/FindFirstFile

#define FW_DIR "E:\\Eos\\Backups\\Firmware"

int Firmware_BankBytes(int sizeCode)
{
    if (sizeCode == EOS_BANK_SIZE_256K) return 0x040000;
    if (sizeCode == EOS_BANK_SIZE_512K) return 0x080000;
    if (sizeCode == EOS_BANK_SIZE_1MB)  return 0x100000;
    return 0;
}

static const char* sizeTag(int code)
{
    if (code == EOS_BANK_SIZE_256K) return "256K";
    if (code == EOS_BANK_SIZE_512K) return "512K";
    if (code == EOS_BANK_SIZE_1MB)  return "1M";
    return "x";
}

// --- no-CRT string helpers ---------------------------------------------------
static char* appStr(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* appName(char* p, const char* s) // sanitize for FATX
{
    unsigned char c;
    while (*s) {
        c = (unsigned char)*s++;
        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == ' ' || c == '.') c = '_';
        *p++ = (char)c;
    }
    return p;
}
static char* appHex1(char* p, int v) { *p++ = (char)(v < 10 ? '0' + v : 'A' + v - 10); return p; }
static char* app2(char* p, int n) { *p++ = (char)('0' + (n / 10) % 10); *p++ = (char)('0' + n % 10); return p; }
static int   fileExists(const char* path) { return GetFileAttributesA(path) != 0xFFFFFFFF; }

static char* fwPath(char* path, const char* base)   // FW_DIR "\" base
{
    char* p = path;
    p = appStr(p, FW_DIR);
    *p++ = '\\';
    p = appStr(p, base);
    *p = 0;
    return path;
}

int Firmware_BackupBank(int bankIdx, char* outPath, int outLen)
{
    unsigned char buf[256];
    char  path[160]; char* p;
    int   ef, code, bytes, pages, pg, n, rc, i, L;
    HANDLE h; DWORD wrote;

    ef = Bank_Ef(bankIdx);
    code = Bank_SizeCode(bankIdx);
    bytes = Firmware_BankBytes(code);
    if (bytes == 0) return FW_ERR_SIZE;
    pages = bytes / 256;

    CreateDirectoryA("E:\\Eos", NULL);
    CreateDirectoryA("E:\\Eos\\Backups", NULL);
    CreateDirectoryA(FW_DIR, NULL);

    for (n = 0; n < 100; ++n) {
        p = path;
        p = appStr(p, FW_DIR "\\fw_");
        p = appName(p, Bank_Name(bankIdx));
        *p++ = '_'; p = appHex1(p, ef & 0x0F);
        *p++ = '_'; p = appStr(p, sizeTag(code));
        *p++ = '_'; p = app2(p, n);
        p = appStr(p, ".bin");
        *p = 0;
        if (!fileExists(path)) break;
    }
    if (n >= 100) return FW_ERR_FILE;

    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FW_ERR_FILE;

    for (pg = 0; pg < pages; ++pg) {
        rc = Flash_ReadPage(ef, pg, buf);
        if (rc != EOS_FLASH_OK) { CloseHandle(h); return FW_ERR_FLASH; }
        if (!WriteFile(h, buf, 256, &wrote, NULL) || wrote != 256) {
            CloseHandle(h); return FW_ERR_FILE;
        }
    }
    CloseHandle(h);

    if (outPath && outLen > 0) {
        L = 0; while (path[L]) ++L;
        if (L > outLen - 1) L = outLen - 1;
        for (i = 0; i < L; ++i) outPath[i] = path[i];
        outPath[L] = 0;
    }
    return FW_OK;
}

// Case-insensitive ".bin" suffix test.
static int endsWithBin(const char* s)
{
    int n = 0; while (s[n]) ++n;
    if (n < 4) return 0;
    s += n - 4;
    return s[0] == '.'
        && (s[1] == 'b' || s[1] == 'B')
        && (s[2] == 'i' || s[2] == 'I')
        && (s[3] == 'n' || s[3] == 'N');
}

// List the firmware folder EXACTLY like the XbDiag FileExplorer (LoadDirectory):
// FindFirstFile("<dir>\\*") + skip . / .. ; we additionally skip dirs and keep
// only .bin in code.
int Firmware_ListBackups(char names[][64], int maxN)
{
    char pattern[200];
    WIN32_FIND_DATA fd; HANDLE h; int n = 0, k, p = 0;
    const char* d = FW_DIR;

    while (d[p] && p < 190) { pattern[p] = d[p]; ++p; }
    if (p > 0 && pattern[p - 1] != '\\') pattern[p++] = '\\';
    pattern[p++] = '*'; pattern[p] = 0;

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 ||
                (fd.cFileName[1] == '.' && fd.cFileName[2] == 0))) continue;   // . / ..
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;       // dirs
        if (!endsWithBin(fd.cFileName)) continue;                          // .bin only
        if (n >= maxN) break;
        for (k = 0; k < 63 && fd.cFileName[k]; ++k) names[n][k] = fd.cFileName[k];
        names[n][k] = 0;
        ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

int Firmware_BinBytes(const char* fileName)
{
    char path[160]; HANDLE h; DWORD sz;
    fwPath(path, fileName);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    sz = GetFileSize(h, NULL);
    CloseHandle(h);
    return (int)sz;
}

int Firmware_RestoreBank(const char* fileName, int bankIdx)
{
    unsigned char fbuf[256], rbuf[256];
    char  path[160];
    int   ef, code, bytes, pages, pg, i, rc;
    HANDLE h; DWORD got, fsz;

    ef = Bank_Ef(bankIdx);
    code = Bank_SizeCode(bankIdx);
    bytes = Firmware_BankBytes(code);
    if (bytes == 0) return FW_ERR_SIZE;
    pages = bytes / 256;

    fwPath(path, fileName);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FW_ERR_FILE;

    fsz = GetFileSize(h, NULL);
    if (fsz != (DWORD)bytes) { CloseHandle(h); return FW_ERR_SIZE; }   // must match

    rc = Flash_EraseBank(ef);
    if (rc != EOS_FLASH_OK) { CloseHandle(h); return FW_ERR_FLASH; }

    for (pg = 0; pg < pages; ++pg) {
        if (!ReadFile(h, fbuf, 256, &got, NULL) || got != 256) {
            CloseHandle(h); return FW_ERR_FILE;
        }
        rc = Flash_ProgramPage(ef, pg, fbuf);
        if (rc != EOS_FLASH_OK) { CloseHandle(h); return FW_ERR_FLASH; }
        rc = Flash_ReadPage(ef, pg, rbuf);
        if (rc != EOS_FLASH_OK) { CloseHandle(h); return FW_ERR_FLASH; }
        for (i = 0; i < 256; ++i)
            if (rbuf[i] != fbuf[i]) { CloseHandle(h); return FW_ERR_VERIFY; }
    }
    CloseHandle(h);

    Flash_Sync(ef);   // reload the served SDRAM copy from the new flash content
    return FW_OK;
}