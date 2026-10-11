// eos_ui.cpp -- shared menu chrome. See eos_ui.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_ui.h"
#include "eos_gfx.h"
#include "eos_font.h"
#include "eos_model.h"   // Darkone 83 theme: 3D character backdrop
#include "eos_plasma.h"  // Darkone 83 theme: soft plasma backdrop filler
#include "eos_config.h" // persisted menu presentation choice
#include "input.h"      // existing D-pad edge masks


// ---- 3D parallax orb field --------------------------------------------
// Real billboards placed at varying depth in the perspective scene, drifting
// in X/Y. Parallax falls out of the projection for free (far orbs move less on
// screen). Colors are theme tokens; motion advances once per frame.

typedef struct Orb3 { float x, y, z, vx, vy, size; int peak, tier; } Orb3;

#define ORB_N 16

static Orb3     s_orb[ORB_N];
static int      s_orbInit = 0;
static DWORD    s_orbLast = 0;
static unsigned s_orbRng = 0x9E3779B9u;

static unsigned orbRnd(void) { s_orbRng = s_orbRng * 1664525u + 1013904223u; return s_orbRng; }
static float    orbF(void) { return (float)((orbRnd() >> 8) & 0xFFFF) / 65536.0f; }   // 0..1
static float    orbRange(float a, float b) { return a + (b - a) * orbF(); }

// RGB blend a -> b by num/den.
static DWORD orbBlend(DWORD a, DWORD b, int num, int den)
{
    int ar, ag, ab, br, bg2, bb;
    if (num < 0) num = 0;
    if (num > den) num = den;
    ar = (a >> 16) & 0xFF; ag = (a >> 8) & 0xFF; ab = a & 0xFF;
    br = (b >> 16) & 0xFF; bg2 = (b >> 8) & 0xFF; bb = b & 0xFF;
    ar += (br - ar) * num / den; ag += (bg2 - ag) * num / den; ab += (bb - ab) * num / den;
    return ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
}

static void orbSeed(Orb3* o, int i)
{
    int tier = (i < 5) ? 0 : (i < 11) ? 1 : 2;        // 5 far, 6 mid, 5 near (all behind menu)
    o->tier = tier;
    o->x = orbRange(-5.5f, 5.5f);
    o->y = orbRange(-3.6f, 3.6f);
    o->vx = orbRange(-0.5f, 0.5f);
    o->vy = orbRange(-0.2f, 0.2f);
    if (tier == 0) { o->z = orbRange(11.0f, 17.0f); o->size = orbRange(5.0f, 8.0f); o->peak = 0x4A; }
    else if (tier == 1) { o->z = orbRange(8.0f, 12.0f); o->size = orbRange(2.6f, 4.2f); o->peak = 0x80; }
    else { o->z = orbRange(5.5f, 8.0f); o->size = orbRange(1.2f, 2.2f); o->peak = 0xAC; }
}

static void orbsStep(void)
{
    DWORD now = GetTickCount();
    float dt;
    int i;
    if (!s_orbInit) { for (i = 0; i < ORB_N; ++i) orbSeed(&s_orb[i], i); s_orbInit = 1; s_orbLast = now; }
    dt = (float)(now - s_orbLast) / 1000.0f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.10f) dt = 0.10f;
    s_orbLast = now;
    for (i = 0; i < ORB_N; ++i) {
        Orb3* o = &s_orb[i];
        o->x += o->vx * dt;
        o->y += o->vy * dt;
        if (o->x < -6.5f) o->x = 6.5f; else if (o->x > 6.5f) o->x = -6.5f;
        if (o->y < -4.2f) o->y = 4.2f; else if (o->y > 4.2f) o->y = -4.2f;
    }
}

static void orbsDraw(void)
{
    int i;
    for (i = 0; i < ORB_N; ++i) {
        Orb3* o = &s_orb[i];
        DWORD c;
        if (o->tier == 0) c = orbBlend(EOS_PURPLE, EOS_BG2, 55, 100);   // far: sinks into bg
        else if (o->tier == 1) c = (i & 1) ? EOS_PURPLE : EOS_GLOW;     // mid: accent mix
        else c = EOS_GLOW;                                              // near: brightest
        Gfx_Orb3D(o->x, o->y, o->z, o->size, c, o->peak);
    }
}

// Solid base fill, then the real 3D parallax field behind everything.
void Ui_Backdrop(void)
{
    IDirect3DTexture8* bg = Theme_BgTex();
    if (bg) {
        // Fullscreen image backdrop. Swizzled POT texture; the used region is
        // the sub-rect (0,0)..(u1,v1), drawn over the whole 640x480 design
        // backbuffer which the NV2A scales to the output mode (480i/480p/720p).
        // Tint is pure white (0xFFFFFFFF), NOT EOS_WHITE -- the diffuse modulates
        // the texture and a themed near-white would tint the photo.
        int dim;
        Gfx_SetFilter(TRUE);   // smooth scale to the output mode
        Gfx_DrawTex(bg, 0, 0, (float)g_scrW, (float)g_scrH,
            0, 0, Theme_BgU1(), Theme_BgV1(), 0xFFFFFFFF);
        Gfx_SetFilter(FALSE);

        dim = Theme_BgDim();
        if (dim > 0)
            Gfx_Fill(0, 0, (float)g_scrW, (float)g_scrH,
                EOS_ARGB((dim * 255) / 100, 0, 0, 0));
        return;                // image mode: no orbs
    }

    // Default: flat fill, then the 3D parallax field behind everything.
    // The Darkone 83 theme swaps the orb field for the character mesh; if the
    // model can't be built (VB/IB/texture), Model_Init() returns 0 and we fall
    // straight back to the orbs -- the backdrop never fails to draw.
    Gfx_Fill(0, 0, (float)g_scrW, (float)g_scrH, EOS_BG);
    if (Theme_BgIsModel() && Model_Init()) {
        Plasma_Draw((float)GetTickCount() * 0.001f);   // soft plasma behind the figure
        Gfx_Begin3D();
        Model_Draw((float)GetTickCount() * 0.001f);
        Gfx_End3D();
    }
    else {
        orbsStep();
        Gfx_Begin3D();
        orbsDraw();
        Gfx_End3D();
    }
}

// Shared 2D text fitting for editor rows, titles, hints and status messages.
// Avoid reducing long values to illegible 5px text. Modest overflow still
// scales as before; exceptional overflow keeps a readable size with an
// explicit ellipsis. No changes to the proven 3D menu text renderers.
#define UI_FIT_MIN_SCALE 0.80f
#define UI_FIT_BUFFER    160

static void uiEllipsize(const char* text, int maxW, char* out)
{
    int n, lo, hi, mid, i;
    if (!text || maxW <= 0) { out[0] = 0; return; }
    n = 0;
    while (text[n] && n < UI_FIT_BUFFER - 4) ++n;
    // The output always ends with dots when its source needs truncation.
    lo = 0; hi = n;
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        for (i = 0; i < mid; ++i) out[i] = text[i];
        out[mid] = '.'; out[mid + 1] = '.'; out[mid + 2] = '.';
        out[mid + 3] = 0;
        if (Font_TextWidthScaled(out, UI_FIT_MIN_SCALE) <= maxW) lo = mid;
        else hi = mid - 1;
    }
    // Omit a dangling space before the ellipsis.
    while (lo > 0 && text[lo - 1] == ' ') --lo;
    for (i = 0; i < lo; ++i) out[i] = text[i];
    out[lo] = '.'; out[lo + 1] = '.'; out[lo + 2] = '.';
    out[lo + 3] = 0;
    if (Font_TextWidthScaled(out, UI_FIT_MIN_SCALE) > maxW) {
        // Extremely small regions receive as many dots as can fit.
        for (i = 2; i >= 0; --i) {
            out[i] = 0;
            if (Font_TextWidthScaled(out, UI_FIT_MIN_SCALE) <= maxW) break;
        }
    }
}

static void uiDrawFitted(int x, int width, int y, const char* text,
    DWORD color, int align) // 0=left, 1=center, 2=right
{
    int tw, drawW, sx;
    float k;
    char clipped[UI_FIT_BUFFER];
    const char* drawText;
    if (!text || !text[0] || width <= 0) return;
    tw = Font_TextWidth(text);
    if (tw <= width) {
        drawText = text; k = 1.0f; drawW = tw;
    }
    else {
        k = (float)width / (float)tw;
        if (k < UI_FIT_MIN_SCALE) {
            k = UI_FIT_MIN_SCALE;
            uiEllipsize(text, width, clipped);
            drawText = clipped;
        }
        else drawText = text;
        drawW = Font_TextWidthScaled(drawText, k);
    }
    if (!drawText[0]) return;
    sx = x;
    if (align == 1) sx = x + (width - drawW) / 2;
    else if (align == 2) sx = x + width - drawW;
    if (k == 1.0f) Font_Draw(sx, y, drawText, color);
    else Font_DrawScaled(sx, y, drawText, color, k);
}

void Ui_TextCenteredFit(int x0, int width, int y, const char* text, DWORD color)
{
    int inset;
    if (width <= 0) return;
    inset = width >= 72 ? 12 : 0;
    uiDrawFitted(x0 + inset, width - inset * 2, y, text, color, 1);
}

void Ui_TextLeftFit(int x, int y, int maxW, const char* text, DWORD color)
{
    uiDrawFitted(x, maxW, y, text, color, 0);
}

