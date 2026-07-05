// eos_theme_custom.h -- disk-loaded custom themes for the Eos loader.
//
// A custom theme is a folder under E:\Eos\Themes\<name>\ containing a theme.ini
// (the authority: colors + background/music filenames + bg_dim). Selection is
// persisted OUT of flash in a tiny E:\Eos\set.dat ("theme=<folder>"), so picking
// a theme costs no flash wear and the last selection is restored on boot. If
// set.dat is absent or points at a broken theme, the loader falls back to the
// built-in theme index (still in flash).
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

// Create E:\Eos and E:\Eos\Themes if missing (harmless if they exist).
void        ThemeCustom_EnsureDir(void);

// Apply E:\Eos\Themes\<folder>\theme.ini: resets to the Eos default palette,
// overlays the ini's colors, sets the background image (or clears to gradient),
// and resolves the music file. Returns 1 on success, 0 if the theme is invalid
// (no readable theme.ini) -- caller should then clear set.dat and stay built-in.
int         ThemeCustom_Apply(const char* folder);

// Music resolution for the boot audio precedence. HasMusic is 1 only when the
// active custom theme named a music file AND that file exists; MusicPath is then
// the full path. Both are cleared when no custom theme (or no/absent music).
int         ThemeCustom_HasMusic(void);
const char* ThemeCustom_MusicPath(void);

// Active custom theme folder name ("" if none applied this boot).
const char* ThemeCustom_ActiveName(void);

// List valid theme folders (those containing a theme.ini) into out[0..N-1].
// Returns the count (<= maxN, spec cap 32).
int         ThemeCustom_Scan(char out[][EOS_FILE_NAME_MAX], int maxN);

// Drop the active custom-theme music/name state so the boot audio precedence
// falls back to the global BGM (used when switching to a built-in theme).
void        ThemeCustom_Clear(void);

// set.dat selection pointer at E:\Eos\set.dat.
int         SetDat_Read(char* outFolder, int cap);   // 1 if "theme=<folder>" present
void        SetDat_Write(const char* folder);        // write "theme=<folder>"
void        SetDat_Clear(void);                       // delete set.dat