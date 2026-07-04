// main.cpp -- EOS Loader entry point.
// Flow: goddess splash (fade-in, skippable) -> main menu loop.
// Menu items are selectable stubs for the POC (Launch Bank / Bank Management /
// Settings). The loader never exits; Launch Bank will later write the Eos 0xEF
// bank register over LPC IO + warm-reset, and the FPGA serves the chosen bank.
#include "eos_gfx.h"
#include "eos_font.h"
#include "eos_console.h"   // Console_ReadLive for the persistent HUD
#include "eos_splash.h"
#include "eos_menu.h"
#include "input.h"
#include "eos_bank.h"
#include "eos_descriptor.h"
#include "eos_config.h"
#include "eos_audio.h"
#include "eos_eeprom_io.h"
#include "eos_firmware_io.h"
#include "eos_hdd.h"
#include "eos_format.h"
#include "eos_flash.h"
#include "eos_file.h"
#include "eos_osk.h"
#include "eos_settings.h"
#include "eos_theme.h"
#include "eos_ui.h"
#include "dd_net.h"
#include "eos_http.h"
#include "dd_ftp.h"


// ---------------------------------------------------------------------------
// App phases. The whole loader is one frame-driven loop with a phase var so
// input is pumped exactly once per frame and shared across splash + menu.
// ---------------------------------------------------------------------------
enum AppPhase {
    PH_SPLASH = 0, PH_MENU, PH_BANKSEL, PH_BANKMGMT, PH_CONFIRM, PH_BROWSE, PH_RENAME, PH_TOOLS, PH_EE_TOOLS, PH_FW_TOOLS, PH_FW_BACKUP, PH_FW_RPICK,
    PH_FW_RTARGET, PH_FW_RCONFIRM, PH_HDD_TOOLS, PH_HDD_INFO, PH_EE_RESTORE, PH_EE_CONFIRM, PH_FORMAT, PH_FORMAT_CONFIRM, PH_SETTINGS, PH_ABOUT, PH_CLEARCFG
};

static AppPhase s_phase = PH_SPLASH;
static DWORD    s_phaseT0 = 0;
static WORD     s_prevBtn = 0;
static int      s_bankSel = 0;   // highlighted bank in PH_BANKSEL
static int      s_mgmtSel = 0;   // highlighted bank in PH_BANKMGMT
static EosLayout s_layout;        // dynamic bank layout (descriptor mirror)
static int       s_layoutOk = 0;  // 1 = a valid descriptor is loaded
static int       s_extReady = 0;  // DEBUG: STATUS bit5 after last large flash

// Map a bank TABLE index to a descriptor SLOT (0..3), or -1 if the bank is not
// a user bank. Table: idx0=boot, idx1..4=user banks 1..4, idx5=recovery. So a
// user bank's descriptor slot is (idx - 1). Only banks with EF 0x3..0x6 qualify.
static int descSlotForBank(int idx)
{
    unsigned char ef = Bank_Ef(idx);
    if (ef >= 0x3 && ef <= 0x6) return (int)(ef - 0x3);   // 0x3->0 .. 0x6->3
    return -1;
}

// Heal a stale descriptor against actual bank occupancy. A bank flashed before
// the descriptor recorded it (e.g. an older 256K flash, or descriptor drift)
// leaves its slot FREE while the bank table says occupied -> the bank would show
// EMPTY and be counted as free. For each user bank that IS occupied but whose
// descriptor slot is FREE, mark the slot NATIVE (256K). Only touches FREE slots,
// so ext anchors/shadows are never disturbed. Persists only if something changed.
static void reconcileDescriptor(void)
{
    int i;
    // DISPLAY-ONLY heal. We correct the in-memory layout so the bank list and
    // free-slot count reflect real occupancy, but we NEVER write the descriptor
    // back to flash here. Writing it would make the FPGA's descriptor_valid go
    // true and route previously-static 256K banks through the dynamic geometry
    // path -- which regressed bank boot. A descriptor is only ever persisted by
    // an actual ext-bank flash (the only thing that truly needs one).
    if (!s_layoutOk) { Desc_InitEmpty(&s_layout); s_layoutOk = 1; }
    for (i = 0; i < Bank_Count(); ++i) {
        int slot = descSlotForBank(i);
        if (slot < 0) continue;                       // not a user bank
        if (s_layout.slot[slot].state == EOS_SLOT_FREE && Bank_Occupied(i)) {
            s_layout.slot[slot].state = EOS_SLOT_NATIVE;
            s_layout.slot[slot].sizeCode = EOS_SZC_256K;
            s_layout.slot[slot].physBase = 0;
        }
    }
}

// pending destructive action awaiting confirmation (PH_CONFIRM)
enum PendingAct { ACT_NONE = 0, ACT_DELETE, ACT_FLASH };
static int  s_pendAct = ACT_NONE;
static int  s_pendIdx = -1;
static char s_confirmMsg[64] = { 0 };

// file browser (PH_BROWSE) state
#define EOS_IMG_BUF_MAX (1024 * 1024)            // largest bank (1MB)
static unsigned char s_imgBuf[EOS_IMG_BUF_MAX];  // BIOS image staging (heap-free)
static char          s_browsePath[EOS_FILE_PATH_MAX] = { 0 };   // "" = drive list
static EosFileEntry  s_entries[EOS_FILE_MAX_ENTRIES];
static int           s_entCount = 0;
static int           s_browseSel = 0;
static int           s_browseScroll = 0;
static int           s_flashTarget = -1;          // bank idx being flashed
static int           s_browseSong = 0;           // 1 = browsing to pick a bg-music track
static char          s_flashPath[EOS_FILE_PATH_MAX] = { 0 };
static int           s_renameTarget = -1;         // bank idx being renamed
static AppPhase      s_renameReturn = PH_BANKMGMT; // phase to return to after rename

// forward decls (browser block is defined below, before main)
static void DoFlash(int idx, const char* path);
static void browseRefresh(void);

// transient status line after a stub selection
static char  s_status[64] = { 0 };
static DWORD s_statusUntil = 0;

// ---- persistent top-right info HUD -----------------------------------------
// CPU temp / MB (ambient) temp / RAM used-free, refreshed on a timer (SMBus is
// shared, so we poll every ~2s rather than every frame) and drawn on top of
// every screen via the Gfx_End overlay hook.
static EosLive s_live = { -1, -1, 0, 0, 0 };
static DWORD   s_liveNext = 0;      // next GetTickCount() at which to re-poll
static int     s_liveRev16 = 0;     // 1 once we know this is a 1.6 board

static void hudPoll(void)
{
    DWORD now = GetTickCount();
    if (now < s_liveNext) return;
    s_liveNext = now + 2000;         // 2s cadence
    Console_ReadLive(&s_live, s_liveRev16);
}

// Draw one right-aligned "Label: value" line at y; returns next y.
#define HUD_K 0.72f   // HUD text scale (smaller than body text, no clipping)

static int hudLine(int right, int y, const char* label, const char* val, DWORD col)
{
    int wv = Font_TextWidthScaled(val, HUD_K);
    int lx = right - 128;                          // label left-aligned in frame
    Font_DrawScaled(lx, y, label, EOS_PURPLE, HUD_K);
    Font_DrawScaled(right - wv, y, val, col, HUD_K);
    return y + 17;                                 // tighter line pitch for scaled text
}

static void hudDraw(void)
{
    int right = g_scrW - 20;
    int y = 16;
    char buf[16];
    int n;

    if (s_phase == PH_SPLASH) return;   // keep the branded splash clean

    hudPoll();

    // Thin frame (outline, not a solid fill) so labels/values stay legible over
    // any theme. Sized for the scaled 3-line block.
    {
        int bx = g_scrW - 150, by = 10, bw = 140, bh = 62;
        DWORD fr = EOS_PURPLE;
        Gfx_Fill((float)bx, (float)by, (float)bw, 1.0f, fr); // top
        Gfx_Fill((float)bx, (float)(by + bh - 1), (float)bw, 1.0f, fr); // bottom
        Gfx_Fill((float)bx, (float)by, 1.0f, (float)bh, fr); // left
        Gfx_Fill((float)(bx + bw - 1), (float)by, 1.0f, (float)bh, fr); // right
    }

    // CPU temp
    if (s_live.tempOK && s_live.cpuTempC >= 0) {
        n = 0;
        if (s_live.cpuTempC >= 100) buf[n++] = (char)('0' + s_live.cpuTempC / 100);
        if (s_live.cpuTempC >= 10)  buf[n++] = (char)('0' + (s_live.cpuTempC / 10) % 10);
        buf[n++] = (char)('0' + s_live.cpuTempC % 10);
        buf[n++] = ' '; buf[n++] = 'C'; buf[n] = 0;
        y = hudLine(right, y, "CPU", buf, EOS_WHITE);
    }
    else {
        y = hudLine(right, y, "CPU", "-- C", EOS_DIM);
    }

    // MB (ambient) temp
    if (s_live.tempOK && s_live.mbTempC >= 0) {
        n = 0;
        if (s_live.mbTempC >= 100) buf[n++] = (char)('0' + s_live.mbTempC / 100);
        if (s_live.mbTempC >= 10)  buf[n++] = (char)('0' + (s_live.mbTempC / 10) % 10);
        buf[n++] = (char)('0' + s_live.mbTempC % 10);
        buf[n++] = ' '; buf[n++] = 'C'; buf[n] = 0;
        y = hudLine(right, y, "MB", buf, EOS_WHITE);
    }
    else {
        y = hudLine(right, y, "MB", "-- C", EOS_DIM);
    }

    // RAM used / total (e.g. "12/128MB"). Total reads 64 or 128 on Xbox.
    {
        int u = s_live.ramUsedMB, t = s_live.ramTotalMB;
        char* p = buf;
        if (u >= 100) *p++ = (char)('0' + u / 100);
        if (u >= 10)  *p++ = (char)('0' + (u / 10) % 10);
        *p++ = (char)('0' + u % 10);
        *p++ = '/';
        if (t >= 100) *p++ = (char)('0' + t / 100);
        if (t >= 10)  *p++ = (char)('0' + (t / 10) % 10);
        *p++ = (char)('0' + t % 10);
        *p++ = 'M'; *p++ = 'B'; *p = 0;
        y = hudLine(right, y, "RAM", buf, EOS_WHITE);
    }
}


