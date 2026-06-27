// eos_nvram.h -- Read/write the OS-section console settings (video, audio,
// language, DVD region) via the kernel.
//
// These all live in the EEPROM OS section, which the kernel checksums itself:
// writing a field with ExSaveNonVolatileSetting(index, ...) is the same path
// the stock dashboard uses and carries no brick risk -- no crypto, no factory
// bytes touched. (Factory fields -- serial, MAC, video standard, game region --
// are NOT here; those need the whole-EEPROM re-encrypt path.)
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#pragma once
#include <xtl.h>

// XC_VIDEO flag bits.
#define NV_VIDEO_WIDESCREEN  0x00010000
#define NV_VIDEO_720p        0x00020000
#define NV_VIDEO_1080i       0x00040000
#define NV_VIDEO_480p        0x00080000
#define NV_VIDEO_LETTERBOX   0x00100000
#define NV_VIDEO_60Hz        0x00400000

// XC_AUDIO flag bits.
#define NV_AUDIO_MONO        0x00000001
#define NV_AUDIO_SURROUND    0x00000002
#define NV_AUDIO_AC3         0x00010000
#define NV_AUDIO_DTS         0x00020000
#define NV_AUDIO_MODE_MASK   0x00000003

// Raw dword get/set. Getters return the current value (0 if the read fails).
// Setters persist immediately via the kernel and return TRUE on success.
DWORD Nvram_GetVideoFlags(void);
BOOL  Nvram_SetVideoFlags(DWORD flags);
DWORD Nvram_GetAudioFlags(void);
BOOL  Nvram_SetAudioFlags(DWORD flags);
DWORD Nvram_GetLanguage(void);
BOOL  Nvram_SetLanguage(DWORD lang);
DWORD Nvram_GetDvdRegion(void);
BOOL  Nvram_SetDvdRegion(DWORD region);

// Audio mode is the low 2 bits: 0 stereo, 1 mono, 2 surround.
int         Nvram_AudioMode(DWORD audioFlags);   // 0/1/2
const char* Nvram_AudioModeStr(int mode);
const char* Nvram_LanguageStr(DWORD lang);       // shared with the info panel