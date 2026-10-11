// eos_webfiles.cpp - Phase 5 WebUI file management (HDD0/HDD1/SD).
// Xbox files are operated on through bounded, non-allocating APIs. No whole-file RAM staging.
#ifdef EOS_WEBFILES_HOST_TEST
#include "webfiles_host.h"
#else
#include <xtl.h>
#include "xboxinternals.h"
#include "eos_sdcard.h"
#endif
#include "eos_webfiles.h"

#define WF_LIST_COUNT 48
#define WF_DEPTH_MAX 12

static int wlen(const char* s) { int n = 0; if (s) while (s[n]) ++n; return n; }
static void wcopy(char* to, int cap, const char* from) { int i = 0; if (cap <= 0)return; while (from && from[i] && i < cap - 1) { to[i] = from[i]; ++i; }to[i] = 0; }
static int wcat(char* to, int cap, const char* s) { int n = wlen(to), i = 0; if (n >= cap)return 0; while (s[i] && n < cap - 1)to[n++] = s[i++]; to[n] = 0; return !s[i]; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
static int weq(const char* a, const char* b) { int i = 0; while (a[i] && b[i]) { if (upper(a[i]) != upper(b[i]))return 0; ++i; }return !a[i] && !b[i]; }
static int wpref(const char* a, const char* b) { int i = 0; while (b[i]) { if (upper(a[i]) != upper(b[i]))return 0; ++i; }return 1; }
static int isName(const char* s) { int i, n = wlen(s); if (!n || n > 63 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.') || s[n - 1] == ' ' || s[n - 1] == '.')return 0; for (i = 0; i < n; i++) { unsigned char c = (unsigned char)s[i]; if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '%')return 0; }return 1; }

// Reserved secondary DOS letters. Never replace a pre-existing symlink: this
// is used only if the running BIOS actually exposes HDD1 partitions.
static const char kHdd1Alias[4] = { 'P','Q','R','V' };
static const char kHdd1Part[4] = { 'C','E','F','G' };
static const int  kHdd1Num[4] = { 2,1,6,7 };
static unsigned char s_hdd1Mapped[4] = { 0,0,0,0 };
static int s_hdd1Checked = 0;
static void secondaryEnsure(void)
{
    if (s_hdd1Checked)return;
    s_hdd1Checked = 1;
    for (int i = 0; i < 4; ++i) {
        char alias[7] = { '\\','?','?','\\',kHdd1Alias[i],':',0 };
        char device[40] = "\\Device\\Harddisk1\\Partition";
        int j = wlen(device); device[j++] = (char)('0' + kHdd1Num[i]); device[j] = 0;
        STRING ln = { (USHORT)6,(USHORT)7,alias };
        STRING dn = { (USHORT)wlen(device),(USHORT)(wlen(device) + 1),device };
        if (IoCreateSymbolicLink(&ln, &dn) == 0) {
            char root[4] = { kHdd1Alias[i],':','\\',0 };
            if (GetFileAttributesA(root) != 0xFFFFFFFFUL)s_hdd1Mapped[i] = 1;
            else IoDeleteSymbolicLink(&ln);
        }
    }
}

struct WfPath { char native[EOS_WF_PATH]; char base[EOS_WF_PATH]; int sd; int root; int writable; };
// root: 0=real file/dir, 1=pseudo-root, 2=HDD0 volume list, 3=HDD1 volume list.
static int resolve(const char* virt, WfPath* p)
{
    int n = wlen(virt); char vol = 0;
    p->native[0] = 0; p->base[0] = 0; p->sd = 0; p->root = 0; p->writable = 0;
    if (!virt || n >= EOS_WF_PATH || n == 0 || virt[0] != '/')return WF_BADPATH;
    if (n == 1) { p->root = 1; return WF_OK; }
    const char* q = virt + 1;
    if (wpref(q, "HDD0") && (q[4] == 0 || q[4] == '/')) {
        q += 4; if (*q == 0 || (*q == '/' && q[1] == 0)) { p->root = 2; return WF_OK; }
        if (*q != '/' || q[1] == 0 || (q[2] && q[2] != '/'))return WF_BADPATH;
        vol = upper(q[1]);
        if (!(vol == 'C' || vol == 'E' || vol == 'F' || vol == 'G' || vol == 'X' || vol == 'Y' || vol == 'Z'))return WF_BADPATH;
        p->native[0] = vol; p->native[1] = ':'; p->native[2] = '\\'; p->native[3] = 0;
        // Mounted HDD volumes expose normal file operations at every depth.
        // Any filesystem access errors are reported by the operation itself.
        p->writable = 1; q += 2;
    }
    else if (wpref(q, "HDD1") && (q[4] == 0 || q[4] == '/')) {
        q += 4; if (*q == 0 || (*q == '/' && q[1] == 0)) { p->root = 3; return WF_OK; }
        if (*q != '/' || q[1] == 0 || (q[2] && q[2] != '/'))return WF_BADPATH;
        vol = upper(q[1]);
        secondaryEnsure();
        for (int i = 0; i < 4; ++i)if (vol == kHdd1Part[i] && s_hdd1Mapped[i]) {
            p->native[0] = kHdd1Alias[i]; p->native[1] = ':'; p->native[2] = '\\'; p->native[3] = 0; break;
        }
        if (!p->native[0])return WF_UNAVAILABLE;
        // Mounted HDD volumes expose normal file operations at every depth.
        // Any filesystem access errors are reported by the operation itself.
        p->writable = 1; q += 2;
    }
    else if (wpref(q, "SD") && (q[2] == 0 || q[2] == '/')) {
        q += 2; p->sd = 1; p->writable = 1; wcopy(p->native, sizeof(p->native), "/");
    }
    else return WF_BADPATH;

    wcopy(p->base, sizeof(p->base), p->native);
    if (*q == 0)return WF_OK;
    if (*q != '/')return WF_BADPATH;
    ++q;
    while (*q) {
        char part[65]; int z = 0;
        while (*q && *q != '/') {
            if (z >= 63)return WF_BADPATH;
            part[z++] = *q++;
        }
        part[z] = 0;
        if (!isName(part))return WF_BADPATH;
        if (p->native[0] && wlen(p->native) > 0) {
            char last = p->native[wlen(p->native) - 1];
            if (last != '/' && last != '\\')if (!wcat(p->native, sizeof(p->native), p->sd ? "/" : "\\"))return WF_BADPATH;
        }
        if (!wcat(p->native, sizeof(p->native), part))return WF_BADPATH;
        if (*q == '/')++q;
    }
    return WF_OK;
}

const char* Wf_Error(int e)
{
    switch (e) {
    case WF_OK:return "OK"; case WF_BADPATH:return "Invalid or too-long path";
    case WF_DENIED:return "This location is read-only"; case WF_NOTFOUND:return "File or folder not found";
    case WF_EXISTS:return "Destination already exists"; case WF_IOERROR:return "Storage operation failed";
    case WF_UNAVAILABLE:return "Drive not available"; case WF_BUSY:return "Another file operation is running";
    case WF_DEPTH:return "Directory nesting limit reached"; case WF_STALE:return "EOSUPLD.TMP or EOSBACK.BAK needs cleanup in this folder"; case WF_INCOMPLETE:return "Incomplete transfer";
    }
    return "Unexpected error";
}
static int present(const WfPath* p)
{
    if (p->root)return 1;
    if (p->sd)return Sd_Mount() == EOS_SD_OK;
    return GetFileAttributesA(p->native) != 0xFFFFFFFFUL;
}
static int isDir(const WfPath* p)
{
    if (p->sd) { FILINFO fi; if (Sd_Mount() != EOS_SD_OK)return 0; return f_stat(p->native, &fi) == FR_OK && (fi.fattrib & AM_DIR); }
    DWORD a = GetFileAttributesA(p->native); return a != 0xFFFFFFFFUL && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static int existsFile(const WfPath* p)
{
    if (p->sd) { FILINFO fi; if (Sd_Mount() != EOS_SD_OK)return 0; return f_stat(p->native, &fi) == FR_OK; }
    return GetFileAttributesA(p->native) != 0xFFFFFFFFUL;
}
static int writablePath(const WfPath* p)
{
    if (p->root || !p->writable)return 0;
    // Partition root may contain new folders, but never be renamed or erased.
    return 1;
}

struct JsonW { char* d; int size, n, failed; };
static void jwInit(JsonW* j, char* b, int cap) { j->d = b; j->size = cap; j->n = 0; j->failed = 0; if (cap)b[0] = 0; }
static void jwC(JsonW* j, char c) { if (j->n >= j->size - 1) { j->failed = 1; return; }j->d[j->n++] = c; j->d[j->n] = 0; }
static void jwS(JsonW* j, const char* s) { while (*s)jwC(j, *s++); }
static void jwU(JsonW* j, unsigned long long v) { char b[24]; int i = 0; if (!v) { jwC(j, '0'); return; }while (v && i < 23) { b[i++] = (char)('0' + (v % 10)); v /= 10; }while (i)jwC(j, b[--i]); }
static void jwEsc(JsonW* j, const char* s) { jwC(j, '"'); for (int i = 0; s[i]; ++i) { unsigned char c = (unsigned char)s[i]; if (c == '"' || c == '\\') { jwC(j, '\\'); jwC(j, (char)c); } else if (c < 32) { jwC(j, '?'); } else jwC(j, (char)c); }jwC(j, '"'); }
static void jwEnt(JsonW* j, const char* nm, int dir, unsigned long long size, int* first)
{
    if (!*first)jwC(j, ','); *first = 0;
    jwS(j, "{\"n\":"); jwEsc(j, nm); jwS(j, ",\"d\":"); jwS(j, dir ? "true" : "false");
    jwS(j, ",\"s\":"); jwU(j, size); jwC(j, '}');
}
// Keep directory names in a fixed-size, alphabetically sorted page. We never
// allocate or retain an entire directory: each requested page is selected with
// at most 49 entries of scratch storage, regardless of directory size.
// Folders appear before files, with case-insensitive alphabetical ordering.
struct WfSortEntry {
    char name[65];
    int dir;
    unsigned long long size;
};
static WfSortEntry s_sortPage[WF_LIST_COUNT + 1];
static int s_sortCount = 0;
static int sortCompare(const WfSortEntry* a, const WfSortEntry* b)
{
    if (a->dir != b->dir)return a->dir ? -1 : 1;
    int i = 0;
    while (a->name[i] && b->name[i]) {
        char x = upper(a->name[i]), y = upper(b->name[i]);
        if (x != y)return (unsigned char)x < (unsigned char)y ? -1 : 1;
        ++i;
    }
    if (a->name[i] || b->name[i])return a->name[i] ? 1 : -1;
    // Deterministic tie breaker for filesystems allowing names that differ
    // only by case. Keeps entries from changing pages between requests.
    for (i = 0; a->name[i] && b->name[i]; ++i) {
        unsigned char x = (unsigned char)a->name[i], y = (unsigned char)b->name[i];
        if (x != y)return x < y ? -1 : 1;
    }
    return 0;
}
static void sortInsert(const char* name, int dir, unsigned long long size,
    const WfSortEntry* after, int useAfter)
{
    if (!isName(name))return;
    WfSortEntry e; wcopy(e.name, sizeof(e.name), name); e.dir = dir; e.size = size;
    if (useAfter && sortCompare(&e, after) <= 0)return;
    int pos = 0;
    while (pos < s_sortCount && sortCompare(&s_sortPage[pos], &e) < 0)++pos;
    if (pos >= WF_LIST_COUNT + 1)return;
    if (s_sortCount < WF_LIST_COUNT + 1)++s_sortCount;
    for (int i = s_sortCount - 1; i > pos; --i)s_sortPage[i] = s_sortPage[i - 1];
    s_sortPage[pos] = e;
}
// One scan selects the next 48 sorted entries and a 49th lookahead. A request
// for a later numeric page walks each preceding page in bounded memory.
static int sortScan(const WfPath* p, const WfSortEntry* after, int useAfter)
{
    s_sortCount = 0;
    if (p->sd) {
        DIR dir; FILINFO fi;
        FRESULT fr = f_opendir(&dir, p->native);
        if (fr != FR_OK)return WF_IOERROR;
        for (;;) {
            fr = f_readdir(&dir, &fi);
            if (fr != FR_OK || !fi.fname[0])break;
            sortInsert(fi.fname, (fi.fattrib & AM_DIR) != 0, (unsigned long long)fi.fsize, after, useAfter);
        }
        f_closedir(&dir);
        if (fr != FR_OK)return WF_IOERROR;
    }
    else {
        char pat[EOS_WF_PATH + 4]; wcopy(pat, sizeof(pat), p->native);
        int len = wlen(pat);
        if (len && pat[len - 1] != '\\' && !wcat(pat, sizeof(pat), "\\"))return WF_BADPATH;
        if (!wcat(pat, sizeof(pat), "*"))return WF_BADPATH;
        WIN32_FIND_DATA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                sortInsert(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, after, useAfter);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    return WF_OK;
}
int Wf_List(const char* virt, int page, char* out, int cap)
{
    WfPath p; JsonW j; int r = resolve(virt, &p), first = 1, count = 0, more = 0;
    if (r != WF_OK)return r;
    // Excessively deep offsets could monopolize the frame-polled WebUI.
    if (page < 0 || page>128 || cap < 2048)return WF_BADPATH;
    jwInit(&j, out, cap); jwS(&j, "{\"ok\":true,\"entries\":[");
    if (p.root == 1) {
        if (!page) {
            jwEnt(&j, "HDD0", 1, 0, &first); secondaryEnsure();
            if (s_hdd1Mapped[0] || s_hdd1Mapped[1] || s_hdd1Mapped[2] || s_hdd1Mapped[3])jwEnt(&j, "HDD1", 1, 0, &first);
            if (Sd_Mount() == EOS_SD_OK)jwEnt(&j, "SD", 1, 0, &first);
        }
    }
    else if (p.root == 2 || p.root == 3) {
        const char* vols = p.root == 2 ? "CEFGXYZ" : "CEFG";
        int skip = page * WF_LIST_COUNT;
        if (p.root == 3)secondaryEnsure();
        for (int i = 0; vols[i]; i++) {
            char vp[24]; int m = 0; const char* base = p.root == 2 ? "/HDD0/" : "/HDD1/";
            for (int k = 0; base[k]; k++)vp[m++] = base[k]; vp[m++] = vols[i]; vp[m] = 0;
            WfPath probe; if (resolve(vp, &probe) != WF_OK || !present(&probe))continue;
            if (skip > 0) { --skip; continue; }if (count >= WF_LIST_COUNT) { more = 1; break; }
            char nm[2] = { vols[i],0 }; jwEnt(&j, nm, 1, 0, &first); ++count;
        }
    }
    else {
        if (!isDir(&p))return WF_NOTFOUND;
        WfSortEntry after; int useAfter = 0;
        for (int pass = 0; pass <= page; ++pass) {
            r = sortScan(&p, &after, useAfter);
            if (r != WF_OK)return r;
            if (pass < page) {
                if (s_sortCount <= WF_LIST_COUNT) { s_sortCount = 0; break; }
                after = s_sortPage[WF_LIST_COUNT - 1]; useAfter = 1;
            }
        }
        count = s_sortCount < WF_LIST_COUNT ? s_sortCount : WF_LIST_COUNT;
        more = s_sortCount > WF_LIST_COUNT;
        for (int i = 0; i < count; ++i)
            jwEnt(&j, s_sortPage[i].name, s_sortPage[i].dir, s_sortPage[i].size, &first);
    }
    jwS(&j, "],\"page\":"); jwU(&j, (unsigned)page); jwS(&j, ",\"more\":"); jwS(&j, more ? "true" : "false");
    jwS(&j, ",\"writable\":"); jwS(&j, p.writable ? "true" : "false"); jwC(&j, '}');
    return j.failed ? WF_IOERROR : j.n;
}

static int getPath(const char* virt, WfPath* p, int write)
{
    int r = resolve(virt, p); if (r != WF_OK)return r;
    if (p->root)return WF_DENIED;
    if (write && !writablePath(p))return WF_DENIED;
    if (p->sd && Sd_Mount() != EOS_SD_OK)return WF_UNAVAILABLE;
    return WF_OK;
}
int Wf_Mkdir(const char* path)
{
    if (Wf_JobBusy())return WF_BUSY;
    WfPath p; int r = getPath(path, &p, 1); if (r != WF_OK)return r;
    if (weq(p.native, p.base))return WF_DENIED;
    if (existsFile(&p))return isDir(&p) ? WF_OK : WF_EXISTS;
    // Only immediate parent may already exist; do not silently create a tree.
    char parent[EOS_WF_PATH]; wcopy(parent, sizeof(parent), p.native);
    int i = wlen(parent) - 1; while (i >= 0 && parent[i] != '/' && parent[i] != '\\')--i;
    if (i < 0)return WF_BADPATH;
    parent[(i == 0) ? 1 : ((i == 2 && parent[1] == ':') ? 3 : i)] = 0;
    WfPath pp = p; wcopy(pp.native, sizeof(pp.native), parent);
    if (!isDir(&pp))return WF_NOTFOUND;
    if (p.sd) { FRESULT fr = f_mkdir(p.native); return(fr == FR_OK || fr == FR_EXIST) ? WF_OK : WF_IOERROR; }
    return CreateDirectoryA(p.native, NULL) ? WF_OK : WF_IOERROR;
}
int Wf_Rename(const char* virt, const char* newName)
{
    if (Wf_JobBusy())return WF_BUSY;
    if (!isName(newName))return WF_BADPATH;
    WfPath p; int r = getPath(virt, &p, 1); if (r != WF_OK)return r;
    if (weq(p.native, p.base))return WF_DENIED;
    int k = wlen(p.native) - 1; while (k >= 0 && p.native[k] != '/' && p.native[k] != '\\')--k;
    if (k < 0)return WF_BADPATH;
    char target[EOS_WF_PATH]; wcopy(target, sizeof(target), p.native); target[k + 1] = 0;
    if (!wcat(target, sizeof(target), newName))return WF_BADPATH;
    if (!existsFile(&p))return WF_NOTFOUND;
    if (weq(target, p.native))return WF_OK;
    WfPath np = p; wcopy(np.native, sizeof(np.native), target);
    if (existsFile(&np))return WF_EXISTS;
    if (p.sd)return f_rename(p.native, target) == FR_OK ? WF_OK : WF_IOERROR;
    return MoveFileA(p.native, target) ? WF_OK : WF_IOERROR;
}

static int removeFile(const char* path, int sd)
{
    if (sd)return f_unlink(path) == FR_OK ? WF_OK : WF_IOERROR;
    return DeleteFileA(path) ? WF_OK : WF_IOERROR;
}
static int removeDir(const char* path, int sd)
{
    if (sd)return f_unlink(path) == FR_OK ? WF_OK : WF_IOERROR;
    return RemoveDirectoryA(path) ? WF_OK : WF_IOERROR;
}

// Directory deletion is a bounded explicit depth-first walk. At most eight
// filesystem actions per frame; there are no recursive C/C++ calls or unbounded
// stack allocations. A failure stops the job, possibly after partial deletion.
struct WfTreeFrame {
    char path[EOS_WF_PATH];
    DIR sdDir; HANDLE h; WIN32_FIND_DATA fd;
    int opened, first, sd;
};
static WfTreeFrame s_tree[WF_DEPTH_MAX];
static int s_treeLevel = -1, s_treeState = 0, s_treeError = WF_OK, s_treeRemoved = 0, s_treeSerial = 0;
static void treeClose(void)
{
    for (int i = 0; i < WF_DEPTH_MAX; i++) {
        if (s_tree[i].opened) {
            if (s_tree[i].sd)f_closedir(&s_tree[i].sdDir);
            else FindClose(s_tree[i].h); s_tree[i].opened = 0;
        }
    }
}
int Wf_JobBusy(void) { return s_treeState == 1; }
int Wf_Delete(const char* virt)
{
    if (Wf_JobBusy())return WF_BUSY;
    WfPath p; int r = getPath(virt, &p, 1); if (r != WF_OK)return r;
    if (!existsFile(&p))return WF_NOTFOUND;
    // A volume root is never a legal deletion target.
    if (weq(p.base, p.native))return WF_DENIED;
    if (!isDir(&p))return removeFile(p.native, p.sd);
    treeClose();
    for (int i = 0; i < WF_DEPTH_MAX; i++)s_tree[i].opened = 0;
    s_treeLevel = 0; s_treeState = 1; s_treeError = WF_OK; s_treeRemoved = 0; ++s_treeSerial;
    wcopy(s_tree[0].path, sizeof(s_tree[0].path), p.native);
    s_tree[0].opened = 0; s_tree[0].first = 1; s_tree[0].sd = p.sd;
    return 1;
}
void Wf_Tick(void)
{
    if (!Wf_JobBusy())return;
    for (int steps = 0; steps < 8 && s_treeState == 1; steps++) {
        WfTreeFrame* f = &s_tree[s_treeLevel];
        if (!f->opened) {
            if (f->sd) { FRESULT fr = f_opendir(&f->sdDir, f->path); if (fr != FR_OK) { s_treeError = WF_IOERROR; s_treeState = 3; break; } }
            else {
                char pat[EOS_WF_PATH + 4]; wcopy(pat, sizeof(pat), f->path);
                int n = wlen(pat); if (n && pat[n - 1] != '\\')wcat(pat, sizeof(pat), "\\");
                if (!wcat(pat, sizeof(pat), "*")) { s_treeError = WF_BADPATH; s_treeState = 3; break; }
                f->h = FindFirstFileA(pat, &f->fd);
                if (f->h == INVALID_HANDLE_VALUE) {
                    // An empty directory may have an unsuccessful find.
                    if (removeDir(f->path, 0) != WF_OK) { s_treeError = WF_IOERROR; s_treeState = 3; break; }
                    ++s_treeRemoved;
                    if (!s_treeLevel)s_treeState = 2; else --s_treeLevel;
                    continue;
                }
            }
            f->opened = 1; f->first = 1;
        }
        char name[65]; int dir = 0, got = 0;
        if (f->sd) {
            FILINFO fi; FRESULT fr;
            for (;;) {
                fr = f_readdir(&f->sdDir, &fi); if (fr != FR_OK) { s_treeError = WF_IOERROR; s_treeState = 3; break; }
                if (!fi.fname[0])break; if (!isName(fi.fname))continue;
                wcopy(name, sizeof(name), fi.fname); dir = (fi.fattrib & AM_DIR) != 0; got = 1; break;
            }
            if (s_treeState == 3)break;
        }
        else {
            for (;;) {
                if (f->first) { f->first = 0; got = 1; }
                else got = FindNextFileA(f->h, &f->fd) ? 1 : 0;
                if (!got)break; if (!isName(f->fd.cFileName))continue;
                wcopy(name, sizeof(name), f->fd.cFileName);
                dir = (f->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; break;
            }
        }
        if (!got) {
            if (f->sd)f_closedir(&f->sdDir); else FindClose(f->h); f->opened = 0;
            if (removeDir(f->path, f->sd) != WF_OK) { s_treeError = WF_IOERROR; s_treeState = 3; break; }
            ++s_treeRemoved;
            if (!s_treeLevel)s_treeState = 2; else --s_treeLevel;
            continue;
        }
        char child[EOS_WF_PATH]; wcopy(child, sizeof(child), f->path);
        if (child[wlen(child) - 1] != '/' && child[wlen(child) - 1] != '\\')
            if (!wcat(child, sizeof(child), f->sd ? "/" : "\\")) { s_treeError = WF_BADPATH; s_treeState = 3; break; }
        if (!wcat(child, sizeof(child), name)) { s_treeError = WF_BADPATH; s_treeState = 3; break; }
        if (dir) {
            if (s_treeLevel + 1 >= WF_DEPTH_MAX) { s_treeError = WF_DEPTH; s_treeState = 3; break; }
            ++s_treeLevel;
            WfTreeFrame* n = &s_tree[s_treeLevel]; n->sd = f->sd; n->opened = 0; n->first = 1;
            wcopy(n->path, sizeof(n->path), child);
        }
        else {
            if (removeFile(child, f->sd) != WF_OK) { s_treeError = WF_IOERROR; s_treeState = 3; break; }
            ++s_treeRemoved;
        }
    }
    if (s_treeState == 3)treeClose();
}
int Wf_JobInfo(char* out, int cap)
{
    JsonW j; jwInit(&j, out, cap); jwS(&j, "{\"ok\":"); jwS(&j, s_treeState == 3 ? "false" : "true");
    jwS(&j, ",\"state\":"); jwEsc(&j, s_treeState == 1 ? "running" : s_treeState == 2 ? "done" : s_treeState == 3 ? "failed" : "idle");
    jwS(&j, ",\"removed\":"); jwU(&j, (unsigned)s_treeRemoved);
    jwS(&j, ",\"id\":"); jwU(&j, (unsigned)s_treeSerial);
    jwS(&j, ",\"error\":"); jwEsc(&j, Wf_Error(s_treeError)); jwC(&j, '}');
    return j.failed ? WF_IOERROR : j.n;
}

static WfPath s_upPath; static char s_upTemp[EOS_WF_PATH], s_upBackup[EOS_WF_PATH];
static HANDLE s_upHandle = INVALID_HANDLE_VALUE; static FIL s_upSdFile; static int s_upSdOpen = 0;
static int s_upActive = 0, s_upOverwrite = 0, s_upExpected = 0, s_upWritten = 0;
static int renameFile(const char* oldp, const char* newp, int sd)
{
    return sd ? (f_rename(oldp, newp) == FR_OK) : (MoveFileA(oldp, newp) != 0);
}
void Wf_UploadAbort(void)
{
    if (s_upActive) {
        if (s_upSdOpen) { f_close(&s_upSdFile); s_upSdOpen = 0; }
        if (s_upHandle != INVALID_HANDLE_VALUE) { CloseHandle(s_upHandle); s_upHandle = INVALID_HANDLE_VALUE; }
        if (s_upTemp[0])removeFile(s_upTemp, s_upPath.sd);
    }
    s_upActive = 0; s_upTemp[0] = 0; s_upBackup[0] = 0;
}
int Wf_UploadBegin(const char* virt, int count, int overwrite)
{
    if (Wf_JobBusy() || s_upActive)return WF_BUSY;
    WfPath p; int r = getPath(virt, &p, 1); if (r != WF_OK)return r;
    if (count < 0)return WF_BADPATH;
    if (weq(p.native, p.base) || isDir(&p))return WF_DENIED;
    if (existsFile(&p) && !overwrite)return WF_EXISTS;
    // Fixed short staging basenames preserve FATX's 42-character filename
    // allowance. Appending a suffix to the requested filename would make many
    // otherwise-valid FATX filenames impossible to upload.
    int k = wlen(p.native) - 1;
    while (k >= 0 && p.native[k] != '/' && p.native[k] != '\\')--k;
    if (k < 0)return WF_BADPATH;
    if (weq(p.native + k + 1, "EOSUPLD.TMP") || weq(p.native + k + 1, "EOSBACK.BAK"))return WF_DENIED;
    char tmp[EOS_WF_PATH], bak[EOS_WF_PATH];
    wcopy(tmp, sizeof(tmp), p.native); tmp[k + 1] = 0;
    if (!wcat(tmp, sizeof(tmp), "EOSUPLD.TMP"))return WF_BADPATH;
    wcopy(bak, sizeof(bak), p.native); bak[k + 1] = 0;
    if (!wcat(bak, sizeof(bak), "EOSBACK.BAK"))return WF_BADPATH;
    WfPath other = p; wcopy(other.native, sizeof(other.native), tmp);
    if (existsFile(&other))return WF_STALE;
    wcopy(other.native, sizeof(other.native), bak);
    if (existsFile(&other))return WF_STALE;
    if (p.sd) {
        if (f_open(&s_upSdFile, tmp, FA_WRITE | FA_CREATE_NEW) != FR_OK)return WF_IOERROR;
        s_upSdOpen = 1;
    }
    else {
        s_upHandle = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_upHandle == INVALID_HANDLE_VALUE)return WF_IOERROR;
    }
    s_upPath = p; wcopy(s_upTemp, sizeof(s_upTemp), tmp); wcopy(s_upBackup, sizeof(s_upBackup), bak);
    s_upExpected = count; s_upWritten = 0; s_upOverwrite = overwrite ? 1 : 0; s_upActive = 1;
    return WF_OK;
}
int Wf_UploadWrite(const char* data, int len)
{
    if (!s_upActive || len<0 || len>s_upExpected - s_upWritten)return WF_INCOMPLETE;
    if (!len)return WF_OK;
    if (s_upPath.sd) { UINT w = 0; FRESULT fr = f_write(&s_upSdFile, data, (UINT)len, &w); if (fr != FR_OK || w != (UINT)len)return WF_IOERROR; }
    else { DWORD wr = 0; if (!WriteFile(s_upHandle, data, (DWORD)len, &wr, NULL) || wr != (DWORD)len)return WF_IOERROR; }
    s_upWritten += len; return WF_OK;
}
int Wf_UploadFinish(void)
{
    if (!s_upActive)return WF_INCOMPLETE;
    if (s_upWritten != s_upExpected) { Wf_UploadAbort(); return WF_INCOMPLETE; }
    int flushok = 1;
    if (s_upPath.sd) { flushok = f_sync(&s_upSdFile) == FR_OK; f_close(&s_upSdFile); s_upSdOpen = 0; }
    else { flushok = FlushFileBuffers(s_upHandle) != 0; CloseHandle(s_upHandle); s_upHandle = INVALID_HANDLE_VALUE; }
    if (!flushok) { Wf_UploadAbort(); return WF_IOERROR; }
    WfPath old = s_upPath;
    int hadOld = existsFile(&old);
    if (hadOld && !s_upOverwrite) { Wf_UploadAbort(); return WF_EXISTS; }
    if (hadOld && !renameFile(old.native, s_upBackup, old.sd)) { Wf_UploadAbort(); return WF_IOERROR; }
    if (!renameFile(s_upTemp, old.native, old.sd)) {
        if (hadOld)renameFile(s_upBackup, old.native, old.sd);
        Wf_UploadAbort(); return WF_IOERROR;
    }
    if (hadOld)removeFile(s_upBackup, old.sd);
    s_upActive = 0; s_upTemp[0] = 0; s_upBackup[0] = 0;
    return WF_OK;
}

static HANDLE s_download = INVALID_HANDLE_VALUE; static FIL s_downloadSd; static int s_downSdOpen = 0;
void Wf_DownloadClose(void)
{
    if (s_downSdOpen) { f_close(&s_downloadSd); s_downSdOpen = 0; }
    if (s_download != INVALID_HANDLE_VALUE) { CloseHandle(s_download); s_download = INVALID_HANDLE_VALUE; }
}
int Wf_DownloadOpen(const char* virt, unsigned long long* sizeOut, char* filename, int fnameCap)
{
    WfPath p; int r = getPath(virt, &p, 0); if (r != WF_OK)return r;
    if (isDir(&p))return WF_DENIED;
    Wf_DownloadClose();
    if (p.sd) {
        if (f_open(&s_downloadSd, p.native, FA_READ) != FR_OK)return WF_NOTFOUND;
        s_downSdOpen = 1; *sizeOut = (unsigned long long)f_size(&s_downloadSd);
    }
    else {
        s_download = CreateFileA(p.native, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_download == INVALID_HANDLE_VALUE)return WF_NOTFOUND;
        DWORD hi = 0, lo = GetFileSize(s_download, &hi);
        if (lo == 0xFFFFFFFFUL && GetLastError() != NO_ERROR) { Wf_DownloadClose(); return WF_IOERROR; }
        *sizeOut = ((unsigned long long)hi << 32) | lo;
    }
    const char* nm = p.native; for (int i = 0; p.native[i]; ++i)if (p.native[i] == '/' || p.native[i] == '\\')nm = p.native + i + 1;
    wcopy(filename, fnameCap, nm);
    return WF_OK;
}
int Wf_DownloadRead(char* out, int cap)
{
    if (s_downSdOpen) { UINT rd = 0; FRESULT fr = f_read(&s_downloadSd, out, (UINT)cap, &rd); if (fr != FR_OK)return -1; return (int)rd; }
    if (s_download != INVALID_HANDLE_VALUE) { DWORD rd = 0; if (!ReadFile(s_download, out, (DWORD)cap, &rd, NULL))return -1; return (int)rd; }
    return -1;
}
void Wf_Shutdown(void)
{
    Wf_UploadAbort(); Wf_DownloadClose(); treeClose(); s_treeState = 0; s_treeLevel = -1;
    for (int i = 0; i < 4; ++i)if (s_hdd1Mapped[i]) {
        char alias[7] = { '\\','?','?','\\',kHdd1Alias[i],':',0 }; STRING ln = { (USHORT)6,(USHORT)7,alias };
        IoDeleteSymbolicLink(&ln); s_hdd1Mapped[i] = 0;
    }
    s_hdd1Checked = 0;
}