static void SetStatus(const char* msg)
{
    int i = 0; for (; msg[i] && i < 63; ++i) s_status[i] = msg[i];
    s_status[i] = 0;
    s_statusUntil = GetTickCount() + 1500;
}

static bool Pressed(WORD now, WORD prev, WORD mask)
{
    return ((now & mask) && !(prev & mask));
}

static void GotoPhase(AppPhase p)
{
    s_phase = p;
    s_phaseT0 = GetTickCount();
    if (p == PH_MENU)     Menu_Init();
    if (p == PH_BANKSEL) { s_bankSel = 0; s_layoutOk = Desc_Load(&s_layout); reconcileDescriptor(); }
    if (p == PH_BANKMGMT) { s_mgmtSel = 0; s_layoutOk = Desc_Load(&s_layout); reconcileDescriptor(); }
    if (p == PH_SETTINGS) Settings_Enter();
}

// ---------------------------------------------------------------------------
// SPLASH: fade the logo in over ~0.6s, hold, advance on A/START or ~2s timeout.
// ---------------------------------------------------------------------------
static void Splash_Frame(WORD b)
{
    DWORD dt = GetTickCount() - s_phaseT0;

    if (Pressed(b, s_prevBtn, BTN_A) || Pressed(b, s_prevBtn, BTN_START) || dt > 2000) {
        GotoPhase(PH_MENU);
        return;
    }

    DWORD a = (dt < 600) ? (dt * 255 / 600) : 255;     // fade-in alpha
    DWORD mod = EOS_ARGB(a, 255, 255, 255);

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Splash_Draw(g_scrW / 2, g_scrH / 2 - 20, 256, mod);
    Font_DrawCentered(0, g_scrW, g_scrH / 2 + 130, "EOS  LOADER",
        EOS_ARGB(a, 168, 85, 247));
    Gfx_End();
}

// ---------------------------------------------------------------------------
// MENU: navigate + select. Selections are stubs (status line) for the POC.
// ---------------------------------------------------------------------------
static void HandleChoice(int id)
{
    switch (id) {
    case EOS_MENU_LAUNCH_BANK: GotoPhase(PH_BANKSEL);                break;
    case EOS_MENU_BANK_MGMT:   GotoPhase(PH_BANKMGMT);               break;
    case EOS_MENU_TOOLS:       GotoPhase(PH_TOOLS);                  break;
    case EOS_MENU_SETTINGS:    GotoPhase(PH_SETTINGS);                break;
    case EOS_MENU_ABOUT:       GotoPhase(PH_ABOUT);                   break;
    default: break;
    }
}

static void Menu_Frame(WORD b)
{
    int chosen = Menu_Step(b, s_prevBtn);   // edge-detected nav + select
    if (chosen >= 0) HandleChoice(chosen);

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Menu_Draw();
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 94, s_status, EOS_PURPLE);

    // network status / web-UI address, lower-left (inside the TV-safe margin so
    // it never clips on overscan). Bare IP for now -- a scheme/port prefix waits
    // until the FTP-vs-HTTP surface is settled.
    if (Net_IsUp()) {
        Font_Draw(40, g_scrH - 70, Net_Ip(), EOS_PURPLE);
    }
    else if (Net_LinkUp()) {
        Font_Draw(40, g_scrH - 70, "Network: acquiring address...", EOS_DIM);
    }
    else {
        Font_Draw(40, g_scrH - 70, "Network: no link", EOS_DIM);
    }
    Gfx_End();
}

