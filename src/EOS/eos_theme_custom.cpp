/*---------------------------------------------------------------------------
    eos_theme_custom.cpp -- disk-loaded custom themes. See eos_theme_custom.h.

    Hand-rolled INI parse over a File_ReadInto'd buffer (no CRT string funcs):
    case-insensitive keys, LF or CRLF lines, whitespace trimmed, "#"/";" treated
    as comments ONLY when they lead a line (so "#RRGGBB" color values survive).
    Unknown keys are ignored, so hand-authored themes with extra keys still load.

    RXDK / MSVC2003 / C89-ish. Win32 file APIs (CreateFileA/WriteFile/
    CreateDirectoryA/DeleteFileA) are available on the Xbox, same as eos_file.cpp.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "eos_theme_custom.h"
#include "eos_theme.h"     /* g_theme, Theme_SetBgImage/ClearBg, Theme_ApplyDefaultPalette */
#include "eos_file.h"      /* File_ReadInto, File_Exists, EOS_FILE_NAME_MAX */

/* ---- module state --------------------------------------------------------- */
static char s_name[EOS_FILE_NAME_MAX] = { 0 };   /* display name (cosmetic)   */
static char s_active[EOS_FILE_NAME_MAX] = { 0 };   /* active folder name         */
static char s_musicPath[256] = { 0 };   /* full path to theme music   */
static int  s_hasMusic = 0;

int         ThemeCustom_HasMusic(void) { return s_hasMusic; }
const char* ThemeCustom_MusicPath(void) { return s_musicPath; }
const char* ThemeCustom_ActiveName(void) { return s_active; }

/* ---- small string helpers (no CRT) --------------------------------------- */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int ci_eq(const char* a, const char* b)
{
    int i = 0;
    for (;;) {
        char ca = lc(a[i]), cb = lc(b[i]);
        if (ca != cb) return 0;
        if (!ca) return 1;
        ++i;
    }
}

static void cpstr(char* dst, int cap, const char* src)
{
    int i = 0;
    if (cap <= 0) return;
    for (; src && src[i] && i < cap - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

static void copyRange(char* dst, int cap, const char* buf, int a, int b)
{
    int i = 0;
    for (; a < b && i < cap - 1; ++a, ++i) dst[i] = buf[a];
    if (cap > 0) dst[i] = 0;
}

static int appendStr(char* dst, int cap, int pos, const char* s)
{
    int i = pos;
    for (; s && *s && i < cap - 1; ++s, ++i) dst[i] = *s;
    if (i < cap) dst[i] = 0;
    return i;
}

/* "E:\Eos\Themes\" + folder + "\" + leaf */
static void buildThemePath(char* dst, int cap, const char* folder, const char* leaf)
{
    int p = 0;
    p = appendStr(dst, cap, p, "E:\\Eos\\Themes\\");
    p = appendStr(dst, cap, p, folder);
    p = appendStr(dst, cap, p, "\\");
    p = appendStr(dst, cap, p, leaf);
    (void)p;
}

static int hexNib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "#RRGGBB" or "RRGGBB" -> 0xFFRRGGBB. Leaves *out untouched on malformed. */
static void parseColor(const char* v, DWORD* out)
{
    int d[6], i;
    if (*v == '#') ++v;
    for (i = 0; i < 6; ++i) { d[i] = hexNib(v[i]); if (d[i] < 0) return; }
    *out = (DWORD)((0xFFu << 24)
        | ((d[0] * 16 + d[1]) << 16)
        | ((d[2] * 16 + d[3]) << 8)
        | (d[4] * 16 + d[5]));
}

static int parseIntv(const char* v)
{
    int n = 0, i = 0;
    while (v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); ++i; }
    return n;
}

/* --------------------------------------------------------------------------- */
void ThemeCustom_EnsureDir(void)
{
    CreateDirectoryA("E:\\Eos", NULL);
    CreateDirectoryA("E:\\Eos\\Themes", NULL);
}

int ThemeCustom_Apply(const char* folder)
{
    static char buf[8192];
    char iniPath[256];
    char bgFile[EOS_FILE_NAME_MAX];
    char musFile[EOS_FILE_NAME_MAX];
    int  n, off, dim;

    if (!folder || !folder[0]) return 0;

    buildThemePath(iniPath, sizeof(iniPath), folder, "theme.ini");
    n = File_ReadInto(iniPath, (unsigned char*)buf, sizeof(buf) - 1);
    if (n <= 0) return 0;                 /* no readable theme.ini -> invalid */
    buf[n] = 0;

    /* base = Eos default palette; missing color keys inherit purple */
    Theme_ApplyDefaultPalette();

    bgFile[0] = 0;
    musFile[0] = 0;
    dim = 0;
    s_name[0] = 0;

    off = 0;
    while (off < n) {
        int ls = off, le = off;
        char keyv[80], valv[192];

        while (le < n && buf[le] != '\n' && buf[le] != '\r') ++le;
        off = le;
        while (off < n && (buf[off] == '\n' || buf[off] == '\r')) ++off;

        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t')) ++ls;
        while (le > ls && (buf[le - 1] == ' ' || buf[le - 1] == '\t')) --le;
        if (ls >= le) continue;                           /* blank */
        if (buf[ls] == '#' || buf[ls] == ';') continue;   /* line-leading comment */

        {
            int eq = ls, ks, ke, vs, ve;
            while (eq < le && buf[eq] != '=') ++eq;
            if (eq >= le) continue;                        /* no '=' */
            ks = ls; ke = eq; vs = eq + 1; ve = le;
            while (ke > ks && (buf[ke - 1] == ' ' || buf[ke - 1] == '\t')) --ke;
            while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) ++vs;
            copyRange(keyv, sizeof(keyv), buf, ks, ke);
            copyRange(valv, sizeof(valv), buf, vs, ve);
        }

        if (ci_eq(keyv, "name"))       cpstr(s_name, sizeof(s_name), valv);
        else if (ci_eq(keyv, "background")) cpstr(bgFile, sizeof(bgFile), valv);
        else if (ci_eq(keyv, "music"))      cpstr(musFile, sizeof(musFile), valv);
        else if (ci_eq(keyv, "bg_dim"))     dim = parseIntv(valv);
        else if (ci_eq(keyv, "bg_top"))     parseColor(valv, &g_theme.bg);
        else if (ci_eq(keyv, "bg_bottom"))  parseColor(valv, &g_theme.bg2);
        else if (ci_eq(keyv, "panel"))      parseColor(valv, &g_theme.panel);
        else if (ci_eq(keyv, "accent"))     parseColor(valv, &g_theme.accent);
        else if (ci_eq(keyv, "glow"))       parseColor(valv, &g_theme.glow);
        else if (ci_eq(keyv, "text"))       parseColor(valv, &g_theme.white);
        else if (ci_eq(keyv, "text_dim"))   parseColor(valv, &g_theme.dim);
        /* version / author / bg_mode / orbs / unknown -> ignored */
    }

    if (dim < 0) dim = 0;
    if (dim > 100) dim = 100;
    if (s_name[0]) g_theme.name = s_name;   /* cosmetic; points at module storage */

    /* background: presence of a filename => image mode, else gradient/fill */
    if (bgFile[0]) {
        char bgPath[256];
        buildThemePath(bgPath, sizeof(bgPath), folder, bgFile);
        if (!Theme_SetBgImage(bgPath, dim)) Theme_ClearBg();  /* colors still applied */
    }
    else {
        Theme_ClearBg();
    }

    /* music: only "has music" if the named file actually exists (else the boot
       audio precedence falls through to the global BGM) */
    s_hasMusic = 0;
    s_musicPath[0] = 0;
    if (musFile[0]) {
        char mp[256];
        buildThemePath(mp, sizeof(mp), folder, musFile);
        if (File_Exists(mp)) { cpstr(s_musicPath, sizeof(s_musicPath), mp); s_hasMusic = 1; }
    }

    cpstr(s_active, sizeof(s_active), folder);
    return 1;
}

