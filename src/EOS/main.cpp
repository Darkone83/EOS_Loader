// main.cpp -- EOS Loader entry point.
// Flow: goddess splash (fade-in, skippable) -> main menu loop.
// Menu items are selectable stubs for the POC (Launch Bank / Bank Management /
// Settings). The loader never exits; Launch Bank will later write the Eos 0xEF
// bank register over LPC IO + warm-reset, and the FPGA serves the chosen bank.
#include "eos_gfx.h"
#include "eos_font.h"
#include "eos_console.h"   // Console_ReadLive for the persistent HUD
#include "eos_clock.h"     // Clock_InitFromRtc (X-RTC boot seed)
#include "eos_lcd.h"       // optional SMBus character-LCD status display
#include "eos_splash.h"
#include "eos_menu.h"
#include "input.h"
#include "eos_bank.h"
#include "eos_descriptor.h"
#include "eos_led.h"
#include "eos_config.h"
#include "eos_audio.h"
#include "eos_eeprom_io.h"
#include "eos_firmware_io.h"
#include "eos_hdd.h"
#include "eos_format.h"
#include "eos_flash.h"
#include "eos_file.h"
#include "eos_sdcard.h"    // onboard SD card: FAT32 browse + BIOS precache/launch
#include "ff.h"            // FatFs types used directly in the SD browse UI
#include "eos_osk.h"
#include "eos_settings.h"
#include "eos_theme.h"
#include "eos_theme_custom.h"  // disk-loaded custom themes + set.dat
#include "eos_cerbios.h"        // Cerbios .ini editor + overclock calculator
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
    PH_FW_RTARGET, PH_FW_RCONFIRM, PH_HDD_TOOLS, PH_HDD_INFO, PH_EE_RESTORE, PH_EE_CONFIRM, PH_FORMAT, PH_FORMAT_CONFIRM, PH_SETTINGS, PH_ABOUT, PH_CLEARCFG,
    PH_CERB_MENU, PH_CERB_EDIT, PH_CERB_SAVED, PH_CERB_OC, PH_CERB_COMBO,
    PH_LEDCOLOR, PH_SDBROWSE, PH_EOS_SCRIPTS
};

static AppPhase s_phase = PH_SPLASH;
static DWORD    s_phaseT0 = 0;
static int      s_menuIntro = 0;   // 1 = play the splash->menu settle ONCE
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
static int           s_browseCerb = 0;           // 1 = browsing to pick a Cerbios path field
static int           s_browseCerbField = -1;     // which Cerbios field the pick fills
static int           s_browseScript = 0;         // 1 = browsing to pick an EOS .eos script to stage
static char          s_flashPath[EOS_FILE_PATH_MAX] = { 0 };
static int           s_renameTarget = -1;         // bank idx being renamed
static AppPhase      s_renameReturn = PH_BANKMGMT; // phase to return to after rename

// SD card browser (PH_SDBROWSE) state -- separate from the HDD browser above:
// sourced from our own FAT32 driver (FatFs) over the SD_BR_* LPC registers,
// not File_ListDir/XTL. Root is "" (no drive prefix -- FF_VOLUMES=1).
static char          s_sdPath[EOS_FILE_PATH_MAX] = { 0 };
static EosFileEntry  s_sdEntries[EOS_FILE_MAX_ENTRIES];
static int           s_sdEntCount = 0;
static int           s_sdSel = 0;
static int           s_sdScroll = 0;

// forward decls (browser block is defined below, before main)
static void DoFlash(int idx, const char* path);
static void browseRefresh(void);
static void EnterSdBrowse(void);

// transient status line after a stub selection
static char  s_status[64] = { 0 };
static DWORD s_statusUntil = 0;

// ---- persistent top-right info HUD -----------------------------------------
// CPU temp / MB (ambient) temp / RAM used-free, refreshed on a timer (SMBus is
// shared, so we poll every ~2s rather than every frame) and drawn on top of
// every screen via the Gfx_End overlay hook.
static EosLive s_live = { -1, -1, 0, 0, 0 };
static DWORD   s_liveNext = 0;      // next GetTickCount() at which to re-poll
static int     s_liveRev16 = 0;     // actual Xbox revision (temperature source)
static int     s_eosMode16 = 0;     // physical EOS 1.6_EN strap, STATUS reg bit 1

