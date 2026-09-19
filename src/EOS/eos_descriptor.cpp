// eos_descriptor.cpp -- Eos dynamic bank-layout descriptor (loader side).
//
// 64-byte on-flash format (bank 0xF, little-endian; mirrors eos_bank_ctrl.v):
//   0x00  MAGIC 'E','O','S','B'
//   0x04  VERSION = 1
//   0x05  SLOT_COUNT = 4
//   0x06  reserved (0)
//   per slot (8 bytes each) @ 0x08, 0x10, 0x18, 0x20:
//     [0] state     0=free 1=native256 2=oversized-anchor 3=shadowed
//     [1] size_code 0=256K 1=512K 2=1M
//     [2..3] reserved
//     [4..6] phys_base (24-bit, relative to FLOOR, little-endian)
//     [7] reserved
//
// Native 256K geometry is fixed by visible slot:
//   slot 0 / EF3 = 0x000000
//   slot 1 / EF4 = 0x040000
//   slot 2 / EF5 = 0x080000
//   slot 3 / EF6 = 0x0C0000
// The serializer enforces those bases so a caller can never persist all native
// slots at physBase=0 and silently redirect Bank 2/3/4 onto Bank 1.
#include <xtl.h>
#include "eos_bank.h"
#include "eos_descriptor.h"
#include "eos_flash.h"

#define DESC_MAGIC0 0x45
#define DESC_MAGIC1 0x4F
#define DESC_MAGIC2 0x53
#define DESC_MAGIC3 0x42
#define DESC_VERSION 0x01

#define DESC_BYTES  64
#define SLOT0_OFF   0x08
#define SLOT_STRIDE 0x08
#define NATIVE_SLOT_STRIDE 0x040000u

#define COLR_MAGIC_OFF 0x2C
#define COLR_DATA_OFF  0x30
#define COLR_M0 0x43
#define COLR_M1 0x4F
#define COLR_M2 0x4C
#define COLR_M3 0x52

#define NEWRGN_OFF  0x3C0000

static void put24le(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
}

static unsigned int get24le(const unsigned char* p)
{
    return (unsigned int)p[0]
        | ((unsigned int)p[1] << 8)
        | ((unsigned int)p[2] << 16);
}

static unsigned int nativeBaseForSlot(int slot)
{
    if (slot < 0 || slot >= EOS_DESC_SLOTS) return 0;
    return (unsigned int)slot * NATIVE_SLOT_STRIDE;
}

void Desc_InitEmpty(EosLayout* lay)
{
    int i;
    lay->valid = 1;
    for (i = 0; i < EOS_DESC_SLOTS; ++i) {
        lay->slot[i].state = EOS_SLOT_FREE;
        lay->slot[i].sizeCode = EOS_SZC_256K;
        lay->slot[i].physBase = 0;
        lay->color[i] = 0xFFFFFFu;
    }
}

int Desc_SlotsFor(int sizeCode)
{
    if (sizeCode == EOS_SZC_1MB)  return 4;
    if (sizeCode == EOS_SZC_512K) return 2;
    return 1;
}

