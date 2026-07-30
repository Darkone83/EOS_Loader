// eos_menu.h -- EOS Loader main menu (Launch Bank / Bank Management / Settings).
// Selectable stubs for the POC; themable later.
#pragma once
#include "eos_gfx.h"

enum EosMenuId
{
    EOS_MENU_LAUNCH_BANK = 0,
    EOS_MENU_BANK_MGMT,
    EOS_MENU_TOOLS,
    EOS_MENU_SETTINGS,
    EOS_MENU_ABOUT,
    EOS_MENU_COUNT
};

void Menu_Init();

// Advance one frame from caller-supplied button state (edge-detected via prev).
// Returns chosen item id on A press, else -1. Caller owns PumpInput/GetButtons.
int  Menu_Step(unsigned short now, unsigned short prev);

// Render the menu (call between Gfx_Begin/Gfx_End).
void Menu_Draw();

// Render with an intro progress 0..255: at 0 the logo sits big and centered
// (matching the splash), at 255 it has settled into the small header slot and
// the menu list is fully in. Menu_Draw() is Menu_DrawIntro(255). Used for the
// splash -> menu hand-off so the two screens feel like one continuous motion.
void Menu_DrawIntro(int progress);

int  Menu_Selected();   // current highlighted index