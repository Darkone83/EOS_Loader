// eos_gfx.cpp -- D3D8 2D layer (NV2A-correct: swizzled A8R8G8B8 + XGSwizzleRect).
// Matches the proven XbTyrian/ScorchedXB/XbDiag pattern:
//   - swizzled D3DFMT_A8R8G8B8 textures (LIN_* does NOT render on NV2A)
//   - XGSwizzleRect to convert linear CPU pixels -> swizzled GPU texture
//   - FVF has NO DIFFUSE (NV2A locks on DrawPrimitiveUP w/ LIN+DIFFUSE); tint is
//     applied via D3DRS_TEXTUREFACTOR + D3DTA_TFACTOR instead.
#include "eos_gfx.h"

IDirect3DDevice8* g_dev = 0;
int         g_scrW = 640;   // DESIGN-space width  (layout authored here)
int         g_scrH = 480;   // DESIGN-space height (layout authored here)
BOOL        g_isWide = FALSE;
const char* g_videoMode = "480i";
// Backbuffer scale/offset: the 640x480 design space is scaled (pillarbox for HD)
// onto the real backbuffer, so 720p renders natively instead of a 480p upscale.
float       g_sx = 1.0f;
float       g_sy = 1.0f;
int         g_ox = 0;
int         g_oy = 0;
static IDirect3D8* s_d3d = 0;
static IDirect3DTexture8* s_white = 0;   // 1x1 white, for solid fills
static IDirect3DTexture8* s_glowTex = 0;   // 64x64 radial glow sprite
static DWORD              s_glowPix[64 * 64];
static DWORD              s_baseFilter = D3DTEXF_POINT;   // LINEAR when HD-scaled

// Pre-transformed, single-texture vertex. NO diffuse channel.
struct QVtx { float x, y, z, rhw; float u, v; };
#define QFVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static void SetState2D()
{
    g_dev->SetVertexShader(QFVF);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // tint = texture * TFACTOR (TFACTOR set per-draw to the requested color)
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    // Default filter: POINT at native 480 (pixel-exact), LINEAR when the design
    // space is scaled up to an HD backbuffer so glyphs/pills don't look blocky.
    g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, s_baseFilter);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, s_baseFilter);
}

// Per-draw filter override. linear=TRUE for the logo's smooth gradient; FALSE
// restores the base filter (POINT at 480, LINEAR when HD-scaled).
void Gfx_SetFilter(BOOL linear)
{
    DWORD m = linear ? D3DTEXF_LINEAR : s_baseFilter;
    g_dev->SetTextureStageState(0, D3DTSS_MINFILTER, m);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, m);
}