// ---------------------------------------------------------------------------
// BANK SELECT: launchable banks plus a TSOP entry (last). A launches -- real
// banks via 0xEF + SMC warm reset (D0 stays asserted, the FPGA serves the bank);
// TSOP releases D0 and warm-resets into the onboard flash. Neither returns on
// hardware. B returns to the menu. TSOP is always present even with zero banks.
// ---------------------------------------------------------------------------
static void BankSel_Frame(WORD b)
{
    int n = Bank_LaunchCount();
    int cap = (n < EOS_BANK_MAX) ? n : EOS_BANK_MAX;
    int hasDiag = Bank_XbDiagPresent();           // XbDiag entry only when installed
    int diagIdx = hasDiag ? cap : -1;             // XbDiag slot (after the real banks)
    int tsopIdx = cap + (hasDiag ? 1 : 0);        // TSOP is always the last item
    int total = tsopIdx + 1;
    int i;

    if (s_bankSel >= total) s_bankSel = total - 1;

    if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
        s_bankSel = (s_bankSel + total - 1) % total;
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
        s_bankSel = (s_bankSel + 1) % total;

    if (Pressed(b, s_prevBtn, BTN_B)) {
        GotoPhase(PH_MENU);
        return;
    }
    if (Pressed(b, s_prevBtn, BTN_A)) {
        // TSOP releases D0 -> onboard flash; XbDiag pages itself into SDRAM (sync)
        // then boots; a real bank keeps D0 asserted and warm-resets so the FPGA
        // serves it. None return on HW.
        if (s_bankSel == tsopIdx)
            Eos_TsopBoot();
        else if (hasDiag && s_bankSel == diagIdx)
            Eos_LaunchXbDiag();
        else {
            // Every bank launches NORMALLY. If the descriptor marks this bank as
            // an oversized anchor, the FPGA redirects its serve to the ext-region
            // SDRAM copy -- no special launch EF needed here.
            Bank_Launch(Bank_LaunchIndex(s_bankSel));
        }
        return;
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("SELECT BANK");

    {
        const char* names[EOS_BANK_MAX + 2];      // real banks + XbDiag + TSOP
        for (i = 0; i < cap; ++i) names[i] = Bank_Name(Bank_LaunchIndex(i));
        if (hasDiag) names[diagIdx] = "XbDiag Lite";
        names[tsopIdx] = "TSOP  (onboard flash)";
        Ui_Menu3D(names, total, s_bankSel);
    }

    if (cap == 0 && !hasDiag)
        Font_DrawCentered(0, g_scrW, g_scrH - 94,
            "No BIOS banks flashed -- flash from Bank Management", EOS_DIM);

    Ui_Footer("A = LAUNCH   B = BACK");
    Gfx_End();
}

// ---------------------------------------------------------------------------
// BANK MANAGEMENT: list ALL banks with status/size; X deletes (erase + persist)
// with a confirm step. Flash/rename need an image source / text entry and are
// wired but pending those pieces. Boot bank is protected.
// ---------------------------------------------------------------------------
static int appendStr(char* out, int p, const char* s)
{
    while (*s && p < 62) out[p++] = *s++;
    out[p] = 0;
    return p;
}

static const char* sizeStr(int code)
{
    if (code == EOS_BANK_SIZE_512K) return "512K";
    if (code == EOS_BANK_SIZE_1MB)  return "1MB";
    return "256K";
}

static void buildMgmtRow(char* out, int idx)
{
    int p = 0;
    out[0] = 0;
    p = appendStr(out, p, Bank_Name(idx));
    p = appendStr(out, p, "   ");
    if (Bank_IsBoot(idx)) {
        p = appendStr(out, p, "[BOOT]");
    }
    else if (Bank_IsLocked(idx)) {
        p = appendStr(out, p, "[LOCKED]");
    }
    else {
        /* Dynamic layout: for the 4 visible user slots, reflect the descriptor
           (a shadowed slot is greyed as unavailable, an oversized anchor shows
           its true 512K/1MB size). Falls back to the legacy occupancy display
           when no descriptor is loaded. */
        int handled = 0;
        int slot = descSlotForBank(idx);
        if (s_layoutOk && slot >= 0) {
            int st = s_layout.slot[slot].state;
            if (st == EOS_SLOT_SHADOW) {
                p = appendStr(out, p, "[--- UNAVAILABLE ---]");
                handled = 1;
            }
            else if (st == EOS_SLOT_ANCHOR) {
                p = appendStr(out, p, "[");
                p = appendStr(out, p, sizeStr(s_layout.slot[slot].sizeCode));
                p = appendStr(out, p, " READY]");
                handled = 1;
            }
            else if (st == EOS_SLOT_NATIVE) {
                p = appendStr(out, p, "[256K READY]");
                handled = 1;
            }
            // EOS_SLOT_FREE: do NOT force [EMPTY] here. The descriptor is only the
            // source of truth for ext banks (shadow/anchor) and native marks; a
            // FREE slot may still hold a 256K BIOS flashed before it was recorded
            // in the descriptor. Fall through to the real bank-table occupancy so
            // banks 3/4 (or any bank with a blank descriptor slot) show correctly.
        }
        if (!handled) {
            if (Bank_Occupied(idx)) {
                p = appendStr(out, p, "[");
                p = appendStr(out, p, sizeStr(Bank_SizeCode(idx)));
                p = appendStr(out, p, " READY]");
            }
            else {
                p = appendStr(out, p, "[EMPTY]");
            }
        }
    }
}

static void DoDelete(int idx)
{
    int rc, slot;
    if (idx < 0 || Bank_IsLocked(idx)) { SetStatus("Protected bank"); return; }

    slot = descSlotForBank(idx);   // descriptor slot 0..3, or -1 if not a user bank

    // If the descriptor marks this slot as oversized (anchor or shadow), clear it
    // descriptor-first: find the owning anchor, erase its new-region blocks, and
    // free the whole footprint. Also the recovery path for stuck descriptor state.
    if (slot >= 0 && Desc_Load(&s_layout) && s_layout.valid &&
        s_layout.slot[slot].state != EOS_SLOT_FREE &&
        s_layout.slot[slot].state != EOS_SLOT_NATIVE) {
        int anchor = slot, span, j;

        // If this is a shadow, walk back (in slot space) to its owning anchor.
        if (s_layout.slot[slot].state == EOS_SLOT_SHADOW) {
            while (anchor > 0 && s_layout.slot[anchor].state != EOS_SLOT_ANCHOR) --anchor;
        }

        if (s_layout.slot[anchor].state == EOS_SLOT_ANCHOR) {
            unsigned int base = s_layout.slot[anchor].physBase;
            span = Desc_SlotsFor(s_layout.slot[anchor].sizeCode);
            // Erase the anchor's new-region blocks if physBase is sane.
            if (base >= EOS_NEWRGN_BASE && base < (EOS_NEWRGN_BASE + 0x100000)) {
                int firstBlk = (int)((base - EOS_NEWRGN_BASE) / 0x10000);
                int nblk = (span == 4) ? 16 : 8;
                int bk;
                for (bk = 0; bk < nblk && (firstBlk + bk) < 16; ++bk)
                    Flash_EraseBlock(EOS_BANK_NEWREGION, firstBlk + bk);
            }
        }
        else {
            span = 1;   // no anchor found -> just clear this one slot
            anchor = slot;
        }

        // Free the footprint in slot space; clear the matching bank table entries.
        // The bank table index for descriptor slot S is the bank whose EF==0x3+S.
        for (j = 0; j < span && (anchor + j) < EOS_DESC_SLOTS; ++j) {
            int tblIdx;
            s_layout.slot[anchor + j].state = EOS_SLOT_FREE;
            s_layout.slot[anchor + j].sizeCode = EOS_SZC_256K;
            s_layout.slot[anchor + j].physBase = 0;
            tblIdx = Bank_IndexForEf((unsigned char)(0x3 + anchor + j));
            if (tblIdx >= 0) Bank_ClearEntry(tblIdx);
        }
        Desc_Save(&s_layout);
        Config_Save();
        SetStatus("Bank cleared");
        return;
    }

    // normal 256K bank (native or plain) -- default range, exactly as before.
    rc = Flash_EraseBank(Bank_Ef(idx));
    if (rc == EOS_FLASH_OK) {
        if (slot >= 0 && Desc_Load(&s_layout) && s_layout.valid &&
            s_layout.slot[slot].state == EOS_SLOT_NATIVE) {
            s_layout.slot[slot].state = EOS_SLOT_FREE;
            s_layout.slot[slot].sizeCode = EOS_SZC_256K;
            s_layout.slot[slot].physBase = 0;
            Desc_Save(&s_layout);
        }
        Bank_ClearEntry(idx);
        Config_Save();
        SetStatus("Bank cleared");
    }
    else {
        SetStatus("Erase FAILED");
    }
}

static void Confirm_Frame(WORD b)
{
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_pendAct == ACT_DELETE) DoDelete(s_pendIdx);
        else if (s_pendAct == ACT_FLASH)  DoFlash(s_pendIdx, s_flashPath);
        s_pendAct = ACT_NONE; s_pendIdx = -1;
        GotoPhase(PH_BANKMGMT);
        return;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) {
        s_pendAct = ACT_NONE; s_pendIdx = -1;
        GotoPhase(PH_BANKMGMT);
        return;
    }
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Font_DrawCentered(0, g_scrW, 150, "CONFIRM", EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, 210, s_confirmMsg, EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 250, "This cannot be undone.", EOS_DIM);
    Font_DrawCentered(0, g_scrW, g_scrH - 66, "A = YES    B = NO", EOS_DIM);
    Gfx_End();
}


static void About_Frame(WORD b)
{
    if (Pressed(b, s_prevBtn, BTN_A) || Pressed(b, s_prevBtn, BTN_B) || Pressed(b, s_prevBtn, BTN_START)) {
        GotoPhase(PH_MENU);
        return;
    }
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Gfx_SetFilter(FALSE);

    // logo (uses its own LINEAR pass internally)
    Splash_Draw(g_scrW / 2, 120, 132, EOS_WHITE);

    Font_DrawCentered(0, g_scrW, 200, "EOS  FPGA  BIOS  LOADER", EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 226, "Version " EOS_LOADER_VERSION, EOS_DIM);

    Font_DrawCentered(0, g_scrW, 274, "Team Resurgent", EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, 300, "Darkone83", EOS_WHITE);

    Font_DrawCentered(0, g_scrW, 344, "Clean-room original Xbox BIOS loader", EOS_DIM);
    Font_DrawCentered(0, g_scrW, 368, "Tang Nano 20K   /   GW2AR-18C", EOS_DIM);

    if (Net_IsUp())
        Font_DrawCentered(0, g_scrW, 404, "Web UI at the address on the menu", EOS_DIM);

    Font_DrawCentered(0, g_scrW, 444, "B  BACK", EOS_DIM);
    Gfx_End();
}

// ---------------------------------------------------------------------------
// TOOLS: a tidy two-level menu. Top level picks a tool category (EEPROM /
// Firmware); each category has its own Backup/Restore sub-menu. Shared list
// helpers below keep every screen consistent (title pill + scrolling pills).
// ---------------------------------------------------------------------------
#define LIST_VIS 6   // max pill rows that fit above the footer

static int navSel(WORD b, int sel, int count)
{
    if (count <= 0) return 0;
    if (Pressed(b, s_prevBtn, BTN_DPAD_UP))   sel = (sel + count - 1) % count;
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN)) sel = (sel + 1) % count;
    return sel;
}

// Standard list screen: title pill + a scrolling column of pills + status line.
static void listScreen(const char* title, const char** items, int count, int sel)
{
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar(title);
    if (count == 0)
        Font_DrawCentered(0, g_scrW, 220, "(nothing here yet)", EOS_DIM);
    else
        Ui_Menu3D(items, count, sel);             // shared 3D perspective list
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 94, s_status, EOS_PURPLE);
    Ui_Footer("D-PAD  MOVE      A  SELECT      B  BACK");
    Gfx_End();
}

static const char* baseName(const char* path);     // defined below
static void RestoreEeprom_Enter(void);
static void FwBackup_Enter(void);
static void FwRestore_Enter(void);
static void HddTools_Enter(void);
static void Format_Enter(void);

static int s_toolSel = 0;   // top category
static int s_eeToolSel = 0;   // EEPROM sub-menu
static int s_fwToolSel = 0;   // Firmware sub-menu

static void DoBackupEeprom(void)
{
    char path[128]; char msg[96]; const char* fn; int p, rc;
    rc = Eeprom_BackupToHdd(path, (int)sizeof(path));
    if (rc == EOS_EE_OK) {
        fn = baseName(path);
        p = 0; p = appendStr(msg, p, "Saved: "); p = appendStr(msg, p, fn); msg[p] = 0;
        SetStatus(msg);
    }
    else {
        SetStatus("EEPROM backup FAILED -- check HDD (E:)");
    }
}

static void Tools_Frame(WORD b)             // top level: tool categories
{
    static const char* cats[5] = { "EEPROM", "Firmware", "HDD", "Format", "Clear Settings" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_MENU); return; }
    s_toolSel = navSel(b, s_toolSel, 5);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_toolSel == 0) { s_eeToolSel = 0; GotoPhase(PH_EE_TOOLS); }
        else if (s_toolSel == 1) { s_fwToolSel = 0; GotoPhase(PH_FW_TOOLS); }
        else if (s_toolSel == 2) { HddTools_Enter(); }
        else if (s_toolSel == 3) { Format_Enter(); }
        else { GotoPhase(PH_CLEARCFG); }
    }
    listScreen("Tools", cats, 5, s_toolSel);
}

