// eos_font.cpp -- AA proportional font rendering from a baked glyph atlas.
//
// The atlas (white RGB, alpha = coverage) is uploaded once; each glyph is a
// 1:1 textured quad tinted by color and alpha-blended, so edges stay smooth.
// Layout is proportional -- use Font_TextWidth for alignment, not a fixed cell.
//
// RXDK / MSVC2003 / C89-ish: no CRT, no heap.
#include "eos_font.h"
#include "eos_font_data.h"

static IDirect3DTexture8* s_atlas = 0;
static DWORD              s_argb[FONT_ATLAS_W * FONT_ATLAS_H];

bool Font_Init()
{
    int i, n = FONT_ATLAS_W * FONT_ATLAS_H;
    for (i = 0; i < n; ++i)
        s_argb[i] = ((DWORD)FONT_ATLAS[i] << 24) | 0x00FFFFFF;   // white + coverage alpha
    s_atlas = Gfx_CreateTexARGB(FONT_ATLAS_W, FONT_ATLAS_H, s_argb);
    return (s_atlas != 0);
}

void Font_Shutdown()
{
    if (s_atlas) { s_atlas->Release(); s_atlas = 0; }
}

static const short* glyph(int c)
{
    if (c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT) return 0;
    return FONT_GLYPH[c - FONT_FIRST];
}

static int spaceAdv(void) { return FONT_GLYPH[' ' - FONT_FIRST][6]; }

int Font_TextWidth(const char* s)
{
    int w = 0, i;
    if (!s) return 0;
    for (i = 0; s[i]; ++i) {
        const short* g = glyph((unsigned char)s[i]);
        w += g ? g[6] : spaceAdv();
    }
    return w;
}

int Font_Draw(int x, int y, const char* s, DWORD color)
{
    int   cx = x, i;
    float aw = (float)FONT_ATLAS_W, ah = (float)FONT_ATLAS_H;
    if (!s) return x;
    for (i = 0; s[i]; ++i) {
        const short* g = glyph((unsigned char)s[i]);
        if (!g) { cx += spaceAdv(); continue; }
        if (g[2] > 0 && g[3] > 0) {                       // glyph has ink
            float u0 = (float)g[0] / aw, v0 = (float)g[1] / ah;
            float u1 = (float)(g[0] + g[2]) / aw, v1 = (float)(g[1] + g[3]) / ah;
            Gfx_DrawTex(s_atlas, (float)(cx + g[4]), (float)(y + g[5]),
                (float)g[2], (float)g[3], u0, v0, u1, v1, color);
        }
        cx += g[6];                                       // proportional advance
    }
    return cx;
}

int Font_DrawCentered(int x0, int width, int y, const char* s, DWORD color)
{
    int w = Font_TextWidth(s);
    int sx = x0 + (width - w) / 2;
    Font_Draw(sx, y, s, color);
    return sx;
}