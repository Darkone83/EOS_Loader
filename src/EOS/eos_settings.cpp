// eos_settings.cpp -- Eos loader Settings hub. See eos_settings.h.
//
// A category hub (pill list) with editable sub-screens: System Info (read-only
// EEPROM), Video, Audio, Region, Network, Date & Time, and Theme. Editable
// fields write through the kernel OS-section path (eos_nvram) or the RTC
// (eos_clock) -- no factory bytes, no crypto, no brick risk. Factory-only
// values (video standard, game region) are shown read-only.
//
// RXDK / MSVC2003 / C89: declarations before statements, no CRT.
#include "eos_settings.h"
#include "eos_gfx.h"
#include "eos_ui.h"
#include "eos_font.h"
#include "eos_eeprom.h"
#include "eos_eeprom_io.h"
#include "eos_ee_data.h"   // decrypted EEPROM read/write (video/region)
#include "eos_console.h"
#include "eos_nvram.h"
#include "eos_clock.h"
#include "eos_theme.h"
#include "eos_config.h"
#include "dd_net.h"
#include "input.h"

// ---- layout ----------------------------------------------------------------
#define TITLE_H   56
#define PILL_W    400
#define PILL_H    38
#define PILL_X    ((g_scrW - PILL_W) / 2)
#define PILL_R    19
#define LIST_Y0   96
#define LIST_DY   48
#define INFO_Y0   100
#define INFO_DY   30
#define INFO_LX   70
#define INFO_VX   300

enum Sub { SUB_HUB = 0, SUB_SYSINFO, SUB_VIDEO, SUB_AUDIO, SUB_REGION, SUB_NETWORK, SUB_DATETIME, SUB_THEME };

static const char* k_hub[] = { "System Info", "Video", "Audio", "Region", "Network", "Date & Time", "Theme" };
#define HUB_COUNT ((int)(sizeof(k_hub) / sizeof(k_hub[0])))

static int       s_sub = SUB_HUB;
static int       s_sel = 0;
static int       s_row = 0;            // row cursor inside an editor
static int       s_themePreview = 0;
static int       s_bgmWork = 0;   // working bg-music on/off (persisted on exit)
static int       s_returnTheme = 0;   // 1 = re-enter THEME after the song browser
static EosEeprom s_eep;
static EosConsole s_con;
static DWORD     s_vflags = 0;         // working video flags
static DWORD     s_aflags = 0;         // working audio flags
static DWORD     s_dvd = 1;            // working dvd region
static int       s_vstdSel = 0;        // 0 NTSC, 1 PAL (working video standard)
static int       s_vstdArm = 0;        // A-armed confirm for the factory write
static const char* s_regMsg = 0;       // last region action result
static int       s_grSel = 0;          // 0 NA, 1 JP, 2 EU (working game region)
static int       s_grArm = 0;          // A-armed confirm for the region write
static int       s_grOk = 0;          // EEPROM decrypted OK -> region editable
static DWORD     s_lang = 1;           // working language
static EosDateTime s_dt;               // working date/time
static int       s_dtField = 0;        // 0..5 = Y M D h m s

// ---- tiny helpers (no CRT) -------------------------------------------------

static void networkEnter(void);   /* defined in the network section, used by the hub */
static bool Pressed(WORD now, WORD prev, WORD mask) { return (now & mask) && !(prev & mask); }

// unsigned int -> decimal string (into out), returns length
static int uitoa(unsigned int v, char* out)
{
    char tmp[12]; int n = 0, p = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[p++] = tmp[--n];
    out[p] = 0;
    return p;
}

static int uitoa2(unsigned int v, char* out)   // zero-padded 2 digits
{
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
    return 2;
}

// ---- shared chrome ---------------------------------------------------------
static void titleBar(const char* title) { Ui_TitleBar(title); }

static void footer(const char* hint) { Ui_Footer(hint); }

// One pill row with a left label and optional right-aligned value. selected ->
// accent fill; otherwise a subtle raised surface. dim = render the row muted
// (used for read-only rows).
static void rowPill(int y, int selected, int dim, const char* label, const char* value)
{
    Ui_PillRow(PILL_X, y, PILL_W, PILL_H, PILL_R, selected, dim, label, value);
}