int Desc_Load(EosLayout* out)
{
    unsigned char pg[256];
    int i, rc;

    Desc_InitEmpty(out);
    out->valid = 0;

    rc = Flash_ReadPage(EOS_BANK_DESCRIPTOR, 0, pg);
    if (rc != EOS_FLASH_OK) return 0;

    if (pg[0] != DESC_MAGIC0 || pg[1] != DESC_MAGIC1 ||
        pg[2] != DESC_MAGIC2 || pg[3] != DESC_MAGIC3 ||
        pg[4] != DESC_VERSION)
        return 0;

    for (i = 0; i < EOS_DESC_SLOTS; ++i) {
        const unsigned char* e = pg + SLOT0_OFF + i * SLOT_STRIDE;
        out->slot[i].state = (unsigned char)(e[0] & 0x03);
        out->slot[i].sizeCode = (unsigned char)(e[1] & 0x03);
        out->slot[i].physBase = get24le(e + 4);

        // Native 256K banks never use dynamic placement. Repair stale legacy
        // descriptors in-memory so every subsequent save writes canonical data.
        if (out->slot[i].state == EOS_SLOT_NATIVE) {
            out->slot[i].sizeCode = EOS_SZC_256K;
            out->slot[i].physBase = nativeBaseForSlot(i);
        }
    }

    if (pg[COLR_MAGIC_OFF + 0] == COLR_M0 && pg[COLR_MAGIC_OFF + 1] == COLR_M1 &&
        pg[COLR_MAGIC_OFF + 2] == COLR_M2 && pg[COLR_MAGIC_OFF + 3] == COLR_M3) {
        for (i = 0; i < EOS_DESC_SLOTS; ++i) {
            const unsigned char* c = pg + COLR_DATA_OFF + i * 3;
            out->color[i] = ((unsigned int)c[0] << 16) |
                ((unsigned int)c[1] << 8) |
                (unsigned int)c[2];
        }
    }

    out->valid = 1;
    return 1;
}

int Desc_Save(const EosLayout* lay)
{
    unsigned char pg[256];
    int i, rc;

    for (i = 0; i < 256; ++i) pg[i] = 0x00;

    pg[0] = DESC_MAGIC0; pg[1] = DESC_MAGIC1;
    pg[2] = DESC_MAGIC2; pg[3] = DESC_MAGIC3;
    pg[4] = DESC_VERSION;
    pg[5] = EOS_DESC_SLOTS;

    for (i = 0; i < EOS_DESC_SLOTS; ++i) {
        unsigned char* e = pg + SLOT0_OFF + i * SLOT_STRIDE;
        unsigned int physBase = lay->slot[i].physBase;
        unsigned char sizeCode = lay->slot[i].sizeCode;

        // CRITICAL: native slots have fixed geometry. Do not trust caller
        // metadata here; older loader paths populated physBase=0 for every slot.
        if (lay->slot[i].state == EOS_SLOT_NATIVE) {
            physBase = nativeBaseForSlot(i);
            sizeCode = EOS_SZC_256K;
        }

        e[0] = lay->slot[i].state;
        e[1] = sizeCode;
        put24le(e + 4, physBase);
    }

    pg[COLR_MAGIC_OFF + 0] = COLR_M0; pg[COLR_MAGIC_OFF + 1] = COLR_M1;
    pg[COLR_MAGIC_OFF + 2] = COLR_M2; pg[COLR_MAGIC_OFF + 3] = COLR_M3;
    for (i = 0; i < EOS_DESC_SLOTS; ++i) {
        unsigned char* c = pg + COLR_DATA_OFF + i * 3;
        c[0] = (unsigned char)((lay->color[i] >> 16) & 0xFF);
        c[1] = (unsigned char)((lay->color[i] >> 8) & 0xFF);
        c[2] = (unsigned char)(lay->color[i] & 0xFF);
    }

    rc = Flash_WriteImageNoSync(EOS_BANK_DESCRIPTOR, pg, 256);
    if (rc != EOS_FLASH_OK) return rc;

    Flash_ReloadDescriptor();
    return EOS_FLASH_OK;
}

int Desc_Erase(void)
{
    int rc = Flash_EraseBank(EOS_BANK_DESCRIPTOR);
    if (rc != EOS_FLASH_OK) return rc;
    Flash_ReloadDescriptor();
    return EOS_FLASH_OK;
}

int Desc_FreeSlots(const EosLayout* lay)
{
    int i, n = 0;
    for (i = 0; i < EOS_DESC_SLOTS; ++i)
        if (lay->slot[i].state == EOS_SLOT_FREE) ++n;
    return n;
}

static int find_run(const EosLayout* lay, int need)
{
    int i, j, ok;
    for (i = 0; i + need <= EOS_DESC_SLOTS; ++i) {
        ok = 1;
        for (j = 0; j < need; ++j) {
            if (lay->slot[i + j].state != EOS_SLOT_FREE) {
                ok = 0;
                break;
            }
        }
        if (ok) return i;
    }
    return -1;
}

