// eos_menu.cpp -- main menu logic + rendering.
#include "eos_menu.h"
#include "eos_gfx.h"
#include "eos_font.h"
#include "input.h"   // BTN_* masks only
#include "eos_ui.h"
#include "eos_splash.h"

static const char* s_items[EOS_MENU_COUNT] =
{
    "Launch Bank",
    "Bank Management",
    "Tools",
    "Settings",
    "About"
};

static int   s_sel = 0;

// Layout (design space; height tracks g_scrH for PAL).
#define ROW_Y0     160     // first item y (fits 5 rows above the footer)
#define ROW_DY     40      // item spacing
#define ROW_H      32
#define ROW_W      300
#define ROW_X      ((g_scrW - ROW_W) / 2)

void Menu_Init()
{
    s_sel = 0;
}

int Menu_Selected() { return s_sel; }

// rising-edge helper
static bool Pressed(WORD now, WORD prev, WORD mask)
{
    return ((now & mask) && !(prev & mask));
}

int Menu_Step(WORD now, WORD prev)
{
    int chosen = -1;

    if (Pressed(now, prev, BTN_DPAD_UP))
        s_sel = (s_sel + EOS_MENU_COUNT - 1) % EOS_MENU_COUNT;
    if (Pressed(now, prev, BTN_DPAD_DOWN))
        s_sel = (s_sel + 1) % EOS_MENU_COUNT;

    if (Pressed(now, prev, BTN_A))
        chosen = s_sel;

    return chosen;
}

void Menu_Draw()
{
    Splash_Draw(g_scrW / 2, 62, 84, EOS_WHITE);   // logo header (headroom for the column)

    Ui_Menu3D(s_items, EOS_MENU_COUNT, s_sel);     // shared 3D perspective menu

    Ui_Footer("D-PAD  MOVE      A  SELECT");
}