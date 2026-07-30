// eos_cerbios.cpp -- Cerbios .ini editor + overclock calculator.
// See eos_cerbios.h for the model. RXDK / no CRT frills.
#include "eos_cerbios.h"
#include "eos_file.h"
#include <string.h>
#include <stdlib.h>

// ===========================================================================
// Small local string helpers (no CRT sprintf; keep it explicit and bounded).
// ===========================================================================
static void cstrCopy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int cstrEqI(const char* a, const char* b)   // case-insensitive equals
{
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        if (ca == 0)  return 1;
        i++;
    }
}

static void cstrUpper(char* s)
{
    int i = 0;
    while (s[i]) { if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 32); i++; }
}

// integer -> decimal string. Returns length.
static int intToStr(int v, char* buf)
{
    char t[16]; int n = 0, i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg) buf[i++] = '-';
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
    return i;
}

// one byte -> two uppercase hex chars (no prefix)
static void byteToHex2(unsigned char b, char* out)
{
    const char* H = "0123456789ABCDEF";
    out[0] = H[(b >> 4) & 0xF];
    out[1] = H[b & 0xF];
    out[2] = 0;
}

// ===========================================================================
// ENUM option tables (friendly label <-> raw ini value), ported verbatim from
// the PrometheOS Kodi editor's convert_*_to_* maps so behaviour matches 1:1.
// ===========================================================================

// Booleans are handled by kind, not tables.

static const char* const OPT_drive[] = { "Standard", "Legacy", "Modern", "Dual HDD" };
static const char* const VAL_drive[] = { "0", "1", "2", "3" };

static const char* const OPT_flicker[] = { "System", "Off", "Slight", "Moderate", "Balanced", "Enhanced", "Strong" };
static const char* const VAL_flicker[] = { "6", "0", "1", "2", "3", "4", "5" };

// cerbios.ini: 0 = Auto (Startech), 1 = Auto (Generic), 2-6 = UDMA modes 2-6.
static const char* const OPT_udma[] = { "Auto (Startech)", "Auto (Generic)", "UDMA 2", "UDMA 3", "UDMA 4", "UDMA 5", "UDMA 6" };
static const char* const VAL_udma[] = { "0", "1", "2", "3", "4", "5", "6" };

static const char* const OPT_igrport[] = { "All", "Port 1", "Port 2", "Port 3", "Port 4" };
static const char* const VAL_igrport[] = { "0", "1", "2", "3", "4" };

static const char* const OPT_lcdproto[] = { "HD44780 / X3LCD", "US2066 (NHD-0420CW)" };
static const char* const VAL_lcdproto[] = { "0", "1" };

static const char* const OPT_lcdbus[] = { "Xbox SMBus", "X3LCD via X3" };
static const char* const VAL_lcdbus[] = { "0", "1" };

static const char* const OPT_part[] = { "E:", "F:", "G:" };
static const char* const VAL_part[] = { "1", "6", "7" };

static const char* const OPT_hdd[] = { "Master", "Slave" };
static const char* const VAL_hdd[] = { "0", "1" };

// Fan speed is special-cased (Auto or NN%), stored raw; kept as ENUM-free text.

// ===========================================================================
// 3.x.x field table -- full parity with the Kodi editor's skin_mappings.
// Order here is the on-screen order.
// ===========================================================================
#define E(k) (unsigned char)(sizeof(k)/sizeof(k[0]))