// Soft accent halo behind a selected pill. Both layers use the filtered radial
// sprite, avoiding the visible nested rounded outlines produced by geometry
// expansion while still keeping a tight highlight close to the face.
static void selGlow(int x, int y, int w, int h, int r)
{
    Gfx_GlowSoft(x + w / 2, y + h / 2, w + 48, h + 30, EOS_GLOW, 32);
    Gfx_GlowSoft(x + w / 2, y + h / 2, w + 18, h + 10 + r / 4, EOS_PURPLE, 18);
}

void Ui_TitleBar(const char* title)
{
    int tw, pw, maxW, ph, px, py;
    if (!title || !title[0]) return;
    tw = Font_TextWidth(title);
    maxW = g_scrW - 64; // preserve a 32px gutter at either edge
    if (maxW <= 0) return;
    pw = tw + 64;
    if (pw > maxW) pw = maxW;
    ph = 44;
    px = (g_scrW - pw) / 2;
    py = 22;

    Gfx_GlowSoft(px + pw / 2, py + ph / 2, pw + 58, ph + 34, EOS_GLOW, 30);
    Gfx_GlowSoft(px + pw / 2, py + ph / 2, pw + 24, ph + 14, EOS_PURPLE, 18);
    // One draw for the entire face: no cap/body boundary and no internal
    // sheen/edge strokes, so the title capsule cannot expose a seam.
    Gfx_FillCapsule(px, py, pw, ph, EOS_PURPLE);
    Ui_TextCenteredFit(px, pw, py + (ph - FONT_CH) / 2, title, EOS_WHITE);
}

// PrometheOS-inspired controller legend. Button symbols are generated from
// EOS' own 2D primitives: no external font/texture, BIOS data or new settings.
// This renderer is deliberately FOOTER-ONLY; the four menu layouts and their
// text-only entries remain untouched. Caller strings and button flows stay the
// same, while each recognized controller name becomes a colored glyph.
#define UI_FOOTER_MAX_PARTS 48
#define UI_FOOTER_MAX_WORD  64

enum UiFooterKind {
    UI_FOOTER_TEXT = 0, UI_FOOTER_A, UI_FOOTER_B, UI_FOOTER_X, UI_FOOTER_Y,
    UI_FOOTER_WHITE, UI_FOOTER_BLACK, UI_FOOTER_DPAD, UI_FOOTER_START
};

typedef struct UiFooterPart {
    const char* src;
    short       len;
    short       gap;
    short       naturalW;
    unsigned char kind;
} UiFooterPart;

static int uiFooterEquals(const char* text, int len, const char* literal)
{
    int i = 0;
    while (i < len && literal[i] && text[i] == literal[i]) ++i;
    return i == len && literal[i] == 0;
}

static int uiFooterKind(const char* text, int len)
{
    if (len == 1) {
        if (text[0] == 'A') return UI_FOOTER_A;
        if (text[0] == 'B') return UI_FOOTER_B;
        if (text[0] == 'X') return UI_FOOTER_X;
        if (text[0] == 'Y') return UI_FOOTER_Y;
    }
    if (uiFooterEquals(text, len, "WHITE")) return UI_FOOTER_WHITE;
    if (uiFooterEquals(text, len, "BLACK")) return UI_FOOTER_BLACK;
    if (uiFooterEquals(text, len, "D-PAD")) return UI_FOOTER_DPAD;
    if (uiFooterEquals(text, len, "START")) return UI_FOOTER_START;
    return UI_FOOTER_TEXT;
}

static int uiFooterGlyphWidth(int kind)
{
    if (kind == UI_FOOTER_START) return 38;
    if (kind == UI_FOOTER_WHITE || kind == UI_FOOTER_BLACK) return 23;
    if (kind == UI_FOOTER_DPAD) return 22;
    return 20; // A/B/X/Y
}

// Return the number of words; -1 requests the legacy plain-text fallback.
// Spaces between control groups are retained as a smaller, consistent gutter.
// All source tokens remain borrowed string slices: no allocation or side effects.
static int uiFooterParse(const char* hint, UiFooterPart* parts, int* totalW)
{
    const char* p = hint;
    int n = 0, total = 0;
    while (*p) {
        int spaces = 0, len = 0, kind, width, i;
        char word[UI_FOOTER_MAX_WORD];
        while (*p == ' ' || *p == '\t') { ++spaces; ++p; }
        if (!*p) break;
        if (n == UI_FOOTER_MAX_PARTS) return -1;
        parts[n].src = p;
        while (p[len] && p[len] != ' ' && p[len] != '\t') ++len;
        if (len <= 0 || len >= UI_FOOTER_MAX_WORD) return -1;
        kind = uiFooterKind(p, len);
        if (kind == UI_FOOTER_TEXT) {
            for (i = 0; i < len; ++i) word[i] = p[i];
            word[len] = 0;
            width = Font_TextWidth(word);
        }
        else width = uiFooterGlyphWidth(kind);
        parts[n].len = (short)len;
        parts[n].kind = (unsigned char)kind;
        parts[n].naturalW = (short)width;
        parts[n].gap = (short)(n == 0 ? 0 : spaces >= 3 ? 14 : 6);
        total += width + parts[n].gap;
        ++n;
        p += len;
    }
    *totalW = total;
    return n;
}

static void uiFooterGlyph(int kind, int x, int y, int h, float scale)
{
    int w = (int)((float)uiFooterGlyphWidth(kind) * scale + 0.5f);
    int d = (int)(18.0f * scale + 0.5f);
    int cx = x + w / 2, cy = y + h / 2;
    int px = cx - d / 2, py = cy - d / 2;
    DWORD face = 0, ink = 0xFF11131A;
    float letterK = 0.84f * scale;
    const char* label = 0;
    int letterW, letterY;

    if (d < 12) d = 12;
    if (kind == UI_FOOTER_DPAD) {
        int bar = d / 3;
        if (bar < 4) bar = 4;
        Gfx_FillRounded(cx - bar / 2, cy - d / 2, bar, d, 2, 0xFFBFC4D5);
        Gfx_FillRounded(cx - d / 2, cy - bar / 2, d, bar, 2, 0xFFBFC4D5);
        Gfx_FillRounded(cx - 2, cy - 2, 4, 4, 2, 0xFF51546A);
        return;
    }
    if (kind == UI_FOOTER_START) {
        // Neutral elongated Start button, not another colored face button.
        int ovalW = w - 2, ovalH = d - 2;
        Gfx_FillRounded(cx - ovalW / 2, cy - ovalH / 2, ovalW, ovalH,
            ovalH / 2, 0xFF71778A);
        Gfx_FillRounded(cx - (ovalW - 2) / 2, cy - (ovalH - 2) / 2,
            ovalW - 2, ovalH - 2, (ovalH - 2) / 2, 0xFF282B39);
        label = "START";
        letterK = 0.43f * scale;
        ink = 0xFFFFFFFF;
    }
    else {
        switch (kind) {
        case UI_FOOTER_A: face = 0xFF36BD54; label = "A"; break;
        case UI_FOOTER_B: face = 0xFFE24C55; label = "B"; ink = 0xFFFFFFFF; break;
        case UI_FOOTER_X: face = 0xFF397EDB; label = "X"; ink = 0xFFFFFFFF; break;
        case UI_FOOTER_Y: face = 0xFFF4CB45; label = "Y"; break;
        case UI_FOOTER_WHITE: face = 0xFFECEDF1; label = "W"; break;
        case UI_FOOTER_BLACK: face = 0xFF272B37; label = "BK";
            letterK = 0.55f * scale; ink = 0xFFFFFFFF; break;
        default: return;
        }
        // Soft 1px neutral outline keeps dark/white buttons distinct on any
        // user theme without producing heavy boxes or decorative plates.
        Gfx_FillRounded(px - 1, py - 1, d + 2, d + 2, (d + 2) / 2,
            0xFF9496A6);
        Gfx_FillRounded(px, py, d, d, d / 2, face);
    }
    if (label) {
        letterW = Font_TextWidthScaled(label, letterK);
        // DejaVu's glyph ink is roughly 12px high with a +2px top bearing.
        letterY = cy - (int)(8.0f * letterK + 0.5f);
        Font_DrawScaled(cx - letterW / 2, letterY, label, ink, letterK);
    }
}

void Ui_Footer(const char* hint)
{
    UiFooterPart parts[UI_FOOTER_MAX_PARTS];
    int n, naturalW, maxPanelW, innerW, pw, ph, px, py;
    int i, x, usedW;
    float scale, cursor;
    DWORD panel;

    if (!hint || !hint[0]) return;
    n = uiFooterParse(hint, parts, &naturalW);
    maxPanelW = g_scrW - 40;
    if (maxPanelW <= 30) return;
    ph = 30;
    py = g_scrH - 72;
    panel = (EOS_PANEL & 0x00FFFFFF) | 0xB8000000;
    if (n < 0 || n == 0 || naturalW <= 0) {
        // Unknown/oversized words never break a footer; retain old behavior.
        int tw = Font_TextWidth(hint);
        pw = tw + 42;
        if (pw > maxPanelW) pw = maxPanelW;
        px = (g_scrW - pw) / 2;
        Gfx_FillRounded(px, py, pw, ph, ph / 2, panel);
        Ui_TextCenteredFit(px, pw, py + (ph - FONT_CH) / 2, hint, EOS_DIM);
        return;
    }

    innerW = maxPanelW - 30;
    scale = 1.0f;
    if (naturalW > innerW) scale = (float)innerW / (float)naturalW;
    usedW = (int)((float)naturalW * scale + 0.5f);
    pw = usedW + 30;
    if (pw > maxPanelW) pw = maxPanelW;
    px = (g_scrW - pw) / 2;
    Gfx_FillRounded(px, py, pw, ph, ph / 2, panel);

    cursor = (float)(px + (pw - usedW) / 2);
    for (i = 0; i < n; ++i) {
        char word[UI_FOOTER_MAX_WORD];
        int j, tx;
        cursor += (float)parts[i].gap * scale;
        tx = (int)(cursor + 0.5f);
        if (parts[i].kind != UI_FOOTER_TEXT)
            uiFooterGlyph(parts[i].kind, tx, py, ph, scale);
        else {
            for (j = 0; j < parts[i].len; ++j) word[j] = parts[i].src[j];
            word[parts[i].len] = 0;
            Font_DrawScaled(tx,
                py + (int)(((float)ph - (float)FONT_CH * scale) * 0.5f),
                word, EOS_DIM, scale);
        }
        cursor += (float)parts[i].naturalW * scale;
    }
}