// ---- hub -------------------------------------------------------------------
static int hubFrame(WORD b, WORD prev)
{
    if (Pressed(b, prev, BTN_DPAD_UP))   s_sel = (s_sel + HUB_COUNT - 1) % HUB_COUNT;
    if (Pressed(b, prev, BTN_DPAD_DOWN)) s_sel = (s_sel + 1) % HUB_COUNT;
    if (Pressed(b, prev, BTN_B)) return 1;
    if (Pressed(b, prev, BTN_A)) {
        s_row = 0;
        switch (s_sel) {
        case 0: s_sub = SUB_SYSINFO; Eeprom_Read(&s_eep); Console_Read(&s_con); break;
        case 1: s_sub = SUB_VIDEO;  s_vflags = Nvram_GetVideoFlags(); break;
        case 2: s_sub = SUB_AUDIO;  s_aflags = Nvram_GetAudioFlags(); break;
        case 3: s_sub = SUB_REGION; Eeprom_Read(&s_eep);
            s_dvd = Nvram_GetDvdRegion(); s_lang = Nvram_GetLanguage();
            s_vstdSel = (s_eep.avRegion == EE_VS_NTSC_J) ? 1
                : (s_eep.avRegion == EE_VS_PAL_I) ? 2
                : (s_eep.avRegion == EE_VS_PAL_M) ? 3 : 0;
            {
                int gr = (int)s_eep.gameRegion;
                s_grOk = (s_eep.gameValid && gr > 0);
                s_grSel = (gr == EE_REGION_JAPAN) ? 1 : (gr == EE_REGION_EURO) ? 2 : 0;
            }
            s_vstdArm = 0; s_grArm = 0; s_regMsg = 0; break;
        case 4: s_sub = SUB_NETWORK; networkEnter(); break;
        case 5: s_sub = SUB_DATETIME; Clock_Get(&s_dt); s_dtField = 0; break;
        case 6: s_sub = SUB_THEME; s_themePreview = Theme_Index();
            s_bgmWork = Config_GetBgmOn(); s_row = 0; break;
        }
    }
    titleBar("SETTINGS");
    Ui_Menu3D(k_hub, HUB_COUNT, s_sel);
    footer("D-PAD  MOVE      A  OPEN      B  BACK");
    return 0;
}

// ---- system info -----------------------------------------------------------
static void infoRow(int idx, const char* label, const char* value)
{
    int y = INFO_Y0 + idx * INFO_DY;
    Font_Draw(INFO_LX, y, label, EOS_DIM);
    Font_Draw(INFO_VX, y, value, EOS_WHITE);
}

static int sysinfoFrame(WORD b, WORD prev)
{
    char mac[20], cpu[16], ram[12];
    int  p;
    if (Pressed(b, prev, BTN_B) || Pressed(b, prev, BTN_A)) { s_sub = SUB_HUB; return 0; }
    titleBar("SYSTEM INFO");
    Eeprom_MacStr(&s_eep, mac);

    if (s_con.cpuMhz > 0) {
        p = uitoa((unsigned)s_con.cpuMhz, cpu);
        cpu[p++] = ' '; cpu[p++] = 'M'; cpu[p++] = 'H'; cpu[p++] = 'z'; cpu[p] = 0;
    }
    else { cpu[0] = '-'; cpu[1] = '-'; cpu[2] = '-'; cpu[3] = '-'; cpu[4] = 0; }

    p = uitoa((unsigned)s_con.ramMB, ram); ram[p++] = ' '; ram[p++] = 'M'; ram[p++] = 'B'; ram[p] = 0;

    infoRow(0, "CPU", cpu);
    infoRow(1, "Xbox Rev", s_con.revStr);
    infoRow(2, "RAM", ram);
    infoRow(3, "Encoder", s_con.encStr);
    infoRow(4, "RTC Expansion", s_con.rtcExpansion ? "Detected" : "Not Detected");
    infoRow(5, "Serial", s_eep.serial[0] ? s_eep.serial : "----");
    infoRow(6, "MAC", mac);
    infoRow(7, "Video Std", Eeprom_VideoStandardStr(&s_eep));
    infoRow(8, "Game Region", Eeprom_GameRegionStr(&s_eep));
    infoRow(9, "Output", g_videoMode);
    footer("B  BACK");
    return 0;
}