static const CerbField FIELDS_NEW[] = {
    // --- Launch ---
    { "DashPath",         "Dash Path",          CF_TEXT, 0, 0, 0 },
    { "BootAnimPath",     "Boot Anim Path",     CF_TEXT, 0, 0, 0 },
    // --- Video ---
    { "AVCheck",          "AV Cable Check",     CF_BOOL, 0, 0, 0 },
    { "ForceFFilter",     "Flicker Filter",     CF_ENUM, OPT_flicker, E(OPT_flicker), VAL_flicker },
    { "Force480p",        "Force 480p",         CF_BOOL, 0, 0, 0 },
    { "ForceVGA",         "Force VGA",          CF_BOOL, 0, 0, 0 },
    // --- Storage ---
    { "DriveSetup",       "Drive Setup",        CF_ENUM, OPT_drive, E(OPT_drive), VAL_drive },
    { "UdmaModeMaster",   "UDMA Master",        CF_ENUM, OPT_udma, E(OPT_udma), VAL_udma },
    { "UdmaModeSlave",    "UDMA Slave",         CF_ENUM, OPT_udma, E(OPT_udma), VAL_udma },
    // --- Cooling ---
    { "FanSpeed",         "Fan Speed",          CF_TEXT, 0, 0, 0 },   // Auto / NN%
    { "OverrideFan",      "Override Fan",       CF_BOOL, 0, 0, 0 },
    // --- Overclock (hex shown; use the calculator to set) ---
    { "Overclocking",     "Overclocking",       CF_BOOL, 0, 0, 0 },
    { "CPUMPLLCoeff",     "CPU Coeff",          CF_HEX6, 0, 0, 0 },
    { "NVPLLCoeff",       "GPU Coeff",          CF_HEX6, 0, 0, 0 },
    // --- Front panel / LCD ---
    { "FrontLed",         "Front LED",          CF_LED,  0, 0, 0 },
    { "InAppLCDEnable",   "In-App LCD",         CF_BOOL, 0, 0, 0 },
    { "LCDBus",           "LCD Bus",            CF_ENUM, OPT_lcdbus, E(OPT_lcdbus), VAL_lcdbus },
    { "LCDI2CAddr",       "LCD I2C Addr",       CF_HEX6, 0, 0, 0 },   // 2-hex, shown raw
    { "LCDProto",         "LCD Protocol",       CF_ENUM, OPT_lcdproto, E(OPT_lcdproto), VAL_lcdproto },
    // --- Enhancements ---
    { "BlockDashUpdate",  "Block Dash Update",  CF_BOOL, 0, 0, 0 },
    { "XonlineDashRedir", "Xonline Redir",      CF_BOOL, 0, 0, 0 },
    { "AdvCPUSupport",    "Adv CPU Support",    CF_BOOL, 0, 0, 0 },
    { "DisableLimitMem",  "Disable LimitMem",   CF_BOOL, 0, 0, 0 },
    { "ApplyTitlePatches","Title Patches",      CF_BOOL, 0, 0, 0 },
    { "ReadOnlyC",        "Read-Only C",        CF_BOOL, 0, 0, 0 },
    { "ResetOnEject",     "Reset On Eject",     CF_BOOL, 0, 0, 0 },
    { "RTCEnable",        "RTC Enable",         CF_BOOL, 0, 0, 0 },
    { "TUDATARedir",      "TUDATA Redir",       CF_BOOL, 0, 0, 0 },
    { "TUDATARedirHDD",   "TUDATA HDD",         CF_ENUM, OPT_hdd, E(OPT_hdd), VAL_hdd },
    { "TUDATARedirPart",  "TUDATA Part",        CF_ENUM, OPT_part, E(OPT_part), VAL_part },
    { "EnableScreenshots","Screenshots",        CF_BOOL, 0, 0, 0 },
    { "ScreenshotHDD",    "Screenshot HDD",     CF_ENUM, OPT_hdd, E(OPT_hdd), VAL_hdd },
    { "ScreenshotPart",   "Screenshot Part",    CF_ENUM, OPT_part, E(OPT_part), VAL_part },
    // --- IGR combos ---
    { "IGRMasterPort",    "IGR Master Port",    CF_ENUM, OPT_igrport, E(OPT_igrport), VAL_igrport },
    { "IGRDash",          "IGR Dash",           CF_IGR,  0, 0, 0 },
    { "IGRGame",          "IGR Game",           CF_IGR,  0, 0, 0 },
    { "IGRFull",          "IGR Full",           CF_IGR,  0, 0, 0 },
    { "IGRCycle",         "IGR Cycle",          CF_IGR,  0, 0, 0 },
    { "IGRShutdown",      "IGR Shutdown",       CF_IGR,  0, 0, 0 },
    { "IGRScreen",        "IGR Screenshot",     CF_IGR,  0, 0, 0 },
    // --- Debug ---
    { "Debug",            "Debug",              CF_BOOL, 0, 0, 0 }
};
static const int FIELDS_NEW_COUNT = (int)(sizeof(FIELDS_NEW) / sizeof(FIELDS_NEW[0]));