void Ui_StatusToast(const char* text)
{
    int tw, pw, ph, px, py;
    DWORD panel, edge;

    if (!text || !text[0]) return;
    tw = Font_TextWidth(text);
    pw = tw + 44;
    if (pw > g_scrW - 70) pw = g_scrW - 70;
    ph = 28;
    px = (g_scrW - pw) / 2;
    py = g_scrH - 116;
    panel = (EOS_PANEL & 0x00FFFFFF) | 0xE0000000;
    edge = (EOS_GLOW & 0x00FFFFFF) | 0x90000000;

    Gfx_GlowSoft(g_scrW / 2, py + ph / 2, pw + 28, ph + 22, EOS_GLOW, 36);
    Gfx_FillRounded(px, py, pw, ph, ph / 2, panel);
    Gfx_Fill((float)(px + 14), (float)(py + ph - 4), (float)(pw - 28), 1.0f, edge);
    Ui_TextCenteredFit(px, pw, py + (ph - FONT_CH) / 2, text, EOS_WHITE);
}

void Ui_ScrollBar(int x, int y, int h, int first, int visible, int total)
{
    int thumbH, travel, thumbY, maxFirst;
    DWORD track, thumb;

    if (total <= visible || visible <= 0 || h < 12) return;
    if (first < 0) first = 0;
    maxFirst = total - visible;
    if (first > maxFirst) first = maxFirst;
    thumbH = h * visible / total;
    if (thumbH < 22) thumbH = 22;
    if (thumbH > h) thumbH = h;
    travel = h - thumbH;
    thumbY = y + ((maxFirst > 0) ? (travel * first / maxFirst) : 0);
    track = (EOS_PANEL & 0x00FFFFFF) | 0x70000000;
    thumb = (EOS_GLOW & 0x00FFFFFF) | 0xE8000000;
    Gfx_FillRounded(x, y, 5, h, 2, track);
    Gfx_FillRounded(x, thumbY, 5, thumbH, 2, thumb);
}

// Transition hooks are intentionally retained as no-ops so phase/navigation
// call sites stay simple, but screen changes are immediate again.
void Ui_TransitionStart(void)
{
}

int Ui_TransitionActive(void)
{
    return 0;
}

void Ui_TransitionDraw(void)
{
}

void Ui_PillCentered(int x, int y, int w, int h, int r, int selected, const char* label)
{
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);
    Ui_TextCenteredFit(x, w, y + (h - FONT_CH) / 2, label, selected ? EOS_WHITE : EOS_DIM);
}

void Ui_PillLeft(int x, int y, int w, int h, int r, int selected, const char* label)
{
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);
    Ui_TextLeftFit(x + 18, y + (h - FONT_CH) / 2, w - 36, label,
        selected ? EOS_WHITE : EOS_DIM);
}

void Ui_PillRow(int x, int y, int w, int h, int r, int selected, int dim,
    const char* label, const char* value)
{
    DWORD lcol, vcol;
    int ty, lw, vw, inner, gap, svw, vx;
    float k;
    lcol = selected ? EOS_WHITE : (dim ? EOS_DIM : EOS_WHITE);
    vcol = selected ? EOS_WHITE : EOS_DIM;
    ty = y + (h - FONT_CH) / 2;
    if (selected) selGlow(x, y, w, h, r);
    Gfx_FillRounded(x, y, w, h, r, selected ? EOS_PURPLE : UI_PILL_BG);

    if (!value || !value[0]) {
        Ui_TextLeftFit(x + 20, ty, w - 40, label, lcol);
        return;
    }

    lw = Font_TextWidth(label);
    vw = Font_TextWidth(value);
    inner = w - 40;
    gap = 18;
    if (lw + vw + gap <= inner) {
        Font_Draw(x + 20, ty, label, lcol);
        Font_Draw(x + w - 20 - vw, ty, value, vcol);
        return;
    }

    k = (float)(inner - gap) / (float)(lw + vw);
    if (k >= UI_FIT_MIN_SCALE) {
        // Modest overflows retain the familiar proportional layout.
        Font_DrawScaled(x + 20, ty, label, lcol, k);
        svw = Font_TextWidthScaled(value, k);
        vx = x + w - 20 - svw;
        Font_DrawScaled(vx, ty, value, vcol, k);
    }
    else {
        // Very long bank names / setting values remain independently legible.
        // Give the value its natural width when possible, up to 55% of the row;
        // never let it force the label to become microscopic.
        int available, valueW, labelW;
        available = inner - gap;
        if (available <= 0) return;
        valueW = vw;
        if (valueW > available * 55 / 100) valueW = available * 55 / 100;
        labelW = available - valueW;
        if (labelW < 0) labelW = 0;
        uiDrawFitted(x + 20, labelW, ty, label, lcol, 0);
        uiDrawFitted(x + w - 20 - valueW, valueW, ty, value, vcol, 2);
    }
}

// Bank Management may reserve the left rail for read-only bank information.
// Normally this region is disabled and every renderer uses its original 640px
// design area. The scoped entry points below restore the default immediately
// after drawing/navigation so no other screen inherits Bank Management bounds.
static int s_menuRegionLeft = 0;
static int s_menuRegionRight = 0;

static void uiMenuRegionSet(int left, int right)
{
    if (left < 0) left = 0;
    if (right > g_scrW) right = g_scrW;
    if (right - left < 180) { s_menuRegionLeft = 0; s_menuRegionRight = 0; }
    else { s_menuRegionLeft = left; s_menuRegionRight = right; }
}

static int uiMenuRegionActive(void)
{
    return s_menuRegionRight > s_menuRegionLeft;
}

static int uiMenuRegionWidth(void)
{
    return uiMenuRegionActive() ? s_menuRegionRight - s_menuRegionLeft : g_scrW;
}

// Keep each 3D renderer's geometry and entry text unchanged. During the
// Bank Management menu draw ONLY, remap its horizontal projection into the
// right-hand safe region, with UNIFORM XY scaling (no squashed bubbles).
// The 2D chrome and background retain the full screen.
static void uiMenuBegin3D(void)
{
    Gfx_Begin3D();
    if (uiMenuRegionActive()) {
        D3DMATRIX projection;
        if (SUCCEEDED(g_dev->GetTransform(D3DTS_PROJECTION, &projection))) {
            const float scale = (float)uiMenuRegionWidth() / (float)g_scrW;
            projection._11 *= scale;
            projection._22 *= scale; // preserve spherical/pill geometry in 3D
            // D3D8 row-vector projection: _31 offsets clip-space X by Z.
            // NDC X maps into [left, right], while uniform scaling preserves
            // each layout's shape, tilt and Z-depth. Nothing persists beyond
            // Gfx_End3D; no custom viewport/scissor state is left behind.
            projection._31 = (float)(s_menuRegionLeft + s_menuRegionRight) /
                (float)g_scrW - 1.0f;
            g_dev->SetTransform(D3DTS_PROJECTION, &projection);
        }
    }
}