static void hudPoll(void)
{
    DWORD now = GetTickCount();
    unsigned char eosStatus = 0;

    if (now < s_liveNext) return;
    s_liveNext = now + 2000;         // 2s cadence

    Console_ReadLive(&s_live, s_liveRev16);

    // EOS gateware exposes the physical 1.6_EN state at updater STATUS 0x04:
    // bit 1 = mode_16.  Read it independently of Xbox revision detection so an
    // accidentally-enabled 1.6_EN strap is visible immediately in the HUD.
    Con_SmbReset();
    if (Con_SmbRead8(0xDC, 0x04, &eosStatus))
        s_eosMode16 = (eosStatus & 0x02) ? 1 : 0;
    else
        s_eosMode16 = 0;
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
    // any theme. Grow by one line only while EOS 1.6_EN is asserted.
    {
        int bx = g_scrW - 150, by = 10, bw = 140, bh = s_eosMode16 ? 79 : 62;
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

    // Configuration warning/status: show only when the physical EOS 1.6_EN
    // strap is asserted. This is the gateware mode state, not Xbox revision.
    if (s_eosMode16)
        y = hudLine(right, y, "1.6 Mode", "Enabled", EOS_WHITE);
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

static const char* PhaseName(AppPhase p)
{
    switch (p) {
    case PH_MENU:        return "Main Menu";
    case PH_BANKSEL:     return "Launch Bank";
    case PH_BANKMGMT:    return "Bank Manager";
    case PH_LEDCOLOR:    return "LED Color";
    case PH_BROWSE:      return "Browse";
    case PH_SDBROWSE:    return "SD Card";
    case PH_RENAME:      return "Rename";
    case PH_TOOLS:       return "Tools";
    case PH_EOS_SCRIPTS: return "EOS Scripts";
    case PH_EE_TOOLS:
    case PH_EE_RESTORE:
    case PH_EE_CONFIRM:  return "EEPROM";
    case PH_FW_TOOLS:
    case PH_FW_BACKUP:
    case PH_FW_RPICK:
    case PH_FW_RTARGET:
    case PH_FW_RCONFIRM: return "Firmware";
    case PH_HDD_TOOLS:
    case PH_HDD_INFO:    return "HDD Tools";
    case PH_FORMAT:
    case PH_FORMAT_CONFIRM: return "Format";
    case PH_CLEARCFG:    return "Reset Settings";
    case PH_CERB_MENU:   return "Cerbios Config";
    case PH_CERB_EDIT:
    case PH_CERB_SAVED:  return "Cerbios Editor";
    case PH_CERB_OC:     return "Overclock Calc";
    case PH_CERB_COMBO:  return "Cerbios Editor";
    case PH_SETTINGS:    return "Settings";
    case PH_ABOUT:       return "About";
    default:             return "Eos Loader";
    }
}

static void GotoPhase(AppPhase p)
{
    s_phase = p;
    s_phaseT0 = GetTickCount();
    s_menuIntro = 0;              // any phase change clears a pending settle;
    // the splash handoff re-arms it after this call
    if (p == PH_MENU)     Menu_Init();
    Lcd_SetContext(PhaseName(p), 0);   // LCD row-0 context follows the screen
    // Bank LED is OFF while in the loader (the onboard LED covers loader status).
    // Only the bank-select screen drives it (XbDiag purple / bank color).
    Led_Show(EOS_LED_OFF, 0);
    if (p == PH_BANKSEL) { s_bankSel = 0; s_layoutOk = Desc_Load(&s_layout); reconcileDescriptor(); }
    if (p == PH_BANKMGMT) { s_mgmtSel = 0; s_layoutOk = Desc_Load(&s_layout); reconcileDescriptor(); }
    if (p == PH_SETTINGS) Settings_Enter();
}

// ---------------------------------------------------------------------------
// SPLASH: fade the logo in over ~0.6s, hold, advance on A/START or ~2s timeout.
// ---------------------------------------------------------------------------
// Splash accent-bloom peak intensity (0..255). Kept low: a lit glow, not a flare.
#define EOS_SPLASH_GLOW 90
// Duration of the splash -> menu settle (ms). Short enough to feel snappy.
#define EOS_MENU_INTRO_MS 260

static void Splash_Frame(WORD b)
{
    DWORD dt = GetTickCount() - s_phaseT0;

    if (Pressed(b, s_prevBtn, BTN_A) || Pressed(b, s_prevBtn, BTN_START) || dt > 2000) {
        GotoPhase(PH_MENU);
        s_menuIntro = 1;          // arm the settle AFTER GotoPhase (only path that sets it)
        return;
    }

    DWORD a = (dt < 600) ? (dt * 255 / 600) : 255;     // fade-in alpha
    DWORD mod = EOS_ARGB(a, 255, 255, 255);

    Gfx_Begin(EOS_BG); Ui_Backdrop();

    // Soft accent bloom behind the logo, rising with the fade alpha so the mark
    // reads as lit. Additive + low peak -> a glow, not a flare.
    {
        int gcx = g_scrW / 2, gcy = g_scrH / 2 - 20;
        int gpk = (int)(a * EOS_SPLASH_GLOW / 255);
        Gfx_GlowSoft(gcx, gcy, 340, 340, EOS_GLOW, gpk);
    }

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
    // While the splash->menu settle plays, swallow menu input so an early press
    // can't act on a menu that's still sliding in (and can't leave the intro
    // armed for a later re-entry). The settle is brief, so this costs nothing.
    int introActive = (s_menuIntro &&
        (GetTickCount() - s_phaseT0) < EOS_MENU_INTRO_MS);

    if (!introActive) {
        int chosen = Menu_Step(b, s_prevBtn);   // edge-detected nav + select
        if (chosen >= 0) HandleChoice(chosen);
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();

    // Settle: for the first EOS_MENU_INTRO_MS after the SPLASH only, the logo
    // eases from the splash placement into the header slot and the list arrives
    // just behind it. Every other way into the menu skips straight to settled.
    if (introActive) {
        DWORD idt = GetTickCount() - s_phaseT0;
        Menu_DrawIntro((int)(idt * 255 / EOS_MENU_INTRO_MS));
    }
    else {
        s_menuIntro = 0;
        Menu_Draw();
    }
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
    int tsopIdx = cap + (hasDiag ? 1 : 0);        // TSOP is always the last hard-flash item
    int sdIdx = tsopIdx + 1;                      // SD Card is always last of all
    int total = sdIdx + 1;
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
        // serves it; SD Card opens the FAT32 browser (see EnterSdBrowse). None of
        // the first three return on HW.
        if (s_bankSel == sdIdx) {
            EnterSdBrowse();
        }
        else if (s_bankSel == tsopIdx) {
            Lcd_HandOff("TSOP");   // freeze the LCD on the hand-off screen
            Eos_TsopBoot();
        }
        else if (hasDiag && s_bankSel == diagIdx) {
            Led_Show(EOS_LED_PURPLE, 0);   // XbDiag -> breathing purple
            Lcd_HandOff("XbDiag");
            Eos_LaunchXbDiag();
        }
        else {
            // Every bank launches NORMALLY. If the descriptor marks this bank as
            // an oversized anchor, the FPGA redirects its serve to the ext-region
            // SDRAM copy -- no special launch EF needed here. Set the bank LED
            // (persists across the warm reset into the launched bank): Recovery
            // (EF 0xA) breathes white; a user bank shows its stored color.
            int _li = Bank_LaunchIndex(s_bankSel);
            if (Bank_Ef(_li) == 0x0A)
                Led_Show(EOS_LED_WHITE, 0);
            else
                Led_Show(EOS_LED_SOLID, Desc_GetColor(_li));
            Lcd_HandOff(Bank_Name(_li));
            Bank_Launch(_li);
        }
        return;
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("SELECT BANK");

    {
        const char* names[EOS_BANK_MAX + 3];      // real banks + XbDiag + TSOP + SD Card
        for (i = 0; i < cap; ++i) names[i] = Bank_Name(Bank_LaunchIndex(i));
        if (hasDiag) names[diagIdx] = "XbDiag Lite";
        names[tsopIdx] = "TSOP  (onboard flash)";
        names[sdIdx] = "SD Card";
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

static void EnterEosScripts(void);
static void EosScripts_Frame(WORD b);

// ---- EOS script: persistent flash region (bank 0x09, phys 0x800000, 128K) ----
// The loader is upload-only (§6): read the .eos text, wrap it in the §4.4 validity
// frame, and commit to the script bank with MAGIC programmed LAST (power-loss
// safe). The gateware pages this region into the EXP scratch window at boot and
// runs it. "Present" = a committed MAGIC 'EOSX' in the frame.
#define EOS_SCRIPT_BANK   0x09
#define EOS_SCRIPT_FRAME  16                 // FRAME_SIZE: frame 0x00-0x0F, text @ 0x10
#define EOS_SCRIPT_MAXTXT 0x1FFF0            // MAX_TEXT_LEN = region 0x20000 - frame 0x10

// Expansion runtime mailbox exposed by eos_i2c/eos_exp_mailbox.  The loader
// uses this only after a script SYNC so a successful flash means "actually
// running", not merely "EOSX exists in bank 9".
#define EOS_EXP_SMB_ADDR      0xDC            // 7-bit 0x6E, shifted Xbox SMBus address
#define EOS_EXP_REG_STATUS    0x40
#define EOS_EXP_REG_FAULT     0x41
#define EOS_EXP_ST_RUNNING    0x01
#define EOS_EXP_ST_FAULT      0x02
#define EOS_EXP_ST_VALID      0x04
#define EOS_EXP_ST_BOOT_GATE  0x08

enum ScriptFlashResult {
    SCRIPT_FLASH_OK = 1,
    SCRIPT_FLASH_BADFILE = -1,
    SCRIPT_FLASH_ERASE_FAIL = -2,
    SCRIPT_FLASH_PROGRAM_FAIL = -3,
    SCRIPT_FLASH_MAGIC_FAIL = -4,
    SCRIPT_FLASH_SYNC_FAIL = -5,
    SCRIPT_FLASH_VERIFY_FAIL = -6,
    SCRIPT_FLASH_ENGINE_TIMEOUT = -7,
    SCRIPT_FLASH_ENGINE_FAULT = -8,
    SCRIPT_FLASH_SMBUS_FAIL = -9
};

static int s_scriptPresent = 0;             // cached MAGIC-valid flag (menu grey-out)
static unsigned char s_scriptLastStatus = 0;
static unsigned char s_scriptLastFault = 0;

// CRC-32 (IEEE, poly 0xEDB88320) over the text body -- matches eos_crc32.v.
static unsigned int Script_Crc32(const unsigned char* p, int n)
{
    unsigned int c = 0xFFFFFFFFu; int i, k;
    for (i = 0; i < n; ++i) {
        c ^= (unsigned int)p[i];
        for (k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (unsigned int)(0 - (int)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

// Re-read the committed frame's MAGIC and cache presence for the menu grey-out.
static int Script_RefreshPresent(void)
{
    unsigned char pg[256];
    if (Flash_ReadPage(EOS_SCRIPT_BANK, 0, pg) != EOS_FLASH_OK) { s_scriptPresent = 0; return 0; }
    s_scriptPresent = (pg[0] == 'E' && pg[1] == 'O' && pg[2] == 'S' && pg[3] == 'X') ? 1 : 0;
    return s_scriptPresent;
}
static int Script_Present(void) { return s_scriptPresent; }

// Program an already-erased script region without another block erase.  The
// generic Flash_WriteImageAtNoSync() deliberately erases every covered block;
// Script_FlashFrom() has already erased the full 128K script region so doing
// that again only adds flash wear and another unnecessary state transition.
static int Script_ProgramNoErase(const unsigned char* data, int len)
{
    int pages, page, off, i, rc;
    unsigned char pg[256];

    pages = (len + 255) / 256;
    for (page = 0; page < pages; ++page) {
        off = page * 256;
        for (i = 0; i < 256; ++i)
            pg[i] = (off + i < len) ? data[off + i] : 0xFF;
        rc = Flash_ProgramPage(EOS_SCRIPT_BANK, page, pg);
        if (rc != EOS_FLASH_OK) return rc;
    }
    return EOS_FLASH_OK;
}

// After bank 9 is synced into the expansion scratch window, wait for the
// runtime to either report RUNNING+VALID+BOOT_GATE, raise a fault, or time out.
// This closes the old false-positive path where "EOSX" was in flash but the
// script had never actually started.
static int Script_WaitForRun(void)
{
    int i, sawRead;
    unsigned char st, fc;

    s_scriptLastStatus = 0;
    s_scriptLastFault = 0;
    sawRead = 0;
    Con_SmbReset();

    for (i = 0; i < 100; ++i) {              // up to ~1 second
        st = 0;
        if (Con_SmbRead8(EOS_EXP_SMB_ADDR, EOS_EXP_REG_STATUS, &st)) {
            sawRead = 1;
            s_scriptLastStatus = st;

            if (st & EOS_EXP_ST_FAULT) {
                fc = 0;
                if (Con_SmbRead8(EOS_EXP_SMB_ADDR, EOS_EXP_REG_FAULT, &fc))
                    s_scriptLastFault = fc;
                return SCRIPT_FLASH_ENGINE_FAULT;
            }

            if ((st & (EOS_EXP_ST_RUNNING | EOS_EXP_ST_VALID | EOS_EXP_ST_BOOT_GATE)) ==
                (EOS_EXP_ST_RUNNING | EOS_EXP_ST_VALID | EOS_EXP_ST_BOOT_GATE))
                return SCRIPT_FLASH_OK;
        }
        Sleep(10);
    }

    return sawRead ? SCRIPT_FLASH_ENGINE_TIMEOUT : SCRIPT_FLASH_SMBUS_FAIL;
}

// Erase the region -> blank -> engine idle (§5.5), then resync the scratch view
// (Flash_Sync of bank 0x09 -> reload_base 0x800000 -> scratch; engine revalidates).
static int Script_Clear(void)
{
    if (Flash_EraseBank(EOS_SCRIPT_BANK) != EOS_FLASH_OK) return 0;
    if (Flash_Sync(EOS_SCRIPT_BANK) != EOS_FLASH_OK) return 0;
    Script_RefreshPresent();
    return s_scriptPresent ? 0 : 1;
}

// Read a .eos text file, wrap it in the §4.4 frame, and commit with MAGIC last.
// Returns a ScriptFlashResult so the UI can distinguish storage, sync, and
// runtime failures instead of reporting every failure as a bad .eos file.
static int Script_FlashFrom(const char* src)
{
    int textLen, i, total; unsigned int crc; unsigned char tgt;
    unsigned char magicPage[256];

    // text body lands at frame offset 16; leave 16 bytes for the frame header
    textLen = File_ReadInto(src, s_imgBuf + EOS_SCRIPT_FRAME, EOS_IMG_BUF_MAX - EOS_SCRIPT_FRAME);
    if (textLen <= 0 || textLen > EOS_SCRIPT_MAXTXT) return SCRIPT_FLASH_BADFILE;

    // TARGET must equal the text's TARGET directive (§4.4/§5.5). Default NOHD(0);
    // HD(1) if a "TARGET HD" directive appears in the body.
    tgt = 0;
    for (i = EOS_SCRIPT_FRAME; i + 9 <= EOS_SCRIPT_FRAME + textLen; ++i) {
        if (s_imgBuf[i] == 'T' && s_imgBuf[i + 1] == 'A' && s_imgBuf[i + 2] == 'R' &&
            s_imgBuf[i + 3] == 'G' && s_imgBuf[i + 4] == 'E' && s_imgBuf[i + 5] == 'T' &&
            s_imgBuf[i + 6] == ' ') {
            int j = i + 7;
            while (j < EOS_SCRIPT_FRAME + textLen && s_imgBuf[j] == ' ') ++j;
            if (j + 1 < EOS_SCRIPT_FRAME + textLen && s_imgBuf[j] == 'H' && s_imgBuf[j + 1] == 'D')
                tgt = 1;
            break;
        }
    }

    crc = Script_Crc32(s_imgBuf + EOS_SCRIPT_FRAME, textLen);

    // frame header -- MAGIC left 0xFF so it is programmed LAST (validity commit)
    s_imgBuf[0] = 0xFF; s_imgBuf[1] = 0xFF; s_imgBuf[2] = 0xFF; s_imgBuf[3] = 0xFF;
    s_imgBuf[4] = 0x01;                          // FMT_VER
    s_imgBuf[5] = tgt;                           // TARGET (0 NOHD / 1 HD)
    s_imgBuf[6] = 0x00;                          // FLAGS
    s_imgBuf[7] = 0x00;                          // RESERVED
    s_imgBuf[8] = (unsigned char)(textLen);      s_imgBuf[9] = (unsigned char)(textLen >> 8);
    s_imgBuf[10] = (unsigned char)(textLen >> 16); s_imgBuf[11] = (unsigned char)(textLen >> 24);
    s_imgBuf[12] = (unsigned char)(crc);          s_imgBuf[13] = (unsigned char)(crc >> 8);
    s_imgBuf[14] = (unsigned char)(crc >> 16);    s_imgBuf[15] = (unsigned char)(crc >> 24);

    total = EOS_SCRIPT_FRAME + textLen;

    // §4.4 commit order: erase -> write frame(minus MAGIC) + text -> MAGIC last -> sync
    if (Flash_EraseBank(EOS_SCRIPT_BANK) != EOS_FLASH_OK) return SCRIPT_FLASH_ERASE_FAIL;
    if (Script_ProgramNoErase(s_imgBuf, total) != EOS_FLASH_OK) return SCRIPT_FLASH_PROGRAM_FAIL;

    // Program page 0 again with just the MAGIC: 0xFF elsewhere clears no bits, so
    // the already-written frame fields + first text bytes stay intact.
    magicPage[0] = 'E'; magicPage[1] = 'O'; magicPage[2] = 'S'; magicPage[3] = 'X';
    for (i = 4; i < 256; ++i) magicPage[i] = 0xFF;
    if (Flash_ProgramPage(EOS_SCRIPT_BANK, 0, magicPage) != EOS_FLASH_OK) return SCRIPT_FLASH_MAGIC_FAIL;

    // SYNC must complete before the expansion frame checker can safely consume
    // the scratch copy.  Do not discard this return code.
    if (Flash_Sync(EOS_SCRIPT_BANK) != EOS_FLASH_OK) return SCRIPT_FLASH_SYNC_FAIL;

    // Verify that the committed MAGIC is really present in flash, then verify
    // the expansion runtime itself reached RUNNING rather than just trusting
    // the persistent image.
    if (!Script_RefreshPresent()) return SCRIPT_FLASH_VERIFY_FAIL;
    return Script_WaitForRun();
}

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

// Pill row geometry -- same values eos_settings.cpp uses, redefined here since
// those #defines are local to that translation unit and not visible in main.cpp.
#ifndef PILL_W
#define PILL_W    400
#define PILL_H    38
#define PILL_X    ((g_scrW - PILL_W) / 2)
#define PILL_R    19
#endif

// local: int -> decimal into buf (>=12). Returns buf for inline use.
static const char* iToB(int v, char* buf)
{
    char t[12]; int n = 0, i = 0;
    if (v < 0) { buf[i++] = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) buf[i++] = t[--n];
    buf[i] = 0;
    return buf;
}

// bounded string copy (local; the module's cstrCopy is static to eos_cerbios.cpp)
static void cstrCopy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// Width in px of the substring val[a..b) (b exclusive), measured with the real
// (non-monospace) font metric.
static int subWidth(const char* val, int a, int b)
{
    char tmp[80]; int p = 0, i;
    for (i = a; i < b && val[i] && p < 79; i++) tmp[p++] = val[i];
    tmp[p] = 0;
    return Font_TextWidth(tmp);
}

// Fit a value string into maxPx for a pill's right side.
//  - selected + overflow  -> MARQUEE: the visible window slides across the
//    string (pauses at the ends) so the whole value is readable
//  - unselected + overflow -> leading "..." + the tail that fits (keeps a path's
//    filename, the useful part, visible)
//  - fits                  -> unchanged
// Writes into out (outCap >= 64). tick advances the marquee (a frame counter).
static const char* fitValue(const char* val, int maxPx, int selected,
    DWORD tick, char* out, int outCap)
{
    int len = 0; while (val[len]) len++;
    if (Font_TextWidth(val) <= maxPx) { cstrCopy(out, outCap, val); return out; }

    if (selected) {
        // Character-window marquee. True pixel-smooth scrolling would spill the
        // pill (no scissor available), so we step by whole characters -- but
        // slowly, with clear pauses at both ends, so it reads as a steady crawl
        // rather than a jitter. Find how many characters overflow, then walk a
        // window across them: [pause at head][crawl to tail][pause at tail][wrap].
        int over = 0, i, j, p = 0;
        // count characters we must scroll past so the tail becomes visible
        for (i = 0; val[i]; i++) {
            if (subWidth(val, i, len) <= maxPx) break;   // from i, the tail fits
        }
        over = i;                        // i = first start offset where tail fits
        {
            int hold = 14;               // frames per step -- slow = smooth-reading
            int pause = 10;              // steps held at each end
            int cycle = over + pause * 2;
            int step, start;
            if (cycle < 1) cycle = 1;
            step = (int)((tick / hold) % (unsigned)cycle);
            start = step - pause;        // negative -> still paused at the head
            if (start < 0) start = 0;
            if (start > over) start = over;
            // fill the window from `start` up to what fits in maxPx
            for (j = start; val[j]; j++) {
                if (subWidth(val, start, j + 1) > maxPx) break;
            }
            while (val[start + p] && p < outCap - 1 && (start + p) < j) {
                out[p] = val[start + p]; p++;
            }
            out[p] = 0;
        }
        return out;
    }

    {   // leading "..." then as much of the tail as fits
        int ellW = Font_TextWidth("...");
        int i = len, p = 0;
        while (i > 0 && ellW + subWidth(val, i - 1, len) <= maxPx) i--;
        out[p++] = '.'; out[p++] = '.'; out[p++] = '.';
        while (val[i] && p < outCap - 1) out[p++] = val[i++];
        out[p] = 0;
        return out;
    }
}

// ===========================================================================
// Cerbios Config Editor -- Tools sub-feature.
//   PH_CERB_MENU : pick 3.x.x / Legacy / Overclock Calc
//   PH_CERB_EDIT : scrolling field editor (load-or-create + Save)
//   PH_CERB_SAVED: brief save-result screen
//   PH_CERB_OC   : overclock calculator (enter targets -> show -> confirm write)
// The heavy lifting (parse, round-trip save, calc) lives in eos_cerbios.*.
// ===========================================================================
static CerbConfig s_cerb;               // active config being edited
static int  s_cerbTarget = CERB_NEW;    // which target the editor is on
static int  s_cerbSel = 0;           // selected field row
static int  s_cerbScroll = 0;           // first visible row
static int  s_cerbSaveOk = 0;           // last save result
static int  s_cerbOsk = 0;           // 1 = hex OSK overlay open for a field
static int  s_cerbOskLen = 6;           // expected hex length for the open field
static DWORD s_cerbTick = 0;          // frame counter, drives the value marquee
static int   s_comboSlot = 0;          // selected slot in the combo editor
static int   s_comboIsLed = 0;          // 1 = editing an LED field, 0 = IGR
static char  s_comboBuf[16] = "";       // working copy of the combo value (bounded)
#define CERB_ROWS_VISIBLE 6             // field rows per screen (fits above footer)

// A "Save" pseudo-row sits at the end of the field list.
#define CERB_SAVE_ROW (s_cerb.fieldCount)

static void CerbEdit_Enter(int target)
{
    s_cerbTarget = target;
    File_MountDrives();        // ensure C:/E: symlinks are live before we read
    Cerb_Load(&s_cerb, target);
    s_cerbSel = 0; s_cerbScroll = 0;
    GotoPhase(PH_CERB_EDIT);
}

// --- branch picker ---------------------------------------------------------
static int s_cerbMenuSel = 0;
static void CerbMenu_Frame(WORD b)
{
    static const char* items[3] = { "Cerbios 3.x.x", "Legacy (2.4.2)", "Overclock Calc" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    s_cerbMenuSel = navSel(b, s_cerbMenuSel, 3);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_cerbMenuSel == 0)      CerbEdit_Enter(CERB_NEW);
        else if (s_cerbMenuSel == 1) CerbEdit_Enter(CERB_LEGACY);
        else                         GotoPhase(PH_CERB_OC);
    }
    listScreen("Cerbios Config Editor", items, 3, s_cerbMenuSel);
}

// --- field editor ----------------------------------------------------------
static void CerbEdit_Frame(WORD b)
{
    int total = s_cerb.fieldCount + 1;   // +1 for the Save row
    int i, y;

    // Hex OSK overlay is modal: while open, it owns input + the screen.
    if (s_cerbOsk) {
        WORD edges = (WORD)(b & ~s_prevBtn);
        int r = Osk_Update(edges);
        if (r == 1) {
            char tmp[OSK_MAX_LEN + 1];
            Osk_GetText(tmp, sizeof(tmp));
            // normalise to "0x" + uppercase hex of the expected width.
            {
                char norm[16]; int ni = 0, j = 0;
                // skip a leading 0x/0X in the typed text if present
                if (tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) j = 2;
                norm[ni++] = '0'; norm[ni++] = 'x';
                for (; tmp[j] && ni < 14; j++) {
                    char c = tmp[j];
                    if (c >= 'a' && c <= 'f') c = (char)(c - 32);
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
                        norm[ni++] = c;
                }
                norm[ni] = 0;
                // only store if we captured at least one hex digit
                if (ni > 2) Cerb_Set(&s_cerb, s_cerbSel, norm);
            }
            s_cerbOsk = 0;
        }
        else if (r == -1) {
            s_cerbOsk = 0;   // cancelled -- leave the field unchanged
        }
        Gfx_Begin(EOS_BG); Ui_Backdrop();
        Osk_Draw();
        Gfx_End();
        s_prevBtn = b;   // OSK consumed this frame's input
        return;
    }

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_CERB_MENU); return; }

    // navigation
    if (Pressed(b, s_prevBtn, BTN_DPAD_UP))   s_cerbSel = (s_cerbSel + total - 1) % total;
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN)) s_cerbSel = (s_cerbSel + 1) % total;

    // keep the selection inside the visible window
    if (s_cerbSel < s_cerbScroll) s_cerbScroll = s_cerbSel;
    if (s_cerbSel >= s_cerbScroll + CERB_ROWS_VISIBLE)
        s_cerbScroll = s_cerbSel - CERB_ROWS_VISIBLE + 1;

    // value editing on a field row
    if (s_cerbSel < s_cerb.fieldCount) {
        int k = s_cerb.fields[s_cerbSel].kind;
        if (k == CF_BOOL) {
            if (Pressed(b, s_prevBtn, BTN_A) ||
                Pressed(b, s_prevBtn, BTN_DPAD_LEFT) ||
                Pressed(b, s_prevBtn, BTN_DPAD_RIGHT))
                Cerb_Cycle(&s_cerb, s_cerbSel, +1);
        }
        else if (k == CF_ENUM) {
            if (Pressed(b, s_prevBtn, BTN_DPAD_RIGHT) || Pressed(b, s_prevBtn, BTN_A))
                Cerb_Cycle(&s_cerb, s_cerbSel, +1);
            if (Pressed(b, s_prevBtn, BTN_DPAD_LEFT))
                Cerb_Cycle(&s_cerb, s_cerbSel, -1);
        }
        else if (k == CF_IGR || k == CF_LED) {
            // A -> open the slot combo editor (buttons for IGR, colors for LED).
            if (Pressed(b, s_prevBtn, BTN_A)) {
                {   // bounded copy: combos are <=4 chars; cap defensively
                    const char* src = Cerb_Get(&s_cerb, s_cerbSel);
                    int ci = 0;
                    while (src[ci] && ci < (int)sizeof(s_comboBuf) - 1) {
                        s_comboBuf[ci] = src[ci]; ci++;
                    }
                    s_comboBuf[ci] = 0;
                    // LED always has 4 segments -- pad short/empty values with 'G'
                    if (k == CF_LED) {
                        while (ci < 4) s_comboBuf[ci++] = 'G';
                        s_comboBuf[ci] = 0;
                    }
                }
                s_comboIsLed = (k == CF_LED);
                s_comboSlot = 0;
                GotoPhase(PH_CERB_COMBO);
                s_prevBtn = b;
                return;
            }
        }
        else if (k == CF_HEX6) {
            // A -> open the hex OSK seeded with the current value.
            if (Pressed(b, s_prevBtn, BTN_A)) {
                // seed with the bare hex digits (strip any 0x); user types 0-F.
                const char* cur = Cerb_Get(&s_cerb, s_cerbSel);
                if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) cur += 2;
                s_cerbOskLen = 6;   // up to 6 hex digits (coeffs); LCD addr uses 2
                Osk_Open(OSK_HEX, cur, s_cerbOskLen);
                s_cerbOsk = 1;
                s_prevBtn = b;
                return;
            }
        }
        else {
            // CF_TEXT: FanSpeed cycles; every other CF_TEXT is a file PATH, so A
            // opens the file browser to pick one (Dash/BootAnim/CD paths).
            const char* key = s_cerb.fields[s_cerbSel].iniKey;
            if (!strcmp(key, "FanSpeed")) {
                if (Pressed(b, s_prevBtn, BTN_DPAD_RIGHT)) Cerb_Cycle(&s_cerb, s_cerbSel, +1);
                if (Pressed(b, s_prevBtn, BTN_DPAD_LEFT))  Cerb_Cycle(&s_cerb, s_cerbSel, -1);
            }
            else if (Pressed(b, s_prevBtn, BTN_A)) {
                // browse for a path -> fills this field on select
                File_MountDrives();
                s_browseCerb = 1;
                s_browseCerbField = s_cerbSel;
                s_browsePath[0] = 0;       // start at the drive list
                browseRefresh();
                GotoPhase(PH_BROWSE);
                s_prevBtn = b;
                return;
            }
        }
    }
    else {
        // Save row
        if (Pressed(b, s_prevBtn, BTN_A)) {
            s_cerbSaveOk = Cerb_Save(&s_cerb);
            GotoPhase(PH_CERB_SAVED);
            return;
        }
    }

    // --- render ---
    s_cerbTick++;                         // advance the marquee on the selected row
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Gfx_SetFilter(FALSE);
    Ui_TitleBar(s_cerbTarget == CERB_LEGACY ? "Cerbios 2.4.2  (C:)" : "Cerbios 3.x.x  (E:)");

    // "new file" hint when we built from defaults
    if (!s_cerb.hadFile)
        Font_DrawCentered(0, g_scrW, 74, "(no file found -- new one will be created on Save)", EOS_DIM);

    y = 96;
    for (i = s_cerbScroll; i < s_cerbScroll + CERB_ROWS_VISIBLE && i < total; i++) {
        int selected = (i == s_cerbSel);
        if (i < s_cerb.fieldCount) {
            const char* lbl = s_cerb.fields[i].label;
            const char* rawv = Cerb_Display(&s_cerb, i);
            char fitted[80];
            // value gets the pill width minus the label column (~170px) and margins
            int valMax = PILL_W - 170 - 40;
            const char* val = fitValue(rawv, valMax, selected, s_cerbTick, fitted, sizeof(fitted));
            int dim = (s_cerb.fields[i].kind == CF_HEX6 ||
                s_cerb.fields[i].kind == CF_LED ||
                s_cerb.fields[i].kind == CF_IGR);   // display-only rows
            Ui_PillRow(PILL_X, y, PILL_W, PILL_H, PILL_R, selected, dim, lbl, val);
        }
        else {
            // the Save row -- centered accent pill
            Ui_PillCentered(PILL_X, y, PILL_W, PILL_H, PILL_R, selected, "Save");
        }
        y += 48;
    }

    Ui_Footer("D-PAD MOVE   L/R or A CHANGE   B BACK");
    Gfx_End();
}

// --- IGR / LED slot combo editor -------------------------------------------
// Shows the combo's slots as a horizontal row of pills. DPAD L/R moves between
// slots, DPAD U/D (or A) cycles the selected slot. B/Start commits back to the
// field; the field keeps whatever the slots show.
static void CerbCombo_Frame(WORD b)
{
    int slots, i, x, y;
    int pw = 130, gap = 12, ph = 46;

    slots = s_comboIsLed ? 4 : Cerb_ComboLen(s_comboBuf);
    if (slots < 1) slots = 1;

    // commit + leave
    if (Pressed(b, s_prevBtn, BTN_B) || Pressed(b, s_prevBtn, BTN_START)) {
        Cerb_Set(&s_cerb, s_cerbSel, s_comboBuf);
        GotoPhase(PH_CERB_EDIT);
        return;
    }

    // move between slots
    if (Pressed(b, s_prevBtn, BTN_DPAD_LEFT))  s_comboSlot = (s_comboSlot + slots - 1) % slots;
    if (Pressed(b, s_prevBtn, BTN_DPAD_RIGHT)) s_comboSlot = (s_comboSlot + 1) % slots;

    // cycle the selected slot's value
    if (Pressed(b, s_prevBtn, BTN_DPAD_UP) || Pressed(b, s_prevBtn, BTN_A)) {
        if (s_comboIsLed) Cerb_LedCycle(s_comboBuf, s_comboSlot, +1);
        else              Cerb_IgrCycle(s_comboBuf, s_comboSlot, +1);
    }
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN)) {
        if (s_comboIsLed) Cerb_LedCycle(s_comboBuf, s_comboSlot, -1);
        else              Cerb_IgrCycle(s_comboBuf, s_comboSlot, -1);
    }

    // --- render ---
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Gfx_SetFilter(FALSE);
    Ui_TitleBar(s_cerb.fields[s_cerbSel].label);

    Font_DrawCentered(0, g_scrW, 120,
        s_comboIsLed ? "Set the LED ring colour for each of the 4 segments"
        : "Set the button for each slot in the combo",
        EOS_DIM);

    // center the row of slot pills
    x = (g_scrW - (slots * pw + (slots - 1) * gap)) / 2;
    y = 180;
    for (i = 0; i < slots; i++) {
        int sel = (i == s_comboSlot);
        const char* nm = s_comboIsLed ? Cerb_LedSlotName(s_comboBuf, i)
            : Cerb_IgrSlotName(s_comboBuf, i);
        Ui_PillCentered(x, y, pw, ph, ph / 2, sel, nm);
        x += pw + gap;
    }

    Font_DrawCentered(0, g_scrW, 260, "L/R  slot     UP/DOWN or A  change", EOS_DIM);
    Ui_Footer("B / START  SAVE + BACK");
    Gfx_End();
}

// --- save-result screen ----------------------------------------------------
static void CerbSaved_Frame(WORD b)
{
    if (Pressed(b, s_prevBtn, BTN_A) || Pressed(b, s_prevBtn, BTN_B)) {
        GotoPhase(PH_CERB_EDIT); return;
    }
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Cerbios Editor");
    if (s_cerbSaveOk) {
        Font_DrawCentered(0, g_scrW, 150, "Saved successfully.", EOS_WHITE);
        Font_DrawCentered(0, g_scrW, 178, s_cerb.path, EOS_PURPLE);
        Font_DrawCentered(0, g_scrW, 220, "Restart to apply changes.", EOS_DIM);
    }
    else {
        Font_DrawCentered(0, g_scrW, 150, "SAVE FAILED -- file not written.", EOS_PURPLE);
        Font_DrawCentered(0, g_scrW, 178, s_cerb.path, EOS_DIM);
        Font_DrawCentered(0, g_scrW, 220, "Check the drive is present + writable.", EOS_DIM);
    }
    Font_DrawCentered(0, g_scrW, 300, "A / B = BACK", EOS_WHITE);
    Gfx_End();
}

// --- overclock calculator --------------------------------------------------
// Simple, new-user-friendly: pick a CPU target and a multiplier, and a GPU
// target; the calc shows the achieved clocks + coeffs; A writes both coeffs
// into the 3.x.x ini (leaving the Overclocking toggle to the user).
static int  s_ocRow = 0;             // 0 CPU MHz, 1 mult, 2 GPU MHz, 3 Compute, 4 Write
static int  s_ocCpu = 733;           // target CPU MHz
static int  s_ocMultX10 = 55;            // multiplier * 10 (5.5x stock)
static int  s_ocGpu = 233;           // target GPU MHz
static int  s_ocDone = 0;             // 1 once computed
static int  s_ocAchCpu = 0, s_ocAchRam = 0, s_ocAchFsb = 0, s_ocAchGpu = 0;
static char s_ocCpuHex[16] = "";
static char s_ocGpuHex[16] = "";
static int  s_ocWriteOk = -1;           // -1 none, 0 fail, 1 ok
// held-direction acceleration for value adjust: track which dir is held and how
// many frames. Taps step by 1; holding ramps the step up over time.
static int  s_ocHeldDir = 0;           // -1 left, +1 right, 0 none
static int  s_ocHeldFrames = 0;         // frames the current dir has been held

static void ocClamp(void)
{
    if (s_ocCpu < 500)  s_ocCpu = 500;   if (s_ocCpu > 1400) s_ocCpu = 1400;
    if (s_ocGpu < 155)  s_ocGpu = 155;   if (s_ocGpu > 400)  s_ocGpu = 400;
    if (s_ocMultX10 < 40) s_ocMultX10 = 40; if (s_ocMultX10 > 130) s_ocMultX10 = 130;
}

static void ocCompute(void)
{
    Cerb_CalcCpu(s_ocCpu, s_ocMultX10, s_ocCpuHex, &s_ocAchCpu, &s_ocAchRam, &s_ocAchFsb);
    Cerb_CalcGpu(s_ocGpu, s_ocGpuHex, &s_ocAchGpu);
    s_ocDone = 1;
}

// write only CPUMPLLCoeff + NVPLLCoeff into E:\Cerbios\cerbios.ini (round-trip)
static void ocWrite(void)
{
    CerbConfig c;
    int fi;
    File_MountDrives();        // ensure E: is live before the round-trip read+write
    Cerb_Load(&c, CERB_NEW);
    for (fi = 0; fi < c.fieldCount; fi++) {
        if (!strcmp(c.fields[fi].iniKey, "CPUMPLLCoeff")) Cerb_Set(&c, fi, s_ocCpuHex);
        if (!strcmp(c.fields[fi].iniKey, "NVPLLCoeff"))   Cerb_Set(&c, fi, s_ocGpuHex);
    }
    s_ocWriteOk = Cerb_Save(&c);
}

static void ocIntRow(int y, int selected, const char* label, int value, const char* suffix)
{
    char v[20], nb[12]; int p = 0;
    p = appendStr(v, p, iToB(value, nb));
    if (suffix) p = appendStr(v, p, suffix);
    v[p] = 0;
    Ui_PillRow(PILL_X, y, PILL_W, PILL_H, PILL_R, selected, 0, label, v);
}

// Held-direction acceleration. Call once per frame with the raw button state.
// Returns the signed increment to apply this frame (0 = none). First press
// steps by 1 immediately; then a short delay; then repeats, and the repeat
// STEP grows the longer the button is held so large jumps don't take forever.
//   ~0.0s : single step of 1 (the initial press)
//   ~0.3s+: repeat at 1/frame-group, step 1
//   ~1.0s+: step 2
//   ~2.0s+: step 5
//   ~3.0s+: step 10
static int ocAccelStep(WORD b)
{
    int dir = 0;
    if (b & BTN_DPAD_RIGHT) dir = +1;
    else if (b & BTN_DPAD_LEFT) dir = -1;

    if (dir == 0) { s_ocHeldDir = 0; s_ocHeldFrames = 0; return 0; }

    // direction just pressed (or changed) -> immediate single step, reset timer
    if (dir != s_ocHeldDir) {
        s_ocHeldDir = dir;
        s_ocHeldFrames = 0;
        return dir;                       // one crisp step on the initial press
    }

    // same direction held: advance the hold timer
    s_ocHeldFrames++;

    // ~60fps assumed. Initial hold delay before auto-repeat kicks in.
    if (s_ocHeldFrames < 20) return 0;    // ~0.33s dead time after first step

    {
        int held = s_ocHeldFrames - 20;   // frames since repeat began
        int stepMag, cadence;
        // grow the per-repeat step magnitude with total hold time
        if (s_ocHeldFrames > 180) stepMag = 10;   // >3s
        else if (s_ocHeldFrames > 120) stepMag = 5;    // >2s
        else if (s_ocHeldFrames > 60)  stepMag = 2;    // >1s
        else                           stepMag = 1;
        cadence = 4;                      // apply a repeat every 4 frames (~15/s)
        if (held % cadence != 0) return 0;
        return dir * stepMag;
    }
}

static void CerbOc_Frame(WORD b)
{
    int y;
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_CERB_MENU); return; }

    // Row navigation only steals UP/DOWN; LEFT/RIGHT drive value adjust below.
    if (Pressed(b, s_prevBtn, BTN_DPAD_UP)) { s_ocRow = (s_ocRow + 5 - 1) % 5; s_ocHeldDir = 0; s_ocHeldFrames = 0; }
    if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN)) { s_ocRow = (s_ocRow + 1) % 5; s_ocHeldDir = 0; s_ocHeldFrames = 0; }

    // Accelerated value adjust: 1-unit taps, ramping to bigger steps on hold.
    // Only the three value rows (0 CPU, 1 mult, 2 GPU) respond to L/R.
    {
        int step = (s_ocRow <= 2) ? ocAccelStep(b) : 0;
        if (step != 0) {
            if (s_ocRow == 0)      s_ocCpu += step;   // 1 MHz per unit
            else if (s_ocRow == 1) s_ocMultX10 += step;   // 0.1x per unit
            else if (s_ocRow == 2) s_ocGpu += step;   // 1 MHz per unit
            s_ocDone = 0;
        }
    }
    ocClamp();

    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_ocRow == 3) ocCompute();
        else if (s_ocRow == 4) {
            if (s_ocDone) { ocWrite(); }   // only write after a compute
        }
    }

    // --- render ---
    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Gfx_SetFilter(FALSE);
    Ui_TitleBar("Overclock Calculator");

    y = 80;
    ocIntRow(y, s_ocRow == 0, "CPU Target", s_ocCpu, " MHz");      y += 46;
    // multiplier shown as e.g. "5.5x"
    {
        // multiplier shown as "N.Mx" (e.g. 55 -> "5.5x")
        char mv[12], nb[12]; int p = 0;
        p = appendStr(mv, p, iToB(s_ocMultX10 / 10, nb));
        mv[p++] = '.';
        mv[p++] = (char)('0' + (s_ocMultX10 % 10));
        mv[p++] = 'x'; mv[p] = 0;
        Ui_PillRow(PILL_X, y, PILL_W, PILL_H, PILL_R, s_ocRow == 1, 0, "Multiplier", mv);
    }
    y += 46;
    ocIntRow(y, s_ocRow == 2, "GPU Target", s_ocGpu, " MHz");      y += 46;

    Ui_PillCentered(PILL_X, y, PILL_W, PILL_H, PILL_R, s_ocRow == 3, "Compute"); y += 46;
    {
        int canWrite = s_ocDone;
        Ui_PillCentered(PILL_X, y, PILL_W, PILL_H, PILL_R, s_ocRow == 4,
            canWrite ? "Write to E:\\Cerbios\\cerbios.ini" : "Write (compute first)");
    }
    y += 50;

    // results
    if (s_ocDone) {
        char line[62], nb[12]; int p;
        // "CPU 733  RAM 200   0x230801"
        p = 0;
        p = appendStr(line, p, "CPU ");  p = appendStr(line, p, iToB(s_ocAchCpu, nb));
        p = appendStr(line, p, "  RAM "); p = appendStr(line, p, iToB(s_ocAchRam, nb));
        p = appendStr(line, p, "  ");     p = appendStr(line, p, s_ocCpuHex);
        line[p] = 0;
        Font_DrawCentered(0, g_scrW, y, line, EOS_WHITE); y += 20;
        // "GPU 233   0x011C01"
        p = 0;
        p = appendStr(line, p, "GPU ");  p = appendStr(line, p, iToB(s_ocAchGpu, nb));
        p = appendStr(line, p, "   ");   p = appendStr(line, p, s_ocGpuHex);
        line[p] = 0;
        Font_DrawCentered(0, g_scrW, y, line, EOS_WHITE); y += 24;
    }

    if (s_ocWriteOk == 1)
        Font_DrawCentered(0, g_scrW, y, "Coeffs written. Enable Overclocking in the editor.", EOS_PURPLE);
    else if (s_ocWriteOk == 0)
        Font_DrawCentered(0, g_scrW, y, "WRITE FAILED -- check E: is present.", EOS_PURPLE);

    Ui_Footer("UP/DN MOVE   L/R ADJUST (hold=faster)   A SELECT   B BACK");
    Gfx_End();
}