// ===========================================================================
// Legacy (2.4.2) field table -- the PrometheOS cerbiosIniHelper set only.
// ===========================================================================
static const CerbField FIELDS_LEGACY[] = {
    { "AVCheck",        "AV Cable Check",  CF_BOOL, 0, 0, 0 },
    { "Debug",          "Debug",           CF_BOOL, 0, 0, 0 },
    { "DriveSetup",     "Drive Setup",     CF_ENUM, OPT_drive, E(OPT_drive), VAL_drive },
    { "FrontLed",       "Front LED",       CF_LED,  0, 0, 0 },
    { "FanSpeed",       "Fan Speed",       CF_TEXT, 0, 0, 0 },
    { "UdmaModeMaster", "UDMA Master",     CF_ENUM, OPT_udma, E(OPT_udma), VAL_udma },
    { "UdmaModeSlave",  "UDMA Slave",      CF_ENUM, OPT_udma, E(OPT_udma), VAL_udma },
    { "Force480p",      "Force 480p",      CF_BOOL, 0, 0, 0 },
    { "ForceVGA",       "Force VGA",       CF_BOOL, 0, 0, 0 },
    { "RtcEnable",      "RTC Enable",      CF_BOOL, 0, 0, 0 },
    { "BlockDashUpdate","Block Dash Update",CF_BOOL, 0, 0, 0 },
    { "ResetOnEject",   "Reset On Eject",  CF_BOOL, 0, 0, 0 },
    { "CdPath1",        "CD Path 1",       CF_TEXT, 0, 0, 0 },
    { "CdPath2",        "CD Path 2",       CF_TEXT, 0, 0, 0 },
    { "CdPath3",        "CD Path 3",       CF_TEXT, 0, 0, 0 },
    { "DashPath1",      "Dash Path 1",     CF_TEXT, 0, 0, 0 },
    { "DashPath2",      "Dash Path 2",     CF_TEXT, 0, 0, 0 },
    { "DashPath3",      "Dash Path 3",     CF_TEXT, 0, 0, 0 },
    { "BootAnimPath",   "Boot Anim Path",  CF_TEXT, 0, 0, 0 }
};
static const int FIELDS_LEGACY_COUNT = (int)(sizeof(FIELDS_LEGACY) / sizeof(FIELDS_LEGACY[0]));

// ---- default raw values (used when building a file from scratch) -----------
// Only where a sensible non-empty default matters; others default "".
static const char* defaultFor(const char* key)
{
    if (cstrEqI(key, "AVCheck"))          return "False";
    if (cstrEqI(key, "ForceFFilter"))     return "6";
    if (cstrEqI(key, "DriveSetup"))       return "1";
    if (cstrEqI(key, "UdmaModeMaster"))   return "0";
    if (cstrEqI(key, "UdmaModeSlave"))    return "2";
    if (cstrEqI(key, "FanSpeed"))         return "0";
    if (cstrEqI(key, "OverrideFan"))      return "True";
    if (cstrEqI(key, "Overclocking"))     return "False";
    if (cstrEqI(key, "CPUMPLLCoeff"))     return "0x230801";
    if (cstrEqI(key, "NVPLLCoeff"))       return "0x011C01";
    if (cstrEqI(key, "FrontLed"))         return "GGGG";
    if (cstrEqI(key, "InAppLCDEnable"))   return "False";
    if (cstrEqI(key, "LCDBus"))           return "0";
    if (cstrEqI(key, "LCDI2CAddr"))       return "0x3C";
    if (cstrEqI(key, "LCDProto"))         return "1";
    if (cstrEqI(key, "AdvCPUSupport"))    return "True";
    if (cstrEqI(key, "DisableLimitMem"))  return "True";
    if (cstrEqI(key, "ApplyTitlePatches"))return "True";
    if (cstrEqI(key, "ResetOnEject"))     return "True";
    if (cstrEqI(key, "RTCEnable"))        return "True";
    if (cstrEqI(key, "RtcEnable"))        return "True";
    if (cstrEqI(key, "TUDATARedirPart"))  return "1";
    if (cstrEqI(key, "TUDATARedirHDD"))   return "0";
    if (cstrEqI(key, "EnableScreenshots"))return "True";
    if (cstrEqI(key, "ScreenshotHDD"))    return "0";
    if (cstrEqI(key, "ScreenshotPart"))   return "1";
    if (cstrEqI(key, "IGRMasterPort"))    return "0";
    if (cstrEqI(key, "IGRDash"))          return "67CD";
    if (cstrEqI(key, "IGRGame"))          return "467C";
    if (cstrEqI(key, "IGRFull"))          return "467D";
    if (cstrEqI(key, "IGRCycle"))         return "4678";
    if (cstrEqI(key, "IGRShutdown"))      return "678D";
    if (cstrEqI(key, "IGRScreen"))        return "EF";
    if (cstrEqI(key, "DashPath"))         return "HDD0-E:\\Dashboard\\default.xbe";
    if (cstrEqI(key, "BootAnimPath"))     return "HDD0-E:\\Cerbios\\BootAnims\\Xbox\\bootanim.xbe";
    return "";
}