// Clear the two config banks (bank table 0xB + settings 0xC) back to factory.
static void ClearCfg_Frame(WORD b)
{
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    if (Pressed(b, s_prevBtn, BTN_A)) {
        int rc = Config_ClearAll();      // erase 0xB + 0xC, reset theme
        Bank_ResetToFactory();           // reset live bank table so it isn't re-saved stale
        Desc_Erase();                    // wipe descriptor -> back to legacy geometry
        Flash_EraseBank(EOS_BANK_NEWREGION);  // clear the oversized-bank region too
        Theme_Init();                    // re-apply the default theme now
        SetStatus(rc == EOS_FLASH_OK ? "Settings + descriptor cleared" : "Clear FAILED -- flash error");
        GotoPhase(PH_TOOLS);
        return;
    }
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Clear User Settings");
    Font_DrawCentered(0, g_scrW, 120, "Resets the bank table and settings", EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 148, "(config banks 0xB + 0xC) to factory.", EOS_DIM);
    Font_DrawCentered(0, g_scrW, 196, "Bank names + saved settings are wiped.", EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, 224, "Flashed BIOS images are NOT touched.", EOS_DIM);
    Font_DrawCentered(0, g_scrW, 300, "A = CLEAR      B = CANCEL", EOS_WHITE);
    Gfx_End();
}

static void EeTools_Frame(WORD b)           // EEPROM: Backup / Restore
{
    static const char* it[2] = { "Backup EEPROM", "Restore EEPROM" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    s_eeToolSel = navSel(b, s_eeToolSel, 2);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_eeToolSel == 0) DoBackupEeprom();
        else                  RestoreEeprom_Enter();
    }
    listScreen("EEPROM Tools", it, 2, s_eeToolSel);
}

static void FwTools_Frame(WORD b)           // Firmware: Backup / Restore
{
    static const char* it[2] = { "Backup Firmware", "Restore Firmware" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    s_fwToolSel = navSel(b, s_fwToolSel, 2);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_fwToolSel == 0) FwBackup_Enter();
        else                  FwRestore_Enter();
    }
    listScreen("Firmware Tools", it, 2, s_fwToolSel);
}

// ---------------------------------------------------------------------------
// RESTORE EEPROM: pick a backup .bin, auto-back-up the CURRENT eeprom, confirm,
// write, then read-back verify. The pre-restore backup is mandatory -- if it
// fails we abort rather than write without a safety net.
// ---------------------------------------------------------------------------
#define EE_RESTORE_MAX 24
static char          s_eeNames[EE_RESTORE_MAX][64];
static int           s_eeCount = 0;
static int           s_eeSel = 0;
static unsigned char s_eeImg[EOS_EEPROM_SIZE];
static char          s_eeSerial[16];
static char          s_eeSafetyName[64];   // basename of the pre-restore backup

static const char* baseName(const char* path)
{
    const char* fn = path; const char* q;
    for (q = path; *q; ++q) if (*q == '\\') fn = q + 1;
    return fn;
}

static void RestoreEeprom_Enter(void)
{
    s_eeCount = Eeprom_ListBackups(s_eeNames, EE_RESTORE_MAX);
    s_eeSel = 0;
    if (s_eeCount == 0) { SetStatus("No backups in E:\\Eos\\Backups"); return; }
    GotoPhase(PH_EE_RESTORE);
}

static void EeRestore_Frame(WORD b)
{
    int i, y, x, w, rc, p; char full[128]; char path[96];

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
        s_eeSel = (s_eeSel + s_eeCount - 1) % s_eeCount;
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
        s_eeSel = (s_eeSel + 1) % s_eeCount;

    if (Pressed(b, s_prevBtn, BTN_A)) {
        p = 0;
        p = appendStr(full, p, "E:\\Eos\\Backups\\");
        p = appendStr(full, p, s_eeNames[s_eeSel]);
        full[p] = 0;
        rc = Eeprom_LoadBin(full, s_eeImg);
        if (rc != EOS_EE_OK) { SetStatus("Load failed"); return; }
        if (Eeprom_ImageValid(s_eeImg) != EOS_EE_OK) { SetStatus("Invalid image -- refused"); return; }
        Eeprom_ImageSerial(s_eeImg, s_eeSerial);
        // MANDATORY pre-restore backup of the live EEPROM.
        rc = Eeprom_BackupToHdd(path, (int)sizeof(path));
        if (rc != EOS_EE_OK) { SetStatus("Pre-backup FAILED -- aborted"); return; }
        {
            const char* bn = baseName(path); int k = 0;
            for (; bn[k] && k < 63; ++k) s_eeSafetyName[k] = bn[k]; s_eeSafetyName[k] = 0;
        }
        GotoPhase(PH_EE_CONFIRM);
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Restore EEPROM");
    w = 460; x = (g_scrW - w) / 2;
    for (i = 0; i < s_eeCount; ++i) {
        y = 96 + i * UI_ROW_DY;
        Ui_PillCentered(x, y, w, UI_PILL_H, UI_PILL_R, i == s_eeSel, s_eeNames[i]);
    }
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 94, s_status, EOS_PURPLE);
    Ui_Footer("D-PAD  MOVE      A  SELECT      B  BACK");
    Gfx_End();
}

static void EeConfirm_Frame(WORD b)
{
    int i, rc, ok; unsigned char chk[EOS_EEPROM_SIZE]; char line[96]; int p;

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_EE_RESTORE); return; }

    if (Pressed(b, s_prevBtn, BTN_A)) {
        rc = Eeprom_WriteImage(s_eeImg);
        if (rc != EOS_EE_OK) { SetStatus("WRITE FAILED -- EEPROM unchanged"); GotoPhase(PH_TOOLS); return; }
        ok = 1;
        if (Eeprom_ReadImage(chk) == EOS_EE_OK) {
            for (i = 0; i < EOS_EEPROM_SIZE; ++i) if (chk[i] != s_eeImg[i]) { ok = 0; break; }
        }
        else ok = 0;
        SetStatus(ok ? "EEPROM restored + verified" : "Restored -- VERIFY mismatch!");
        GotoPhase(PH_TOOLS);
        return;
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Confirm Restore");

    Font_DrawCentered(0, g_scrW, 110, "This OVERWRITES the console EEPROM.", EOS_WHITE);

    p = 0; p = appendStr(line, p, "From:   "); p = appendStr(line, p, s_eeNames[s_eeSel]); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 156, line, EOS_DIM);
    p = 0; p = appendStr(line, p, "Serial: "); p = appendStr(line, p, s_eeSerial); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 182, line, EOS_DIM);
    p = 0; p = appendStr(line, p, "Current EEPROM saved as:"); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 222, line, EOS_DIM);
    Font_DrawCentered(0, g_scrW, 248, s_eeSafetyName, EOS_PURPLE);

    Font_DrawCentered(0, g_scrW, 300, "A bad EEPROM can stop the console booting.", EOS_DIM);
    Font_DrawCentered(0, g_scrW, 350, "A = WRITE      B = CANCEL", EOS_WHITE);
    Gfx_End();
}

// ---------------------------------------------------------------------------
// FIRMWARE: per-bank backup + restore. Backup dumps a bank's flash to a .bin in
// E:\Eos\Backups\Firmware. Restore is size-matched to the target bank, erases,
// programs, verifies every page, syncs, then marks the bank occupied + saves.
// ---------------------------------------------------------------------------
#define FW_LIST_MAX 64
static char s_fwNames[FW_LIST_MAX][64];
static int  s_fwCount = 0;
static int  s_fwBankSel = 0;   // backup: source bank
static int  s_fwFileSel = 0;   // restore: file pick
static char s_fwFile[64];      // restore: selected file
static int  s_fwTgtSel = 0;   // restore: target bank

static void bankNames(const char** out, int* n)
{
    int i, c = Bank_Count();
    if (c > 8) c = 8;
    for (i = 0; i < c; ++i) out[i] = Bank_Name(i);
    *n = c;
}

static void FwBackup_Enter(void) { s_fwBankSel = 0; GotoPhase(PH_FW_BACKUP); }

static void FwBackup_Frame(WORD b)
{
    const char* names[8]; int n; char path[160]; char msg[96]; int p, rc;
    bankNames(names, &n);
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_FW_TOOLS); return; }
    s_fwBankSel = navSel(b, s_fwBankSel, n);
    if (Pressed(b, s_prevBtn, BTN_A) && n > 0) {
        rc = Firmware_BackupBank(s_fwBankSel, path, (int)sizeof(path));
        if (rc == FW_OK) {
            p = 0; p = appendStr(msg, p, "Saved: "); p = appendStr(msg, p, baseName(path)); msg[p] = 0;
            SetStatus(msg);
        }
        else if (rc == FW_ERR_FLASH) SetStatus("Backup FAILED -- flash read error");
        else                           SetStatus("Backup FAILED -- check HDD (E:)");
    }
    listScreen("Backup Firmware", names, n, s_fwBankSel);
}