static void Tools_Frame(WORD b)             // top level: tool categories
{
    static const char* cats[7] = { "EEPROM", "Firmware", "HDD", "Cerbios Config Editor", "EOS Scripts", "Format", "Clear Settings" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_MENU); return; }
    s_toolSel = navSel(b, s_toolSel, 7);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_toolSel == 0) { s_eeToolSel = 0; GotoPhase(PH_EE_TOOLS); }
        else if (s_toolSel == 1) { s_fwToolSel = 0; GotoPhase(PH_FW_TOOLS); }
        else if (s_toolSel == 2) { HddTools_Enter(); }
        else if (s_toolSel == 3) { s_cerbMenuSel = 0; GotoPhase(PH_CERB_MENU); }
        else if (s_toolSel == 4) { EnterEosScripts(); }
        else if (s_toolSel == 5) { Format_Enter(); }
        else { GotoPhase(PH_CLEARCFG); }
    }
    listScreen("Tools", cats, 7, s_toolSel);
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
static EosHddInfo       s_hddInfo;
static EosPartitionInfo s_hddParts[HDD_PART_MAX];
static int              s_hddPartCount = 0;
static int              s_hddOk = 0;
static int              s_hddToolSel = 0;
static int              s_hddArm = 0;

static int appendUInt(char* out, int p, unsigned long v)
{
    char tmp[12]; int n = 0;
    if (v == 0) { out[p++] = '0'; return p; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n > 0) out[p++] = tmp[--n];
    return p;
}