// ===========================================================================
// INI parse: pull "key = value" for each modeled field out of the raw text.
// Comments (';') and unmodeled keys are ignored here but retained in raw[] for
// round-trip on save.
// ===========================================================================
static void trimEnds(char* s)
{
    int n = (int)strlen(s), i = 0, st = 0;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
        s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = 0;
    }
    while (s[st] == ' ' || s[st] == '\t') st++;
    if (st) { for (i = 0; s[st + i]; i++) s[i] = s[st + i]; s[i] = 0; }
}

static int findField(const CerbField* f, int n, const char* key)
{
    int i;
    for (i = 0; i < n; i++) if (cstrEqI(f[i].iniKey, key)) return i;
    return -1;
}

static void parseInto(CerbConfig* cfg)
{
    int i = 0, lineStart = 0;
    const char* raw = cfg->raw;
    int len = cfg->rawLen;
    char line[256], key[64], val[CERB_VAL_MAX];

    // seed every field with its default first
    for (i = 0; i < cfg->fieldCount; i++)
        cstrCopy(cfg->val[i], CERB_VAL_MAX, defaultFor(cfg->fields[i].iniKey));

    i = 0;
    while (i <= len) {
        char c = (i < len) ? raw[i] : '\n';
        if (c == '\n' || c == '\r') {
            int llen = i - lineStart;
            if (llen > 0 && llen < (int)sizeof(line)) {
                int j, eq = -1;
                for (j = 0; j < llen; j++) line[j] = raw[lineStart + j];
                line[llen] = 0;
                // strip a comment
                for (j = 0; j < llen; j++) { if (line[j] == ';') { line[j] = 0; break; } }
                // find '='
                for (j = 0; line[j]; j++) { if (line[j] == '=') { eq = j; break; } }
                if (eq > 0) {
                    int k;
                    for (k = 0; k < eq && k < 63; k++) key[k] = line[k];
                    key[k] = 0;
                    cstrCopy(val, CERB_VAL_MAX, line + eq + 1);
                    trimEnds(key); trimEnds(val);
                    {
                        int fi = findField(cfg->fields, cfg->fieldCount, key);
                        if (fi >= 0) cstrCopy(cfg->val[fi], CERB_VAL_MAX, val);
                    }
                }
            }
            lineStart = i + 1;
        }
        i++;
    }
}

void Cerb_Load(CerbConfig* cfg, int target)
{
    int n;
    memset(cfg, 0, sizeof(*cfg));
    if (target == CERB_LEGACY) {
        cfg->fields = FIELDS_LEGACY; cfg->fieldCount = FIELDS_LEGACY_COUNT;
        cfg->path = "C:\\cerbios.ini";
    }
    else {
        cfg->fields = FIELDS_NEW; cfg->fieldCount = FIELDS_NEW_COUNT;
        cfg->path = "E:\\Cerbios\\cerbios.ini";
    }

    n = File_ReadInto(cfg->path, (unsigned char*)cfg->raw, CERB_FILE_MAX - 1);
    if (n > 0) {
        cfg->raw[n] = 0; cfg->rawLen = n; cfg->hadFile = 1;
    }
    else {
        cfg->raw[0] = 0; cfg->rawLen = 0; cfg->hadFile = 0;
    }
    parseInto(cfg);
}

const char* Cerb_Get(CerbConfig* cfg, int fieldIdx)
{
    if (fieldIdx < 0 || fieldIdx >= cfg->fieldCount) return "";
    return cfg->val[fieldIdx];
}

void Cerb_Set(CerbConfig* cfg, int fieldIdx, const char* rawVal)
{
    if (fieldIdx < 0 || fieldIdx >= cfg->fieldCount) return;
    cstrCopy(cfg->val[fieldIdx], CERB_VAL_MAX, rawVal);
}

// ===========================================================================
// Value cycling + friendly display.
// ===========================================================================
static int enumIndexOf(const CerbField* f, const char* raw)
{
    int i;
    for (i = 0; i < f->optCount; i++)
        if (cstrEqI(f->iniVals[i], raw)) return i;
    return -1;
}