// ---- video -----------------------------------------------------------------
static const char* aspectStr(DWORD f)
{
    if (f & NV_VIDEO_WIDESCREEN) return "Widescreen";
    if (f & NV_VIDEO_LETTERBOX)  return "Letterbox";
    return "Normal";
}

static int videoFrame(WORD b, WORD prev)
{
    int i, y;
    const int rows = 4;   // 480p, 720p, 1080i, Aspect
    if (Pressed(b, prev, BTN_B)) { s_sub = SUB_HUB; return 0; }
    if (Pressed(b, prev, BTN_DPAD_UP))   s_row = (s_row + rows - 1) % rows;
    if (Pressed(b, prev, BTN_DPAD_DOWN)) s_row = (s_row + 1) % rows;

    if (s_row < 3) {                       // boolean toggles
        DWORD bit = (s_row == 0) ? NV_VIDEO_480p : (s_row == 1) ? NV_VIDEO_720p : NV_VIDEO_1080i;
        if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_DPAD_LEFT) || Pressed(b, prev, BTN_DPAD_RIGHT)) {
            s_vflags ^= bit;
            Nvram_SetVideoFlags(s_vflags);
        }
    }
    else {                               // aspect cycle
        if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_DPAD_RIGHT) || Pressed(b, prev, BTN_DPAD_LEFT)) {
            DWORD a = s_vflags & (NV_VIDEO_WIDESCREEN | NV_VIDEO_LETTERBOX);
            s_vflags &= ~(NV_VIDEO_WIDESCREEN | NV_VIDEO_LETTERBOX);
            if (a == 0)                    s_vflags |= NV_VIDEO_WIDESCREEN;
            else if (a == NV_VIDEO_WIDESCREEN) s_vflags |= NV_VIDEO_LETTERBOX;
            /* else -> Normal (cleared) */
            Nvram_SetVideoFlags(s_vflags);
        }
    }

    titleBar("VIDEO");
    for (i = 0; i < rows; ++i) {
        const char* label; const char* val;
        y = LIST_Y0 + i * LIST_DY;
        if (i == 0) { label = "480p";   val = (s_vflags & NV_VIDEO_480p) ? "On" : "Off"; }
        else if (i == 1) { label = "720p";  val = (s_vflags & NV_VIDEO_720p) ? "On" : "Off"; }
        else if (i == 2) { label = "1080i"; val = (s_vflags & NV_VIDEO_1080i) ? "On" : "Off"; }
        else { label = "Aspect"; val = aspectStr(s_vflags); }
        rowPill(y, i == s_row, 0, label, val);
    }
    footer("D-PAD  MOVE / CHANGE      A  TOGGLE      B  BACK");
    return 0;
}

// ---- audio -----------------------------------------------------------------
static int audioFrame(WORD b, WORD prev)
{
    int i, y;
    const int rows = 3;   // Mode, AC-3, DTS
    if (Pressed(b, prev, BTN_B)) { s_sub = SUB_HUB; return 0; }
    if (Pressed(b, prev, BTN_DPAD_UP))   s_row = (s_row + rows - 1) % rows;
    if (Pressed(b, prev, BTN_DPAD_DOWN)) s_row = (s_row + 1) % rows;

    if (s_row == 0) {                      // audio mode cycle
        if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_DPAD_RIGHT) || Pressed(b, prev, BTN_DPAD_LEFT)) {
            int m = Nvram_AudioMode(s_aflags);
            m = (m + 1) % 3;
            s_aflags &= ~NV_AUDIO_MODE_MASK;
            if (m == 1) s_aflags |= NV_AUDIO_MONO;
            else if (m == 2) s_aflags |= NV_AUDIO_SURROUND;
            Nvram_SetAudioFlags(s_aflags);
        }
    }
    else {                               // AC-3 / DTS toggle
        DWORD bit = (s_row == 1) ? NV_AUDIO_AC3 : NV_AUDIO_DTS;
        if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_DPAD_LEFT) || Pressed(b, prev, BTN_DPAD_RIGHT)) {
            s_aflags ^= bit;
            Nvram_SetAudioFlags(s_aflags);
        }
    }

    titleBar("AUDIO");
    for (i = 0; i < rows; ++i) {
        const char* label; const char* val;
        y = LIST_Y0 + i * LIST_DY;
        if (i == 0) { label = "Mode"; val = Nvram_AudioModeStr(Nvram_AudioMode(s_aflags)); }
        else if (i == 1) { label = "Dolby AC-3"; val = (s_aflags & NV_AUDIO_AC3) ? "On" : "Off"; }
        else { label = "DTS"; val = (s_aflags & NV_AUDIO_DTS) ? "On" : "Off"; }
        rowPill(y, i == s_row, 0, label, val);
    }
    footer("D-PAD  MOVE / CHANGE      A  TOGGLE      B  BACK");
    return 0;
}