static void FwRestore_Enter(void)
{
    s_fwCount = Firmware_ListBackups(s_fwNames, FW_LIST_MAX);
    s_fwFileSel = 0;
    if (s_fwCount == 0) { SetStatus("No firmware backups yet"); return; }
    GotoPhase(PH_FW_RPICK);
}

static void FwRestPick_Frame(WORD b)
{
    const char* items[FW_LIST_MAX]; int i, k;
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_FW_TOOLS); return; }
    s_fwFileSel = navSel(b, s_fwFileSel, s_fwCount);
    if (Pressed(b, s_prevBtn, BTN_A) && s_fwCount > 0) {
        k = 0;
        while (s_fwNames[s_fwFileSel][k] && k < 63) { s_fwFile[k] = s_fwNames[s_fwFileSel][k]; ++k; }
        s_fwFile[k] = 0;
        s_fwTgtSel = 0; GotoPhase(PH_FW_RTARGET);
    }
    for (i = 0; i < s_fwCount; ++i) items[i] = s_fwNames[i];
    {
        char ttl[40]; int tp = 0;
        const char* t = "Restore FW: ";
        while (t[tp] && tp < 30) { ttl[tp] = t[tp]; tp++; }
        if (s_fwCount >= 10) ttl[tp++] = (char)('0' + (s_fwCount / 10) % 10);
        ttl[tp++] = (char)('0' + s_fwCount % 10);
        ttl[tp++] = ' '; ttl[tp++] = 'f'; ttl[tp++] = 'i'; ttl[tp++] = 'l';
        ttl[tp++] = 'e'; ttl[tp++] = 's'; ttl[tp] = 0;
        listScreen(ttl, items, s_fwCount, s_fwFileSel);
    }
}

static void FwRestTarget_Frame(WORD b)
{
    const char* names[8]; int n;
    bankNames(names, &n);
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_FW_RPICK); return; }
    s_fwTgtSel = navSel(b, s_fwTgtSel, n);
    if (Pressed(b, s_prevBtn, BTN_A) && n > 0) GotoPhase(PH_FW_RCONFIRM);
    listScreen("Restore to bank", names, n, s_fwTgtSel);
}

// Pull the <name> out of "fw_<name>_<ef>_<size>_NN.bin" (strips the fw_ prefix,
// the .bin extension, and the trailing _<ef>_<size>_<NN> fields).
static void FwNameFromFile(const char* file, char* out, int outLen)
{
    int len = 0, start = 0, end, cuts, i, o, p;
    while (file[len]) ++len;
    if (file[0] == 'f' && file[1] == 'w' && file[2] == '_') start = 3;
    end = len;
    if (end >= 4 && file[end - 4] == '.') end -= 4;          // strip ".bin"
    for (i = end - 1, cuts = 0; i > start && cuts < 3; --i)  // strip _ef_size_NN
        if (file[i] == '_') { end = i; ++cuts; }
    o = 0;
    for (p = start; p < end && o < outLen - 1; ++p) out[o++] = file[p];
    out[o] = 0;
    if (o == 0) { out[0] = 0; }                              // fallback: empty -> keeps old name
}

static void FwRestConfirm_Frame(WORD b)
{
    char line[96]; int p, rc, fbytes, bbytes, match, code;
    code = Bank_SizeCode(s_fwTgtSel);
    fbytes = Firmware_BinBytes(s_fwFile);
    bbytes = Firmware_BankBytes(code);
    match = (fbytes == bbytes && fbytes > 0);

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_FW_RTARGET); return; }
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (!match) { SetStatus("Size mismatch -- refused"); GotoPhase(PH_FW_TOOLS); return; }
        rc = Firmware_RestoreBank(s_fwFile, s_fwTgtSel);
        if (rc == FW_OK) {
            char fwnm[EOS_BANK_NAMELEN];
            Bank_SetOccupied(s_fwTgtSel, 1, code);
            // Apply the bank name from the restored file, then persist it.
            FwNameFromFile(s_fwFile, fwnm, EOS_BANK_NAMELEN);
            if (fwnm[0]) Bank_SetName(s_fwTgtSel, fwnm);
            Config_Save();
            SetStatus("Restored -- name the bank");
            s_renameTarget = s_fwTgtSel;
            s_renameReturn = PH_FW_TOOLS;
            Osk_Open(OSK_TEXT, Bank_Name(s_fwTgtSel), EOS_BANK_NAMELEN - 1);  // pre-filled, editable
            GotoPhase(PH_RENAME);
            return;
        }
        if (rc == FW_ERR_VERIFY)      SetStatus("Restore FAILED -- verify mismatch");
        else if (rc == FW_ERR_FLASH)  SetStatus("Restore FAILED -- flash error");
        else if (rc == FW_ERR_SIZE)   SetStatus("Restore FAILED -- size mismatch");
        else                          SetStatus("Restore FAILED -- file error");
        GotoPhase(PH_FW_TOOLS);
        return;
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Confirm Restore");
    Font_DrawCentered(0, g_scrW, 110, "This ERASES and rewrites the bank.", EOS_WHITE);
    p = 0; p = appendStr(line, p, "File:   "); p = appendStr(line, p, s_fwFile); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 156, line, EOS_DIM);
    p = 0; p = appendStr(line, p, "Target: "); p = appendStr(line, p, Bank_Name(s_fwTgtSel)); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 182, line, EOS_DIM);
    Font_DrawCentered(0, g_scrW, 222, match ? "Image size matches the bank."
        : "SIZE MISMATCH -- cannot restore",
        match ? EOS_DIM : EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, 300, "A = WRITE      B = CANCEL", EOS_WHITE);
    Gfx_End();
}

// ---------------------------------------------------------------------------
// HDD TOOLS: drive info + ATA security. Unlock removes security; Lock binds the
// drive to this console's key. Both are armed (press A twice). The password is
// derived from the kernel HDD key -- see eos_hdd.
// ---------------------------------------------------------------------------
static EosHddInfo s_hddInfo;
static int        s_hddOk = 0;
static int        s_hddToolSel = 0;
static int        s_hddArm = 0;

static int appendUInt(char* out, int p, unsigned long v)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[p++] = '0'; return p; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) out[p++] = tmp[--n];
    return p;
}

static void hddSecLine(unsigned short s, char* out)
{
    int p = 0;
    if (!(s & HDD_SEC_SUPPORTED)) { p = appendStr(out, p, "Not supported"); out[p] = 0; return; }
    if (s & HDD_SEC_LOCKED) p = appendStr(out, p, "Locked ");
    if (s & HDD_SEC_FROZEN) p = appendStr(out, p, "Frozen ");
    p = appendStr(out, p, (s & HDD_SEC_ENABLED) ? "Enabled" : "Disabled");
    out[p] = 0;
}

static void HddTools_Enter(void)
{
    s_hddToolSel = 0; s_hddArm = 0;
    s_hddOk = (Hdd_Identify(&s_hddInfo) == HDD_OK);
    GotoPhase(PH_HDD_TOOLS);
}

static void HddTools_Frame(WORD b)
{
    static const char* it[3] = { "Drive Info", "Unlock (remove security)", "Lock to this console" };
    int prev, rc;

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    prev = s_hddToolSel; s_hddToolSel = navSel(b, s_hddToolSel, 3);
    if (prev != s_hddToolSel) s_hddArm = 0;

    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_hddToolSel == 0) { GotoPhase(PH_HDD_INFO); return; }
        if (!s_hddOk) { SetStatus("No drive detected"); }
        else if (s_hddToolSel == 1) {                  // UNLOCK (remove security)
            if (!s_hddArm) s_hddArm = 1;
            else {
                rc = Hdd_Unlock(); s_hddArm = 0;
                SetStatus(rc == HDD_OK ? "Security removed"
                    : rc == HDD_ERR_UNSUPP ? "Drive has no security"
                    : rc == HDD_ERR_NODISK ? "No drive detected"
                    : "Unlock FAILED");
                s_hddOk = (Hdd_Identify(&s_hddInfo) == HDD_OK);
            }
        }
        else {                                       // LOCK to this console
            if (!s_hddArm) s_hddArm = 1;
            else {
                rc = Hdd_Lock(); s_hddArm = 0;
                SetStatus(rc == HDD_OK ? "Locked to this console"
                    : rc == HDD_ERR_STATE ? "Already locked"
                    : rc == HDD_ERR_UNSUPP ? "Drive has no security"
                    : rc == HDD_ERR_NODISK ? "No drive detected"
                    : "Lock FAILED");
                s_hddOk = (Hdd_Identify(&s_hddInfo) == HDD_OK);
            }
        }
    }

    if (s_hddArm)
        SetStatus(s_hddToolSel == 1 ? "Press A again to REMOVE security"
            : "Press A again to LOCK this drive");
    listScreen("HDD Tools", it, 3, s_hddToolSel);
}

