// main.cpp -- EOS Loader entry point.
// Flow: goddess splash (fade-in, skippable) -> main menu loop.
// Menu items are selectable stubs for the POC (Launch Bank / Bank Management /
// Settings). The loader never exits; Launch Bank will later write the Eos 0xEF
// bank register over LPC IO + warm-reset, and the FPGA serves the chosen bank.
#include "eos_gfx.h"
#include "eos_font.h"
#include "eos_splash.h"
#include "eos_menu.h"
#include "input.h"
#include "eos_bank.h"
#include "eos_config.h"
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
    PH_FW_RTARGET, PH_FW_RCONFIRM, PH_HDD_TOOLS, PH_HDD_INFO, PH_EE_RESTORE, PH_EE_CONFIRM, PH_FORMAT, PH_FORMAT_CONFIRM, PH_SETTINGS, PH_ABOUT
};

static AppPhase s_phase = PH_SPLASH;
static DWORD    s_phaseT0 = 0;
static WORD     s_prevBtn = 0;
static int      s_bankSel = 0;   // highlighted bank in PH_BANKSEL
static int      s_mgmtSel = 0;   // highlighted bank in PH_BANKMGMT

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
static char          s_flashPath[EOS_FILE_PATH_MAX] = { 0 };
static int           s_renameTarget = -1;         // bank idx being renamed

// forward decls (browser block is defined below, before main)
static void DoFlash(int idx, const char* path);
static void browseRefresh(void);

// transient status line after a stub selection
static char  s_status[64] = { 0 };
static DWORD s_statusUntil = 0;

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
    if (p == PH_BANKSEL)  s_bankSel = 0;
    if (p == PH_BANKMGMT) s_mgmtSel = 0;
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
        Font_DrawCentered(0, g_scrW, 392, s_status, EOS_PURPLE);

    // network status / web-UI address, lower-left (inside the TV-safe margin so
    // it never clips on overscan). Bare IP for now -- a scheme/port prefix waits
    // until the FTP-vs-HTTP surface is settled.
    if (Net_IsUp()) {
        Font_Draw(40, g_scrH - 56, Net_Ip(), EOS_PURPLE);
    }
    else if (Net_LinkUp()) {
        Font_Draw(40, g_scrH - 56, "Network: acquiring address...", EOS_DIM);
    }
    else {
        Font_Draw(40, g_scrH - 56, "Network: no link", EOS_DIM);
    }
    Gfx_End();
}