// ---- region ----------------------------------------------------------------
static const char* dvdStr(DWORD r)
{
    static char s[12];
    if (r == 0 || r > 8) return "Region-free";
    s[0] = 'R'; s[1] = 'e'; s[2] = 'g'; s[3] = 'i'; s[4] = 'o'; s[5] = 'n'; s[6] = ' ';
    s[7] = (char)('0' + (int)r); s[8] = 0;
    return s;
}

static int regionFrame(WORD b, WORD prev)
{
    int i, y;
    const int rows = 4;   // 0 Video Std (factory write), 1 Game Region RO, 2 DVD, 3 Language
    if (Pressed(b, prev, BTN_B)) { s_sub = SUB_HUB; return 0; }
    if (Pressed(b, prev, BTN_DPAD_UP)) { s_row = (s_row + rows - 1) % rows; s_vstdArm = 0; s_grArm = 0; }
    if (Pressed(b, prev, BTN_DPAD_DOWN)) { s_row = (s_row + 1) % rows; s_vstdArm = 0; s_grArm = 0; }

    if (s_row == 0) {                      // VIDEO STANDARD -- EEPROM factory write
        if (Pressed(b, prev, BTN_DPAD_RIGHT)) { s_vstdSel = (s_vstdSel + 1) % 4; s_vstdArm = 0; s_regMsg = 0; }
        if (Pressed(b, prev, BTN_DPAD_LEFT)) { s_vstdSel = (s_vstdSel + 3) % 4; s_vstdArm = 0; s_regMsg = 0; }
        if (Pressed(b, prev, BTN_A)) {
            if (!s_vstdArm) { s_vstdArm = 1; }     // first A arms; second A writes
            else {
                static const DWORD vsMap[4] =
                { EE_VS_NTSC_M, EE_VS_NTSC_J, EE_VS_PAL_I, EE_VS_PAL_M };
                int rc = EeData_SetVideoStd(vsMap[s_vstdSel]);
                s_regMsg = rc ? "Written -- reboot to apply" : "Write FAILED -- aborted";
                s_vstdArm = 0;
            }
        }
    }
    else if (s_row == 1 && s_grOk) {     // GAME REGION -- encrypted EEPROM write
        if (Pressed(b, prev, BTN_DPAD_RIGHT)) { s_grSel = (s_grSel + 1) % 3; s_grArm = 0; s_regMsg = 0; }
        if (Pressed(b, prev, BTN_DPAD_LEFT)) { s_grSel = (s_grSel + 2) % 3; s_grArm = 0; s_regMsg = 0; }
        if (Pressed(b, prev, BTN_A)) {
            if (!s_grArm) { s_grArm = 1; }
            else {
                int reg = (s_grSel == 1) ? EE_REGION_JAPAN : (s_grSel == 2) ? EE_REGION_EURO : EE_REGION_NA;
                int rc = EeData_SetGameRegion((DWORD)reg);
                s_regMsg = rc ? "Region written -- reboot to apply"
                    : "Unsupported/again -- not written";
                s_grArm = 0;
            }
        }
    }
    else if (s_row == 2) {               // DVD region cycle 0..8 (NVRAM, live)
        if (Pressed(b, prev, BTN_DPAD_RIGHT) || Pressed(b, prev, BTN_A)) { s_dvd = (s_dvd + 1) % 9; Nvram_SetDvdRegion(s_dvd); }
        if (Pressed(b, prev, BTN_DPAD_LEFT)) { s_dvd = (s_dvd + 8) % 9; Nvram_SetDvdRegion(s_dvd); }
    }
    else if (s_row == 3) {               // language cycle 1..9 (NVRAM, live)
        if (Pressed(b, prev, BTN_DPAD_RIGHT) || Pressed(b, prev, BTN_A)) { s_lang = (s_lang % 9) + 1; Nvram_SetLanguage(s_lang); }
        if (Pressed(b, prev, BTN_DPAD_LEFT)) { s_lang = (s_lang + 7) % 9 + 1; Nvram_SetLanguage(s_lang); }
    }

    titleBar("REGION");
    for (i = 0; i < rows; ++i) {
        const char* label; const char* val; int dim = 0;
        y = LIST_Y0 + i * LIST_DY;
        if (i == 0) {
            static const char* vsName[4] = { "NTSC-M", "NTSC-J", "PAL-I", "PAL-M" };
            label = "Video Standard";   val = vsName[s_vstdSel & 3];
        }
        else if (i == 1) {
            if (s_grOk) {
                label = "Game Region";
                val = (s_grSel == 1) ? "Japan" : (s_grSel == 2) ? "Europe" : "North America";
            }
            else { label = "Game Region (RO)"; val = Eeprom_GameRegionStr(&s_eep); dim = 1; }
        }
        else if (i == 2) { label = "DVD Region";       val = dvdStr(s_dvd); }
        else { label = "Language";         val = Nvram_LanguageStr(s_lang); }
        rowPill(y, i == s_row, dim, label, val);
    }
    if ((s_row == 0 && s_vstdArm) || (s_row == 1 && s_grArm))
        Font_DrawCentered(0, g_scrW, LIST_Y0 + rows * LIST_DY + 6,
            "Press A again to WRITE EEPROM (backs up first)", EOS_PURPLE);
    else if (s_regMsg)
        Font_DrawCentered(0, g_scrW, LIST_Y0 + rows * LIST_DY + 6, s_regMsg, EOS_PURPLE);
    footer("D-PAD MOVE / CHANGE   A SET   B BACK   (factory writes back up)");
    return 0;
}