// ---- Shared 3D menu (perspective) --------------------------------------
// Per-channel ARGB blend (kept for receding-panel color fades).
static DWORD perspLerp(DWORD a, DWORD b, int num, int den)
{
    int aa, ar, ag, ab, ba, br, bg2, bb;
    if (num < 0) num = 0;
    if (num > den) num = den;
    aa = (a >> 24) & 0xFF; ar = (a >> 16) & 0xFF; ag = (a >> 8) & 0xFF; ab = a & 0xFF;
    ba = (b >> 24) & 0xFF; br = (b >> 16) & 0xFF; bg2 = (b >> 8) & 0xFF; bb = b & 0xFF;
    aa += (ba - aa) * num / den;
    ar += (br - ar) * num / den;
    ag += (bg2 - ag) * num / den;
    ab += (bb - ab) * num / den;
    return ((DWORD)aa << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
}

// Tuning knobs (world units / radians). Camera at the origin looking +Z.
#define M3_R        1.85f
#define M3_ZC       3.95f
#define M3_STEP     0.20f
#define M3_K        0.0052f
#define M3_HH       0.11f
#define M3_PAD      0.18f
#define M3_MAX_HW   1.42f
#define M3_FAN      4
#define M3_AMAX     1.20f
#define M3_SWAY     0.05f
#define M3_EASE     10.0f
#define M3_SEL_GLOW 120

static float s_selAnim = 0.0f;
static int   s_animCount = -1;
static DWORD s_animTick = 0;

// 3D wheel safe-area defaults in the 640x480 design coordinate system.
// Title pills end at y=66; the helper/footer pill begins at y=408.
#define M3_SAFE_TOP_DEFAULT    76
#define M3_SAFE_BOTTOM_DEFAULT 398
#define M3_SAFE_FADE           20

// Project a world-space Y/Z point into the same 640x480 design coordinates
// used by the 2D chrome. Gfx_Begin3D uses a 60-degree vertical FOV with
// proj._22=1.73205 and a 480-high viewport.
static float wheelProjectY(float y, float z)
{
    if (z <= 0.001f) return 240.0f;
    return 240.0f - (y * 1.73205f / z) * 240.0f;
}

static DWORD wheelScaleAlpha(DWORD c, float k)
{
    DWORD a;
    if (k <= 0.0f) return c & 0x00FFFFFF;
    if (k >= 1.0f) return c;
    a = (c >> 24) & 0xFF;
    a = (DWORD)((float)a * k);
    return (c & 0x00FFFFFF) | (a << 24);
}

// Returns 0..1 for how visible a receding pill should be inside the reserved
// carousel band. Fade is driven by the projected pill edges, so tilt/bob remain
// naturally accounted for.
static float wheelSafeFade(float yc, float zc, float ca, float sa, int topY, int bottomY)
{
    float y1, z1, y2, z2, p1, p2, top, bottom, kt, kb, k;
    y1 = yc + M3_HH * ca; z1 = zc + M3_HH * sa;
    y2 = yc - M3_HH * ca; z2 = zc - M3_HH * sa;
    p1 = wheelProjectY(y1, z1);
    p2 = wheelProjectY(y2, z2);
    top = (p1 < p2) ? p1 : p2;
    bottom = (p1 > p2) ? p1 : p2;

    kt = (top - (float)topY) / (float)M3_SAFE_FADE;
    kb = ((float)bottomY - bottom) / (float)M3_SAFE_FADE;
    k = (kt < kb) ? kt : kb;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return k;
}

static void wheelItem(const char* label, int i, int sel, float aSway, float selPulse,
    int topY, int bottomY)
{
    float a, aa, ca, sa, yc, zc, twh, hw, f, textK, safeFade;
    int ia, tw;
    DWORD pill, txt;

    a = (s_selAnim - (float)i) * M3_STEP + aSway;
    aa = (a < 0.0f) ? -a : a;
    if (aa > M3_AMAX) return;

    Gfx_SinCos(a, &sa, &ca);
    yc = M3_R * sa;
    zc = M3_ZC - M3_R * ca;
    tw = Font_TextWidth(label);
    textK = M3_K;
    twh = (float)tw * textK * 0.5f;
    if (twh + M3_PAD > M3_MAX_HW && tw > 0) {
        textK = ((M3_MAX_HW - M3_PAD) * 2.0f) / (float)tw;
        twh = (float)tw * textK * 0.5f;
    }
    hw = twh + M3_PAD;
    if (hw < M3_HH * 1.6f) hw = M3_HH * 1.6f;

    f = 1.0f - aa / M3_AMAX;
    if (f < 0.0f) f = 0.0f;
    ia = (int)(f * 100.0f);

    if (i == sel) {
        zc -= 0.075f + selPulse * 0.025f;
        hw += M3_HH * (0.10f + selPulse * 0.06f);
        if (hw > M3_MAX_HW + 0.06f) hw = M3_MAX_HW + 0.06f;
        pill = (EOS_PURPLE & 0x00FFFFFF) | 0x78000000;
        txt = EOS_WHITE | 0xFF000000;
    }
    else {
        int pa;
        pa = 0x22 + ia * 0x66 / 100;
        pill = (perspLerp(EOS_PANEL, EOS_BG, 100 - ia, 100) & 0x00FFFFFF) | ((DWORD)pa << 24);
        txt = (perspLerp(EOS_DIM, EOS_BG, 100 - ia, 100) & 0x00FFFFFF)
            | ((DWORD)(0x40 + ia * 0xBF / 100) << 24);

        safeFade = wheelSafeFade(yc, zc, ca, sa, topY, bottomY);
        if (safeFade <= 0.0f) return;
        pill = wheelScaleAlpha(pill, safeFade);
        txt = wheelScaleAlpha(txt, safeFade);
    }

    if (i == sel) {
        int glowPeak;
        glowPeak = M3_SEL_GLOW + (int)(selPulse * 38.0f);
        Gfx_GlowX3D(0.0f, yc, zc + 0.018f, ca, sa,
            hw + 0.19f, M3_HH + 0.11f, EOS_GLOW, glowPeak);
    }

    Gfx_PillX3D(0.0f, yc, zc, ca, sa, hw, M3_HH, pill);
    Font_Draw3D(0.0f, yc, zc, ca, sa, textK, label, txt);
}

static void Ui_MenuClassicBounded(const char** items, int count, int sel, int topY, int bottomY)
{
    DWORD now = GetTickCount();
    float dt, d, aSway, pulseSin, selPulse;
    int   ad, side, j;

    if (count <= 0) return;
    if (sel < 0) sel = 0; else if (sel >= count) sel = count - 1;

    // Advance the easing toward the selected index (snap on list change / wrap).
    if (s_animCount != count) { s_selAnim = (float)sel; s_animCount = count; s_animTick = now; }
    dt = (float)(now - s_animTick) / 1000.0f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.10f) dt = 0.10f;
    s_animTick = now;
    d = (float)sel - s_selAnim;
    if (d > 2.5f || d < -2.5f) s_selAnim = (float)sel;      // wrapped -- don't spin around
    else { float t = M3_EASE * dt; if (t > 1.0f) t = 1.0f; s_selAnim += d * t; }

    { float sv; Gfx_SinCos((float)now * 0.0011f, &sv, 0); aSway = M3_SWAY * sv; }   // slow idle rock
    Gfx_SinCos((float)now * 0.0024f, &pulseSin, 0);
    selPulse = (pulseSin + 1.0f) * 0.5f;

    uiMenuBegin3D();
    // Farthest (most rotated) first so nearer pills overlap on top.
    for (ad = M3_FAN; ad >= 1; --ad) {
        for (side = -1; side <= 1; side += 2) {
            j = sel + side * ad;
            if (j < 0 || j >= count) continue;
            wheelItem(items[j], j, sel, aSway, selPulse, topY, bottomY);
        }
    }
    wheelItem(items[sel], sel, sel, aSway, selPulse, topY, bottomY);        // selected last, on top
    Gfx_End3D();
}

// ---- Optional Dynamic Grid -----------------------------------------------
// Presentation only: uses the same entry indices and callbacks as Classic.
// Wider one-column cells are automatic for long labels and short 1-2 item
// menus, keeping text legible and the familiar Up/Down input useful.
#define GRID_MARGIN       48
#define GRID_GAP          12
#define GRID_CELL_H       40
#define GRID_ROW_STEP     49
#define GRID_EASE         14.0f

static float s_gridFx = 0.0f, s_gridFy = 0.0f;
static DWORD s_gridTick = 0;
static int s_gridReady = 0;
static int s_gridCount = -1, s_gridCols = 0;
static int s_gridTop = -1, s_gridBottom = -1, s_gridFirst = -1;
static const char** s_gridItems = 0;
static const char* s_gridFirstLabel = 0;

static int gridColumns(const char** items, int count)
{
    int i, w, maxText;
    if (count <= 2) return 1;
    w = (uiMenuRegionWidth() - (uiMenuRegionActive() ? 40 : GRID_MARGIN * 2)
        - GRID_GAP) / 2;
    maxText = w - 30;
    if (maxText < 40) return 1;
    for (i = 0; i < count; ++i)
        if (items[i] && Font_TextWidth(items[i]) > maxText) return 1;
    return 2;
}

// Bubble spatial navigation is implemented alongside its renderer below.
static int b3Navigate(const char** items, int count, int sel,
    int up, int down, int left, int right);
static int o3Navigate(int count, int sel, int up, int down, int left, int right);

// Navigation is deliberately separate from menu actions. Classic retains its
// exact previous Up/Down wrap and ignores Left/Right; the grid navigates the
// geometry shown on screen, never an invisible or incomplete cell.
int Ui_MenuNavigate(const char** items, int count, int sel, WORD now, WORD prev)
{
    int up, down, left, right, cols;
    if (count <= 0 || !items) return 0;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;
    up = ((now & BTN_DPAD_UP) && !(prev & BTN_DPAD_UP));
    down = ((now & BTN_DPAD_DOWN) && !(prev & BTN_DPAD_DOWN));
    left = ((now & BTN_DPAD_LEFT) && !(prev & BTN_DPAD_LEFT));
    right = ((now & BTN_DPAD_RIGHT) && !(prev & BTN_DPAD_RIGHT));
    if (!up && !down && !left && !right) return sel;
    if (Config_GetMenuLayout() == 2)
        return b3Navigate(items, count, sel, up, down, left, right);
    if (Config_GetMenuLayout() == 3)
        return o3Navigate(count, sel, up, down, left, right);
    if (Config_GetMenuLayout() != 1) {
        if (up) sel = (sel + count - 1) % count;
        if (down) sel = (sel + 1) % count;
        return sel;
    }

    cols = gridColumns(items, count);
    if (up && sel >= cols) sel -= cols;
    if (down && sel + cols < count) sel += cols;
    else if (down && cols == 2 && sel / cols < (count - 1) / cols)
        sel = count - 1; // incomplete final row: choose its real last cell
    if (cols == 2) {
        if (left && (sel % 2) == 1) --sel;
        if (right && (sel % 2) == 0 && sel + 1 < count) ++sel;
    }
    return sel;
}

