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

// Splash logo geometry, so the intro can start exactly where the splash left the
// logo (center, big) and land exactly on the header slot (top, small).
#define MENU_LOGO_X   (g_scrW / 2)
#define MENU_LOGO_HDR_Y   62
#define MENU_LOGO_HDR_SZ  84
#define MENU_LOGO_SPL_Y   (g_scrH / 2 - 20)
#define MENU_LOGO_SPL_SZ  256

// ilerp: integer lerp a->b by t in 0..255.
static int ilerp(int a, int b, int t)
{
    return a + (b - a) * t / 255;
}

// smoothstep s(x)=3x^2-2x^3 (x=t/255), returned on 0..255 so the settle eases
// in AND out. Peak intermediate is 3*255^3 = 49.7M, well within 32-bit int, so
// no wide types are needed. Verified: 0->0, 128->128, 255->255, monotonic.
static int ease255(int t)
{
    int num, r;
    if (t <= 0)   return 0;
    if (t >= 255) return 255;
    num = 3 * t * t * 255 - 2 * t * t * t;
    r = num / (255 * 255);
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    return r;
}

void Menu_DrawIntro(int progress)
{
    int p, e, lx, ly, lsz;

    p = progress; if (p < 0) p = 0; else if (p > 255) p = 255;
    e = ease255(p);

    // Logo travels from splash placement -> header slot as the intro completes.
    lx = MENU_LOGO_X;
    ly = ilerp(MENU_LOGO_SPL_Y, MENU_LOGO_HDR_Y, e);
    lsz = ilerp(MENU_LOGO_SPL_SZ, MENU_LOGO_HDR_SZ, e);
    Splash_Draw(lx, ly, lsz, EOS_WHITE);

    // The menu list and footer fade/settle in over the SECOND half of the intro,
    // so the logo leads and the options arrive just after it lands. Below the
    // threshold we draw nothing for them; Ui_Menu3D already eases its own motion.
    if (p >= 128) {
        Ui_Menu3D(s_items, EOS_MENU_COUNT, s_sel);
        Ui_Footer("D-PAD  MOVE      A  SELECT");
    }
}

void Menu_Draw()
{
    Menu_DrawIntro(255);   // fully settled
}