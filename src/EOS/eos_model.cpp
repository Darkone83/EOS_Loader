// eos_model.cpp -- Darkone83 theme background model. See eos_model.h.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT beyond
// malloc/free + memcpy. POD only -- no global constructors.
#include "eos_model.h"
#include "eos_gfx.h"          // g_dev, Gfx_CreateTexARGB, EOS_* palette
#include "eos_model_data.h"   // packed kEosModelPos/Nrm/UV + kEosModelIdx (+ counts)
#include "eos_model_tex.h"    // kEosModelTexJpg (512x512 JPEG blob)
#include <xtl.h>

// stb_image: declarations only. The single STB_IMAGE_IMPLEMENTATION lives in
// eos_image.cpp, so this just links against it.
#include "stb_image.h"

// --- header sanity: catch a stale/mislabeled eos_model.h (the filename was
// reused for data in an earlier revision). One clear message beats a cascade.
#if !defined(EOS_MODEL_API_H)
#error "eos_model.h is not the render-module API header. Replace your eos_model.h with the one that declares Model_Init/Model_Draw/Model_Free."
#endif
#if !defined(EOS_MODEL_DATA_H)
#error "eos_model_data.h missing or mislabeled (expects the packed kEosModelPos/Nrm/UV arrays)."
#endif
#if !defined(EOS_MODEL_TEX_GUARD_H)
#error "eos_model_tex.h missing or mislabeled (expects the kEosModelTexJpg albedo blob)."
#endif

// ---- Placement (world units; camera at origin looking +Z, Y up) -----------
// The menu wheel sits centered at z ~2.1..3.95. The orb field spans z 5.5..17.
// Park the figure LEFT and BEHIND the wheel so she reads as the backdrop and
// the existing linear fog (start 2.55 / end 11.0) blends her into EOS_BG just
// like the far orbs. All tunable -- nothing else depends on these.
#define MDL_HEIGHT   5.20f    // world-unit height (mesh is normalized to 1.0)
#define MDL_X       -2.45f    // left of center
#define MDL_Y       -3.15f    // feet near the bottom of the 4:3 frame
#define MDL_Z        6.90f    // behind the menu, inside the fog band
#define MDL_SPINRATE 0.22f    // radians/sec idle turntable
#define MDL_YAW0     0.35f    // base yaw so she faces slightly toward center

#define MDL_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

static IDirect3DVertexBuffer8* s_vb = 0;
static IDirect3DIndexBuffer8* s_ib = 0;
static IDirect3DTexture8* s_tex = 0;
static int                     s_ready = 0;
static int                     s_tried = 0;   // one-shot: don't retry a failure every frame

// ---- helpers --------------------------------------------------------------
static void colF(DWORD argb, float* r, float* g, float* b)
{
    *r = (float)((argb >> 16) & 0xFF) / 255.0f;
    *g = (float)((argb >> 8) & 0xFF) / 255.0f;
    *b = (float)(argb & 0xFF) / 255.0f;
}

static IDirect3DTexture8* buildTexture(void)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* rgba;
    DWORD* argb;
    IDirect3DTexture8* tex;
    int i, n;

    rgba = stbi_load_from_memory(kEosModelTexJpg, (int)EOS_MODEL_TEX_LEN,
        &w, &h, &comp, 4);
    if (!rgba || w != EOS_MODEL_TEX_W || h != EOS_MODEL_TEX_H) {
        if (rgba) stbi_image_free(rgba);
        return 0;
    }
    n = w * h;
    argb = (DWORD*)malloc((unsigned)n * sizeof(DWORD));
    if (!argb) { stbi_image_free(rgba); return 0; }
    for (i = 0; i < n; ++i) {
        unsigned char* p = rgba + i * 4;
        argb[i] = EOS_ARGB(0xFF, p[0], p[1], p[2]);   // opaque; RGB from decode
    }
    tex = Gfx_CreateTexARGB(w, h, argb);              // swizzled A8R8G8B8 (POT)
    free(argb);
    stbi_image_free(rgba);
    return tex;
}

int Model_Init(void)
{
    void* dst;
    unsigned vbBytes, ibBytes;
    int vi;
    EosModelVtx* vw;

    if (s_ready) return 1;
    if (s_tried) return 0;      // already failed once -- stay on the orb fallback
    s_tried = 1;
    if (!g_dev) return 0;

    vbBytes = (unsigned)EOS_MODEL_VERTS * sizeof(EosModelVtx);
    ibBytes = (unsigned)EOS_MODEL_INDICES * sizeof(unsigned short);

    if (FAILED(g_dev->CreateVertexBuffer(vbBytes, 0, MDL_FVF,
        D3DPOOL_MANAGED, &s_vb)) || !s_vb) {
        Model_Free(); return 0;
    }
    dst = 0;
    if (FAILED(s_vb->Lock(0, 0, (BYTE**)&dst, 0)) || !dst) { Model_Free(); return 0; }
    // Expand packed (int16 pos / int8 normal / int16 uv) into the float VB.
    // int->float only (no __ftol2_sse). Quality is identical to the float mesh.
    vw = (EosModelVtx*)dst;
    for (vi = 0; vi < EOS_MODEL_VERTS; ++vi) {
        const unsigned short* p = &kEosModelPos[vi * 3];
        const signed char* n = &kEosModelNrm[vi * 3];
        const unsigned short* t = &kEosModelUV[vi * 2];
        vw[vi].x = kEosModelPosBias[0] + (float)p[0] * kEosModelPosScale[0];
        vw[vi].y = kEosModelPosBias[1] + (float)p[1] * kEosModelPosScale[1];
        vw[vi].z = kEosModelPosBias[2] + (float)p[2] * kEosModelPosScale[2];
        vw[vi].nx = (float)n[0] * (1.0f / 127.0f);
        vw[vi].ny = (float)n[1] * (1.0f / 127.0f);
        vw[vi].nz = (float)n[2] * (1.0f / 127.0f);
        vw[vi].u = (float)t[0] * (1.0f / 65535.0f);
        vw[vi].v = (float)t[1] * (1.0f / 65535.0f);
    }
    s_vb->Unlock();

    if (FAILED(g_dev->CreateIndexBuffer(ibBytes, 0, D3DFMT_INDEX16,
        D3DPOOL_MANAGED, &s_ib)) || !s_ib) {
        Model_Free(); return 0;
    }
    dst = 0;
    if (FAILED(s_ib->Lock(0, 0, (BYTE**)&dst, 0)) || !dst) { Model_Free(); return 0; }
    memcpy(dst, kEosModelIdx, ibBytes);
    s_ib->Unlock();

    s_tex = buildTexture();
    if (!s_tex) { Model_Free(); return 0; }

    s_ready = 1;
    return 1;
}