static void Ui_MenuGridBounded(const char** items, int count, int sel, int topY, int bottomY)
{
    int cols, rows, visible, maxVisible, first, row, i;
    int width, x0, cellW, y0, y, x, cellTextY;
    float targetX, targetY, dt, t;
    DWORD now, face;

    if (!items || count <= 0) return;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;
    if (bottomY <= topY + GRID_CELL_H) { topY = M3_SAFE_TOP_DEFAULT; bottomY = M3_SAFE_BOTTOM_DEFAULT; }
    cols = gridColumns(items, count);
    rows = (count + cols - 1) / cols;
    maxVisible = (bottomY - topY - GRID_CELL_H) / GRID_ROW_STEP + 1;
    if (maxVisible < 1) maxVisible = 1;
    visible = (rows < maxVisible) ? rows : maxVisible;
    first = sel / cols - visible / 2;
    if (first < 0) first = 0;
    if (first > rows - visible) first = rows - visible;

    width = uiMenuRegionWidth() - (uiMenuRegionActive() ? 40 : GRID_MARGIN * 2);
    if (width > 544) width = 544;
    if (width < 120) width = 120;
    cellW = (width - ((cols == 2) ? GRID_GAP : 0)) / cols;
    x0 = (uiMenuRegionActive() ? s_menuRegionLeft : 0) +
        (uiMenuRegionWidth() - width) / 2;
    y0 = topY + (bottomY - topY - (visible - 1) * GRID_ROW_STEP - GRID_CELL_H) / 2;
    targetX = (float)(x0 + (sel % cols) * (cellW + GRID_GAP));
    targetY = (float)(y0 + (sel / cols - first) * GRID_ROW_STEP);
    now = GetTickCount();

    // Snap only when entering a new menu, switching layouts or scrolling the
    // visible window. Between selections, move the focus smoothly and quickly.
    if (!s_gridReady || s_gridCount != count || s_gridCols != cols ||
        s_gridTop != topY || s_gridBottom != bottomY || s_gridFirst != first ||
        s_gridItems != items || s_gridFirstLabel != items[0] ||
        (DWORD)(now - s_gridTick) > 300) {
        s_gridFx = targetX; s_gridFy = targetY;
        s_gridReady = 1;
    }
    else {
        dt = (float)(now - s_gridTick) * 0.001f;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.08f) dt = 0.08f;
        t = GRID_EASE * dt;
        if (t > 1.0f) t = 1.0f;
        s_gridFx += (targetX - s_gridFx) * t;
        s_gridFy += (targetY - s_gridFy) * t;
    }
    s_gridTick = now;
    s_gridCount = count; s_gridCols = cols;
    s_gridTop = topY; s_gridBottom = bottomY; s_gridFirst = first;
    s_gridItems = items; s_gridFirstLabel = items[0];

    // All geometry remains inside the caller's protected title/footer band.
    face = (EOS_PANEL & 0x00FFFFFF) | 0xC0000000;
    for (row = first; row < first + visible; ++row) {
        y = y0 + (row - first) * GRID_ROW_STEP;
        for (i = row * cols; i < (row + 1) * cols && i < count; ++i) {
            x = x0 + (i % cols) * (cellW + GRID_GAP);
            Gfx_FillRounded(x, y, cellW, GRID_CELL_H, 11, face);
        }
    }

    Gfx_GlowSoft((int)s_gridFx + cellW / 2, (int)s_gridFy + GRID_CELL_H / 2,
        cellW + 30, GRID_CELL_H + 24, EOS_GLOW, 54);
    Gfx_FillRounded((int)s_gridFx, (int)s_gridFy, cellW, GRID_CELL_H, 11,
        (EOS_PURPLE & 0x00FFFFFF) | 0xE8000000);

    for (row = first; row < first + visible; ++row) {
        y = y0 + (row - first) * GRID_ROW_STEP;
        cellTextY = y + (GRID_CELL_H - FONT_CH) / 2;
        for (i = row * cols; i < (row + 1) * cols && i < count; ++i) {
            x = x0 + (i % cols) * (cellW + GRID_GAP);
            Ui_TextCenteredFit(x, cellW, cellTextY, items[i],
                (i == sel) ? EOS_WHITE : EOS_DIM);
        }
    }
    if (rows > visible)
        Ui_ScrollBar(x0 + width + 10, topY + 8, bottomY - topY - 16,
            first, visible, rows);
}

// ---- Floating Bubbles: procedural 3D constellation -----------------------
// Each distinct menu/page gets a deterministic, irregular spatial arrangement.
// Never a grid, never a rotating carousel. The same page remains stable across
// frames and visits; only restrained float/parallax and focus pressure animate.
// The selected sphere travels toward the camera while neighbors yield space.
// This renderer owns presentation, NOT menu indices, actions or boot behavior.
#define B3_PAGE_SIZE 6
#define B3_FOCAL     415.692f  // 240 * cot(60 degrees / 2)
#define B3_EASE      11.5f
#define B3_FOCUS_Z   3.35f

// Retain at most six drawn bubbles for legibility at 480i and 480p.
typedef struct BubbleAnchor {
    float x, y, z, diameter; // x/y are projected 640x480 design coordinates
} BubbleAnchor;

typedef struct BubbleNode {
    int index;
    float x, y, z, diameter;
    float focus;
} BubbleNode;

static float s_b3Focus[B3_PAGE_SIZE] = { 0, 0, 0, 0, 0, 0 };
static DWORD s_b3Tick = 0;
static int s_b3Count = -1, s_b3Page = -1;
static int s_b3Top = -1, s_b3Bottom = -1;
static const char** s_b3Items = 0;
static const char* s_b3FirstLabel = 0;

static float b3Min(float a, float b) { return a < b ? a : b; }
static float b3Abs(float a) { return a < 0.0f ? -a : a; }

// Local PRNG, never coupled to the backdrop RNG or the framebuffer timing.
// Hashing menu text means different menus get different constellations, but a
// given menu is never shuffled while a user is searching for an entry.
static unsigned int b3Seed(const char** items, int count, int first, int visible)
{
    unsigned int h;
    int i, j;
    h = 2166136261u;
    h = (h ^ (unsigned int)count) * 16777619u;
    h = (h ^ (unsigned int)first) * 16777619u;
    for (i = 0; i < visible; ++i) {
        const unsigned char* p = (const unsigned char*)items[first + i];
        if (p) {
            for (j = 0; j < 64 && p[j]; ++j)
                h = (h ^ (unsigned int)p[j]) * 16777619u;
        }
        h = (h ^ 0xFFu) * 16777619u;
    }
    return h ? h : 0xA83DE9B1u;
}