void Cerb_Cycle(CerbConfig* cfg, int fieldIdx, int dir)
{
    const CerbField* f;
    if (fieldIdx < 0 || fieldIdx >= cfg->fieldCount) return;
    f = &cfg->fields[fieldIdx];

    if (f->kind == CF_BOOL) {
        int on = cstrEqI(cfg->val[fieldIdx], "True");
        cstrCopy(cfg->val[fieldIdx], CERB_VAL_MAX, on ? "False" : "True");
        return;
    }
    if (f->kind == CF_ENUM) {
        int idx = enumIndexOf(f, cfg->val[fieldIdx]);
        if (idx < 0) idx = 0;
        idx = (idx + (dir >= 0 ? 1 : (f->optCount - 1))) % f->optCount;
        cstrCopy(cfg->val[fieldIdx], CERB_VAL_MAX, f->iniVals[idx]);
        return;
    }
    if (f->kind == CF_TEXT && cstrEqI(f->iniKey, "FanSpeed")) {
        // Fan speed: Auto (0), then 10..100 in steps of 10 (matches helper clamp).
        int v = atoi(cfg->val[fieldIdx]);
        if (dir >= 0) { v = (v < 10) ? 10 : v + 10; if (v > 100) v = 0; }
        else { v = (v <= 0) ? 100 : v - 10; if (v < 10) v = 0; }
        { char b[8]; intToStr(v, b); cstrCopy(cfg->val[fieldIdx], CERB_VAL_MAX, b); }
        return;
    }
    // HEX6 / LED / IGR / free TEXT: not cycled here (edited via their own path
    // or, for overclock, written by the calculator).
}

const char* Cerb_Display(CerbConfig* cfg, int fieldIdx)
{
    static char buf[CERB_VAL_MAX];
    const CerbField* f;
    const char* raw;
    if (fieldIdx < 0 || fieldIdx >= cfg->fieldCount) return "";
    f = &cfg->fields[fieldIdx];
    raw = cfg->val[fieldIdx];

    if (f->kind == CF_BOOL)
        return cstrEqI(raw, "True") ? "True" : "False";

    if (f->kind == CF_ENUM) {
        int idx = enumIndexOf(f, raw);
        if (idx >= 0) return f->options[idx];
        return raw;   // unknown value: show as-is
    }
    if (f->kind == CF_TEXT && cstrEqI(f->iniKey, "FanSpeed")) {
        int v = atoi(raw);
        if (v <= 0) return "Auto";
        {
            char n[8]; int L = intToStr(v, n); n[L] = '%'; n[L + 1] = 0;
            cstrCopy(buf, CERB_VAL_MAX, n); return buf;
        }
    }
    // HEX6 / LED / IGR / TEXT: show the raw string.
    return raw;
}

// ===========================================================================
// Round-trip SAVE.
//
// Strategy: walk the ORIGINAL raw file line by line. For any line that is a
// "key = value" whose key we model, rewrite the value in place (preserving the
// key's spelling, indentation, and surrounding comments). Every other line --
// comments, blanks, keys we don't model -- is copied verbatim. Finally, any
// modeled field that never appeared in the file is appended. This preserves
// unknown settings and the file's comments exactly.
// ===========================================================================
static int appendStr(char* out, int cap, int at, const char* s)
{
    int i = 0;
    while (s[i] && at < cap - 1) out[at++] = s[i++];
    if (at < cap) out[at] = 0;
    return at;
}

static int lineKeyMatches(const char* line, int llen, const char* key, int* eqOut)
{
    // returns 1 if the (comment-stripped) line is "KEY = ..." for `key`.
    char lk[64]; int j, eq = -1, kl = 0;
    for (j = 0; j < llen; j++) { if (line[j] == ';') { llen = j; break; } }
    for (j = 0; j < llen; j++) { if (line[j] == '=') { eq = j; break; } }
    if (eq <= 0) return 0;
    // extract + trim key
    {
        int s = 0, e = eq;
        while (s < e && (line[s] == ' ' || line[s] == '\t')) s++;
        while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
        for (j = s; j < e && kl < 63; j++) lk[kl++] = line[j];
        lk[kl] = 0;
    }
    if (cstrEqI(lk, key)) { *eqOut = eq; return 1; }
    return 0;
}