bool Gfx_Init()
{
    s_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!s_d3d) return false;

    // --- Detect the console's AV configuration (DarkDash/XbDiag pattern). ---
    // Honor the encoder's flags for signal type + aspect, but keep a fixed
    // 640-wide design-space backbuffer; the NV2A scales it to the real output.
    // Priority: 720p > 1080i > 480p > 576i (PAL-I 50Hz) > 480i (NTSC/PAL-M/PAL60).
    DWORD vidStd = XGetVideoStandard();
    DWORD vidFlags = XGetVideoFlags();

    BOOL  isPAL_I = (vidStd == XC_VIDEO_STANDARD_PAL_I);
    BOOL  hasPAL60 = (vidFlags & XC_VIDEO_FLAGS_PAL_60Hz) != 0;
    BOOL  truePAL50 = (isPAL_I && !hasPAL60);

    DWORD ppFlags = 0;
    int   bbW = 640;
    int   bbH = 480;

    if (vidFlags & XC_VIDEO_FLAGS_HDTV_720p) {
        ppFlags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
        bbW = 1280; bbH = 720; g_videoMode = "720p";
    }
    else if (vidFlags & XC_VIDEO_FLAGS_HDTV_1080i) {
        ppFlags = D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN;
        bbW = 1920; bbH = 1080; g_videoMode = "1080i";
    }
    else if (vidFlags & XC_VIDEO_FLAGS_HDTV_480p) {
        ppFlags = D3DPRESENTFLAG_PROGRESSIVE;
        g_videoMode = "480p";
    }
    else if (truePAL50) {
        // True PAL-I 576i 50Hz: 4:3 backbuffer is 640x576, interlaced.
        ppFlags = D3DPRESENTFLAG_INTERLACED;
        bbH = 576; g_videoMode = "576i";
    }
    else {
        // NTSC 480i / PAL-M / PAL-60: interlaced default, no flag.
        g_videoMode = "480i";
    }
    // g_isWide reflects the console's EEPROM aspect (16:9 vs 4:3). The HD signal
    // flag above is separate -- a 720p signal is physically 16:9, but the console
    // may still be set to 4:3, in which case we pillarbox the 4:3 content.
    if (vidFlags & XC_VIDEO_FLAGS_WIDESCREEN) {
        ppFlags |= D3DPRESENTFLAG_WIDESCREEN; g_isWide = TRUE;
    }

    // Design space is ALWAYS 640x480; the backbuffer is the native mode size.
    // SD (<=640 wide) fills the frame; HD pillarboxes the 4:3 design so the logo
    // stays round. All draws are scaled by (g_sx,g_sy) + offset (g_ox,g_oy).
    g_scrW = 640; g_scrH = 480;

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = bbW;
    pp.BackBufferHeight = bbH;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.EnableAutoDepthStencil = FALSE;
    pp.Flags = ppFlags;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr) || !g_dev) {
        // HD mode rejected (e.g. VRAM) -> fall back to 640x480 480p/480i.
        bbW = 640; bbH = 480; g_videoMode = "480p";
        pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
        pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
        hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
        if (FAILED(hr) || !g_dev) return false;
    }

    // Compute design->backbuffer scale, honoring the EEPROM aspect.
    // Integer offsets only -- no float->int casts (this project has no __ftol2_sse).
    //   SD               : render 1:1 to the buffer; the WIDESCREEN present flag
    //                      lets the TV stretch a 4:3 buffer for anamorphic 16:9.
    //   HD + widescreen  : fill the 16:9 frame (content uses the full width).
    //   HD + 4:3         : pillarbox the 4:3 design (bars on the sides).
    if (bbW <= 640) {
        g_sx = (float)bbW / 640.0f; g_sy = (float)bbH / 480.0f; g_ox = 0; g_oy = 0;
    }
    else if (g_isWide) {
        g_sx = (float)bbW / 640.0f; g_sy = (float)bbH / 480.0f; g_ox = 0; g_oy = 0;
    }
    else {
        int scaledW, scaledH;
        if (bbW * 480 < bbH * 640) {              // width-limited
            scaledW = bbW;                  scaledH = bbW * 480 / 640;
            g_sx = g_sy = (float)bbW / 640.0f;
        }
        else {                                  // height-limited (typical 16:9)
            scaledH = bbH;                  scaledW = bbH * 640 / 480;
            g_sx = g_sy = (float)bbH / 480.0f;
        }
        g_ox = (bbW - scaledW) / 2;               // integer math, no ftol
        g_oy = (bbH - scaledH) / 2;
    }
    s_baseFilter = (g_sx > 1.01f || g_sy > 1.01f) ? D3DTEXF_LINEAR : D3DTEXF_POINT;

    DWORD wpx = 0xFFFFFFFF;
    s_white = Gfx_CreateTexARGB(1, 1, &wpx);

    // Radial glow sprite: white RGB, smooth quadratic alpha falloff to 0 at the
    // edge. Drawn additive + LINEAR-scaled, it gives a smooth ambient light blob
    // (no concentric banding). Integer math only -- no __ftol2_sse.
    {
        int gx, gy, r2 = 32 * 32;
        for (gy = 0; gy < 64; ++gy)
            for (gx = 0; gx < 64; ++gx) {
                int dx = gx - 32, dy = gy - 32, d2 = dx * dx + dy * dy, a;
                if (d2 >= r2) a = 0;
                else { int lin = (r2 - d2) * 255 / r2; a = lin * lin / 255; }
                s_glowPix[gy * 64 + gx] = ((DWORD)a << 24) | 0x00FFFFFF;
            }
        s_glowTex = Gfx_CreateTexARGB(64, 64, s_glowPix);
    }
    return (s_white != 0);
}