static float b3Random(unsigned int* state)
{
    *state = *state * 1664525u + 1013904223u;
    return (float)((*state >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}

// Organic scatter: candidate rejection keeps each sphere separated in its
// projected XY position. The depth is independent and varies substantially;
// there are no prescribed rows, columns, or six fixed coordinates.
static void b3Generate(const char** items, int count, int page,
    int topY, int bottomY, BubbleAnchor* out, int* outVisible)
{
    unsigned int state;
    float usableH;
    int first, n, i, t, j;
    first = page * B3_PAGE_SIZE;
    n = count - first;
    if (n < 0) n = 0;
    if (n > B3_PAGE_SIZE) n = B3_PAGE_SIZE;
    *outVisible = n;
    if (n <= 0) return;
    if (bottomY <= topY + 170) {
        topY = M3_SAFE_TOP_DEFAULT;
        bottomY = M3_SAFE_BOTTOM_DEFAULT;
    }
    if (n == 1) {
        out[0].x = 320.0f;
        out[0].y = (float)(topY + bottomY) * 0.5f;
        out[0].z = 5.7f;
        out[0].diameter = 1.05f;
        return;
    }
    state = b3Seed(items, count, first, n);
    usableH = (float)(bottomY - topY) - 164.0f;
    if (usableH < 70.0f) usableH = 70.0f;
    for (i = 0; i < n; ++i) {
        float best = -100000000.0f;
        BubbleAnchor chosen;
        chosen.x = 320.0f; chosen.y = (float)(topY + bottomY) * 0.5f;
        chosen.z = 6.0f; chosen.diameter = 1.04f;
        for (t = 0; t < 72; ++t) {
            BubbleAnchor c;
            float minMargin, ri;
            c.x = 95.0f + b3Random(&state) * 450.0f;
            c.y = (float)topY + 82.0f + b3Random(&state) * usableH;
            c.z = 5.0f + b3Random(&state) * 3.10f;
            c.diameter = 0.99f + b3Random(&state) * 0.14f;
            ri = c.diameter * 0.5f * B3_FOCAL / c.z;
            minMargin = 1000000.0f;
            for (j = 0; j < i; ++j) {
                float dx, dy, rj, separation, m;
                dx = c.x - out[j].x;
                dy = c.y - out[j].y;
                rj = out[j].diameter * 0.5f * B3_FOCAL / out[j].z;
                separation = ri + rj + 23.0f;
                m = dx * dx + dy * dy - separation * separation;
                if (m < minMargin) minMargin = m;
            }
            // Avoid near-identical horizontal tracks even when circles don't
            // overlap. This prevents the familiar two-column / row look.
            for (j = 0; j < i; ++j) {
                if (b3Abs(c.y - out[j].y) < 15.0f) minMargin -= 1000000.0f;
                if (b3Abs(c.x - out[j].x) < 26.0f) minMargin -= 1000000.0f;
            }
            if (minMargin > best) { best = minMargin; chosen = c; }
            if (minMargin > 2200.0f) break;
        }
        out[i] = chosen;
    }
    // Avoid nearly coincident Y positions. This is a minimum spacing rule,
    // not a row layout: X, Z and actual Y spacing stay procedurally varied.
    // Crucially, focus bob can no longer reverse visible Up/Down order.
    {
        int order[B3_PAGE_SIZE], k, pick;
        float low, high, gap, overflow;
        for (i = 0; i < n; ++i) order[i] = i;
        for (i = 1; i < n; ++i) {
            pick = order[i]; j = i;
            while (j > 0 && out[order[j - 1]].y > out[pick].y) {
                order[j] = order[j - 1]; --j;
            }
            order[j] = pick;
        }
        low = (float)topY + 82.0f;
        high = (float)bottomY - 82.0f;
        gap = 19.0f;
        if (n > 1 && high - low < gap * (float)(n - 1))
            gap = (high - low) / (float)(n - 1);
        for (k = 1; k < n; ++k) {
            float targetY = out[order[k - 1]].y + gap;
            if (out[order[k]].y < targetY) out[order[k]].y = targetY;
        }
        overflow = out[order[n - 1]].y - high;
        if (overflow > 0.0f)
            for (k = 0; k < n; ++k) out[k].y -= overflow;
        overflow = low - out[order[0]].y;
        if (overflow > 0.0f)
            for (k = 0; k < n; ++k) out[k].y += overflow;
    }
}

// Up/Down follow the NEXT physical bubble above/below on screen. This walks
// every entry in a page without imposing row/column geometry. At the vertical
// edge, it advances to the adjacent page (wrapping just like Classic).
// Left/Right choose the closest bubble in that visual direction.
static int b3Navigate(const char** items, int count, int sel,
    int up, int down, int left, int right)
{
    BubbleAnchor a[B3_PAGE_SIZE], b[B3_PAGE_SIZE];
    int page, local, first, visible, nextPage, nextFirst, nextVisible;
    int i, bestIndex, topY, bottomY, pages;
    float bestScore, score, dx, dy, distance;

    if (count <= 1) return sel;
    if ((up && down) || (left && right)) return sel;
    if (!up && !down && !left && !right) return sel;
    page = sel / B3_PAGE_SIZE;
    first = page * B3_PAGE_SIZE;
    local = sel - first;
    topY = (s_b3Top >= 0) ? s_b3Top : M3_SAFE_TOP_DEFAULT;
    bottomY = (s_b3Bottom >= 0) ? s_b3Bottom : M3_SAFE_BOTTOM_DEFAULT;
    b3Generate(items, count, page, topY, bottomY, a, &visible);
    if (local < 0 || local >= visible) return sel;

    bestIndex = -1;
    bestScore = 10000000.0f;
    for (i = 0; i < visible; ++i) {
        if (i == local) continue;
        dx = a[i].x - a[local].x;
        dy = a[i].y - a[local].y;
        if (up && dy < -0.001f) {
            score = -dy; // nearest strictly higher bubble
        }
        else if (down && dy > 0.001f) {
            score = dy;  // nearest strictly lower bubble
        }
        else if (left && !up && !down && dx < -8.0f) {
            score = -dx + b3Abs(dy) * 0.62f;
        }
        else if (right && !up && !down && dx > 8.0f) {
            score = dx + b3Abs(dy) * 0.62f;
        }
        else continue;
        if (score < bestScore) { bestScore = score; bestIndex = i; }
    }
    if (bestIndex >= 0) return first + bestIndex;
    if (!up && !down) return sel; // no spatial neighbor left/right

    pages = (count + B3_PAGE_SIZE - 1) / B3_PAGE_SIZE;
    nextPage = (page + (down ? 1 : pages - 1)) % pages;
    nextFirst = nextPage * B3_PAGE_SIZE;
    b3Generate(items, count, nextPage, topY, bottomY, b, &nextVisible);
    bestIndex = 0;
    bestScore = 10000000.0f;
    for (i = 0; i < nextVisible; ++i) {
        // Prefer the topmost/bottommost bubble, but preserve horizontal
        // continuity where two candidates are visually nearly level.
        distance = b3Abs(b[i].x - a[local].x);
        score = (down ? b[i].y : -b[i].y) + distance * 0.03f;
        if (score < bestScore) { bestScore = score; bestIndex = i; }
    }
    return nextFirst + bestIndex;
}

// Split labels near the midpoint for two-line text painted directly on the
// sphere. There are no bubble-overlaid pills, cards, or rectangular panels.
static int b3SplitLabel(const char* label, char* a, char* b)
{
    int len, i, mid, score, best, n, j;
    len = 0;
    while (label[len] && len < 95) ++len;
    if (label[len]) return 0;
    mid = -1; best = 10000;
    for (i = 1; i < len - 1; ++i) {
        if (label[i] != ' ') continue;
        score = i * 2 - len;
        if (score < 0) score = -score;
        if (score < best) { best = score; mid = i; }
    }
    if (mid < 0) return 0;
    for (i = 0; i < mid; ++i) a[i] = label[i];
    a[mid] = 0;
    j = 0;
    for (n = mid + 1; n < len; ++n) b[j++] = label[n];
    b[j] = 0;
    return 1;
}

// Explicit far-to-near sorting matters: UI quads are alpha blended and have
// no depth writes. This is true projected depth, not 2D size-only animation.
static void b3DrawNode(const BubbleNode* node, const char* label, DWORD now,
    int topY, int bottomY)
{
    float x, y, z, wx, wy, frontZ, diameter, radius;
    float wobble, pulse, textScale, textK;
    int peak, mix;
    DWORD tint, textColor, shadow;

    if (!label || !label[0]) return;
    z = node->z;
    diameter = node->diameter;
    radius = diameter * 0.5f * B3_FOCAL / z;

    // Unique, slow-phase drift: visual life without rerolling the menu layout.
    Gfx_SinCos((float)now * 0.00074f + (float)node->index * 1.43f,
        &wobble, &pulse);
    x = node->x + pulse * (3.2f - node->focus * 1.5f);
    y = node->y + wobble * (3.1f - node->focus * 1.3f);
    if (y - radius < (float)topY + 4.0f) y = (float)topY + radius + 4.0f;
    if (y + radius > (float)bottomY - 4.0f) y = (float)bottomY - radius - 4.0f;
    if (x - radius < 8.0f) x = radius + 8.0f;
    if (x + radius > 632.0f) x = 632.0f - radius;

    wx = (x - 320.0f) * z / B3_FOCAL;
    wy = (240.0f - y) * z / B3_FOCAL;

    // Strong near/far separation, with a bright face brought forward for focus.
    mix = (int)(node->focus * 100.0f);
    tint = orbBlend(EOS_PURPLE, EOS_BG2,
        (int)(25.0f + (z - 4.5f) * 14.0f), 100);
    tint = orbBlend(tint, orbBlend(EOS_PURPLE, EOS_GLOW, 30, 100), mix, 100);
    peak = (int)(199.0f - (z - 4.5f) * 19.0f + node->focus * 64.0f);
    if (peak > 255) peak = 255;
    if (peak < 95) peak = 95;
    if (node->focus > 0.01f)
        Gfx_GlowX3D(wx, wy, z + 0.034f, 1.0f, 0.0f,
            diameter * 0.69f, diameter * 0.69f, EOS_GLOW,
            (int)(node->focus * 110.0f));
    Gfx_Orb3D(wx, wy, z, diameter, tint, peak);

    frontZ = z - 0.055f;
    textScale = 0.83f + node->focus * 0.15f;
    textColor = (orbBlend(EOS_DIM, EOS_WHITE, (int)(node->focus * 100.0f), 100)
        & 0x00FFFFFF) | 0xFF000000;
    shadow = (EOS_BG & 0x00FFFFFF) | 0xB0000000;
    {
        char top[96], lower[96];
        int twoLines;
        float contentWidth, sphereWidth, lineShift;
        float topWidth, lowerWidth;
        twoLines = b3SplitLabel(label, top, lower);
        sphereWidth = (2.0f * radius) * 0.89f;
        if (twoLines) {
            topWidth = (float)Font_TextWidth(top);
            lowerWidth = (float)Font_TextWidth(lower);
            contentWidth = topWidth > lowerWidth ? topWidth : lowerWidth;
            if (contentWidth <= 0.0f) twoLines = 0;
        }
        if (!twoLines) {
            contentWidth = (float)Font_TextWidth(label);
            if (contentWidth > 0.0f) {
                float fit = sphereWidth / contentWidth;
                if (textScale > fit) textScale = fit;
            }
            if (textScale > 0.99f) textScale = 0.99f;
            textK = textScale * frontZ / B3_FOCAL;
            Font_Draw3D(wx + 0.008f, wy - 0.009f, frontZ + 0.003f,
                1.0f, 0.0f, textK, label, shadow);
            Font_Draw3D(wx, wy, frontZ, 1.0f, 0.0f,
                textK, label, textColor);
        }
        else {
            if (contentWidth > 0.0f && contentWidth * textScale > sphereWidth)
                textScale = sphereWidth / contentWidth;
            textK = textScale * frontZ / B3_FOCAL;
            lineShift = 7.5f * frontZ / B3_FOCAL;
            Font_Draw3D(wx + 0.008f, wy + lineShift - 0.009f,
                frontZ + 0.003f, 1.0f, 0.0f, textK, top, shadow);
            Font_Draw3D(wx, wy + lineShift, frontZ,
                1.0f, 0.0f, textK, top, textColor);
            Font_Draw3D(wx + 0.008f, wy - lineShift - 0.009f,
                frontZ + 0.003f, 1.0f, 0.0f, textK, lower, shadow);
            Font_Draw3D(wx, wy - lineShift, frontZ,
                1.0f, 0.0f, textK, lower, textColor);
        }
    }
}

static void Ui_MenuBubblesBounded(const char** items, int count, int sel,
    int topY, int bottomY)
{
    BubbleAnchor anchors[B3_PAGE_SIZE];
    BubbleNode nodes[B3_PAGE_SIZE], temp;
    DWORD now;
    float dt, rate, target, selFocus, centerY, selX, selY;
    int page, first, visible, totalPages, i, j, index, selectedLocal;
    int snap;

    if (!items || count <= 0) return;
    if (sel < 0) sel = 0;
    else if (sel >= count) sel = count - 1;
    if (bottomY <= topY + 170) {
        topY = M3_SAFE_TOP_DEFAULT;
        bottomY = M3_SAFE_BOTTOM_DEFAULT;
    }
    page = sel / B3_PAGE_SIZE;
    first = page * B3_PAGE_SIZE;
    selectedLocal = sel - first;
    b3Generate(items, count, page, topY, bottomY, anchors, &visible);
    now = GetTickCount();
    snap = s_b3Count != count || s_b3Page != page ||
        s_b3Items != items || s_b3FirstLabel != items[0] ||
        s_b3Top != topY || s_b3Bottom != bottomY ||
        (DWORD)(now - s_b3Tick) > 350;
    dt = (float)(now - s_b3Tick) * 0.001f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.09f) dt = 0.09f;
    rate = b3Min(1.0f, B3_EASE * dt);

    for (i = 0; i < visible; ++i) {
        index = first + i;
        target = index == sel ? 1.0f : 0.0f;
        if (snap) s_b3Focus[i] = target;
        else s_b3Focus[i] += (target - s_b3Focus[i]) * rate;
    }

    centerY = (float)(topY + bottomY) * 0.5f;
    selFocus = s_b3Focus[selectedLocal];
    selX = anchors[selectedLocal].x +
        (320.0f - anchors[selectedLocal].x) * 0.08f * selFocus;
    selY = anchors[selectedLocal].y +
        (centerY - anchors[selectedLocal].y) * 0.035f * selFocus;

    for (i = 0; i < visible; ++i) {
        float focus, baseX, baseY;
        index = first + i;
        focus = s_b3Focus[i];
        baseX = anchors[i].x;
        baseY = anchors[i].y;
        nodes[i].index = index;
        nodes[i].focus = focus;
        if (i == selectedLocal) {
            // Pull the active bubble off the constellation and toward the
            // viewer. Its original position is still recognizable.
            nodes[i].x = baseX + (320.0f - baseX) * 0.08f * focus;
            nodes[i].y = baseY + (centerY - baseY) * 0.035f * focus;
        }
        else {
            float dx, dy, pressure;
            dx = baseX - selX;
            dy = baseY - selY;
            pressure = 1.0f - (dx * dx + dy * dy) / (190.0f * 190.0f);
            if (pressure < 0.0f) pressure = 0.0f;
            nodes[i].x = baseX + dx * 0.16f * pressure * selFocus;
            nodes[i].y = baseY + dy * 0.16f * pressure * selFocus;
        }
        nodes[i].z = anchors[i].z + (B3_FOCUS_Z - anchors[i].z) * focus;
        nodes[i].diameter = anchors[i].diameter + 0.12f * focus;
    }

    s_b3Count = count;
    s_b3Page = page;
    s_b3Items = items;
    s_b3FirstLabel = items[0];
    s_b3Top = topY;
    s_b3Bottom = bottomY;
    s_b3Tick = now;

    // Draw distant spheres first. Focusing never changes their list indices.
    for (i = 1; i < visible; ++i) {
        temp = nodes[i]; j = i;
        while (j > 0 && nodes[j - 1].z < temp.z) {
            nodes[j] = nodes[j - 1]; --j;
        }
        nodes[j] = temp;
    }
    uiMenuBegin3D();
    for (i = 0; i < visible; ++i)
        b3DrawNode(&nodes[i], items[nodes[i].index], now, topY, bottomY);
    Gfx_End3D();

    totalPages = (count + B3_PAGE_SIZE - 1) / B3_PAGE_SIZE;
    if (totalPages > 1)
        Ui_ScrollBar(g_scrW - 25, topY + 24, bottomY - topY - 48,
            page, 1, totalPages);
}

// ---- Orbit 3D: elliptical depth ring -------------------------------------
// Unlike Classic's vertical wheel and Bubbles' scattered constellation, Orbit
// places text directly on a tilted ellipse in XYZ. The focused entry occupies
// the near, lower apex. Neighbors rotate smoothly along the same path;
// opposing entries recede toward the far, upper apex. The orbital guide is
// hidden; the selected item is pulled forward into a soft luminous focus field.
// No blocks, pills, solid faces, borders, orbit track, or underline.
// Presentation only: all menu indices, callbacks, and boot paths are unchanged.
#define O3_PI           3.14159265f
#define O3_TAU          6.28318531f
#define O3_FOCAL        415.692f
#define O3_CENTER_Z     6.40f
#define O3_RADIUS_Z     2.15f
#define O3_RADIUS_X     2.85f
#define O3_RADIUS_Y     0.78f
#define O3_TILT         0.24f
#define O3_DRAW_LIMIT    9
#define O3_EASE         11.5f

typedef struct OrbitNode {
    int index;
    float x, y, z, angle, screenX, screenY;
    float edgeFade;
} OrbitNode;

static float s_oTravel = 0.0f;
static float s_oFocusLift = 1.0f; // eased selection depth; renderer only
static int s_oLastSel = -1, s_oCount = -1;
static int s_oTop = -1, s_oBottom = -1;
static const char** s_oItems = 0;
static const char* s_oFirstLabel = 0;
static DWORD s_oTick = 0;

static float o3Abs(float x) { return x < 0.0f ? -x : x; }
static float o3Min(float a, float b) { return a < b ? a : b; }
static float o3Max(float a, float b) { return a > b ? a : b; }

// Orbit's D-pad semantics reflect screen placement: positive indices approach
// from the right, negative indices from the left. Up/Down retain their familiar
// previous/next meanings, and Left/Right offer the same natural movement.
static int o3Navigate(int count, int sel, int up, int down, int left, int right)
{
    if (count <= 1 || (up && down) || (left && right)) return sel;
    if (up || left) return (sel + count - 1) % count;
    if (down || right) return (sel + 1) % count;
    return sel;
}

// Normalise signed item offsets so wrap-around rotates one position rather
// than swinging all the way around. For even counts the opposite item has one
// unambiguous position at the far apex.
static int o3Offset(int index, int selected, int count)
{
    int d = index - selected;
    if (d > count / 2) d -= count;
    if (d < -count / 2) d += count;
    return d;
}

// The 3D path itself has a near/far depth span of 4.3 world units. Height and
// roll make it a visibly tilted ellipse instead of a flat 2D circle. Keep the
// projected path inside each caller's reserved header/footer-safe region.
static void o3Position(float angle, int topY, int bottomY, OrbitNode* node)
{
    float si, co, midY, screenY;
    Gfx_SinCos(angle, &si, &co);
    midY = (float)(topY + bottomY) * 0.5f;
    node->angle = angle;
    node->z = O3_CENTER_Z - O3_RADIUS_Z * co;
    node->x = O3_RADIUS_X * si;
    node->y = (240.0f - midY) * node->z / O3_FOCAL
        - O3_RADIUS_Y * co + O3_TILT * si;
    screenY = 240.0f - node->y * O3_FOCAL / node->z;
    if (screenY < (float)topY + 32.0f)
        node->y = (240.0f - (float)topY - 32.0f) * node->z / O3_FOCAL;
    if (screenY > (float)bottomY - 32.0f)
        node->y = (240.0f - (float)bottomY + 32.0f) * node->z / O3_FOCAL;
    node->screenX = 320.0f + node->x * O3_FOCAL / node->z;
    node->screenY = 240.0f - node->y * O3_FOCAL / node->z;
}

// One clean text-only item. Perspective (not a simulated scale-only effect)
// determines position and depth. Long labels wrap into two centered lines,
// with a minimum fit-to-screen cap that prevents overlap with the side edges.
static void o3DrawText(const OrbitNode* node, const char* label, int selected)
{
    float dep, screenScale, worldK, tilt, sa, ca, px, maxW, lineShift;
    float textWidth, fit, kFade, labelMaxW;
    int alpha, split;
    char first[96], second[96];
    DWORD color, shadow, bloom;
    OrbitNode pulled;
    if (!label || !label[0] || node->edgeFade <= 0.0f) return;

    // Orbit focus is actual forward Z movement, not a flat rectangle. Preserve
    // its projected X/Y center so the user never loses track of the selection.
    // The surrounding entries stay on the original elliptical 3D path.
    if (selected) {
        float nearZ = o3Max(3.15f, node->z - 0.46f * s_oFocusLift);
        pulled = *node;
        pulled.x = node->x * nearZ / node->z;
        pulled.y = node->y * nearZ / node->z;
        pulled.z = nearZ;
        node = &pulled;
    }

    dep = (node->z - (O3_CENTER_Z - O3_RADIUS_Z)) / (2.0f * O3_RADIUS_Z);
    if (dep < 0.0f) dep = 0.0f;
    if (dep > 1.0f) dep = 1.0f;
    screenScale = selected ? 1.19f + 0.13f * s_oFocusLift : 0.81f - 0.23f * dep;
    maxW = selected ? 246.0f : 198.0f - 106.0f * dep;
    px = o3Min(node->screenX - 14.0f, 626.0f - node->screenX) * 2.0f;
    if (px < maxW) maxW = px;
    if (maxW < 34.0f) return;

    // No enclosing cell or padding: the label itself is the focal object.
    labelMaxW = selected ? maxW - 6.0f : maxW;
    if (labelMaxW < 16.0f) labelMaxW = 16.0f;
    split = 0;
    textWidth = (float)Font_TextWidth(label);
    if (textWidth * screenScale > labelMaxW)
        split = b3SplitLabel(label, first, second);
    if (split) {
        float a = (float)Font_TextWidth(first);
        float b = (float)Font_TextWidth(second);
        textWidth = a > b ? a : b;
    }
    if (textWidth > 0.0f && textWidth * screenScale > labelMaxW) {
        fit = labelMaxW / textWidth;
        screenScale = o3Min(screenScale, fit);
    }
    if (screenScale <= 0.0f) return;

    kFade = node->edgeFade;
    alpha = selected ? 255 : (int)((211.0f - 104.0f * dep) * kFade);
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    color = (orbBlend(EOS_DIM, EOS_WHITE, selected ? 100 : 27, 100)
        & 0x00FFFFFF) | ((DWORD)alpha << 24);
    shadow = (EOS_BG & 0x00FFFFFF) | ((DWORD)(alpha / 2) << 24);
    bloom = (EOS_GLOW & 0x00FFFFFF) | 0x88000000u;
    worldK = screenScale * node->z / O3_FOCAL;
    tilt = 0.13f * (node->x / O3_RADIUS_X);
    Gfx_SinCos(tilt, &sa, &ca);

    if (selected) {
        // Two radial glow textures at DIFFERENT Z depths create a soft halo
        // around the floating typography, not a rectangular menu cell. Both
        // naturally vanish at their edges. The gentle breathing is limited to
        // light intensity; the text does not wander or obscure neighboring items.
        float wave, glowW, glowH;
        int outerPeak, innerPeak;
        Gfx_SinCos((float)GetTickCount() * 0.0018f, &wave, 0);
        glowW = o3Min(maxW, o3Max(134.0f, textWidth * screenScale + 45.0f));
        glowW = glowW * node->z / (2.0f * O3_FOCAL);
        glowH = (split ? 34.0f : 23.0f) * node->z / O3_FOCAL;
        outerPeak = 83 + (int)(11.0f * wave);
        innerPeak = 116 + (int)(16.0f * wave);
        Gfx_GlowX3D(node->x, node->y, node->z + 0.31f,
            ca, sa, glowW * 1.25f, glowH * 1.44f, EOS_PURPLE, outerPeak);
        Gfx_GlowX3D(node->x, node->y, node->z + 0.13f,
            ca, sa, glowW * 0.93f, glowH * 0.78f, EOS_GLOW, innerPeak);
    }

    if (split) {
        lineShift = (selected ? 13.0f : 9.0f) * node->z / O3_FOCAL;
        if (selected) {
            // A faint enlarged echo behind the glyphs gives visible text
            // depth without using a flat button body or outlines.
            Font_Draw3D(node->x, node->y + lineShift,
                node->z + 0.080f, ca, sa, worldK * 1.045f, first, bloom);
            Font_Draw3D(node->x, node->y - lineShift,
                node->z + 0.080f, ca, sa, worldK * 1.045f, second, bloom);
        }
        Font_Draw3D(node->x + 0.008f, node->y + lineShift - 0.008f,
            node->z + 0.018f, ca, sa, worldK, first, shadow);
        Font_Draw3D(node->x, node->y + lineShift,
            node->z - 0.018f, ca, sa, worldK, first, color);
        Font_Draw3D(node->x + 0.008f, node->y - lineShift - 0.008f,
            node->z + 0.018f, ca, sa, worldK, second, shadow);
        Font_Draw3D(node->x, node->y - lineShift,
            node->z - 0.018f, ca, sa, worldK, second, color);
    }
    else {
        if (selected)
            Font_Draw3D(node->x, node->y,
                node->z + 0.080f, ca, sa, worldK * 1.045f, label, bloom);
        Font_Draw3D(node->x + 0.008f, node->y - 0.008f,
            node->z + 0.018f, ca, sa, worldK, label, shadow);
        Font_Draw3D(node->x, node->y,
            node->z - 0.018f, ca, sa, worldK, label, color);
    }
}

static void Ui_MenuOrbitBounded(const char** items, int count, int sel,
    int topY, int bottomY)
{
    OrbitNode nodes[O3_DRAW_LIMIT], tmp;
    DWORD now;
    float dt, rate, step, angle, dist, faded;
    int i, n, j, d, shift, newMenu;

    if (!items || count <= 0) return;
    if (sel < 0) sel = 0;
    if (sel >= count) sel = count - 1;
    if (bottomY > M3_SAFE_BOTTOM_DEFAULT) bottomY = M3_SAFE_BOTTOM_DEFAULT;
    if (bottomY - topY < 195) {
        topY = M3_SAFE_TOP_DEFAULT;
        bottomY = M3_SAFE_BOTTOM_DEFAULT;
    }
    now = GetTickCount();
    newMenu = s_oCount != count || s_oItems != items ||
        s_oFirstLabel != items[0] || s_oTop != topY ||
        s_oBottom != bottomY || (DWORD)(now - s_oTick) > 350;
    if (newMenu) {
        s_oTravel = 0.0f;
        s_oFocusLift = 1.0f; // no pop-in when opening a menu
        s_oLastSel = sel;
        s_oTick = now;
    }
    else if (sel != s_oLastSel) {
        s_oFocusLift = 0.0f; // new entry eases out of its orbit toward viewer
        shift = sel - s_oLastSel;
        if (shift > count / 2) shift -= count;
        if (shift < -count / 2) shift += count;
        if (shift > 2 || shift < -2) s_oTravel = 0.0f;
        else s_oTravel += (float)shift;
        if (s_oTravel > 2.2f || s_oTravel < -2.2f) s_oTravel = 0.0f;
        s_oLastSel = sel;
    }
    dt = (float)(now - s_oTick) * 0.001f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.09f) dt = 0.09f;
    rate = o3Min(1.0f, dt * O3_EASE);
    s_oTravel += (0.0f - s_oTravel) * rate;
    s_oFocusLift += (1.0f - s_oFocusLift) * o3Min(1.0f, dt * 13.0f);
    if (o3Abs(s_oTravel) < 0.002f) s_oTravel = 0.0f;
    s_oItems = items;
    s_oFirstLabel = items[0];
    s_oCount = count;
    s_oTop = topY;
    s_oBottom = bottomY;
    s_oTick = now;

    // Complete ring for short menus; for longer lists show a legible moving
    // window of neighbors rather than packing 20 labels onto one ellipse.
    step = O3_TAU / (float)(count <= 8 ? count : 8);
    n = 0;
    for (i = 0; i < count; ++i) {
        d = o3Offset(i, sel, count);
        if (count > 8 && (d < -4 || d > 4)) continue;
        angle = ((float)d + s_oTravel) * step;
        dist = o3Abs(angle);
        if (count > 8 && dist >= 2.88f) continue;
        if (n >= O3_DRAW_LIMIT) continue;
        nodes[n].index = i;
        o3Position(angle, topY, bottomY, &nodes[n]);
        faded = (count > 8 && dist > 2.28f) ?
            (2.88f - dist) / 0.60f : 1.0f;
        nodes[n].edgeFade = o3Max(0.0f, o3Min(1.0f, faded));
        ++n;
    }
    // Alpha-blended 3D text needs distant entries submitted first. The focused
    // item comes forward in Z and must ALWAYS draw last, even mid-rotation.
    for (i = 1; i < n; ++i) {
        tmp = nodes[i]; j = i;
        while (j > 0 && nodes[j - 1].z < tmp.z) {
            nodes[j] = nodes[j - 1]; --j;
        }
        nodes[j] = tmp;
    }

    uiMenuBegin3D();
    for (i = 0; i < n; ++i)
        if (nodes[i].index != sel)
            o3DrawText(&nodes[i], items[nodes[i].index], 0);
    for (i = 0; i < n; ++i)
        if (nodes[i].index == sel) {
            o3DrawText(&nodes[i], items[nodes[i].index], 1);
            break;
        }
    Gfx_End3D();
    if (count > 8)
        Ui_ScrollBar(g_scrW - 25, topY + 25, bottomY - topY - 50,
            sel, 1, count);
}

