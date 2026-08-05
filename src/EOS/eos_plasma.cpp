// eos_plasma.cpp -- Darkone83 theme soft-plasma backdrop. See eos_plasma.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT beyond
// malloc/free. POD only -- no global constructors.
#include "eos_plasma.h"
#include "eos_gfx.h"     // g_dev, g_scrW/H, Gfx_CreateTexARGB, Gfx_DrawTex, Gfx_SinCos, EOS_* palette
#include <xtl.h>

// POT tiling source. Integer harmonics (3,4,2,5 cycles) make it wrap seamlessly,
// so the scrolled fullscreen draws never show a seam. 256^2 RAM only (~256 KB).
#define PLZ_N     256
#define PLZ_TWO_PI 6.28318531f

static IDirect3DTexture8* s_tex = 0;
static int                s_ready = 0;
static int                s_tried = 0;

int Plasma_Init(void)
{
    DWORD* px;
    int x, y;

    if (s_ready) return 1;
    if (s_tried) return 0;
    s_tried = 1;
    if (!g_dev) return 0;

    px = (DWORD*)malloc((unsigned)(PLZ_N * PLZ_N) * sizeof(DWORD));
    if (!px) return 0;

    for (y = 0; y < PLZ_N; ++y) {
        float fy = (float)y / (float)PLZ_N;
        for (x = 0; x < PLZ_N; ++x) {
            float fx = (float)x / (float)PLZ_N;
            float s0, s1, s2, s3, v;
            int   iv;
            Gfx_SinCos(PLZ_TWO_PI * (3.0f * fx), &s0, 0);
            Gfx_SinCos(PLZ_TWO_PI * (4.0f * fy), &s1, 0);
            Gfx_SinCos(PLZ_TWO_PI * (2.0f * (fx + fy)), &s2, 0);
            Gfx_SinCos(PLZ_TWO_PI * (5.0f * (fx - fy)), &s3, 0);
            v = 0.5f + 0.5f * (0.34f * s0 + 0.30f * s1 + 0.22f * s2 + 0.14f * s3);
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            iv = (int)(v * 255.0f);                 // int<-float ok (no __ftol2 concern for storage path)
            if (iv < 0) iv = 0; else if (iv > 255) iv = 255;
            px[y * PLZ_N + x] = EOS_ARGB(0xFF, iv, iv, iv);  // grayscale; tinted at draw
        }
    }

    s_tex = Gfx_CreateTexARGB(PLZ_N, PLZ_N, px);
    free(px);
    if (!s_tex) return 0;
    s_ready = 1;
    return 1;
}

int Plasma_Ready(void) { return s_ready; }

void Plasma_Free(void)
{
    if (s_tex) { s_tex->Release(); s_tex = 0; }
    s_ready = 0;
}

void Plasma_Draw(float tsec)
{
    float w, h, t1, t2;
    float su1, sv1, su2, sv2;
    DWORD baseTint, hiTint;

    if (!s_ready) { if (!Plasma_Init()) return; }

    w = (float)g_scrW; h = (float)g_scrH;
    t1 = 1.60f;                 // texture repeats across the screen (base layer)
    t2 = 1.15f;                 // highlight layer repeats less -> larger, softer blobs
    su1 = tsec * 0.018f; sv1 = tsec * 0.012f;   // base drifts down-right
    su2 = -tsec * 0.015f; sv2 = tsec * 0.021f;   // highlights drift the other way

    // Tile the scrolled source. Default D3D address mode is WRAP; set it anyway
    // so a prior draw can't leave it clamped.
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);

    // Soft purple base, alpha-blended over the EOS_BG fill (kept low so it never
    // competes with the figure or the menu pills).
    baseTint = (EOS_PURPLE & 0x00FFFFFF) | 0x3A000000;   // ~23% alpha
    Gfx_DrawTex(s_tex, 0.0f, 0.0f, w, h, su1, sv1, su1 + t1, sv1 + t1, baseTint);

    // Fainter additive highlight veins drifting the other way -> plasma motion.
    hiTint = (EOS_GLOW & 0x00FFFFFF) | 0x22000000;       // ~13%, additive
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    Gfx_DrawTex(s_tex, 0.0f, 0.0f, w, h, su2, sv2, su2 + t2, sv2 + t2, hiTint);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}