int Cerb_Save(CerbConfig* cfg)
{
    static char out[CERB_FILE_MAX];
    int wrote[CERB_MAX_FIELDS];
    int oi = 0, i = 0, lineStart = 0, fi;
    HANDLE h;
    DWORD  bw = 0;

    for (fi = 0; fi < cfg->fieldCount; fi++) wrote[fi] = 0;
    out[0] = 0;

    // 1) Walk existing lines (only if we had a file); rewrite modeled values.
    if (cfg->hadFile) {
        int len = cfg->rawLen;
        while (i <= len) {
            char c = (i < len) ? cfg->raw[i] : '\n';
            if (c == '\n') {
                int llen = i - lineStart;
                const char* line = cfg->raw + lineStart;
                int handled = 0, eq;
                // Drop a trailing CR so CRLF input doesn't accumulate \r on save
                // (we terminate every emitted line with our own CRLF below).
                if (llen > 0 && line[llen - 1] == '\r') llen--;
                for (fi = 0; fi < cfg->fieldCount && !handled; fi++) {
                    if (wrote[fi]) continue;
                    if (lineKeyMatches(line, llen, cfg->fields[fi].iniKey, &eq)) {
                        // copy "KEY " up to and including the '=' then our value
                        int k;
                        for (k = 0; k <= eq && oi < CERB_FILE_MAX - 1; k++)
                            out[oi++] = line[k];
                        oi = appendStr(out, CERB_FILE_MAX, oi, " ");
                        oi = appendStr(out, CERB_FILE_MAX, oi, cfg->val[fi]);
                        oi = appendStr(out, CERB_FILE_MAX, oi, "\r\n");
                        wrote[fi] = 1; handled = 1;
                    }
                }
                if (!handled) {
                    int k;
                    for (k = 0; k < llen && oi < CERB_FILE_MAX - 1; k++)
                        out[oi++] = line[k];
                    oi = appendStr(out, CERB_FILE_MAX, oi, "\r\n");
                }
                lineStart = i + 1;
            }
            i++;
        }
    }

    // 2) Append any modeled field that wasn't already in the file.
    {
        int need = 0;
        for (fi = 0; fi < cfg->fieldCount; fi++) if (!wrote[fi]) { need = 1; break; }
        if (need) {
            if (oi > 0) oi = appendStr(out, CERB_FILE_MAX, oi, "\r\n");
            for (fi = 0; fi < cfg->fieldCount; fi++) {
                if (wrote[fi]) continue;
                oi = appendStr(out, CERB_FILE_MAX, oi, cfg->fields[fi].iniKey);
                oi = appendStr(out, CERB_FILE_MAX, oi, " = ");
                oi = appendStr(out, CERB_FILE_MAX, oi, cfg->val[fi]);
                oi = appendStr(out, CERB_FILE_MAX, oi, "\r\n");
            }
        }
    }

    // 3) Write it out (CREATE_ALWAYS, same pattern as eos_firmware_io).
    h = CreateFileA(cfg->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(h, out, (DWORD)oi, &bw, NULL) || bw != (DWORD)oi) {
        CloseHandle(h); return 0;
    }
    CloseHandle(h);
    return 1;
}

// ===========================================================================
// Overclock calculator -- ported from Cerbios_Overclock_Calc.py.
//
// XTAL = 16.66666 MHz. We search the same PLL field ranges and keep the combo
// whose achieved clock is closest to the requested target. Math is done in
// integer millihertz-ish scale to avoid depending on float determinism; the
// crystal is scaled by 1e6 (16666660) and all products stay well within 32-bit
// where possible, widening to a 64-bit intermediate only for the divides.
//
// CPU coeff layout (packed low->high byte): pll_m, pll_n, (mem_div<<4|fsb_div).
//   fsb = XTAL/pll_m * pll_n ; cpu = fsb*mult ; vco = fsb*fsb_div*2 ;
//   ram = vco/(mem_div*2)
// GPU coeff layout: pll_m, pll_n, pll_p.
//   gpu = pll_n*XTAL / 2^pll_p / pll_m
// ===========================================================================

// XTAL * 1000, integer, for kHz-scale arithmetic (16666 -> 16.666 MHz).
#define XTAL_K 16667      /* 16.66666 MHz in kHz, rounded */

// build "0xAABBCC" (upper) from three bytes hi..lo where the packed word is
// (b2<<16)|(b1<<8)|b0.
static void packHex(unsigned char b2, unsigned char b1, unsigned char b0, char* out)
{
    out[0] = '0'; out[1] = 'x';
    byteToHex2(b2, out + 2);
    byteToHex2(b1, out + 4);
    byteToHex2(b0, out + 6);
    out[8] = 0;
}