// ---- network ---------------------------------------------------------------
static int          s_netMode = DD_NET_DHCP;
static unsigned long s_ip = 0, s_mask = 0, s_gw = 0, s_dns = 0, s_dns2 = 0;
static int          s_netRow = 0;     // 0 Mode,1 IP,2 Subnet,3 Gateway,4 DNS,5 Apply
static int          s_netEdit = 0;    // 1 = editing octets of the current row
static int          s_octet = 0;      // 0..3
static const char* s_netMsg = 0;

static int getOctet(unsigned long a, int o) { return (int)((a >> (o * 8)) & 0xFF); }
static void setOctet(unsigned long* a, int o, int v)
{
    unsigned long m = 0xFFUL << (o * 8);
    *a = (*a & ~m) | (((unsigned long)(v & 0xFF)) << (o * 8));
}

static unsigned long parseIp(const char* s)
{
    unsigned long o[4]; int idx = 0, val = 0, have = 0, i;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (i = 0; s[i]; ++i) {
        if (s[i] >= '0' && s[i] <= '9') { val = val * 10 + (s[i] - '0'); have = 1; }
        else if (s[i] == '.') { if (idx < 3) o[idx++] = (unsigned long)(val & 0xFF); val = 0; have = 0; }
        else break;
    }
    if (have && idx < 4) o[idx] = (unsigned long)(val & 0xFF);
    return o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24);
}

static unsigned long* netAddr(int row)
{
    if (row == 1) return &s_ip;
    if (row == 2) return &s_mask;
    if (row == 3) return &s_gw;
    if (row == 4) return &s_dns;
    return 0;
}

static void networkEnter(void)
{
    int mode; unsigned long ip, mask, gw, d1, d2;
    Net_LoadConfig(&mode, &ip, &mask, &gw, &d1, &d2);
    s_netMode = (mode == DD_NET_STATIC) ? DD_NET_STATIC : DD_NET_DHCP;
    s_ip = ip; s_mask = mask; s_gw = gw; s_dns = d1; s_dns2 = d2;
    // Seed empty fields from the live DHCP lease so editing starts somewhere sane.
    if (s_ip == 0 && Net_IsUp()) s_ip = parseIp(Net_Ip());
    if (s_mask == 0 && Net_IsUp()) s_mask = parseIp(Net_Subnet());
    if (s_gw == 0 && Net_IsUp()) s_gw = parseIp(Net_Gateway());
    if (s_dns == 0 && Net_IsUp()) s_dns = parseIp(Net_Dns());
    s_netRow = 0; s_netEdit = 0; s_octet = 0; s_netMsg = 0;
}