int Desc_CanPlace(const EosLayout* lay, int sizeCode, int* anchorSlot)
{
    int need = Desc_SlotsFor(sizeCode);
    int at = find_run(lay, need);
    if (at < 0) return 0;
    if (anchorSlot) *anchorSlot = at;
    return 1;
}

unsigned int Desc_NewRegionBase(int slot, int sizeCode)
{
    if (sizeCode == EOS_SZC_1MB) return EOS_NEWRGN_BASE;
    if (slot >= 2)               return EOS_NEWRGN_BASE + EOS_NEWRGN_HALF;
    return EOS_NEWRGN_BASE;
}

int Desc_CanPlaceAt(const EosLayout* lay, int slot, int sizeCode)
{
    int need = Desc_SlotsFor(sizeCode);
    EosLayout tmp;
    int i, j;

    if (slot < 0 || slot >= EOS_DESC_SLOTS) return 0;
    if (slot + need > EOS_DESC_SLOTS) return 0;

    if (sizeCode == EOS_SZC_512K && (slot & 1)) return 0;
    if (sizeCode == EOS_SZC_1MB && slot != 0) return 0;

    tmp = *lay;
    Desc_FreeFootprint(&tmp, slot, need);
    for (j = 0; j < need; ++j)
        if (tmp.slot[slot + j].state != EOS_SLOT_FREE) return 0;

    if (sizeCode == EOS_SZC_1MB) {
        for (i = 0; i < EOS_DESC_SLOTS; ++i)
            if (tmp.slot[i].state != EOS_SLOT_FREE) return 0;
    }
    return 1;
}

int Desc_PlaceNative(EosLayout* lay, unsigned int physBaseRelFloor)
{
    int at = find_run(lay, 1);
    (void)physBaseRelFloor;
    if (at < 0) return -1;
    lay->slot[at].state = EOS_SLOT_NATIVE;
    lay->slot[at].sizeCode = EOS_SZC_256K;
    lay->slot[at].physBase = nativeBaseForSlot(at);
    return at;
}

unsigned int Desc_PlaceOversizedAt(EosLayout* lay, int slot, int sizeCode)
{
    int need = Desc_SlotsFor(sizeCode);
    unsigned int base;
    int j;

    if (sizeCode != EOS_SZC_512K && sizeCode != EOS_SZC_1MB)
        return (unsigned int)-1;
    if (!Desc_CanPlaceAt(lay, slot, sizeCode))
        return (unsigned int)-1;

    Desc_FreeFootprint(lay, slot, need);
    base = Desc_NewRegionBase(slot, sizeCode);

    lay->slot[slot].state = EOS_SLOT_ANCHOR;
    lay->slot[slot].sizeCode = (unsigned char)sizeCode;
    lay->slot[slot].physBase = base;
    for (j = 1; j < need; ++j) {
        lay->slot[slot + j].state = EOS_SLOT_SHADOW;
        lay->slot[slot + j].sizeCode = EOS_SZC_256K;
        lay->slot[slot + j].physBase = 0;
    }
    return base;
}

int Desc_PlaceOversized(EosLayout* lay, int sizeCode, unsigned int newRegionBaseRelFloor)
{
    int need = Desc_SlotsFor(sizeCode);
    int at;
    (void)newRegionBaseRelFloor;

    if (sizeCode != EOS_SZC_512K && sizeCode != EOS_SZC_1MB) return -1;
    for (at = 0; at + need <= EOS_DESC_SLOTS; ++at) {
        if (Desc_CanPlaceAt(lay, at, sizeCode)) {
            if (Desc_PlaceOversizedAt(lay, at, sizeCode) != (unsigned int)-1)
                return at;
        }
    }
    return -1;
}