int Model_Ready(void) { return s_ready; }

void Model_Free(void)
{
    if (s_vb) { s_vb->Release();  s_vb = 0; }
    if (s_ib) { s_ib->Release();  s_ib = 0; }
    if (s_tex) { s_tex->Release(); s_tex = 0; }
    s_ready = 0;
    // NOTE: leave s_tried set; a mid-run failure shouldn't thrash retries.
}

void Model_Draw(float tsec)
{
    D3DMATRIX w, saveW;
    D3DLIGHT8 key, rim;
    D3DMATERIAL8 mtl;
    float ca, sa, yaw;
    float kr, kg, kb, rr, rg, rb;
    float sx, sy, sz;

    if (!s_ready) { if (!Model_Init()) return; }

    // ---- world matrix: scale * rotateY(yaw) * translate ----
    yaw = MDL_YAW0 + tsec * MDL_SPINRATE;
    Gfx_SinCos(yaw, &sa, &ca);
    sx = sy = sz = MDL_HEIGHT;            // mesh normalized to height 1.0
    memset(&w, 0, sizeof(w));
    w._11 = sx * ca;  w._13 = sx * (-sa);
    w._22 = sy;
    w._31 = sz * sa;  w._33 = sz * ca;
    w._41 = MDL_X;     w._42 = MDL_Y;   w._43 = MDL_Z;  w._44 = 1.0f;

    g_dev->GetTransform(D3DTS_WORLD, &saveW);
    g_dev->SetTransform(D3DTS_WORLD, &w);

    // ---- lights: cool key from upper-front-right, purple rim from lower-left
    // behind. Rim tint tracks the theme accent so it always reads "Darkone". ----
    colF(0xFFDDE6FF, &kr, &kg, &kb);      // key: slightly cool white
    colF(EOS_PURPLE, &rr, &rg, &rb);      // rim: theme accent (168,85,247)

    memset(&key, 0, sizeof(key));
    key.Type = D3DLIGHT_DIRECTIONAL;
    key.Diffuse.r = kr; key.Diffuse.g = kg; key.Diffuse.b = kb; key.Diffuse.a = 1.0f;
    key.Direction.x = -0.45f; key.Direction.y = -0.55f; key.Direction.z = 0.70f;

    memset(&rim, 0, sizeof(rim));
    rim.Type = D3DLIGHT_DIRECTIONAL;
    rim.Diffuse.r = rr * 1.15f; rim.Diffuse.g = rg * 1.15f; rim.Diffuse.b = rb * 1.15f; rim.Diffuse.a = 1.0f;
    rim.Direction.x = 0.60f; rim.Direction.y = 0.25f; rim.Direction.z = -0.75f;

    memset(&mtl, 0, sizeof(mtl));
    mtl.Diffuse.r = mtl.Diffuse.g = mtl.Diffuse.b = mtl.Diffuse.a = 1.0f;
    mtl.Ambient.r = mtl.Ambient.g = mtl.Ambient.b = 1.0f;

    // ---- state: opaque, depth-tested, lit. Save nothing that Gfx_End3D()
    // already restores; restore the rest explicitly below. ----
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);   // opaque
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);    // depth resolves order; winding-agnostic
    g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
    g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);    // world scale != 1
    g_dev->SetRenderState(D3DRS_AMBIENT, EOS_ARGB(0xFF, 20, 16, 32));  // low purple-black

    g_dev->SetMaterial(&mtl);
    g_dev->SetLight(0, &key); g_dev->LightEnable(0, TRUE);
    g_dev->SetLight(1, &rim); g_dev->LightEnable(1, TRUE);

    g_dev->SetTexture(0, s_tex);
    g_dev->SetVertexShader(MDL_FVF);
    g_dev->SetStreamSource(0, s_vb, sizeof(EosModelVtx));
    g_dev->SetIndices(s_ib, 0);
    g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, EOS_MODEL_VERTS,
        0, EOS_MODEL_TRIS);

    // ---- restore the 3D-pass defaults the orb/menu draws expect ----
    g_dev->LightEnable(0, FALSE);
    g_dev->LightEnable(1, FALSE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);  // = FVF3
    g_dev->SetTransform(D3DTS_WORLD, &saveW);
}