// Draw an editable a.b.c.d inside the pill, right-aligned, highlighting the
// active octet while editing.
static void drawAddr(int y, int rowSel, unsigned long a, int editingRow)
{
    char oc[4][6]; int i, w = 0, x;
    for (i = 0; i < 4; ++i) uitoa((unsigned)getOctet(a, i), oc[i]);
    for (i = 0; i < 4; ++i) w += Font_TextWidth(oc[i]);
    w += 3 * Font_TextWidth(".");             // three dots
    x = PILL_X + PILL_W - 22 - w;
    for (i = 0; i < 4; ++i) {
        DWORD c = (editingRow && i == s_octet) ? EOS_PURPLE : (rowSel ? EOS_WHITE : EOS_DIM);
        Font_Draw(x, y + 11, oc[i], c); x += Font_TextWidth(oc[i]);
        if (i < 3) { Font_Draw(x, y + 11, ".", rowSel ? EOS_WHITE : EOS_DIM); x += Font_TextWidth("."); }
    }
}

static void netAddrRow(int row, const char* label, unsigned long a, int isStatic, const char* dhcpStr)
{
    int y = LIST_Y0 + row * LIST_DY;
    int sel = (s_netRow == row);
    rowPill(y, sel, !isStatic, label, 0);
    if (isStatic) {
        drawAddr(y, sel, a, sel && s_netEdit);
    }
    else {
        int vx = PILL_X + PILL_W - 22 - Font_TextWidth(dhcpStr);
        Font_Draw(vx, y + 11, dhcpStr, EOS_DIM);
    }
}

static int networkFrame(WORD b, WORD prev)
{
    const int rows = 6;
    int isStatic = (s_netMode == DD_NET_STATIC);
    const char* dip; const char* dsub; const char* dgw; const char* ddns;

    if (s_netEdit) {                          // ---- octet edit submode ----
        unsigned long* a = netAddr(s_netRow);
        if (Pressed(b, prev, BTN_DPAD_LEFT))  s_octet = (s_octet + 3) % 4;
        if (Pressed(b, prev, BTN_DPAD_RIGHT)) s_octet = (s_octet + 1) % 4;
        if (a && Pressed(b, prev, BTN_DPAD_UP))   setOctet(a, s_octet, (getOctet(*a, s_octet) + 1) & 0xFF);
        if (a && Pressed(b, prev, BTN_DPAD_DOWN)) setOctet(a, s_octet, (getOctet(*a, s_octet) + 255) & 0xFF);
        if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_B)) s_netEdit = 0;
    }
    else {                                   // ---- row navigation ----
        if (Pressed(b, prev, BTN_B)) { s_sub = SUB_HUB; return 0; }
        if (Pressed(b, prev, BTN_DPAD_UP))   s_netRow = (s_netRow + rows - 1) % rows;
        if (Pressed(b, prev, BTN_DPAD_DOWN)) s_netRow = (s_netRow + 1) % rows;
        if (s_netRow == 0) {
            if (Pressed(b, prev, BTN_A) || Pressed(b, prev, BTN_DPAD_LEFT) || Pressed(b, prev, BTN_DPAD_RIGHT)) {
                s_netMode = (s_netMode == DD_NET_STATIC) ? DD_NET_DHCP : DD_NET_STATIC;
                s_netMsg = 0;
            }
        }
        else if (s_netRow >= 1 && s_netRow <= 4) {
            if (isStatic && Pressed(b, prev, BTN_A)) { s_netEdit = 1; s_octet = 0; }
        }
        else if (s_netRow == 5) {
            if (Pressed(b, prev, BTN_A))
                s_netMsg = Net_ApplyConfig(s_netMode, s_ip, s_mask, s_gw, s_dns, s_dns2)
                ? "Applied - restarting network" : "Apply refused";
        }
    }

    titleBar("NETWORK");
    rowPill(LIST_Y0 + 0 * LIST_DY, s_netRow == 0, 0, "Mode", isStatic ? "Static" : "DHCP");

    dip = Net_IsUp() ? Net_Ip() : "--";
    dsub = Net_IsUp() ? Net_Subnet() : "--";
    dgw = Net_IsUp() ? Net_Gateway() : "--";
    ddns = Net_IsUp() ? Net_Dns() : "--";
    netAddrRow(1, "IP Address", s_ip, isStatic, dip);
    netAddrRow(2, "Subnet", s_mask, isStatic, dsub);
    netAddrRow(3, "Gateway", s_gw, isStatic, dgw);
    netAddrRow(4, "DNS", s_dns, isStatic, ddns);
    rowPill(LIST_Y0 + 5 * LIST_DY, s_netRow == 5, 0, "Apply & Restart", 0);

    if (s_netMsg) Font_DrawCentered(0, g_scrW, LIST_Y0 + 6 * LIST_DY + 6, s_netMsg, EOS_PURPLE);

    if (s_netEdit)        footer("D-PAD  L/R OCTET   UP/DN VALUE   A/B DONE");
    else if (isStatic)    footer("D-PAD MOVE   A EDIT/APPLY   B BACK");
    else                  footer("D-PAD MOVE   A CHANGE MODE   B BACK");
    return 0;
}