int Desc_FreeSlot(EosLayout* lay, int slot)
{
    int span, j;
    if (slot < 0 || slot >= EOS_DESC_SLOTS) return 0;

    if (lay->slot[slot].state == EOS_SLOT_ANCHOR) {
        span = Desc_SlotsFor(lay->slot[slot].sizeCode);
        for (j = 0; j < span && (slot + j) < EOS_DESC_SLOTS; ++j) {
            lay->slot[slot + j].state = EOS_SLOT_FREE;
            lay->slot[slot + j].sizeCode = EOS_SZC_256K;
            lay->slot[slot + j].physBase = 0;
        }
        return 1;
    }

    if (lay->slot[slot].state == EOS_SLOT_NATIVE) {
        lay->slot[slot].state = EOS_SLOT_FREE;
        lay->slot[slot].sizeCode = EOS_SZC_256K;
        lay->slot[slot].physBase = 0;
        return 1;
    }

    return 0;
}

int Desc_FreeFootprint(EosLayout* lay, int at, int need)
{
    int i, freed = 0;
    if (at < 0) return 0;

    for (i = 0; i < EOS_DESC_SLOTS; ++i) {
        int st = lay->slot[i].state;
        if (st == EOS_SLOT_ANCHOR) {
            int span = Desc_SlotsFor(lay->slot[i].sizeCode);
            int lo = i, hi = i + span;
            int rlo = at, rhi = at + need;
            if (lo < rhi && rlo < hi) {
                Desc_FreeSlot(lay, i);
                ++freed;
            }
        }
        else if (st == EOS_SLOT_NATIVE) {
            if (i >= at && i < at + need) {
                Desc_FreeSlot(lay, i);
                ++freed;
            }
        }
    }
    return freed;
}

const unsigned int Eos_LedPalette[EOS_LED_PALETTE_N] = {
    0xFFFFFF,
    0xFF0000,
    0xFF6000,
    0xFFD000,
    0x30FF00,
    0x00FFC0,
    0x00C0FF,
    0x0040FF,
    0xA855F7,
    0xFF00E0,
    0xFF3080,
    0xFEFEFE
};

const char* const Eos_LedPaletteName[EOS_LED_PALETTE_N] = {
    "Off","Red","Orange","Amber","Green","Teal","Cyan","Blue",
    "Purple","Magenta","Pink","White"
};

static int descColorSlot(int bankIdx)
{
    unsigned char ef = Bank_Ef(bankIdx);
    if (ef >= 0x3 && ef <= 0x6) return (int)(ef - 0x3);
    return -1;
}

unsigned int Desc_GetColor(int bankIdx)
{
    EosLayout lay;
    int slot = descColorSlot(bankIdx);
    if (slot < 0) return 0xFFFFFFu;
    Desc_Load(&lay);
    return lay.color[slot];
}

int Desc_SetColor(int bankIdx, unsigned int rgb)
{
    EosLayout lay;
    int slot = descColorSlot(bankIdx);
    int i;

    if (slot < 0) return -1;
    Desc_Load(&lay);

    // Reconcile every occupied native user bank before persisting color data.
    // Use the canonical physical base for that visible slot; older code wrote
    // zero here for Bank 2/3/4, which is the loader-side mapping bug.
    for (i = 0; i < Bank_Count(); ++i) {
        unsigned char ef = Bank_Ef(i);
        int s;
        if (ef < 0x3 || ef > 0x6) continue;
        s = (int)(ef - 0x3);
        if (Bank_Occupied(i) && lay.slot[s].state == EOS_SLOT_FREE) {
            lay.slot[s].state = EOS_SLOT_NATIVE;
            lay.slot[s].sizeCode = EOS_SZC_256K;
            lay.slot[s].physBase = nativeBaseForSlot(s);
        }
        else if (lay.slot[s].state == EOS_SLOT_NATIVE) {
            lay.slot[s].sizeCode = EOS_SZC_256K;
            lay.slot[s].physBase = nativeBaseForSlot(s);
        }
    }

    lay.color[slot] = rgb & 0xFFFFFFu;
    return Desc_Save(&lay);
}