static void HddInfo_Frame(WORD b)
{
    char line[80]; int p, y;

    if (Pressed(b, s_prevBtn, BTN_B) || Pressed(b, s_prevBtn, BTN_A)) { GotoPhase(PH_HDD_TOOLS); return; }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Drive Info");
    if (!s_hddOk) {
        Font_DrawCentered(0, g_scrW, 160, "No drive detected on the primary channel.", EOS_DIM);
    }
    else {
        y = 120;
        Font_Draw(80, y, "Model", EOS_DIM); Font_Draw(260, y, s_hddInfo.model, EOS_WHITE); y += 40;
        Font_Draw(80, y, "Serial", EOS_DIM); Font_Draw(260, y, s_hddInfo.serial, EOS_WHITE); y += 40;
        p = 0; p = appendUInt(line, p, s_hddInfo.sizeMB / 1024); p = appendStr(line, p, " GB (");
        p = appendUInt(line, p, s_hddInfo.sizeMB); p = appendStr(line, p, " MB)"); line[p] = 0;
        Font_Draw(80, y, "Size", EOS_DIM); Font_Draw(260, y, line, EOS_WHITE); y += 40;
        hddSecLine(s_hddInfo.security, line);
        Font_Draw(80, y, "Security", EOS_DIM); Font_Draw(260, y, line, EOS_WHITE); y += 40;
    }
    Ui_Footer("B  BACK");
    Gfx_End();
}


/* ---- Format (HDD staging: partition + format a fresh drive) ------------- */
static int           s_fmtArm = 0;
static int           s_fmtHasDisk = 0;
static unsigned long s_fmtTotalMB = 0, s_fmtEMB = 0, s_fmtFMB = 0;

static void Format_Enter(void)
{
    s_fmtArm = 0;
    s_fmtHasDisk = Format_PlanInfo(&s_fmtTotalMB, &s_fmtEMB, &s_fmtFMB);
    GotoPhase(PH_FORMAT);
}

static void Format_Frame(WORD b)            /* overview + first gate */
{
    char line[80]; int p, y;

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    if (Pressed(b, s_prevBtn, BTN_A) && s_fmtHasDisk) { s_fmtArm = 0; GotoPhase(PH_FORMAT_CONFIRM); return; }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Stage Hard Drive");

    if (!s_fmtHasDisk) {
        Font_DrawCentered(0, g_scrW, 170, "No drive detected on the primary channel.", EOS_DIM);
        Ui_Footer("B  BACK");
        Gfx_End();
        return;
    }

    Font_DrawCentered(0, g_scrW, 104, "Create the standard Xbox partition layout", EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 128, "and format the entire drive.", EOS_WHITE);

    y = 180;
    Font_Draw(140, y, "Drive size", EOS_DIM);
    p = 0; p = appendUInt(line, p, s_fmtTotalMB / 1024); p = appendStr(line, p, " GB"); line[p] = 0;
    Font_Draw(320, y, line, EOS_WHITE); y += 34;

    Font_Draw(140, y, "C E X Y Z", EOS_DIM);
    Font_Draw(320, y, "system + caches", EOS_WHITE); y += 34;

    Font_Draw(140, y, "F  extended", EOS_DIM);
    if (s_fmtFMB) {
        p = 0; p = appendUInt(line, p, s_fmtFMB / 1024); p = appendStr(line, p, " GB"); line[p] = 0;
        Font_Draw(320, y, line, EOS_WHITE);
    }
    else { Font_Draw(320, y, "none (small drive)", EOS_DIM); }
    y += 46;

    Font_DrawCentered(0, g_scrW, y, "ERASES THE ENTIRE DRIVE.", EOS_PURPLE);

    Ui_Footer("A  CONTINUE     B  CANCEL");
    Gfx_End();
}

static void FormatConfirm_Frame(WORD b)     /* final armed confirm */
{
    if (Pressed(b, s_prevBtn, BTN_B)) { s_fmtArm = 0; GotoPhase(PH_FORMAT); return; }

    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (!s_fmtArm) { s_fmtArm = 1; }
        else {
            int rc = Format_StageDrive();
            s_fmtArm = 0;
            SetStatus(rc == FMT_OK ? "Drive staged -- standard layout written"
                : Format_ErrStr(rc));
            GotoPhase(PH_TOOLS);
            return;
        }
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Stage Hard Drive");
    Font_DrawCentered(0, g_scrW, 150, "This ERASES the entire drive and writes", EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 174, "a fresh partition table.", EOS_WHITE);
    Font_DrawCentered(0, g_scrW, 232,
        s_fmtArm ? "Press A again to STAGE THE DRIVE" : "Press A to confirm",
        EOS_PURPLE);
    Font_DrawCentered(0, g_scrW, 266, "B to cancel", EOS_DIM);
    Ui_Footer("A  CONFIRM     B  CANCEL");
    Gfx_End();
}

static void BankMgmt_Frame(WORD b)
{
    int  n = Bank_Count();
    int  i;

    if (s_mgmtSel >= n) s_mgmtSel = (n > 0) ? n - 1 : 0;

    if (n > 0) {
        if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
            s_mgmtSel = (s_mgmtSel + n - 1) % n;
        if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
            s_mgmtSel = (s_mgmtSel + 1) % n;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_MENU); return; }

    if (Pressed(b, s_prevBtn, BTN_X)) {
        // Delete (erase) non-boot banks -> confirm first. Deletable if the bank
        // is occupied OR the descriptor still marks this slot (anchor/shadow/
        // native) -- so a stuck descriptor entry on a blank bank can be cleared.
        int dslot = descSlotForBank(s_mgmtSel);
        int descMarked = (s_layoutOk && dslot >= 0 &&
            s_layout.slot[dslot].state != EOS_SLOT_FREE);
        if (Bank_IsLocked(s_mgmtSel)) {
            SetStatus("Cannot delete locked bank");
        }
        else if (!Bank_Occupied(s_mgmtSel) && !descMarked) {
            SetStatus("Bank already empty");
        }
        else {
            int p = 0;
            s_confirmMsg[0] = 0;
            p = appendStr(s_confirmMsg, p, "Delete ");
            p = appendStr(s_confirmMsg, p, Bank_Name(s_mgmtSel));
            appendStr(s_confirmMsg, p, " ?");
            s_pendAct = ACT_DELETE; s_pendIdx = s_mgmtSel;
            GotoPhase(PH_CONFIRM);
            return;
        }
    }
    if (Pressed(b, s_prevBtn, BTN_A)) {
        // Flash a BIOS into this bank -> browse for an image file. Large BIOSes
        // auto-place into a free new-region half regardless of the selected bank;
        // a 256K goes into this specific bank, so only block 256K-into-shadow
        // (which DoFlash also guards). Selection is permissive; DoFlash decides.
        if (Bank_IsLocked(s_mgmtSel)) {
            SetStatus("Cannot flash locked bank");
        }
        else {
            s_flashTarget = s_mgmtSel;
            s_browsePath[0] = 0;        // start at the drive list
            browseRefresh();
            GotoPhase(PH_BROWSE);
            return;
        }
    }
    if (Pressed(b, s_prevBtn, BTN_Y)) {
        // Rename this bank via the on-screen keyboard.
        if (Bank_IsLocked(s_mgmtSel)) {
            SetStatus("Cannot rename locked bank");
        }
        else {
            s_renameTarget = s_mgmtSel;
            s_renameReturn = PH_BANKMGMT;
            Osk_Open(OSK_TEXT, Bank_Name(s_mgmtSel), EOS_BANK_NAMELEN - 1);
            GotoPhase(PH_RENAME);
            return;
        }
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("BANK MANAGEMENT");

    {
        /* free-slot / 1MB-budget indicator (dynamic bank layout) */
        char hdr[48]; int hp = 0; int freeSlots;
        freeSlots = s_layoutOk ? Desc_FreeSlots(&s_layout) : 4;
        hp = appendStr(hdr, hp, "User banks:  ");
        hdr[hp++] = (char)("0123456789"[freeSlots & 0x0F]);
        hp = appendStr(hdr, hp, " of 4 free  (1MB budget)");
        hdr[hp] = 0;
        Font_DrawCentered(0, g_scrW, 92, hdr, EOS_DIM);
    }

    {
        static char rows[EOS_BANK_MAX][64];
        const char* ptrs[EOS_BANK_MAX];
        int cap = (n < EOS_BANK_MAX) ? n : EOS_BANK_MAX;
        for (i = 0; i < cap; ++i) { buildMgmtRow(rows[i], i); ptrs[i] = rows[i]; }
        Ui_Menu3D(ptrs, cap, s_mgmtSel);
    }

    Font_DrawCentered(0, g_scrW, g_scrH - 66,
        "A = FLASH   X = DELETE   Y = RENAME   B = BACK", EOS_DIM);
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 94, s_status, EOS_PURPLE);
    Gfx_End();
}

// ---------------------------------------------------------------------------
// FILE BROWSER: navigate drives/folders, select a BIOS image to flash into the
// target bank. Heap-free: image loads into s_imgBuf, capped to bank capacity.
// ---------------------------------------------------------------------------
static int mLen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

static void browseRefresh(void)
{
    if (s_browsePath[0] == 0)
        s_entCount = File_ListDrives(s_entries, EOS_FILE_MAX_ENTRIES);
    else
        s_entCount = File_ListDir(s_browsePath, s_entries, EOS_FILE_MAX_ENTRIES);
    s_browseSel = 0;
    s_browseScroll = 0;
}

