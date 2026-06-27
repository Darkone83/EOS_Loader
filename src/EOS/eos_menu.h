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

int  Menu_Selected();   // current highlighted index