void Gfx_Shutdown()
{
    if (s_glowTex) { s_glowTex->Release(); s_glowTex = 0; }
    if (s_white) { s_white->Release(); s_white = 0; }
    if (g_dev) { g_dev->Release();   g_dev = 0; }
    if (s_d3d) { s_d3d->Release();   s_d3d = 0; }
}

void Gfx_Begin(DWORD clear_argb)
{
    g_dev->Clear(0, NULL, D3DCLEAR_TARGET, clear_argb, 1.0f, 0);
    g_dev->BeginScene();
    SetState2D();
}

void Gfx_End()
{
    g_dev->EndScene();
    g_dev->Present(NULL, NULL, NULL, NULL);
}

// Swizzled A8R8G8B8 texture from a linear CPU ARGB buffer (XGSwizzleRect).
IDirect3DTexture8* Gfx_CreateTexARGB(int w, int h, const DWORD* pixels)
{
    IDirect3DTexture8* tex = 0;
    HRESULT hr = g_dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, 0, &tex);
    if (FAILED(hr) || !tex) return 0;

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) { tex->Release(); return 0; }
    // linear src (w*4 pitch) -> swizzled dst expected by NV2A
    XGSwizzleRect(pixels, w * 4, NULL, lr.pBits, w, h, NULL, 4);
    tex->UnlockRect(0);
    return tex;
}

static void DrawQuadUV(IDirect3DTexture8* tex, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, DWORD c)
{
    QVtx v[4];
    v[0].x = g_ox + x * g_sx;       v[0].y = g_oy + y * g_sy;       v[0].u = u0; v[0].v = v0;
    v[1].x = g_ox + (x + w) * g_sx;   v[1].y = g_oy + y * g_sy;       v[1].u = u1; v[1].v = v0;
    v[2].x = g_ox + x * g_sx;       v[2].y = g_oy + (y + h) * g_sy;   v[2].u = u0; v[2].v = v1;
    v[3].x = g_ox + (x + w) * g_sx;   v[3].y = g_oy + (y + h) * g_sy;   v[3].u = u1; v[3].v = v1;
    for (int i = 0; i < 4; ++i) { v[i].z = 0.0f; v[i].rhw = 1.0f; }
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, c);   // tint
    g_dev->SetTexture(0, tex);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QVtx));
}

void Gfx_DrawTex(IDirect3DTexture8* tex, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, DWORD color)
{
    DrawQuadUV(tex, x, y, w, h, u0, v0, u1, v1, color);
}

void Gfx_Fill(float x, float y, float w, float h, DWORD color)
{
    DrawQuadUV(s_white, x, y, w, h, 0, 0, 1, 1, color);
}

// Vertical gradient via horizontal bands (no FVF/pipeline change -- reuses the
// white-texture tint path). The TV's scaler blurs the band seams away.
void Gfx_FillVGradient(int x, int y, int w, int h, DWORD top, DWORD bottom)
{
    int   bands = h / 6; int i;
    int   a0 = (top >> 24) & 0xFF, r0 = (top >> 16) & 0xFF, g0 = (top >> 8) & 0xFF, b0 = top & 0xFF;
    int   a1 = (bottom >> 24) & 0xFF, r1 = (bottom >> 16) & 0xFF, g1 = (bottom >> 8) & 0xFF, b1 = bottom & 0xFF;
    if (bands < 2) bands = 2;
    for (i = 0; i < bands; ++i) {
        int by = y + (h * i) / bands;
        int by2 = y + (h * (i + 1)) / bands;
        DWORD c = ((DWORD)(a0 + (a1 - a0) * i / (bands - 1)) << 24) |
            ((DWORD)(r0 + (r1 - r0) * i / (bands - 1)) << 16) |
            ((DWORD)(g0 + (g1 - g0) * i / (bands - 1)) << 8) |
            ((DWORD)(b0 + (b1 - b0) * i / (bands - 1)));
        Gfx_Fill((float)x, (float)by, (float)w, (float)(by2 - by), c);
    }
}