static void browseUp(void)
{
    int n = mLen(s_browsePath), i;
    if (n <= 3) { s_browsePath[0] = 0; browseRefresh(); return; }  // -> drive list
    i = n - 1;
    while (i > 0 && s_browsePath[i] != '\\') --i;
    if (i <= 2) s_browsePath[3] = 0;        // back to "X:\"
    else        s_browsePath[i] = 0;
    browseRefresh();
}

static void browseInto(const char* name)
{
    int n, i;
    if (s_browsePath[0] == 0) {
        // drive list: name is "C:" -> "C:\"
        s_browsePath[0] = name[0]; s_browsePath[1] = ':';
        s_browsePath[2] = '\\';    s_browsePath[3] = 0;
    }
    else {
        n = mLen(s_browsePath);
        if (n > 0 && s_browsePath[n - 1] != '\\' && n < EOS_FILE_PATH_MAX - 1)
            s_browsePath[n++] = '\\';
        for (i = 0; name[i] && n < EOS_FILE_PATH_MAX - 1; ++i) s_browsePath[n++] = name[i];
        s_browsePath[n] = 0;
    }
    browseRefresh();
}

static void buildFullPath(char* out, const char* name)
{
    int p = 0, i = 0;
    while (s_browsePath[p] && p < EOS_FILE_PATH_MAX - 1) { out[p] = s_browsePath[p]; ++p; }
    out[p] = 0;
    if (p > 0 && out[p - 1] != '\\' && p < EOS_FILE_PATH_MAX - 1) out[p++] = '\\';
    while (name[i] && p < EOS_FILE_PATH_MAX - 1) out[p++] = name[i++];
    out[p] = 0;
}

static void fileToBankName(char* out, int cap, const char* fname)
{
    int i, dot = -1, p;
    for (i = 0; fname[i]; ++i) if (fname[i] == '.') dot = i;
    for (p = 0; fname[p] && p < cap - 1 && (dot < 0 || p < dot); ++p) out[p] = fname[p];
    out[p] = 0;
}

static int sizeCodeForLen(int len)
{
    if (len <= 256 * 1024) return EOS_BANK_SIZE_256K;
    if (len <= 512 * 1024) return EOS_BANK_SIZE_512K;
    return EOS_BANK_SIZE_1MB;
}

static void DoFlash(int idx, const char* path)
{
    int  got, rc, sc, n, s;
    char nm[EOS_BANK_NAMELEN];

    if (idx < 0 || Bank_IsLocked(idx)) { SetStatus("Protected bank"); return; }

    // Read the file. Up to 1MB so a large BIOS can be read even though the
    // target slot's nominal capacity is 256K (it will go to the new region).
    got = File_ReadInto(path, s_imgBuf, EOS_IMG_BUF_MAX);
    if (got < 0) { SetStatus("Read failed / too big (max 1MB)"); return; }
    if (got == 0) { SetStatus("Empty file"); return; }

    sc = sizeCodeForLen(got);

    // bank name from the file (last path component, extension stripped)
    n = mLen(path); s = n;
    while (s > 0 && path[s - 1] != '\\') --s;
    fileToBankName(nm, EOS_BANK_NAMELEN, path + s);

    if (sc == EOS_BANK_SIZE_256K) {
        // -------- 256K: DEFAULT range, exactly as before. No descriptor. -----
        // A 256K goes into THIS specific bank. Block it if this bank is currently
        // part of an oversized bank (its own anchor, or a shadow of one) -- the
        // user must delete that oversized bank first.
        int dslot = descSlotForBank(idx);
        if (dslot >= 0 && Desc_Load(&s_layout) && s_layout.valid &&
            (s_layout.slot[dslot].state == EOS_SLOT_SHADOW ||
                s_layout.slot[dslot].state == EOS_SLOT_ANCHOR)) {
            SetStatus("Bank used by an oversized BIOS - delete it first");
            return;
        }
        rc = Flash_WriteImage(Bank_Ef(idx), s_imgBuf, got);
        if (rc != EOS_FLASH_OK) { SetStatus("Flash FAILED"); return; }
        Bank_SetOccupied(idx, 1, sc);
        if (nm[0]) Bank_SetName(idx, nm);
        // Record this 256K in the descriptor as NATIVE so the budget/auto-place
        // logic sees the slot as consumed. Without this, a later large-bank
        // auto-place would treat this slot as FREE and overwrite it.
        if (dslot >= 0) {
            if (!Desc_Load(&s_layout) || !s_layout.valid) Desc_InitEmpty(&s_layout);
            s_layout.slot[dslot].state = EOS_SLOT_NATIVE;
            s_layout.slot[dslot].sizeCode = EOS_SZC_256K;
            s_layout.slot[dslot].physBase = 0;
            Desc_Save(&s_layout);
        }
        Config_Save();
        SetStatus("Flashed OK");
        return;
    }

    // -------- large BIOS (512K / 1MB): goes to the NEW REGION -----------------
    // Virtually mapped to the selected slot. Bytes written to the new region at
    // an offset derived from the slot; descriptor records the anchor + shadows.
    {
        int szc = (sc == EOS_BANK_SIZE_1MB) ? EOS_SZC_1MB : EOS_SZC_512K;
        int need = Desc_SlotsFor(szc);
        int slot = -1;   // auto-chosen anchor slot
        unsigned int nrbase;
        int startPage, j;

        if (descSlotForBank(idx) < 0) { SetStatus("Not a user bank"); return; }
        if (!Desc_Load(&s_layout) || !s_layout.valid) Desc_InitEmpty(&s_layout);

        // AUTO-PLACE: find the first valid free anchor for this size, ignoring
        // which bank the user selected. 512K anchors on an even slot (0 or 2) with
        // both it and the next slot free; 1MB needs all four slots free at slot 0.
        if (szc == EOS_SZC_1MB) {
            int allFree = 1;
            for (j = 0; j < EOS_DESC_SLOTS; ++j)
                if (s_layout.slot[j].state != EOS_SLOT_FREE) { allFree = 0; break; }
            if (allFree) slot = 0;
        }
        else {
            int cand;
            for (cand = 0; cand <= 2; cand += 2) {   // even slots 0, 2
                if (s_layout.slot[cand].state == EOS_SLOT_FREE &&
                    s_layout.slot[cand + 1].state == EOS_SLOT_FREE) {
                    slot = cand; break;
                }
            }
        }
        if (slot < 0) {
            SetStatus((szc == EOS_SZC_1MB) ? "1MB needs all banks free" : "No free pair - free some banks");
            return;
        }

        // new-region offset: slots 0/1 -> +0, slots 2/3 -> +512K; 1MB -> +0
        nrbase = (szc == EOS_SZC_1MB) ? EOS_NEWRGN_BASE
            : (slot >= 2) ? (EOS_NEWRGN_BASE + EOS_NEWRGN_HALF)
            : EOS_NEWRGN_BASE;
        startPage = (int)((nrbase - EOS_NEWRGN_BASE) / 256);

        SetStatus("Writing new region...");
        rc = Flash_WriteImageAtNoSync(EOS_BANK_NEWREGION, startPage, s_imgBuf, got);
        if (rc != EOS_FLASH_OK) { SetStatus("Flash FAILED (new region)"); return; }

        // Page the freshly-written new region into its SDRAM home so the bank is
        // launchable now, without needing a cold power-cycle to re-run preload.
        Flash_SyncNewRegion();
        s_extReady = Flash_NewRegionReady();   // DEBUG: did the ext region go resident?

        SetStatus("Writing descriptor...");
        s_layout.slot[slot].state = EOS_SLOT_ANCHOR;
        s_layout.slot[slot].sizeCode = (unsigned char)szc;
        s_layout.slot[slot].physBase = nrbase;
        for (j = 1; j < need; ++j) {
            s_layout.slot[slot + j].state = EOS_SLOT_SHADOW;
            s_layout.slot[slot + j].sizeCode = EOS_SZC_256K;
            s_layout.slot[slot + j].physBase = 0;
        }
        if (Desc_Save(&s_layout) != EOS_FLASH_OK) { SetStatus("Descriptor write FAILED"); return; }

        // Mark occupancy on the ACTUAL anchor bank (the auto-chosen slot), not
        // the bank the user happened to select. The anchor bank's table index is
        // the one whose EF == 0x3 + slot. Its shadow banks are also marked so the
        // UI/launch list stay consistent.
        {
            int anchorTbl = Bank_IndexForEf((unsigned char)(0x3 + slot));
            if (anchorTbl >= 0) {
                Bank_SetOccupied(anchorTbl, 1, sc);
                if (nm[0]) Bank_SetName(anchorTbl, nm);
            }
        }
        Config_Save();
        SetStatus(s_extReady ? "Flashed OK (large) - ext RESIDENT"
            : "Flashed (large) - ext NOT resident!");
        return;
    }
}