/* ---- scan / clear --------------------------------------------------------- */
int ThemeCustom_Scan(char out[][EOS_FILE_NAME_MAX], int maxN)
{
    static EosFileEntry ents[64];
    int n, i, count = 0;
    char ini[256];
    n = File_ListDir("E:\\Eos\\Themes", ents, 64);
    for (i = 0; i < n && count < maxN; ++i) {
        if (!ents[i].is_dir) continue;
        if (ents[i].name[0] == '.') continue;          /* skip . and .. */
        buildThemePath(ini, sizeof(ini), ents[i].name, "theme.ini");
        if (File_Exists(ini)) { cpstr(out[count], EOS_FILE_NAME_MAX, ents[i].name); ++count; }
    }
    return count;
}

void ThemeCustom_Clear(void)
{
    s_hasMusic = 0;
    s_musicPath[0] = 0;
    s_active[0] = 0;
}

/* ---- set.dat -------------------------------------------------------------- */
int SetDat_Read(char* outFolder, int cap)
{
    unsigned char buf[128];
    int n, i, vs, ve;

    if (cap > 0) outFolder[0] = 0;
    n = File_ReadInto("E:\\Eos\\set.dat", buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;

    for (i = 0; i < n && buf[i] != '='; ++i) {}
    if (i >= n) return 0;
    vs = i + 1; ve = vs;
    while (ve < n && buf[ve] != '\r' && buf[ve] != '\n') ++ve;
    while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) ++vs;
    while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) --ve;
    copyRange(outFolder, cap, (const char*)buf, vs, ve);
    return outFolder[0] ? 1 : 0;
}

void SetDat_Write(const char* folder)
{
    HANDLE h;
    DWORD  wr;
    char   line[96];
    int    p;

    h = CreateFileA("E:\\Eos\\set.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    p = appendStr(line, sizeof(line), 0, "theme=");
    p = appendStr(line, sizeof(line), p, folder ? folder : "");
    p = appendStr(line, sizeof(line), p, "\r\n");
    WriteFile(h, line, (DWORD)p, &wr, NULL);
    CloseHandle(h);
}

void SetDat_Clear(void)
{
    DeleteFileA("E:\\Eos\\set.dat");
}