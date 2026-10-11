// EOS DLC / title-update metadata signer.
// Adapted as a bounded, standalone EOS implementation from the PrometheOS
// DLC/Update Signer algorithm. No XBE certificate rewrites or alternate-title
// mounts are necessary: EOS already mounts the full E: data partition.
// Intentionally touches only the first 20 signature bytes of ContentMeta.xbx.
#include "eos_dlc.h"

#ifdef EOS_DLC_HOST_TEST
#include "eos_dlc_host_stubs.h"  // test harness only, not shipped in Xbox project
#else
#include <xtl.h>
#include "xboxinternals.h"   // XboxHDKey, XcHMAC
#endif
#include <stdlib.h>

#define DLC_MIN_HEADER   0x6Cu
#define DLC_MAX_HEADER   0x200000u  // cap untrusted ContentMeta header at 2 MiB
#define DLC_MAGIC        0x46534358u

static void dlcClear(EosDlcReport* r)
{
    unsigned int* p = (unsigned int*)r;
    unsigned int i;
    for (i = 0; i < sizeof(*r) / sizeof(unsigned int); ++i) p[i] = 0;
}
static int dlcHexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int dlcReadTitleId(const char* name, unsigned int* id)
{
    unsigned int v = 0;
    int i;
    for (i = 0; i < 8; ++i) {
        int x = dlcHexDigit(name[i]);
        if (!name[i] || x < 0) return 0;
        v = (v << 4) | (unsigned int)x;
    }
    if (name[8] != 0 || v == 0 || v == 0xFFFFFFFFu) return 0;
    *id = v;
    return 1;
}
static int dlcOfferDir(const char* name)
{
    int i;
    for (i = 0; i < 16; ++i)
        if (!name[i] || dlcHexDigit(name[i]) < 0) return 0;
    return name[16] == 0;
}
static int dlcAppend(char* dest, int cap, const char* append)
{
    int n = 0, j = 0;
    while (n < cap && dest[n]) ++n;
    if (n >= cap) return 0;
    while (append[j] && n < cap - 1) dest[n++] = append[j++];
    dest[n] = 0;
    return append[j] == 0;
}
static int dlcCopy(char* dest, int cap, const char* in)
{
    dest[0] = 0;
    return dlcAppend(dest, cap, in);
}
static unsigned int dlc32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
        ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static int dlcEqual20(const unsigned char* a, const unsigned char* b)
{
    int i;
    for (i = 0; i < 20; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

// Process exactly one confirmed ContentMeta.xbx, validating its actual header
// against the file length before allocating anything or writing to disk.
static void dlcProcess(const char* path, unsigned int titleId, int mode, EosDlcReport* report)
{
    HANDLE h;
    DWORD size, rd = 0, headerSize, toRead, written = 0;
    unsigned char pre[0x6C], oldSig[20], key[20], signature[20], verify[20];
    unsigned char* body = 0;
    unsigned int magic;
    int result = 0;

    ++report->found;
    h = CreateFileA(path, mode == DLC_SCAN_ONLY ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
        FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { ++report->failed; return; }
    size = GetFileSize(h, 0);
    if (size == 0xFFFFFFFFu || size < DLC_MIN_HEADER) goto done;
    if (!ReadFile(h, pre, DLC_MIN_HEADER, &rd, 0) || rd != DLC_MIN_HEADER) goto done;
    magic = dlc32(pre + 0x14);
    headerSize = dlc32(pre + 0x18);
    if (magic != DLC_MAGIC || headerSize < DLC_MIN_HEADER ||
        headerSize > DLC_MAX_HEADER || headerSize > size) goto done;

    toRead = headerSize - 0x14u;
    body = (unsigned char*)malloc(toRead);
    if (!body) goto done;
    if (SetFilePointer(h, 0x14, 0, FILE_BEGIN) == 0xFFFFFFFFu) goto done;
    rd = 0;
    if (!ReadFile(h, body, toRead, &rd, 0) || rd != toRead) goto done;
    { int k; for (k = 0; k < 20; ++k) oldSig[k] = pre[k]; }
    XcHMAC(XboxHDKey, 16, (PBYTE)&titleId, 4, 0, 0, key);
    XcHMAC(key, 20, body, toRead, 0, 0, signature);
    if (dlcEqual20(oldSig, signature)) ++report->valid;
    if (headerSize == DLC_MIN_HEADER) ++report->simpleContent;
    if (mode == DLC_SCAN_ONLY || (mode == DLC_SIGN_INVALID && dlcEqual20(oldSig, signature))) {
        ++report->skipped;
        result = 1;
        goto done;
    }
    if (SetFilePointer(h, 0, 0, FILE_BEGIN) == 0xFFFFFFFFu) goto done;
    if (!WriteFile(h, signature, 20, &written, 0) || written != 20 || !FlushFileBuffers(h)) goto done;
    if (SetFilePointer(h, 0, 0, FILE_BEGIN) == 0xFFFFFFFFu) goto done;
    rd = 0;
    if (!ReadFile(h, verify, 20, &rd, 0) || rd != 20 || !dlcEqual20(verify, signature)) goto done;
    ++report->signedCount;
    result = 1;
done:
    if (!result) ++report->failed;
    if (body) free(body);
    CloseHandle(h);
}
static void dlcFileIfPresent(const char* folder, unsigned int id,
    int mode, EosDlcReport* r)
{
    char path[256];
    if (!dlcCopy(path, sizeof(path), folder) ||
        !dlcAppend(path, sizeof(path), "\\ContentMeta.xbx")) return;
    if (GetFileAttributesA(path) != 0xFFFFFFFFu) dlcProcess(path, id, mode, r);
}
static void dlcScanOffers(const char* folder, unsigned int id,
    int mode, EosDlcReport* r)
{
    WIN32_FIND_DATAA f;
    HANDLE h;
    char pat[256], path[256];
    if (!dlcCopy(pat, sizeof(pat), folder) || !dlcAppend(pat, sizeof(pat), "\\*")) return;
    h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !dlcOfferDir(f.cFileName)) continue;
        if (!dlcCopy(path, sizeof(path), folder) ||
            !dlcAppend(path, sizeof(path), "\\") ||
            !dlcAppend(path, sizeof(path), f.cFileName)) continue;
        dlcFileIfPresent(path, id, mode, r);
    } while (FindNextFileA(h, &f));
    FindClose(h);
}
int Dlc_Run(int mode, EosDlcReport* report)
{
    const char* root = "E:\\TDATA";
    WIN32_FIND_DATAA f;
    HANDLE h;
    char path[256], sub[256], pat[256];
    unsigned int id;
    if (!report) return 0;
    dlcClear(report);
    if (mode != DLC_SCAN_ONLY && mode != DLC_SIGN_ALL && mode != DLC_SIGN_INVALID) return 0;
    {
        DWORD a = GetFileAttributesA(root);
        if (a == 0xFFFFFFFFu || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    }
    if (!dlcCopy(pat, sizeof(pat), root) || !dlcAppend(pat, sizeof(pat), "\\*")) return 0;
    h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return 1; /* existing but empty TDATA */
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !dlcReadTitleId(f.cFileName, &id)) continue;
        ++report->titleFolders;
        if (!dlcCopy(path, sizeof(path), root) ||
            !dlcAppend(path, sizeof(path), "\\") ||
            !dlcAppend(path, sizeof(path), f.cFileName)) continue;
        if (dlcCopy(sub, sizeof(sub), path) && dlcAppend(sub, sizeof(sub), "\\$C")) {
            dlcFileIfPresent(sub, id, mode, report);
            dlcScanOffers(sub, id, mode, report);
        }
        if (dlcCopy(sub, sizeof(sub), path) && dlcAppend(sub, sizeof(sub), "\\$U"))
            dlcFileIfPresent(sub, id, mode, report);
    } while (FindNextFileA(h, &f));
    FindClose(h);
    return 1;
}
