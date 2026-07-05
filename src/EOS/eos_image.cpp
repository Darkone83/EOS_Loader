/*---------------------------------------------------------------------------
    eos_image.cpp -- image-file -> swizzled POT D3D texture.

    Owns the project's single STB_IMAGE_IMPLEMENTATION. Decodes PNG / JPG / BMP
    from a File_ReadInto'd RAM buffer, packs RGBA -> ARGB, places it at the
    top-left of a next-power-of-two buffer (zero-padded, 1px edge-replicated),
    and hands that to the gfx layer's proven Gfx_CreateTexARGB (swizzled
    A8R8G8B8 via XGSwizzleRect).

    Why POT-swizzle and not linear: the NV2A does not sample LIN_* textures
    (eos_gfx.cpp documents this) -- a linear texture creates fine but renders
    black. Swizzle is the only path that samples, and swizzle is only correct
    for power-of-two dimensions. Rectangular POT (1024x512) is fine; non-POT is
    not. The caller draws the used sub-rect via the returned UV extents.

    stb is configured memory-only + scalar (see the defines below), matching
    eos_audio's MINIMP3_NO_SIMD. stb's (int)float casts lower through
    _ftol2_sse (eos_ftoi.cpp), which links. Compiled with /GL like every other
    .cpp; inherits the project default (no per-file settings).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>          /* malloc / free */
#include <string.h>          /* memcpy / memset */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_PSD
#include "stb_image.h"

#include "eos_image.h"
#include "eos_gfx.h"         /* Gfx_CreateTexARGB */
#include "eos_file.h"        /* File_ReadInto / File_Exists */

/* Encoded-file cap. Real theme backgrounds land ~1.5-3 MB after the browser
   resize; 4 MB leaves headroom for a hand-placed file. */
#define IMG_FILE_MAX  (4 * 1024 * 1024)

   /* Last-failure reason (bring-up diagnostic; see Image_LastError). */
static const char* s_err = "ok";
const char* Image_LastError(void) { return s_err; }

static int nextPot(int v)
{
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Place a w*h ARGB image at the top-left of a next-POT buffer, replicate the
   used-region's right/bottom edge 1px into the pad (so bilinear at the border
   samples image color, not the zero padding -> no dark fringe), and build the
   swizzled texture via the gfx layer. Returns the texture + UV extents. */
static IDirect3DTexture8* makeBgTex(const DWORD* argb, int w, int h,
    float* outU1, float* outV1)
{
    int    potW = nextPot(w);
    int    potH = nextPot(h);
    DWORD* pad;
    IDirect3DTexture8* tex;
    int    y;

    pad = (DWORD*)malloc((size_t)potW * (size_t)potH * 4);
    if (!pad) { s_err = "img: malloc(POT pad) failed"; return 0; }
    memset(pad, 0, (size_t)potW * (size_t)potH * 4);

    for (y = 0; y < h; ++y)
        memcpy(pad + (size_t)y * potW, argb + (size_t)y * w, (size_t)w * 4);

    /* right edge */
    if (w < potW)
        for (y = 0; y < h; ++y)
            pad[(size_t)y * potW + w] = pad[(size_t)y * potW + (w - 1)];
    /* bottom edge (copies the +1 right-edge pixel too) */
    if (h < potH)
        memcpy(pad + (size_t)h * potW, pad + (size_t)(h - 1) * potW,
            (size_t)((w < potW) ? (w + 1) : w) * 4);

    tex = Gfx_CreateTexARGB(potW, potH, pad);   /* proven swizzled path */
    free(pad);
    if (!tex) { s_err = "img: Gfx_CreateTexARGB(POT) failed"; return 0; }

    if (outU1) *outU1 = (float)w / (float)potW;
    if (outV1) *outV1 = (float)h / (float)potH;
    return tex;
}

int Image_LoadTexture(const char* path, int maxW, int maxH,
    IDirect3DTexture8** outTex, int* outW, int* outH,
    float* outU1, float* outV1)
{
    unsigned char* file = 0;
    int                fileLen = 0;
    int                w = 0, h = 0, comp = 0;
    unsigned char* rgba = 0;
    int                dw, dh;
    DWORD* argb = 0;
    IDirect3DTexture8* tex = 0;

    if (outTex) *outTex = 0;
    if (outW)   *outW = 0;
    if (outH)   *outH = 0;
    if (outU1)  *outU1 = 1.0f;
    if (outV1)  *outV1 = 1.0f;
    if (!path || !outTex) return 0;
    s_err = "ok";

    /* ---- read the encoded file into RAM ---------------------------------- */
    file = (unsigned char*)malloc(IMG_FILE_MAX);
    if (!file) { s_err = "img: malloc(file 4MB) failed"; return 0; }
    fileLen = File_ReadInto(path, file, IMG_FILE_MAX);
    if (fileLen <= 0) {
        s_err = File_Exists(path) ? "img: read failed or file >4MB"
            : "img: file not found";
        free(file);
        return 0;
    }

    /* ---- decode to RGBA (forces 4 components) ---------------------------- */
    rgba = stbi_load_from_memory(file, fileLen, &w, &h, &comp, 4);
    free(file);
    if (!rgba || w <= 0 || h <= 0) {
        const char* r = stbi_failure_reason();
        s_err = r ? r : "img: decode failed";
        if (rgba) stbi_image_free(rgba);
        return 0;
    }

    /* ---- target size: nearest downscale, aspect preserved ---------------- */
    dw = w; dh = h;
    if (maxW > 0 && dw > maxW) { dh = dh * maxW / dw; dw = maxW; if (dh < 1) dh = 1; }
    if (maxH > 0 && dh > maxH) { dw = dw * maxH / dh; dh = maxH; if (dw < 1) dw = 1; }

    /* ---- pack RGBA -> ARGB (0xAARRGGBB), sampling nearest on downscale ---- */
    argb = (DWORD*)malloc((size_t)dw * (size_t)dh * 4);
    if (!argb) { s_err = "img: malloc(argb) failed"; stbi_image_free(rgba); return 0; }
    {
        int x, y;
        for (y = 0; y < dh; ++y) {
            int                  sy = y * h / dh;
            const unsigned char* srow = rgba + (size_t)sy * (size_t)w * 4;
            DWORD* drow = argb + (size_t)y * (size_t)dw;
            for (x = 0; x < dw; ++x) {
                int                  sx = x * w / dw;
                const unsigned char* p = srow + (size_t)sx * 4;
                drow[x] = ((DWORD)p[3] << 24) |      /* A */
                    ((DWORD)p[0] << 16) |      /* R */
                    ((DWORD)p[1] << 8) |      /* G */
                    (DWORD)p[2];              /* B */
            }
        }
    }
    stbi_image_free(rgba);

    /* ---- build the swizzled POT texture ---------------------------------- */
    tex = makeBgTex(argb, dw, dh, outU1, outV1);
    free(argb);
    if (!tex) return 0;   /* s_err set by makeBgTex */

    *outTex = tex;
    if (outW) *outW = dw;
    if (outH) *outH = dh;
    (void)comp;
    return 1;
}