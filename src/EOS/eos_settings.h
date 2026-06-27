#pragma once
// eos_settings.h -- Eos loader Settings screen.
//
// Self-contained settings UI that mirrors the main menu layout (purple title
// bar, centered rows, footer hint). Holds three sub-screens: a settings list,
// a System Info panel (decoded EEPROM + video mode), and a live Theme picker.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#pragma once
#include <xtl.h>

void Settings_Enter(void);                  // reset to the list + (re)read EEPROM
// Step one frame: handle input and draw. Returns 1 when the user leaves Settings
// (back to the main menu), else 0. Caller supplies current + previous buttons.
int  Settings_Frame(WORD b, WORD prevBtn);