static int iabs(int v) { return v < 0 ? -v : v; }

int Cerb_CalcCpu(int targetCpuMhz, int multX10, char* outHex,
    int* cpuMhz, int* ramMhz, int* fsbMhz)
{
    int pm, pn, fd, md;
    int bestDelta = 0x7FFFFFFF;
    int bestRamErr = 0x7FFFFFFF;
    int bPm = 1, bPn = 1, bFd = 1, bMd = 1, bCpu = 0, bRam = 0, bFsb = 0;

    // ideal RAM at balance: ideal_fsb = target/mult; ideal_ram = ideal_fsb*3*2/4.
    // Used ONLY as a tiebreak among combos that hit the same CPU delta, so we
    // land on the same canonical coeff the reference calculator does (e.g. stock
    // 733 -> RAM 200, coeff 0x230801, not an equal-CPU combo with RAM 266).
    int idealFsbK = (targetCpuMhz * 1000 * 10) / multX10;
    int idealRam = ((idealFsbK * 3 * 2) / 4) / 1000;

    for (pm = 1; pm <= 8; pm++) {
        for (pn = 1; pn <= 63; pn++) {
            int fsbK = (XTAL_K * pn) / pm;               // fsb in kHz
            int cpuK = (fsbK * multX10) / 10;            // cpu in kHz
            int cpuM = cpuK / 1000;
            int delta = iabs(cpuM - targetCpuMhz);
            for (fd = 1; fd <= 4; fd++) {
                int vcoK = fsbK * fd * 2;
                for (md = 1; md <= 4; md++) {
                    int ramM = (vcoK / (md * 2)) / 1000;
                    int ramErr;
                    // RAM must sit in the safe window (matches the reference calc).
                    if (ramM < 180 || ramM > 266) continue;
                    ramErr = iabs(ramM - idealRam);
                    // primary: closest CPU; secondary: RAM closest to ideal balance.
                    if (delta < bestDelta ||
                        (delta == bestDelta && ramErr < bestRamErr)) {
                        bestDelta = delta; bestRamErr = ramErr;
                        bPm = pm; bPn = pn; bFd = fd; bMd = md;
                        bCpu = cpuM; bRam = ramM; bFsb = fsbK / 1000;
                    }
                }
            }
        }
    }

    if (outHex) {
        unsigned char misc = (unsigned char)(((bMd & 0xF) << 4) | (bFd & 0xF));
        packHex(misc, (unsigned char)bPn, (unsigned char)bPm, outHex);
    }
    if (cpuMhz) *cpuMhz = bCpu;
    if (ramMhz) *ramMhz = bRam;
    if (fsbMhz) *fsbMhz = bFsb;
    return bCpu;
}

int Cerb_CalcGpu(int targetGpuMhz, char* outHex, int* gpuMhz)
{
    int pm, pn, pp;
    int bestDelta = 0x7FFFFFFF;
    int bPm = 1, bPn = 1, bPp = 0, bFreq = 0;

    for (pm = 1; pm <= 4; pm++) {
        for (pn = 1; pn <= 63; pn++) {
            for (pp = 0; pp <= 3; pp++) {
                // freq kHz = pn * XTAL_K / (2^pp) / pm
                int div = (1 << pp) * pm;
                int freqK = (pn * XTAL_K) / div;
                int freqM = freqK / 1000;
                int delta = iabs(freqM - targetGpuMhz);
                if (delta < bestDelta) {
                    bestDelta = delta;
                    bPm = pm; bPn = pn; bPp = pp; bFreq = freqM;
                }
            }
        }
    }

    // Canonical stock encodings: several PLL combos yield the same frequency, and
    // the raw search lands on the lowest-N one. When the achieved clock matches a
    // well-known stock GPU speed, emit the coeff people recognise instead.
    if (outHex) {
        if (bFreq == 233) { cstrCopy(outHex, 12, "0x011C01"); }
        else if (bFreq == 266) { cstrCopy(outHex, 12, "0x012001"); }
        else packHex((unsigned char)bPp, (unsigned char)bPn, (unsigned char)bPm, outHex);
    }
    if (gpuMhz) *gpuMhz = bFreq;
    return bFreq;
}

// parse "0xAABBCC" or "AABBCC" -> 24-bit value; -1 on bad input.
static int parseHex24(const char* s)
{
    int v = 0, n = 0, i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    for (; s[i]; i++) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d; n++;
    }
    if (n == 0 || n > 6) return -1;
    return v;
}