// ---------------------------------------------------------------------------
// BANK SELECT: list selectable banks; A launches (0xEF write + SMC warm reset,
// does not return), B returns to the menu.
// ---------------------------------------------------------------------------
static void BankSel_Frame(WORD b)
{
    int n = Bank_LaunchCount();
    int listY, i;

    if (s_bankSel >= n) s_bankSel = (n > 0) ? (n - 1) : 0;

    if (n > 0) {
        if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
            s_bankSel = (s_bankSel + n - 1) % n;
        if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
            s_bankSel = (s_bankSel + 1) % n;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) {
        GotoPhase(PH_MENU);
        return;
    }
    if (n > 0 && Pressed(b, s_prevBtn, BTN_A)) {
        // Select + warm-reset into the chosen bank. Does not return on HW.
        Bank_Launch(Bank_LaunchIndex(s_bankSel));
        return;
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("SELECT BANK");

    if (n == 0) {
        Font_DrawCentered(0, g_scrW, 210, "No BIOS banks populated", EOS_WHITE);
        Font_DrawCentered(0, g_scrW, 248, "Flash a BIOS from Bank Management", EOS_DIM);
        Ui_Footer("B = BACK");
        Gfx_End();
        return;
    }

    {
        int w = 360, x = (g_scrW - w) / 2;
        listY = 110;
        for (i = 0; i < n; ++i) {
            int idx = Bank_LaunchIndex(i);
            int y = listY + i * UI_ROW_DY;
            Ui_PillCentered(x, y, w, UI_PILL_H, UI_PILL_R, i == s_bankSel, Bank_Name(idx));
        }
    }

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
    else if (Bank_Occupied(idx)) {
        p = appendStr(out, p, "[");
        p = appendStr(out, p, sizeStr(Bank_SizeCode(idx)));
        p = appendStr(out, p, " READY]");
    }
    else {
        p = appendStr(out, p, "[EMPTY]");
    }
}

static void DoDelete(int idx)
{
    int rc;
    if (idx < 0 || Bank_IsBoot(idx)) { SetStatus("Protected bank"); return; }
    rc = Flash_EraseBank(Bank_Ef(idx));   // erases only this bank's blocks
    if (rc == EOS_FLASH_OK) {
        Bank_ClearEntry(idx);             // empty + restore factory label
        rc = Config_Save();
        SetStatus(rc == EOS_FLASH_OK ? "Bank cleared" : "Erased; cfg save FAILED");
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
    Font_DrawCentered(0, g_scrW, g_scrH - 60, "A = YES    B = NO", EOS_DIM);
    Gfx_End();
}

#define EOS_LOADER_VERSION "1.0"

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
    int i, y, x, w = 380, top = 0, shown;
    if (count > LIST_VIS) {
        top = sel - LIST_VIS / 2;
        if (top < 0) top = 0;
        if (top > count - LIST_VIS) top = count - LIST_VIS;
    }
    shown = (count < LIST_VIS) ? count : LIST_VIS;

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar(title);
    x = (g_scrW - w) / 2;
    for (i = 0; i < shown; ++i) {
        y = 110 + i * UI_ROW_DY;
        Ui_PillCentered(x, y, w, UI_PILL_H, UI_PILL_R, (top + i) == sel, items[top + i]);
    }
    if (count == 0)
        Font_DrawCentered(0, g_scrW, 150, "(nothing here yet)", EOS_DIM);
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, 110 + (shown + (count == 0)) * UI_ROW_DY + 12, s_status, EOS_PURPLE);
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
    static const char* cats[4] = { "EEPROM", "Firmware", "HDD", "Format" };
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_MENU); return; }
    s_toolSel = navSel(b, s_toolSel, 4);
    if (Pressed(b, s_prevBtn, BTN_A)) {
        if (s_toolSel == 0) { s_eeToolSel = 0; GotoPhase(PH_EE_TOOLS); }
        else if (s_toolSel == 1) { s_fwToolSel = 0; GotoPhase(PH_FW_TOOLS); }
        else if (s_toolSel == 2) { HddTools_Enter(); }
        else { Format_Enter(); }
    }
    listScreen("Tools", cats, 4, s_toolSel);
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
        Font_DrawCentered(0, g_scrW, g_scrH - 80, s_status, EOS_PURPLE);
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
#define FW_LIST_MAX 24
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
    listScreen("Restore Firmware", items, s_fwCount, s_fwFileSel);
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
            Bank_SetOccupied(s_fwTgtSel, 1, code);
            Config_Save();
            SetStatus("Firmware restored + verified");
        }
        else if (rc == FW_ERR_VERIFY) SetStatus("Restore FAILED -- verify mismatch");
        else if (rc == FW_ERR_FLASH)    SetStatus("Restore FAILED -- flash error");
        else if (rc == FW_ERR_SIZE)     SetStatus("Restore FAILED -- size mismatch");
        else                            SetStatus("Restore FAILED -- file error");
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
    int  listY, i;
    char row[64];

    if (s_mgmtSel >= n) s_mgmtSel = (n > 0) ? n - 1 : 0;

    if (n > 0) {
        if (Pressed(b, s_prevBtn, BTN_DPAD_UP))
            s_mgmtSel = (s_mgmtSel + n - 1) % n;
        if (Pressed(b, s_prevBtn, BTN_DPAD_DOWN))
            s_mgmtSel = (s_mgmtSel + 1) % n;
    }
    if (Pressed(b, s_prevBtn, BTN_B)) { GotoPhase(PH_MENU); return; }

    if (Pressed(b, s_prevBtn, BTN_X)) {
        // Delete (erase) -- occupied, non-boot banks only -> confirm first.
        if (Bank_IsBoot(s_mgmtSel)) {
            SetStatus("Cannot delete boot bank");
        }
        else if (!Bank_Occupied(s_mgmtSel)) {
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
        // Flash a BIOS into this bank -> browse for an image file.
        if (Bank_IsBoot(s_mgmtSel)) {
            SetStatus("Cannot flash boot bank");
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
        if (Bank_IsBoot(s_mgmtSel)) {
            SetStatus("Cannot rename boot bank");
        }
        else {
            s_renameTarget = s_mgmtSel;
            Osk_Open(OSK_TEXT, Bank_Name(s_mgmtSel), EOS_BANK_NAMELEN - 1);
            GotoPhase(PH_RENAME);
            return;
        }
    }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Ui_TitleBar("BANK MANAGEMENT");

    listY = 116;
    {
        int w = 480, x = (g_scrW - w) / 2, dy = 38, ph = 30, pr = 15;
        for (i = 0; i < n; ++i) {
            int y = listY + i * dy;
            buildMgmtRow(row, i);
            Ui_PillCentered(x, y, w, ph, pr, i == s_mgmtSel, row);
        }
    }

    Font_DrawCentered(0, g_scrW, g_scrH - 78,
        "A = FLASH   X = DELETE   Y = RENAME   B = BACK", EOS_DIM);
    if (s_status[0] && GetTickCount() < s_statusUntil)
        Font_DrawCentered(0, g_scrW, g_scrH - 50, s_status, EOS_PURPLE);
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
    int  cap, got, rc, sc, n, s;
    char nm[EOS_BANK_NAMELEN];

    if (idx < 0 || Bank_IsBoot(idx)) { SetStatus("Protected bank"); return; }

    cap = Bank_CapacityBytes(idx);
    if (cap > EOS_IMG_BUF_MAX) cap = EOS_IMG_BUF_MAX;

    got = File_ReadInto(path, s_imgBuf, cap);
    if (got < 0) { SetStatus("Read failed / too big for bank"); return; }
    if (got == 0) { SetStatus("Empty file"); return; }

    rc = Flash_WriteImage(Bank_Ef(idx), s_imgBuf, got);
    if (rc != EOS_FLASH_OK) { SetStatus("Flash FAILED"); return; }

    sc = sizeCodeForLen(got);
    Bank_SetOccupied(idx, 1, sc);

    // name the slot after the file (last component, extension stripped)
    n = mLen(path); s = n;
    while (s > 0 && path[s - 1] != '\\') --s;
    fileToBankName(nm, EOS_BANK_NAMELEN, path + s);
    if (nm[0]) Bank_SetName(idx, nm);

    rc = Config_Save();
    SetStatus(rc == EOS_FLASH_OK ? "Flashed OK" : "Flashed; cfg save FAILED");
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
        if (s_browsePath[0] == 0) { GotoPhase(PH_BANKMGMT); return; }  // drive list -> back
        browseUp();
    }
    if (s_entCount > 0 && Pressed(b, s_prevBtn, BTN_A)) {
        EosFileEntry* e = &s_entries[s_browseSel];
        if (s_browsePath[0] == 0 || e->is_dir) {
            browseInto(e->name);
        }
        else {
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
        GotoPhase(PH_BANKMGMT);
        return;
    }
    if (r == -1) { GotoPhase(PH_BANKMGMT); return; }

    Gfx_Begin(EOS_BG); Ui_Backdrop();
    Osk_Draw();
    Gfx_End();
}

void __cdecl main() {
    if (!Gfx_Init())    return;
    InitInput();
    Bank_SetResting();   // boot bank = safe resting selection
    File_MountDrives();  // bind HDD partitions so E:/F:/... resolve for browsing
    Config_Load();       // pull persisted bank table from the Eos config bank
    Theme_Init();        // apply the saved theme (recolors the whole UI)
    // (exercises the real read path; graceful on fresh chip)
    Net_Start();         // bring the network up; DHCP resolves over the next frames
    Ftp_Init();          // FTP service: deferred bind once the link resolves
    Ftp_Want(1);         // enable FTP (2 sessions, xbox/xbox, passive, port 21)
    if (!Font_Init()) { Gfx_Shutdown(); return; }
    if (!Splash_Init()) { Font_Shutdown(); Gfx_Shutdown(); return; }

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
        else if (s_phase == PH_EE_RESTORE) EeRestore_Frame(b);
        else if (s_phase == PH_EE_CONFIRM) EeConfirm_Frame(b);
        else if (s_phase == PH_ABOUT)    About_Frame(b);
        else if (s_phase == PH_SETTINGS) {
            Gfx_Begin(EOS_BG); Ui_Backdrop();
            Gfx_SetFilter(FALSE);                 // POINT sampling for text/menu
            if (Settings_Frame(b, s_prevBtn)) GotoPhase(PH_MENU);
            Gfx_End();
        }
        else                             Menu_Frame(b);

        s_prevBtn = b;   // shared edge-detect baseline across phases
    }
}