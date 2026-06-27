// eos_ui.cpp -- shared menu chrome. See eos_ui.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_ui.h"
#include "eos_gfx.h"
#include "eos_font.h"


// Themed gradient backdrop + a faint accent sheen along the top edge for depth.
void Ui_Backdrop(void)
{
    // Solid base avoids the gradient's visible banding; soft additive light
    // blobs give the ambient glow (DarkDash bloom idea).
    Gfx_Fill(0, 0, (float)g_scrW, (float)g_scrH, EOS_BG);
    Gfx_GlowSoft(g_scrW / 2, 72, 520, 300, EOS_GLOW, 0x12);  // behind logo
    Gfx_GlowSoft(g_scrW / 2, g_scrH - 30, 600, 320, EOS_PURPLE, 0x0E);  // lower accent
    Gfx_GlowSoft(g_scrW - 60, g_scrH / 2, 360, 420, EOS_GLOW, 0x09);  // right edge
}

// Soft accent halo behind a selected pill.
static void selGlow(int x, int y, int w, int h, int r)
{
    Gfx_GlowRounded(x, y, w, h, r, EOS_GLOW);   // soft additive bloom halo
}

void Ui_TitleBar(const char* title)
{
    int tw = Font_TextWidth(title);
    int pw = tw + 64;
    int ph = 44;
    int px = (g_scrW - pw) / 2;
    int py = 22;
    selGlow(px, py, pw, ph, ph / 2);
    Gfx_FillRounded(px, py, pw, ph, ph / 2, EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, py + (ph - FONT_CH) / 2, title, EOS_WHITE);
}

void Ui_Footer(const char* hint)
{
    Font_DrawCentered(0, g_scrW, g_scrH - 52, hint, EOS_DIM);
}

void Ui_PillCentered(int x, int y, int w, int h, int r, int selected, const char* label)
{
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);
    Font_DrawCentered(x, w, y + (h - FONT_CH) / 2, label, selected ? EOS_WHITE : EOS_DIM);
}

void Ui_PillLeft(int x, int y, int w, int h, int r, int selected, const char* label)
{
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);
    Font_Draw(x + 18, y + (h - FONT_CH) / 2, label, selected ? EOS_WHITE : EOS_DIM);
}

void Ui_PillRow(int x, int y, int w, int h, int r, int selected, int dim,
    const char* label, const char* value)
{
    DWORD lcol = selected ? EOS_WHITE : (dim ? EOS_DIM : EOS_WHITE);
    int   ty = y + (h - FONT_CH) / 2;
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);
    Font_Draw(x + 20, ty, label, lcol);
    if (value) {
        int vx = x + w - 20 - Font_TextWidth(value);
        Font_Draw(vx, ty, value, selected ? EOS_WHITE : EOS_DIM);
    }
}