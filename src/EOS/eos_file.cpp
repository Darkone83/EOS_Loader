// eos_file.cpp -- see eos_file.h. Standard RXDK file APIs (the same calls
// DarkDash's dd_fileops uses), trimmed to the read + enumerate subset and made
// heap-free (caller buffers, no malloc).
#include <xtl.h>
#include "eos_file.h"
#include "dd_mount.h"   // Mount_HddPartitions
#include "eos_sdcard.h" // SD:\ virtual path -> FatFs

static int  fLen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// Case-insensitive name compare (ASCII). Returns <0, 0, >0 like strcmp.
static int  fICmp(const char* a, const char* b)
{
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (ca == 0)  return 0;
        ++i;
    }
}

// Browser sort order: directories first, then files; each group alphabetical
// (case-insensitive). Returns 1 if entry a should come before entry b.
static int  fEntryLess(const EosFileEntry* a, const EosFileEntry* b)
{
    if (a->is_dir != b->is_dir) return a->is_dir ? 1 : 0;   // dirs before files
    return fICmp(a->name, b->name) < 0 ? 1 : 0;
}

// Heap-free, stable insertion sort of the entry list (n is small: a directory
// listing). No CRT, no malloc -- matches the rest of eos_file.
static void fSortEntries(EosFileEntry* e, int n)
{
    int i, j;
    for (i = 1; i < n; ++i) {
        EosFileEntry key = e[i];
        j = i - 1;
        while (j >= 0 && fEntryLess(&key, &e[j])) {
            e[j + 1] = e[j];
            --j;
        }
        e[j + 1] = key;
    }
}
static void fCopy(char* d, int cap, const char* s)
{
    int i = 0;
    if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; ++i; }
    d[i] = 0;
}

// The loader keeps Xbox HDD paths in their native "E:\..." form.  For the
// few features that can also consume files from the EOS SD volume, expose a
// tiny virtual "SD:\..." prefix and translate only at this lowest file layer.
// That lets image/audio/theme callers stay completely storage-agnostic.
static int fIsSd(const char* p)
{
    return p && p[0] == 'S' && p[1] == 'D' && p[2] == ':';
}

static void fSdPath(char* d, int cap, const char* s)
{
    int i = 3, p = 0;
    if (cap <= 0) return;
    if (s[i] != '\\' && s[i] != '/') d[p++] = '/';
    while (s[i] && p < cap - 1) {
        char c = s[i++];
        d[p++] = (c == '\\') ? '/' : c;
    }
    if (p == 0) d[p++] = '/';
    d[p] = 0;
}

void File_MountDrives(void)
{
    Mount_HddPartitions();
}

int File_Exists(const char* path)
{
    if (fIsSd(path)) {
        char sp[EOS_FILE_PATH_MAX]; FILINFO fi;
        if (Sd_Mount() != EOS_SD_OK) return 0;
        fSdPath(sp, sizeof(sp), path);
        return f_stat(sp, &fi) == FR_OK;
    }
    return GetFileAttributesA(path) != 0xFFFFFFFF;
}

int File_IsDir(const char* path)
{
    if (fIsSd(path)) {
        char sp[EOS_FILE_PATH_MAX]; FILINFO fi;
        if (Sd_Mount() != EOS_SD_OK) return 0;
        fSdPath(sp, sizeof(sp), path);
        return f_stat(sp, &fi) == FR_OK && (fi.fattrib & AM_DIR);
    }
    DWORD a = GetFileAttributesA(path);
    return (a != 0xFFFFFFFF) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int File_ListDrives(EosFileEntry* out, int maxEntries)
{
    static const char k_letters[7] = { 'C', 'E', 'F', 'G', 'X', 'Y', 'Z' };
    char root[4];
    int  i, n = 0;

    for (i = 0; i < 7 && n < maxEntries; ++i) {
        root[0] = k_letters[i]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        if (GetFileAttributesA(root) != 0xFFFFFFFF) {
            out[n].name[0] = k_letters[i]; out[n].name[1] = ':'; out[n].name[2] = 0;
            out[n].is_dir = 1;
            ++n;
        }
    }
    return n;
}

int File_ListDir(const char* path, EosFileEntry* out, int maxEntries)
{
    if (fIsSd(path)) {
        char sp[EOS_FILE_PATH_MAX]; DIR dir; FILINFO fno; FRESULT fr;
        int n = 0;
        if (Sd_Mount() != EOS_SD_OK) return 0;
        fSdPath(sp, sizeof(sp), path);
        fr = f_opendir(&dir, sp);
        if (fr != FR_OK) return 0;
        for (;;) {
            fr = f_readdir(&dir, &fno);
            if (fr != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.' &&
                (fno.fname[1] == 0 || (fno.fname[1] == '.' && fno.fname[2] == 0))) continue;
            if (n >= maxEntries) break;
            fCopy(out[n].name, EOS_FILE_NAME_MAX, fno.fname);
            out[n].is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
            ++n;
        }
        f_closedir(&dir);
        fSortEntries(out, n);
        return n;
    }

    char            pat[EOS_FILE_PATH_MAX + 4];
    WIN32_FIND_DATA fd;
    HANDLE          h;
    int             n = 0, p;

    fCopy(pat, sizeof(pat), path);
    p = fLen(pat);
    if (p > 0 && pat[p - 1] != '\\' && p < (int)sizeof(pat) - 2) pat[p++] = '\\';
    pat[p++] = '*'; pat[p] = 0;

    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || fd.cFileName[1] == '.')) continue;
        if (n >= maxEntries) break;
        fCopy(out[n].name, EOS_FILE_NAME_MAX, fd.cFileName);
        out[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        ++n;
    } while (FindNextFile(h, &fd));
    FindClose(h);

    fSortEntries(out, n);   // directories first, then files; each alphabetical
    return n;
}

int File_ReadInto(const char* path, unsigned char* buf, int cap)
{
    if (fIsSd(path)) {
        char sp[EOS_FILE_PATH_MAX]; FIL fp; FRESULT fr; UINT got = 0; FSIZE_t sz;
        if (Sd_Mount() != EOS_SD_OK) return -1;
        fSdPath(sp, sizeof(sp), path);
        fr = f_open(&fp, sp, FA_READ);
        if (fr != FR_OK) return -1;
        sz = f_size(&fp);
        if (sz > (FSIZE_t)cap) { f_close(&fp); return -1; }
        fr = f_read(&fp, buf, (UINT)sz, &got);
        f_close(&fp);
        return (fr == FR_OK && got == (UINT)sz) ? (int)sz : -1;
    }

    HANDLE h;
    DWORD  sz, got = 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    sz = GetFileSize(h, NULL);
    if (sz == 0xFFFFFFFF || (int)sz > cap) { CloseHandle(h); return -1; }
    if (sz && (!ReadFile(h, buf, sz, &got, NULL) || got != sz)) {
        CloseHandle(h); return -1;
    }
    CloseHandle(h);
    return (int)sz;
}