// Append one partition size using the unit chosen for the whole row. For GB we
// keep one decimal place without pulling floating point / sprintf into the UI.
static int hddAppendSize(char* out, int p, unsigned long mb, int useGB)
{
    if (useGB) {
        unsigned long whole = mb / 1024;
        unsigned long tenth = ((mb % 1024) * 10 + 512) / 1024;
        if (tenth >= 10) { ++whole; tenth = 0; }
        p = appendUInt(out, p, whole);
        out[p++] = '.';
        out[p++] = (char)('0' + (int)tenth);
    }
    else {
        p = appendUInt(out, p, mb);
    }
    out[p] = 0;
    return p;
}

static void hddPartLine(const EosPartitionInfo* pi, char* out)
{
    int p = 0;
    int useGB = (pi->totalMB >= 1024);

    p = hddAppendSize(out, p, pi->usedMB, useGB);
    p = appendStr(out, p, " / ");
    p = hddAppendSize(out, p, pi->totalMB, useGB);
    p = appendStr(out, p, useGB ? " GB   " : " MB   ");
    p = hddAppendSize(out, p, pi->freeMB, useGB);
    p = appendStr(out, p, useGB ? " GB free   " : " MB free   ");
    p = appendUInt(out, p, (unsigned long)pi->usedPercent);
    p = appendStr(out, p, "%");
    out[p] = 0;
}