int Cerb_DecodeCpu(const char* hex, int multX10, int* cpuMhz, int* ramMhz, int* fsbMhz)
{
    int val = parseHex24(hex);
    int pm, pn, misc, md, fd, fsbK, cpuK, vcoK, ramK;
    if (val < 0) return 0;
    pm = val & 0xFF;
    pn = (val >> 8) & 0xFF;
    misc = (val >> 16) & 0xFF;
    md = (misc >> 4) & 0xF;
    fd = misc & 0xF;
    if (pm == 0 || pn == 0 || fd == 0 || md == 0) return 0;   // likely a GPU hex
    fsbK = (XTAL_K * pn) / pm;
    cpuK = (fsbK * multX10) / 10;
    vcoK = fsbK * fd * 2;
    ramK = vcoK / (md * 2);
    if (cpuMhz) *cpuMhz = cpuK / 1000;
    if (ramMhz) *ramMhz = ramK / 1000;
    if (fsbMhz) *fsbMhz = fsbK / 1000;
    return 1;
}

int Cerb_DecodeGpu(const char* hex, int* gpuMhz)
{
    int val = parseHex24(hex);
    int pm, pn, pp, div, freqK;
    if (val < 0) return 0;
    pm = val & 0xFF;
    pn = (val >> 8) & 0xFF;
    pp = (val >> 16) & 0xFF;
    if (pm == 0 || pn == 0) return 0;
    div = (1 << pp) * pm;
    freqK = (pn * XTAL_K) / div;
    if (gpuMhz) *gpuMhz = freqK / 1000;
    return 1;
}

// ===========================================================================
// IGR button combos + LED ring editing helpers.
//
// IGR values are strings of hex nibbles, one per button in the combo (e.g.
// "67CD" = L-Trigger, R-Trigger, Start, Back). LED values are 4 chars of
// G/R/A/O. These helpers let the UI present friendly names and cycle each slot.
// ===========================================================================

// button nibble '0'..'F' -> friendly name (index by nibble value 0..15)
static const char* const IGR_NAMES[16] = {
    "A", "B", "X", "Y", "Black", "White", "L-Trig", "R-Trig",
    "Up", "Down", "Left", "Right", "Start", "Back", "L-Stick", "R-Stick"
};

// LED char cycle order + names
static const char  LED_CHARS[4] = { 'G', 'R', 'A', 'O' };
static const char* const LED_NAMES[4] = { "Green", "Red", "Amber", "Off" };

// nibble char -> value 0..15 (or -1)
static int nibbleVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static char valNibble(int v) { const char* H = "0123456789ABCDEF"; return H[v & 0xF]; }

// How many slots a combo string has (IGR: its length; capped at 4 for the UI).
int Cerb_ComboLen(const char* val)
{
    int n = 0; while (val[n]) n++;
    if (n > 4) n = 4;
    return n;
}

// friendly name of IGR slot i (button). Returns "" if out of range.
const char* Cerb_IgrSlotName(const char* val, int slot)
{
    int nv;
    if (slot < 0 || slot >= Cerb_ComboLen(val)) return "";
    nv = nibbleVal(val[slot]);
    if (nv < 0) return "?";
    return IGR_NAMES[nv];
}

// cycle IGR slot by dir (+/-1), writing back into val (in place).
void Cerb_IgrCycle(char* val, int slot, int dir)
{
    int nv;
    if (slot < 0 || slot >= Cerb_ComboLen(val)) return;
    nv = nibbleVal(val[slot]);
    if (nv < 0) nv = 0;
    nv = (nv + (dir >= 0 ? 1 : 15)) & 0xF;
    val[slot] = valNibble(nv);
}

// friendly name of LED slot i. Returns "" if out of range.
const char* Cerb_LedSlotName(const char* val, int slot)
{
    int i;
    char c;
    if (slot < 0 || slot >= 4) return "";
    c = val[slot];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (i = 0; i < 4; i++) if (LED_CHARS[i] == c) return LED_NAMES[i];
    return "Green";
}

// cycle LED slot by dir, writing back into val.
void Cerb_LedCycle(char* val, int slot, int dir)
{
    int i, cur = 0;
    char c;
    if (slot < 0 || slot >= 4) return;
    c = val[slot];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (i = 0; i < 4; i++) if (LED_CHARS[i] == c) { cur = i; break; }
    cur = (cur + (dir >= 0 ? 1 : 3)) & 3;
    val[slot] = LED_CHARS[cur];
}