void Ui_Menu3DBounded(const char** items, int count, int sel, int topY, int bottomY)
{
    int layout = Config_GetMenuLayout();
    if (layout == 1)
        Ui_MenuGridBounded(items, count, sel, topY, bottomY);
    else if (layout == 2)
        Ui_MenuBubblesBounded(items, count, sel, topY, bottomY);
    else if (layout == 3)
        Ui_MenuOrbitBounded(items, count, sel, topY, bottomY);
    else
        Ui_MenuClassicBounded(items, count, sel, topY, bottomY);
}

void Ui_Menu3D(const char** items, int count, int sel)
{
    Ui_Menu3DBounded(items, count, sel, M3_SAFE_TOP_DEFAULT, M3_SAFE_BOTTOM_DEFAULT);
}

// Bank Management only: reserve the left rail. All other menus use the
// original Ui_MenuNavigate / Ui_Menu3D entry points without region state.
int Ui_MenuNavigateInRegion(const char** items, int count, int sel,
    WORD now, WORD prev, int left, int right)
{
    int result;
    uiMenuRegionSet(left, right);
    result = Ui_MenuNavigate(items, count, sel, now, prev);
    uiMenuRegionSet(0, 0);
    return result;
}

void Ui_Menu3DInRegion(const char** items, int count, int sel,
    int left, int right)
{
    uiMenuRegionSet(left, right);
    Ui_Menu3D(items, count, sel);
    uiMenuRegionSet(0, 0);
}