// Drive Info queries filesystem usage once on entry, not every rendered frame.
static void HddInfo_Refresh(void)
{
    s_hddPartCount = 0;
    s_hddOk = (Hdd_Identify(&s_hddInfo) == HDD_OK);
    if (s_hddOk) {
        File_MountDrives();
        s_hddPartCount = Hdd_GetPartitions(s_hddParts, HDD_PART_MAX);
    }
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
    HddInfo_Refresh();
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
        if (s_hddToolSel == 0) {
            HddInfo_Refresh();
            GotoPhase(PH_HDD_INFO);
            return;
        }
        if (!s_hddOk) { SetStatus("No drive detected"); }
        else if (s_hddToolSel == 1) {                  // UNLOCK (remove security)
            if (!s_hddArm) s_hddArm = 1;
            else {
                rc = Hdd_Unlock(); s_hddArm = 0;
                SetStatus(rc == HDD_OK ? "Security removed"
                    : rc == HDD_OK_VSC ? "VSC recovery used - saved E:\\Eos\\unlock.txt"
                    : rc == HDD_OK_VSC_NOSAVE ? "VSC unlocked - password file NOT saved"
                    : rc == HDD_ERR_UNSUPP ? "Drive has no security"
                    : rc == HDD_ERR_NODISK ? "No drive detected"
                    : "Unlock FAILED");
                HddInfo_Refresh();
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
                HddInfo_Refresh();
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
    char line[80]; int p, y, i;

    if (Pressed(b, s_prevBtn, BTN_B) || Pressed(b, s_prevBtn, BTN_A)) { GotoPhase(PH_HDD_TOOLS); return; }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("Drive Info");
    if (!s_hddOk) {
        Font_DrawCentered(0, g_scrW, 160, "No drive detected on the primary channel.", EOS_DIM);
    }
    else {
        // Compact the drive-identify block slightly so C/E/F/G usage fits cleanly
        // on the same 480p screen without scrolling.
        y = 82;
        Font_Draw(70, y, "Model", EOS_DIM); Font_Draw(210, y, s_hddInfo.model, EOS_WHITE); y += 26;
        Font_Draw(70, y, "Serial", EOS_DIM); Font_Draw(210, y, s_hddInfo.serial, EOS_WHITE); y += 26;
        p = 0; p = appendUInt(line, p, s_hddInfo.sizeMB / 1024); p = appendStr(line, p, " GB (");
        p = appendUInt(line, p, s_hddInfo.sizeMB); p = appendStr(line, p, " MB)"); line[p] = 0;
        Font_Draw(70, y, "Size", EOS_DIM); Font_Draw(210, y, line, EOS_WHITE); y += 26;
        hddSecLine(s_hddInfo.security, line);
        Font_Draw(70, y, "Security", EOS_DIM); Font_Draw(210, y, line, EOS_WHITE); y += 36;

        Font_Draw(70, y, "PARTITIONS", EOS_PURPLE); y += 24;

        if (s_hddPartCount <= 0) {
            Font_Draw(100, y, "Partition usage unavailable.", EOS_DIM);
        }
        else {
            for (i = 0; i < s_hddPartCount && i < HDD_PART_MAX; ++i) {
                const EosPartitionInfo* pi = &s_hddParts[i];
                char drv[4];
                int barX = 110;
                int barW = g_scrW - 180;
                int fillW;

                drv[0] = pi->drive; drv[1] = ':'; drv[2] = 0;
                hddPartLine(pi, line);

                Font_Draw(70, y, drv, EOS_WHITE);
                Font_Draw(110, y, line, EOS_DIM);

                // Small percentage bar directly beneath the numeric usage line.
                fillW = (barW * (int)pi->usedPercent) / 100;
                Gfx_Fill((float)barX, (float)(y + 19), (float)barW, 7.0f, EOS_ARGB(72, 255, 255, 255));
                if (fillW > 0)
                    Gfx_Fill((float)barX, (float)(y + 19), (float)fillW, 7.0f, EOS_PURPLE);
                y += 40;
            }
        }
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

    // Black -> set this bank's LED color. Excluded: locked banks and shadowed
    // slots (a slot swallowed by a large bank's shadow has no independent LED
    // color -- the color lives on the anchor).
    if (Pressed(b, s_prevBtn, BTN_BLACK)) {
        int cslot = descSlotForBank(s_mgmtSel);
        if (Bank_IsLocked(s_mgmtSel)) {
            SetStatus("Cannot set color on locked bank");
        }
        else if (cslot < 0) {
            SetStatus("Not a user bank");
        }
        else if (s_layout.slot[cslot].state == EOS_SLOT_SHADOW) {
            SetStatus("Slot is part of a large bank");
        }
        else {
            LedPick_Open(s_mgmtSel, PH_BANKMGMT);
            GotoPhase(PH_LEDCOLOR);
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
        "A = FLASH   X = DELETE   Y = RENAME   Blk = LED COLOR", EOS_DIM);
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

static int s_scriptSel = 0;   // EOS Scripts menu selection

static void EnterEosScripts(void)
{
    Script_RefreshPresent();                  // read the committed frame's MAGIC
    s_scriptSel = Script_Present() ? 1 : 0;   // park on the enabled row
    GotoPhase(PH_EOS_SCRIPTS);
}

// EOS Scripts: two mutually-exclusive actions. Flash stages a picked .eos into
// the slot (enabled only when empty); Clear removes it (enabled only when one is
// present). The disabled row is drawn dimmed and A is gated, so exactly one
// action is ever live.
static void EosScripts_Frame(WORD b)
{
    int present = Script_Present();
    int y = 180;

    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_TOOLS); return; }
    s_scriptSel = navSel(b, s_scriptSel, 2);

    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_scriptSel == 0 && !present) {          // Flash Script -> pick a .eos
            s_browseScript = 1;
            s_browsePath[0] = 0;                     // start at the drive list
            browseRefresh();
            GotoPhase(PH_BROWSE);
            return;
        }
        else if (s_scriptSel == 1 && present) {      // Clear Script
            SetStatus(Script_Clear() ? "Script cleared" : "Clear FAILED");
        }
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("EOS Scripts");
    Ui_PillRow(PILL_X, y, PILL_W, PILL_H, PILL_R, s_scriptSel == 0, present ? 1 : 0,
        "Flash Script", present ? "" : "Select .eos");
    Ui_PillRow(PILL_X, y + 48, PILL_W, PILL_H, PILL_R, s_scriptSel == 1, present ? 0 : 1,
        "Clear Script", present ? "Staged" : "");
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 94, s_status, EOS_PURPLE);
    Ui_Footer("D-PAD  MOVE      A  SELECT      B  BACK");
    Gfx_End();
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
        // Option B: flash is fully committed. Offer the LED color picker as an
        // optional trailing step (B backs out without affecting the flash).
        if (s_flashTarget >= 0) {
            LedPick_Open(s_flashTarget, PH_BANKMGMT);
            GotoPhase(PH_LEDCOLOR);
        }
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
        // Option B: flash committed -> optional LED color picker (B = no change).
        // A large BIOS auto-places into an anchor slot that may differ from the
        // originally-selected bank, so color the ACTUAL anchor, not s_flashTarget.
        {
            int anchorTbl = Bank_IndexForEf((unsigned char)(0x3 + slot));
            if (anchorTbl >= 0) {
                LedPick_Open(anchorTbl, PH_BANKMGMT);
                GotoPhase(PH_LEDCOLOR);
            }
        }
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
            if (s_browseCerb) { s_browseCerb = 0; GotoPhase(PH_CERB_EDIT); return; }
            if (s_browseSong) { s_browseSong = 0; GotoPhase(PH_SETTINGS); return; }
            if (s_browseScript) { s_browseScript = 0; GotoPhase(PH_EOS_SCRIPTS); return; }
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
            if (s_browseCerb) {
                // Cerbios paths use the "HDD0-" device prefix (e.g.
                // HDD0-E:\\Dashboard\\default.xbe). The browser gives a plain
                // "E:\\..." path, so prepend "HDD0-" before storing it.
                char cerbPath[EOS_FILE_PATH_MAX + 8];
                int cp = 0;
                buildFullPath(s_flashPath, e->name);   // plain "X:\\dir\\file"
                cp = appendStr(cerbPath, 0, "HDD0-");
                {
                    int j = 0; while (s_flashPath[j] && cp < (int)sizeof(cerbPath) - 1)
                        cerbPath[cp++] = s_flashPath[j++]; cerbPath[cp] = 0;
                }
                if (s_browseCerbField >= 0)
                    Cerb_Set(&s_cerb, s_browseCerbField, cerbPath);
                s_browseCerb = 0;
                GotoPhase(PH_CERB_EDIT);
                return;
            }
            if (s_browseSong) {
                buildFullPath(s_flashPath, e->name);   // reuse as path scratch
                Config_SetBgmPath(s_flashPath);        // persist the selected track
                s_browseSong = 0;
                GotoPhase(PH_SETTINGS);                // re-lands on THEME (s_returnTheme)
                return;
            }
            if (s_browseScript) {
                int rc;
                char msg[64];
                buildFullPath(s_flashPath, e->name);   // plain "X:\\dir\\file"
                rc = Script_FlashFrom(s_flashPath);    // §4.4 commit + runtime verification

                if (rc == SCRIPT_FLASH_OK) {
                    SetStatus("Script flashed + running");
                }
                else if (rc == SCRIPT_FLASH_BADFILE) {
                    SetStatus("Flash FAILED -- bad/oversized .eos");
                }
                else if (rc == SCRIPT_FLASH_ERASE_FAIL) {
                    SetStatus("Script erase FAILED");
                }
                else if (rc == SCRIPT_FLASH_PROGRAM_FAIL) {
                    SetStatus("Script program FAILED");
                }
                else if (rc == SCRIPT_FLASH_MAGIC_FAIL) {
                    SetStatus("Script commit FAILED");
                }
                else if (rc == SCRIPT_FLASH_SYNC_FAIL) {
                    SetStatus("Script SYNC FAILED");
                }
                else if (rc == SCRIPT_FLASH_VERIFY_FAIL) {
                    SetStatus("Script verify FAILED");
                }
                else if (rc == SCRIPT_FLASH_ENGINE_FAULT) {
                    static const char hx[] = "0123456789ABCDEF";
                    int p = 0;
                    p = appendStr(msg, p, "Script FAULT 0x");
                    msg[p++] = hx[(s_scriptLastFault >> 4) & 0x0F];
                    msg[p++] = hx[s_scriptLastFault & 0x0F];
                    msg[p] = 0;
                    SetStatus(msg);
                }
                else if (rc == SCRIPT_FLASH_SMBUS_FAIL) {
                    SetStatus("Script flashed -- mailbox unreadable");
                }
                else {
                    static const char hx[] = "0123456789ABCDEF";
                    int p = 0;
                    p = appendStr(msg, p, "Engine not running st=0x");
                    msg[p++] = hx[(s_scriptLastStatus >> 4) & 0x0F];
                    msg[p++] = hx[s_scriptLastStatus & 0x0F];
                    msg[p] = 0;
                    SetStatus(msg);
                }
                s_browseScript = 0;
                GotoPhase(PH_EOS_SCRIPTS);
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
    Ui_TitleBar(s_browseCerb ? "SELECT FILE" : (s_browseSong ? "SELECT MUSIC" : (s_browseScript ? "SELECT SCRIPT" : "SELECT BIOS IMAGE")));
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
// SD CARD BROWSER: FAT32, our own driver (eos_sdcard.cpp / FatFs) over the
// SD_BR_* LPC registers -- NOT File_ListDir/XTL (that's the HDD path above).
// This on-console browser remains read/launch only; the WebUI BIOS Manager may
// additionally use the SD_BW_* path to upload/delete BIOS files. Selecting a
// file here resolves it to a raw contiguous LBA run, precaches it into NRGN_SD
// in hardware, and launches bank 0x0 without touching on-board flash.
// ---------------------------------------------------------------------------
static void sdSortEntries(void)
{
    int i, j;
    for (i = 0; i < s_sdEntCount - 1; ++i) {
        for (j = 0; j < s_sdEntCount - 1 - i; ++j) {
            EosFileEntry* a = &s_sdEntries[j];
            EosFileEntry* c = &s_sdEntries[j + 1];
            int swap;
            if (a->is_dir != c->is_dir) {
                swap = c->is_dir && !a->is_dir;
            }
            else {
                int k = 0;
                while (a->name[k] && c->name[k]) {
                    char ca = a->name[k], cb = c->name[k];
                    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
                    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
                    if (ca != cb) break;
                    ++k;
                }
                swap = (unsigned char)a->name[k] > (unsigned char)c->name[k];
            }
            if (swap) { EosFileEntry t = *a; *a = *c; *c = t; }
        }
    }
}

static void sdBrowseRefresh(void)
{
    DIR    dir;
    FILINFO fno;
    FRESULT fr;
    int    n = 0;

    fr = f_opendir(&dir, (s_sdPath[0] == 0) ? "/" : s_sdPath);
    if (fr == FR_OK) {
        for (;;) {
            fr = f_readdir(&dir, &fno);
            if (fr != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.') continue;       // skip dotfiles / "." / ".."
            if (n >= EOS_FILE_MAX_ENTRIES) break;
            {
                int k = 0;
                while (fno.fname[k] && k < EOS_FILE_NAME_MAX - 1) {
                    s_sdEntries[n].name[k] = fno.fname[k]; ++k;
                }
                s_sdEntries[n].name[k] = 0;
            }
            s_sdEntries[n].is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
            ++n;
        }
        f_closedir(&dir);
    }
    s_sdEntCount = n;
    s_sdSel = 0;
    s_sdScroll = 0;
    sdSortEntries();
}

static void sdBrowseUp(void)
{
    int n = mLen(s_sdPath), i;
    if (n == 0) return;                        // already at root
    i = n - 1;
    while (i > 0 && s_sdPath[i] != '/') --i;
    s_sdPath[i] = 0;
    sdBrowseRefresh();
}

static void sdBrowseInto(const char* name)
{
    int n = mLen(s_sdPath), i;
    if (n > 0 && s_sdPath[n - 1] != '/' && n < EOS_FILE_PATH_MAX - 1) s_sdPath[n++] = '/';
    for (i = 0; name[i] && n < EOS_FILE_PATH_MAX - 1; ++i) s_sdPath[n++] = name[i];
    s_sdPath[n] = 0;
    sdBrowseRefresh();
}

static void sdBuildFullPath(char* out, const char* name)
{
    int p = 0, i = 0;
    while (s_sdPath[p] && p < EOS_FILE_PATH_MAX - 1) { out[p] = s_sdPath[p]; ++p; }
    if (p > 0 && out[p - 1] != '/' && p < EOS_FILE_PATH_MAX - 1) out[p++] = '/';
    while (name[i] && p < EOS_FILE_PATH_MAX - 1) out[p++] = name[i++];
    out[p] = 0;
}

// Called from BankSel_Frame when "SD Card" is chosen. Mounts the volume (cheap,
// idempotent-ish -- f_mount just (re)binds the FATFS object) and lists the
// root; only switches to PH_SDBROWSE on success, so a missing/bad card just
// shows a status line and leaves the user on the bank-select screen.
static void EnterSdBrowse(void)
{
    int rc = Sd_Mount();
    if (rc != EOS_SD_OK) {
        SetStatus(Sd_CardReady() ? "SD card: not a valid FAT32 volume" : "No SD card detected");
        return;
    }
    s_sdPath[0] = 0;
    sdBrowseRefresh();
    GotoPhase(PH_SDBROWSE);
}

static void SdBrowse_Frame(WORD b)
{
    int vis = 10, i, top;

    if (s_sdSel >= s_sdEntCount) s_sdSel = (s_sdEntCount > 0) ? s_sdEntCount - 1 : 0;

    if (s_sdEntCount > 0) {
        if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
            s_sdSel = (s_sdSel + s_sdEntCount - 1) % s_sdEntCount;
        if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
            s_sdSel = (s_sdSel + 1) % s_sdEntCount;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) {
        if (s_sdPath[0] == 0) { GotoPhase(PH_BANKSEL); return; }
        sdBrowseUp();
    }
    if (s_sdEntCount > 0 && Pressed(b, s_prevBtn, BTN_A)) {
        EosFileEntry* e = &s_sdEntries[s_sdSel];
        if (e->is_dir) {
            sdBrowseInto(e->name);
        }
        else {
            char    path[EOS_FILE_PATH_MAX];
            FIL     fp;
            FRESULT fr;
            sdBuildFullPath(path, e->name);
            fr = f_open(&fp, path, FA_READ);
            if (fr != FR_OK) {
                SetStatus("Could not open file");
            }
            else {
                unsigned long lba; unsigned int sectors; int szc;
                int rc = Sd_ResolveFile(&fp, &lba, &sectors, &szc);
                f_close(&fp);
                if (rc == EOS_SD_FRAGMENTED)
                    SetStatus("File is fragmented -- copy it fresh to the card");
                else if (rc == EOS_SD_TOOBIG)
                    SetStatus("Not a 256K/512K/1MB BIOS image");
                else if (rc == EOS_SD_OK) {
                    // Staging feedback: SD Card gets its own distinct color
                    // (magenta/neon pink) -- not a reuse of XbDiag's purple,
                    // even though both are "paging into SDRAM before boot".
                    Led_Show(EOS_LED_MAGENTA, 0);
                    Lcd_HandOff(e->name);
                    Sd_PrecacheAndLaunch(lba, sectors, szc);   // no return on success
                    SetStatus("SD card error during staging");  // only reached on failure
                }
            }
        }
    }

    if (s_sdSel < s_sdScroll) s_sdScroll = s_sdSel;
    if (s_sdSel >= s_sdScroll + vis) s_sdScroll = s_sdSel - vis + 1;

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("SELECT BIOS (SD CARD)");
    Font_DrawCentered(0, g_scrW, 76, (s_sdPath[0] ? s_sdPath : "(root)"), EOS_DIM);

    top = 104;
    if (s_sdEntCount == 0)
        Font_DrawCentered(0, g_scrW, 200, "(empty)", EOS_DIM);

    {
        int w = 500, x = (g_scrW - w) / 2;
        for (i = 0; i < vis; ++i) {
            int  ei = s_sdScroll + i;
            int  y = top + i * 30;
            char row[EOS_FILE_NAME_MAX + 4];
            int  p = 0;
            if (ei >= s_sdEntCount) break;
            {
                int k = 0;
                while (s_sdEntries[ei].name[k] && p < (int)sizeof(row) - 2) { row[p++] = s_sdEntries[ei].name[k]; ++k; }
            }
            if (s_sdEntries[ei].is_dir) row[p++] = '/';
            row[p] = 0;
            Ui_PillLeft(x, y, w, 26, 13, ei == s_sdSel, row);
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
    int on;
    const char* path;
    int i;

    if (!s_aReady) { if (!Audio_Init()) return; s_aReady = 1; }

    // Audio source precedence: a custom theme's music takes center stage --
    // the global BGM never plays alongside it. Otherwise fall back to the
    // global BGM (persisted on/off + path in flash). A theme whose music
    // file is missing resolves as 'no theme music', landing here on global.
    if (ThemeCustom_HasMusic()) { on = 1; path = ThemeCustom_MusicPath(); }
    else { on = Config_GetBgmOn(); path = Config_GetBgmPath(); }

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
    Theme_Init();        // built-in theme (fallback base)
    ThemeCustom_EnsureDir();               // create E:\Eos\Themes if missing
    {   // If a custom theme is selected (E:\Eos\set.dat), apply it over the
        // built-in: colors + background + resolve its music. A stale/broken
        // selection falls back to the built-in and clears set.dat.
        char folder[EOS_FILE_NAME_MAX];
        if (SetDat_Read(folder, sizeof(folder))) {
            if (!ThemeCustom_Apply(folder)) SetDat_Clear();
        }
    }
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
    Clock_InitFromRtc();   // X-RTC (if present) is the source of truth for date/time
    Lcd_Init();            // optional SMBus status LCD (no-op if none present)
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
        Lcd_Tick(&s_live);   // optional status LCD (throttled + shadow-diffed; no-op if none)

        if (s_phase == PH_SPLASH)   Splash_Frame(b);
        else if (s_phase == PH_BANKSEL)  BankSel_Frame(b);
        else if (s_phase == PH_BANKMGMT) BankMgmt_Frame(b);
        else if (s_phase == PH_LEDCOLOR) {
            int nx = LedPick_Frame(b, s_prevBtn);
            if (nx >= 0) GotoPhase((AppPhase)nx);
        }
        else if (s_phase == PH_CONFIRM)  Confirm_Frame(b);
        else if (s_phase == PH_BROWSE)   Browse_Frame(b);
        else if (s_phase == PH_SDBROWSE) SdBrowse_Frame(b);
        else if (s_phase == PH_RENAME)   Rename_Frame(b);
        else if (s_phase == PH_TOOLS)    Tools_Frame(b);
        else if (s_phase == PH_EOS_SCRIPTS) EosScripts_Frame(b);
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
        else if (s_phase == PH_CERB_MENU)  CerbMenu_Frame(b);
        else if (s_phase == PH_CERB_EDIT)  CerbEdit_Frame(b);
        else if (s_phase == PH_CERB_SAVED) CerbSaved_Frame(b);
        else if (s_phase == PH_CERB_OC)    CerbOc_Frame(b);
        else if (s_phase == PH_CERB_COMBO) CerbCombo_Frame(b);
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