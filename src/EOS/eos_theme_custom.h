// eos_theme_custom.h -- disk-loaded custom themes for the Eos loader.
//
// A custom theme is a folder under E:\Eos\Themes\<name>\ or
// SD:\Eos\Themes\<name>\ containing a theme.ini
// (the authority: colors + background/music filenames + bg_dim). The active
// custom source + folder identity are persisted in the EOS settings flash block,
// so an SD theme has no HDD dependency. Missing media falls back for that boot
// without clearing the saved selection.
//
// theme.ini keys (case-insensitive, LF or CRLF, "#"/";" line-leading comments):
//   name, background, music, bg_dim(0..100),
//   colors: bg_top, bg_bottom, panel, accent, glow, text, text_dim  (#RRGGBB)
// Unknown keys (version, author, bg_mode, orbs, ...) are ignored.
//
// RXDK / MSVC2003 / C89-ish: declarations before statements, no CRT strings.
#pragma once
#include <xtl.h>
#include "eos_file.h"   // EOS_FILE_NAME_MAX
#include "eos_config.h" // EOS_THEME_SOURCE_*

typedef struct EosThemeEntry {
    char name[EOS_FILE_NAME_MAX];
    unsigned char source;
} EosThemeEntry;

// Apply the explicitly selected source\Eos\Themes\<folder>\theme.ini:
// resets to the Eos default palette,
// overlays the ini's colors, sets the background image (or clears to gradient),
// and resolves the music file. Returns 1 on success, 0 if the theme is invalid
// (no readable theme.ini) -- caller simply stays on the built-in fallback.
int         ThemeCustom_Apply(const char* folder, int source);
int         ThemeCustom_ApplySaved(void);

// Music resolution for the boot audio precedence. HasMusic is 1 only when the
// active custom theme named a music file AND that file exists; MusicPath is then
// the full path. Both are cleared when no custom theme (or no/absent music).
int         ThemeCustom_HasMusic(void);
const char* ThemeCustom_MusicPath(void);

// Active custom theme folder name ("" if none applied this boot).
const char* ThemeCustom_ActiveName(void);

// List valid theme folders from HDD then SD (those containing a theme.ini).
// Source is retained, so identical folder names on HDD and SD remain distinct.
int         ThemeCustom_Scan(EosThemeEntry out[], int maxN);

// Drop the active custom-theme music/name state so the boot audio precedence
// falls back to the global BGM (used when switching to a built-in theme).
void        ThemeCustom_Clear(void);