// --- rounded pill ----------------------------------------------------------
// Corners are drawn as solid triangle fans (pure geometry) rather than a
// scaled/mirrored mask texture: UV-mirroring a corner mask with bilinear
// filtering leaves a half-texel offset that shows up as faint seams. Fans share
// exact integer edges with the body fills, so there are no seams at all.

// Unit quarter-arc, cos/sin * 1024 over 0..90 deg (9 samples = 8 facets).
static const int k_arc[9][2] = {
    {1024,0},{1004,200},{946,392},{851,569},{724,724},
    {569,851},{392,946},{200,1004},{0,1024}
};

static void cornerFan(int cx, int cy, int r, int sx, int sy, DWORD color)
{
    QVtx v[10];
    int i;
    v[0].x = g_ox + (float)cx * g_sx; v[0].y = g_oy + (float)cy * g_sy;   // fan hub
    for (i = 0; i < 9; ++i) {
        v[i + 1].x = g_ox + (float)(cx + sx * (r * k_arc[i][0]) / 1024) * g_sx;
        v[i + 1].y = g_oy + (float)(cy + sy * (r * k_arc[i][1]) / 1024) * g_sy;
    }
    for (i = 0; i < 10; ++i) { v[i].z = 0.0f; v[i].rhw = 1.0f; v[i].u = 0.0f; v[i].v = 0.0f; }
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, color);
    g_dev->SetTexture(0, s_white);
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 8, v, sizeof(QVtx));
}

void Gfx_FillRounded(int x, int y, int w, int h, int r, DWORD color)
{
    int maxr = (w < h ? w : h) / 2;
    if (r > maxr) r = maxr;
    if (r < 1) { Gfx_Fill((float)x, (float)y, (float)w, (float)h, color); return; }

    // Body: middle full-height band + the two side bands between the corners.
    Gfx_Fill((float)(x + r), (float)y, (float)(w - 2 * r), (float)h, color);
    Gfx_Fill((float)x, (float)(y + r), (float)r, (float)(h - 2 * r), color);
    Gfx_Fill((float)(x + w - r), (float)(y + r), (float)r, (float)(h - 2 * r), color);

    // Four rounded corners.
    cornerFan(x + r, y + r, r, -1, -1, color);   // TL
    cornerFan(x + w - r, y + r, r, +1, -1, color);   // TR
    cornerFan(x + r, y + h - r, r, -1, +1, color);   // BL
    cornerFan(x + w - r, y + h - r, r, +1, +1, color);   // BR
}

// Soft ambient glow blob: one additive, LINEAR-scaled draw of the radial glow
// sprite -- perfectly smooth (no concentric banding). color tints it, peak sets
// overall intensity (the sprite's alpha falloff does the shaping).
void Gfx_GlowSoft(int cx, int cy, int w, int h, DWORD color, int peak)
{
    DWORD tint = (color & 0x00FFFFFF) | ((DWORD)(peak & 0xFF) << 24);
    if (!s_glowTex) return;
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);   // additive
    Gfx_SetFilter(TRUE);                                    // smooth upscale
    Gfx_DrawTex(s_glowTex, (float)(cx - w / 2), (float)(cy - h / 2),
        (float)w, (float)h, 0.0f, 0.0f, 1.0f, 1.0f, tint);
    Gfx_SetFilter(FALSE);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}
void Gfx_GlowRounded(int x, int y, int w, int h, int r, DWORD color)
{
    static const int grow[4] = { 2, 6, 11, 17 };
    static const int alph[4] = { 0x2E, 0x1C, 0x11, 0x09 };
    DWORD t = GetTickCount();
    int   ph = (int)((t >> 3) & 0xFF);
    int   tri = (ph < 128) ? ph : (255 - ph);   // 0..127 triangle (~2s period)
    int   mul = 208 + (tri >> 1);               // ~208..271 (scaled /256)
    int   i, g;
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);          // additive
    for (i = 0; i < 4; ++i) {
        int   a = (alph[i] * mul) >> 8;
        DWORD c = (color & 0x00FFFFFF) | ((DWORD)(a & 0xFF) << 24);
        g = grow[i];
        Gfx_FillRounded(x - g, y - g, w + 2 * g, h + 2 * g, r + g, c);
    }
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);  // restore
}