static void Browse_Frame(WORD b)
{
    int vis = 10, i, top;

    if (s_browseSel >= s_entCount) s_browseSel = (s_entCount > 0) ? s_entCount - 1 : 0;

    if (s_entCount > 0) {
        if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
            s_browseSel = (s_browseSel + s_entCount - 1) % s_entCount;
        if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
            s_browseSel = (s_browseSel + 1) % s_entCount;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) {
        if (s_browsePath[0] == 0) {
            if (s_browseSong) { s_browseSong = 0; GotoPhase(PH_SETTINGS); return; }
            GotoPhase(PH_BANKMGMT); return;                             // drive list -> back
        }
        browseUp();
    }
    if (s_entCount > 0 && Pressed(b, s_prevBtn, BTN_A)) {
        EosFileEntry* e = &s_entries[s_browseSel];
        if (s_browsePath[0] == 0 || e->is_dir) {
            browseInto(e->name);
        }
        else {
            if (s_browseSong) {
                buildFullPath(s_flashPath, e->name);   // reuse as path scratch
                Config_SetBgmPath(s_flashPath);        // persist the selected track
                s_browseSong = 0;
                GotoPhase(PH_SETTINGS);                // re-lands on THEME (s_returnTheme)
                return;
            }
            // file selected -> confirm flashing it into the target bank
            int p;
            buildFullPath(s_flashPath, e->name);
            s_confirmMsg[0] = 0;
            p = appendStr(s_confirmMsg, 0, "Flash ");
            p = appendStr(s_confirmMsg, p, e->name);
            p = appendStr(s_confirmMsg, p, " -> ");
            appendStr(s_confirmMsg, p, Bank_Name(s_flashTarget));
            s_pendAct = ACT_FLASH; s_pendIdx = s_flashTarget;
            GotoPhase(PH_CONFIRM);
            return;
        }
    }

    // keep the selection inside the visible window
    if (s_browseSel < s_browseScroll) s_browseScroll = s_browseSel;
    if (s_browseSel >= s_browseScroll + vis) s_browseScroll = s_browseSel - vis + 1;

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("SELECT BIOS IMAGE");
    Font_DrawCentered(0, g_scrW, 76, (s_browsePath[0] ? s_browsePath : "(drives)"), EOS_DIM);

    top = 104;
    if (s_entCount == 0)
        Font_DrawCentered(0, g_scrW, 200, "(empty)", EOS_DIM);

    {
        int w = 500, x = (g_scrW - w) / 2;
        for (i = 0; i < vis; ++i) {
            int  ei = s_browseScroll + i;
            int  y = top + i * 30;
            char row[EOS_FILE_NAME_MAX + 4];
            int  p;
            if (ei >= s_entCount) break;
            p = 0; row[0] = 0;
            p = appendStr(row, p, s_entries[ei].name);
            if (s_entries[ei].is_dir) appendStr(row, p, "/");
            Ui_PillLeft(x, y, w, 26, 13, ei == s_browseSel, row);
        }
    }

    Ui_Footer("A = OPEN/SELECT    B = UP/BACK");
    Gfx_End();
}

// ---------------------------------------------------------------------------
// RENAME: OSK overlay; on confirm, persist the new name to the config bank.
// ---------------------------------------------------------------------------
static void Rename_Frame(WORD b)
{
    WORD edges = (WORD)(b & ~s_prevBtn);
    int  r = Osk_Update(edges);

    if (r == 1) {
        char name[EOS_BANK_NAMELEN];
        Osk_GetText(name, sizeof(name));
        if (name[0]) {                       // empty -> keep the old name
            Bank_SetName(s_renameTarget, name);
            SetStatus(Config_Save() == EOS_FLASH_OK ? "Renamed" : "Renamed; cfg save FAILED");
        }
        GotoPhase(s_renameReturn);
        return;
    }
    if (r == -1) { GotoPhase(s_renameReturn); return; }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Osk_Draw();
    Gfx_End();
}

// Start / stop / restart background music to match the persisted settings.
// Called at boot and on leaving Settings -- NOT per frame (StartMusic is heavy).
static void audioSync(void)
{
    static int  s_aReady = 0;
    static char s_aPath[EOS_BGM_PATH_MAX] = { 0 };
    int on = Config_GetBgmOn();
    const char* path = Config_GetBgmPath();
    int i;

    if (!s_aReady) { if (!Audio_Init()) return; s_aReady = 1; }

    if (on && path[0]) {
        int changed = 0;
        for (i = 0; i < EOS_BGM_PATH_MAX - 1; ++i) {
            if (s_aPath[i] != path[i]) { changed = 1; break; }
            if (!path[i]) break;
        }
        if (!Audio_MusicPlaying() || changed) {
            Audio_SetMusicPath(path);
            Audio_StartMusic(1);
            for (i = 0; i < EOS_BGM_PATH_MAX - 1 && path[i]; ++i) s_aPath[i] = path[i];
            s_aPath[i] = 0;
        }
    }
    else {
        if (Audio_MusicPlaying()) Audio_StopMusic();
        s_aPath[0] = 0;
    }
}

void __cdecl main() {
    if (!Gfx_Init())    return;
    InitInput();
    Bank_SetResting();   // boot bank = safe resting selection
    File_MountDrives();  // bind HDD partitions so E:/F:/... resolve for browsing
    Config_Load();       // pull persisted bank table from the Eos config bank
    Bank_XbDiagPresent(); // prime the XbDiag probe cache at boot: the one-time
    // flash read happens here, never in the web request path
    Theme_Init();        // apply the saved theme (recolors the whole UI)
    audioSync();         // start background music if enabled in settings
    // (exercises the real read path; graceful on fresh chip)
    Net_Start();         // bring the network up; DHCP resolves over the next frames
    Ftp_Init();          // FTP service: deferred bind once the link resolves
    Ftp_Want(1);         // enable FTP (2 sessions, xbox/xbox, passive, port 21)
    if (!Font_Init()) { Gfx_Shutdown(); return; }
    if (!Splash_Init()) { Font_Shutdown(); Gfx_Shutdown(); return; }

    // Persistent top-right HUD (CPU/MB temp, RAM). Detect a 1.6 board once so
    // the temp read uses the PIC path (no ADM1032 on 1.6), then draw it on top
    // of every frame via the Gfx overlay hook.
    {
        EosConsole con;
        Console_Read(&con);
        s_liveRev16 = (con.revStr && con.revStr[0] == '1' && con.revStr[2] == '6') ? 1 : 0;
    }
    Gfx_SetOverlay(hudDraw);

    GotoPhase(PH_SPLASH);

    // single frame-driven loop: pump input ONCE, dispatch by phase.
    for (;;) {
        PumpInput();
        WORD b = GetButtons();

        // network + web server, serviced every frame regardless of phase.
        // The HTTP listener follows the link: bound while up, dropped on loss.
        Net_Poll();
        if (Net_IsUp() && !Http_IsUp()) Http_Start();
        if (!Net_IsUp() && Http_IsUp()) Http_Stop();
        Http_Poll();
        Ftp_Tick();   // FTP service (start/stop tracks the link internally)
        Audio_Update();   // service the DirectSound mixer (required every frame)

        if (s_phase == PH_SPLASH)   Splash_Frame(b);
        else if (s_phase == PH_BANKSEL)  BankSel_Frame(b);
        else if (s_phase == PH_BANKMGMT) BankMgmt_Frame(b);
        else if (s_phase == PH_CONFIRM)  Confirm_Frame(b);
        else if (s_phase == PH_BROWSE)   Browse_Frame(b);
        else if (s_phase == PH_RENAME)   Rename_Frame(b);
        else if (s_phase == PH_TOOLS)    Tools_Frame(b);
        else if (s_phase == PH_EE_TOOLS) EeTools_Frame(b);
        else if (s_phase == PH_FW_TOOLS) FwTools_Frame(b);
        else if (s_phase == PH_FW_BACKUP)   FwBackup_Frame(b);
        else if (s_phase == PH_FW_RPICK)    FwRestPick_Frame(b);
        else if (s_phase == PH_FW_RTARGET)  FwRestTarget_Frame(b);
        else if (s_phase == PH_FW_RCONFIRM) FwRestConfirm_Frame(b);
        else if (s_phase == PH_HDD_TOOLS)   HddTools_Frame(b);
        else if (s_phase == PH_HDD_INFO)    HddInfo_Frame(b);
        else if (s_phase == PH_FORMAT)         Format_Frame(b);
        else if (s_phase == PH_FORMAT_CONFIRM) FormatConfirm_Frame(b);
        else if (s_phase == PH_CLEARCFG)       ClearCfg_Frame(b);
        else if (s_phase == PH_EE_RESTORE) EeRestore_Frame(b);
        else if (s_phase == PH_EE_CONFIRM) EeConfirm_Frame(b);
        else if (s_phase == PH_ABOUT)    About_Frame(b);
        else if (s_phase == PH_SETTINGS) {
            Gfx_Begin(EOS_BG); Ui_Backdrop();
            Gfx_SetFilter(FALSE);                 // POINT sampling for text/menu
            {
                int sr = Settings_Frame(b, s_prevBtn);
                if (sr == 1) { GotoPhase(PH_MENU); audioSync(); }
                else if (sr == 2) {   // THEME -> pick a background-music track
                    s_browseSong = 1; s_browsePath[0] = 0; browseRefresh();
                    GotoPhase(PH_BROWSE);
                }
            }
            Gfx_End();
        }
        else                             Menu_Frame(b);

        s_prevBtn = b;   // shared edge-detect baseline across phases
    }
}