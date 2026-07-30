// eos_cerbios.h -- Cerbios .ini config editor for the EOS loader.
//
// Two config targets, plus an overclock calculator:
//   3.x.x  (new)    -> E:\Cerbios\cerbios.ini   full field set (~40 fields)
//   Legacy (2.4.2)  -> C:\cerbios.ini           small field set (~20 fields)
//   Overclock calc  -> computes CPUMPLLCoeff / NVPLLCoeff, writes 3.x.x only
//
// Each editor: if the target .ini exists it is loaded (round-trip -- lines we
// don't model are preserved verbatim); if not, it is built from defaults. An
// explicit Save writes the file back.
//
// RXDK / MSVC2003 / C89-ish: declarations before statements, no CRT frills.
#pragma once
#include <xtl.h>

// ---- field kinds ----------------------------------------------------------
// How each row is presented / edited in the UI.
enum CerbFieldKind {
    CF_BOOL = 0,   // True / False toggle
    CF_ENUM,       // one of a fixed option list (DPAD L/R cycles)
    CF_HEX6,       // 6-digit hex coefficient (overclock), read-only display here
    CF_LED,        // 4-char LED ring (G/R/A/O each), edited per-char
    CF_IGR,        // 4-button in-game-reset combo (hex nibble each)
    CF_TEXT        // free path/text (display; edited elsewhere or left as-is)
};

// One editable field. `options`/`optCount` used for CF_ENUM. `iniKey` is the
// literal key as it appears in cerbios.ini.
typedef struct CerbField {
    const char* iniKey;     // e.g. "AVCheck"
    const char* label;      // e.g. "AV Cable Check"
    unsigned char      kind;       // CerbFieldKind
    const char* const* options;    // CF_ENUM: friendly option strings
    unsigned char      optCount;   // CF_ENUM: number of options
    const char* const* iniVals;    // CF_ENUM: matching raw ini values (parallel to options)
} CerbField;

// A loaded config: the raw file text (preserved for round-trip) plus the parsed
// current value string for each modeled field.
#define CERB_MAX_FIELDS   48
#define CERB_VAL_MAX      104     // longest value we store (paths ~99 + slack)
#define CERB_FILE_MAX     16384   // full commented cerbios.ini is ~9.3KB; 16K headroom

typedef struct CerbConfig {
    const CerbField* fields;                 // -> table (3.x.x or legacy)
    int              fieldCount;
    char             val[CERB_MAX_FIELDS][CERB_VAL_MAX];  // raw ini value per field
    // round-trip buffer: the original file, so unknown lines survive a save.
    char             raw[CERB_FILE_MAX];
    int              rawLen;
    int              hadFile;                // 1 = loaded from disk, 0 = built fresh
    const char* path;                   // target ini path (device form)
} CerbConfig;

// ---- which target -----------------------------------------------------------
enum CerbTarget { CERB_NEW = 0, CERB_LEGACY = 1 };

// ---- public API -------------------------------------------------------------
// Load target's ini (or defaults if absent). Always leaves cfg fully populated.
void Cerb_Load(CerbConfig* cfg, int target);

// Save cfg back to its path. Returns 1 on success. Preserves unmodeled lines.
int  Cerb_Save(CerbConfig* cfg);

// Field value access by index (the editor cycles these).
const char* Cerb_Get(CerbConfig* cfg, int fieldIdx);
void        Cerb_Set(CerbConfig* cfg, int fieldIdx, const char* rawVal);

// For CF_ENUM: advance the field's option by +1/-1 (wraps). No-op otherwise
// unless the kind provides its own cycling (BOOL toggles).
void        Cerb_Cycle(CerbConfig* cfg, int fieldIdx, int dir);

// Friendly display string for a field's current value (maps raw->option label).
const char* Cerb_Display(CerbConfig* cfg, int fieldIdx);

// ---- overclock calculator ---------------------------------------------------
// Returns the packed 6-hex coefficient string (e.g. "0x232304") for a CPU target,
// and fills the achieved CPU/RAM/FSB. multX10 = multiplier*10 (55 = 5.5x).
// Writes into out (>= 12 bytes). Returns achieved CPU MHz (rounded).
int  Cerb_CalcCpu(int targetCpuMhz, int multX10, char* outHex,
    int* cpuMhz, int* ramMhz, int* fsbMhz);

// GPU target -> NVPLLCoeff. Returns achieved GPU MHz.
int  Cerb_CalcGpu(int targetGpuMhz, char* outHex, int* gpuMhz);

// Decode a coefficient back to human values (for the editor's OC display).
// Returns 1 if it parses as a plausible CPU coeff, fills cpu/ram/fsb.
int  Cerb_DecodeCpu(const char* hex, int multX10, int* cpuMhz, int* ramMhz, int* fsbMhz);
int  Cerb_DecodeGpu(const char* hex, int* gpuMhz);

// ---- IGR combo + LED ring slot editing --------------------------------------
// IGR values are hex-nibble strings (one button per slot); LED values are 4x
// G/R/A/O. These present friendly slot names and cycle each slot in place.
int         Cerb_ComboLen(const char* val);
const char* Cerb_IgrSlotName(const char* val, int slot);
void        Cerb_IgrCycle(char* val, int slot, int dir);
const char* Cerb_LedSlotName(const char* val, int slot);
void        Cerb_LedCycle(char* val, int slot, int dir);