// ---- date & time -----------------------------------------------------------
static void dtStep(int dir)
{
    switch (s_dtField) {
    case 0: s_dt.year += dir; break;
    case 1: s_dt.month += dir; break;
    case 2: s_dt.day += dir; break;
    case 3: s_dt.hour += dir; break;
    case 4: s_dt.minute += dir; break;
    case 5: s_dt.second += dir; break;
    }
    // wrap month/day-of-week-ish: rely on clamp for bounds, but allow wrap on edges
    if (s_dt.month < 1)  s_dt.month = 12;
    if (s_dt.month > 12) s_dt.month = 1;
    Clock_Clamp(&s_dt);
}

static int datetimeFrame(WORD b, WORD prev)
{
    const int rows = 6;   // Year, Month, Day, Hour, Minute, Second
    char val[16]; int i, y, k; const char* applied = 0; const char* m;

    if (Pressed(b, prev, BTN_B)) { s_sub = SUB_HUB; return 0; }
    if (Pressed(b, prev, BTN_DPAD_UP))    s_dtField = (s_dtField + rows - 1) % rows;
    if (Pressed(b, prev, BTN_DPAD_DOWN))  s_dtField = (s_dtField + 1) % rows;
    if (Pressed(b, prev, BTN_DPAD_RIGHT)) dtStep(+1);
    if (Pressed(b, prev, BTN_DPAD_LEFT))  dtStep(-1);
    if (Pressed(b, prev, BTN_A)) applied = Clock_Set(&s_dt) ? "Clock updated" : "Set failed";

    titleBar("DATE & TIME");
    for (i = 0; i < rows; ++i) {
        const char* label;
        y = LIST_Y0 + i * LIST_DY;
        switch (i) {
        case 0:  label = "Year";   uitoa((unsigned)s_dt.year, val); break;
        case 1:  label = "Month";  m = Clock_MonthName(s_dt.month);
            for (k = 0; m[k] && k < 15; ++k) val[k] = m[k]; val[k] = 0; break;
        case 2:  label = "Day";    uitoa2((unsigned)s_dt.day, val); break;
        case 3:  label = "Hour";   uitoa2((unsigned)s_dt.hour, val); break;
        case 4:  label = "Minute"; uitoa2((unsigned)s_dt.minute, val); break;
        default: label = "Second"; uitoa2((unsigned)s_dt.second, val); break;
        }
        rowPill(y, i == s_dtField, 0, label, val);
    }
    if (applied)
        Font_DrawCentered(0, g_scrW, LIST_Y0 + rows * LIST_DY + 6, applied, EOS_PURPLE);
    footer("D-PAD  UP/DN FIELD   L/R ADJUST   A APPLY   B BACK");
    return 0;
}

// ---- theme -----------------------------------------------------------------
static int sAppendS(char* d, int p, const char* srcs)
{
    int i = 0; while (srcs[i]) { d[p++] = srcs[i]; ++i; } return p;
}

