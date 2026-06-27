// eos_nvram.cpp -- OS-section console settings read/write. See eos_nvram.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_nvram.h"

#define XC_LANGUAGE     0x07
#define XC_VIDEO        0x08
#define XC_AUDIO        0x09
#define XC_DVD_REGION   0x12

extern "C" ULONG __stdcall ExQueryNonVolatileSetting(DWORD ValueIndex, DWORD* Type,
    PVOID Value, DWORD ValueLength,
    DWORD* ResultLength);
extern "C" ULONG __stdcall ExSaveNonVolatileSetting(DWORD ValueIndex, DWORD Type,
    PVOID Value, DWORD ValueLength);

static DWORD getDword(DWORD index)
{
    DWORD type = 0, size = 0, v = 0;
    if (ExQueryNonVolatileSetting(index, &type, &v, 4, &size) != 0) return 0;
    return v;
}

static BOOL setDword(DWORD index, DWORD v)
{
    // Type 4 = ULONG (REG_DWORD-equivalent) as the kernel expects for these.
    return ExSaveNonVolatileSetting(index, 4, &v, 4) == 0;
}

DWORD Nvram_GetVideoFlags(void) { return getDword(XC_VIDEO); }
BOOL  Nvram_SetVideoFlags(DWORD flags) { return setDword(XC_VIDEO, flags); }
DWORD Nvram_GetAudioFlags(void) { return getDword(XC_AUDIO); }
BOOL  Nvram_SetAudioFlags(DWORD flags) { return setDword(XC_AUDIO, flags); }
DWORD Nvram_GetLanguage(void) { return getDword(XC_LANGUAGE); }
BOOL  Nvram_SetLanguage(DWORD lang) { return setDword(XC_LANGUAGE, lang); }
DWORD Nvram_GetDvdRegion(void) { return getDword(XC_DVD_REGION); }
BOOL  Nvram_SetDvdRegion(DWORD region) { return setDword(XC_DVD_REGION, region); }

int Nvram_AudioMode(DWORD audioFlags)
{
    if (audioFlags & NV_AUDIO_SURROUND) return 2;
    if (audioFlags & NV_AUDIO_MONO)     return 1;
    return 0;
}

const char* Nvram_AudioModeStr(int mode)
{
    if (mode == 1) return "Mono";
    if (mode == 2) return "Surround";
    return "Stereo";
}

const char* Nvram_LanguageStr(DWORD lang)
{
    switch (lang) {
    case 1:  return "English";
    case 2:  return "Japanese";
    case 3:  return "German";
    case 4:  return "French";
    case 5:  return "Spanish";
    case 6:  return "Italian";
    case 7:  return "Korean";
    case 8:  return "Chinese";
    case 9:  return "Portuguese";
    }
    return "Not set";
}