// eos_image.h -- image-file -> D3D texture loader for the Eos loader.
//
// Decodes PNG / JPG / BMP into a SWIZZLED A8R8G8B8 texture via the gfx layer's
// proven path (Gfx_CreateTexARGB). The NV2A only samples swizzled textures
// (LIN_* does not render), and swizzle requires power-of-two dimensions -- so a
// non-POT image is placed at the top-left of the next-POT texture, zero-padded
// with a 1px edge-replicated border, and drawn via the returned UV extents
// (0..outU1, 0..outV1). Rectangular POT (e.g. 1024x512) swizzles correctly;
// only NON-power-of-two is invalid.
//
// The used region is drawn over a full-screen quad; the NV2A scales the 640-wide
// design backbuffer to the active video mode (480i / 480p / 720p).
//
// RXDK / MSVC2003 / C89-ish: declarations before statements, no CRT beyond
// malloc/free (+ intrinsic memcpy/memset). Owns the project's single
// STB_IMAGE_IMPLEMENTATION.
#pragma once
#include <xtl.h>

// Decode 'path' into a swizzled POT texture.
//
//   path       full path, e.g. "E:\\Eos\\Themes\\Foo\\background.jpg"
//   maxW,maxH  cap on the decoded image (nearest-downscaled, aspect preserved,
//              to fit within). 0 = that axis unbounded. Keep this at the design
//              framebuffer (640x480) so the POT container stays small (<=1024x512).
//   outTex     receives the POT texture. The CALLER Releases it. NULL on failure.
//   outW,outH  receive the USED image dimensions inside the POT texture (optional).
//   outU1,outV1 receive the UV extents of the used region: outU1 = usedW/potW,
//              outV1 = usedH/potH. Draw with Gfx_DrawTex(tex,...,0,0,u1,v1,...).
//
// Returns 1 on success, 0 on failure (bad path, too big, decode error, OOM,
// texture creation failed). On failure *outTex is left NULL.
int Image_LoadTexture(const char* path, int maxW, int maxH,
    IDirect3DTexture8** outTex, int* outW, int* outH,
    float* outU1, float* outV1);

// Reason the last Image_LoadTexture() call failed (names the stage). Valid
// after a 0 return; never NULL. For bring-up diagnostics.
const char* Image_LastError(void);