// THEME screen: theme select + background-music enable + single-track select.
// Row 0 Theme (L/R cycles+preview), Row 1 Background Music (L/R toggles), Row 2
// Track (A opens the song browser -> returns 2). B commits theme+bgm and backs out.
static int themeFrame(WORD b, WORD prev)
{
    int n = Theme_Count(), j, last;
    char line[96]; int p; DWORD col;
    const char* nm; const char* tk; const char* base;

    if (Pressed(b, prev, BTN_DPAD_UP))   s_row = (s_row + 2) % 3;
    if (Pressed(b, prev, BTN_DPAD_DOWN)) s_row = (s_row + 1) % 3;

    if (s_row == 0) {
        if (Pressed(b, prev, BTN_DPAD_LEFT)) { s_themePreview = (s_themePreview + n - 1) % n; Theme_Preview(s_themePreview); }
        if (Pressed(b, prev, BTN_DPAD_RIGHT)) { s_themePreview = (s_themePreview + 1) % n;     Theme_Preview(s_themePreview); }
    }
    else if (s_row == 1) {
        if (Pressed(b, prev, BTN_DPAD_LEFT) || Pressed(b, prev, BTN_DPAD_RIGHT)) s_bgmWork ^= 1;
    }

    if (s_row == 2 && Pressed(b, prev, BTN_A)) {
        s_returnTheme = 1;          /* come back to THEME after the browse */
        return 2;                   /* main.cpp opens the single-track browser */
    }
    if (Pressed(b, prev, BTN_B) || (Pressed(b, prev, BTN_A) && s_row != 2)) {
        Theme_Commit();
        Config_SetBgmOn(s_bgmWork); /* persist bg-music enable */
        s_sub = SUB_HUB;
        return 0;
    }

    titleBar("THEME");
    nm = Theme_Name(s_themePreview);
    tk = Config_GetBgmPath();

    col = (s_row == 0) ? EOS_WHITE : EOS_DIM;
    p = sAppendS(line, 0, "Theme:            "); p = sAppendS(line, p, nm ? nm : "?"); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 150, line, col);

    col = (s_row == 1) ? EOS_WHITE : EOS_DIM;
    p = sAppendS(line, 0, "Background Music:  "); p = sAppendS(line, p, s_bgmWork ? "On" : "Off"); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 185, line, col);

    base = tk; last = -1;
    for (j = 0; tk[j]; ++j) if (tk[j] == '\\' || tk[j] == '/') last = j;
    if (last >= 0) base = tk + last + 1;
    col = (s_row == 2) ? EOS_WHITE : EOS_DIM;
    p = sAppendS(line, 0, "Track:            "); p = sAppendS(line, p, (base && base[0]) ? base : "(none)"); line[p] = 0;
    Font_DrawCentered(0, g_scrW, 220, line, col);

    {
        int sw = 80, x0 = (g_scrW - (sw * 4 + 30)) / 2, sy = 258;
        Gfx_FillRounded(x0, sy, sw, 62, 12, EOS_PURPLE);
        Gfx_FillRounded(x0 + (sw + 10), sy, sw, 62, 12, EOS_WHITE);
        Gfx_FillRounded(x0 + (sw + 10) * 2, sy, sw, 62, 12, EOS_DIM);
        Gfx_FillRounded(x0 + (sw + 10) * 3, sy, sw, 62, 12, EOS_PURPLE);
        Font_DrawCentered(0, g_scrW, sy + 74, "Accent    Text    Dim    Accent", EOS_DIM);
    }

    footer("D-PAD  MOVE / CHANGE      A  SELECT      B  BACK");
    return 0;
}

// ---- entry / dispatch ------------------------------------------------------
void Settings_Enter(void)
{
    if (s_returnTheme) {
        /* returning from the song browser: keep the working theme/bgm state that
           survived the detour; just land back on the Track row. */
        s_returnTheme = 0;
        s_sub = SUB_THEME;
        s_row = 2;
    }
    else {
        s_sub = SUB_HUB;
        s_sel = 0;
        s_row = 0;
    }
    Eeprom_Read(&s_eep);
}

int Settings_Frame(WORD b, WORD prevBtn)
{
    switch (s_sub) {
    case SUB_SYSINFO:  return sysinfoFrame(b, prevBtn);
    case SUB_VIDEO:    return videoFrame(b, prevBtn);
    case SUB_AUDIO:    return audioFrame(b, prevBtn);
    case SUB_REGION:   return regionFrame(b, prevBtn);
    case SUB_NETWORK:  return networkFrame(b, prevBtn);
    case SUB_DATETIME: return datetimeFrame(b, prevBtn);
    case SUB_THEME:    return themeFrame(b, prevBtn);
    }
    return hubFrame(b, prevBtn);
}