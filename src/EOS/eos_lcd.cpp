/*---------------------------------------------------------------------------
    eos_lcd.cpp -- see eos_lcd.h.

    US2066 driver + primitives ported from DarkDash's dd_lcd (proven), with the
    dd_smbus broker swapped for the loader's Con_Smb* bus. Per-cell shadow-diff
    (LCDChar skips unchanged cells) plus a per-line check so idle costs nothing.
    Phase 1: US2066 only. Phase 2 adds the HD44780/PCF8574 driver behind the
    same vtable (the command bytes are shared HD44780; only the transport and
    the OLED contrast extension differ).

    RXDK / MSVC2003 / C89: declarations before statements, no CRT str*.
---------------------------------------------------------------------------*/
#include "eos_lcd.h"
#include "eos_console.h"   /* Con_SmbReset / Con_SmbRead8 / Con_SmbWrite8, EosLive */
#include "dd_net.h"        /* Net_Ip */
#include "eos_file.h"      /* File_ReadInto / File_Exists (lcd.dat)          */

#define LCD_COLS       20
#define LCD_ROWS       4
#define LCD_UPDATE_MS  250      /* status refresh throttle (~4/sec)            */

/* Shared HD44780 command set (US2066 uses the same fundamental commands). */
#define CMD_CLEAR   0x01
#define CMD_ENTRY   0x06
#define CMD_DISPON  0x0C
#define CMD_FUNC    0x38
#define DDRAM(a)    (0x80 | (BYTE)(a))
/* DDRAM row bases differ by controller: US2066 20x4 steps 0x20/line; a
   standard HD44780 20x4 wraps 0x00/0x40/0x14/0x54. Per-driver, not shared. */
static const BYTE k_rowUS[LCD_ROWS] = { 0x00, 0x20, 0x40, 0x60 };
static const BYTE k_rowHD[LCD_ROWS] = { 0x00, 0x40, 0x14, 0x54 };

/* ---- driver vtable -------------------------------------------------------- */
typedef struct LcdDriver {
    const char* name;
    const BYTE* rowBase;      /* 4 DDRAM row base addresses (controller-specific) */
    void (*init)(unsigned char addr8);
    void (*wcmd)(unsigned char addr8, BYTE cmd);
    void (*wdata)(unsigned char addr8, BYTE data);
    void (*contrast)(unsigned char addr8, BYTE v);
    int  hasContrast;
} LcdDriver;

/* ---- config / state ------------------------------------------------------- */
static int           s_drv = LCD_DRV_NONE;     /* set from lcd.dat at boot; absent = Disabled     */
static unsigned char s_addr8 = 0x78;             /* US2066 0x3C -> 8-bit 0x78 */
static int           s_bright = 0x7F;
static int           s_present = 0;
static int           s_frozen = 0;                /* hand-off freeze */
static const LcdDriver* s_d = 0;

/* ---- shadow / cursor tracking (from dd_lcd) ------------------------------- */
static BYTE s_shadow[LCD_ROWS][LCD_COLS];
static int  s_hwRow = -1, s_hwCol = -1, s_lRow = -1, s_lCol = -1;
static char s_lastLine[LCD_ROWS][LCD_COLS + 1];

/* ---- context ------------------------------------------------------------- */
static char s_ctxTop[LCD_COLS + 1] = "Eos Loader";
static char s_ctxBank[LCD_COLS + 1] = "";

/* ---- tiny string helpers (no CRT) ---------------------------------------- */
static int  SL(const char* s) { int n = 0; while (s && s[n]) ++n; return n; }
static void LCopy(char* d, int cap, const char* s) { int i = 0; if (cap <= 0) return; while (s && s[i] && i < cap - 1) { d[i] = s[i]; ++i; } d[i] = 0; }
static int  SEq(const char* a, const char* b) { int i = 0; for (;;) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; ++i; } }
static int  AppS(char* o, int p, const char* s) { int i = 0; while (s && s[i]) o[p++] = s[i++]; o[p] = 0; return p; }
static int  AppI(char* o, int p, int v) { char t[12]; int n = 0; if (v < 0) v = 0; if (v == 0) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } while (n) o[p++] = t[--n]; o[p] = 0; return p; }
static int  AppT(char* o, int p, int c) { if (c < 0) { o[p++] = '-'; o[p++] = '-'; o[p] = 0; return p; } return AppI(o, p, c); }

/* fill dst with s, space-padded to exactly LCD_COLS, null-terminated */
static void PadLine(char* dst, const char* s)
{
    int i = 0;
    for (; s && s[i] && i < LCD_COLS; ++i) dst[i] = s[i];
    for (; i < LCD_COLS; ++i) dst[i] = ' ';
    dst[LCD_COLS] = 0;
}

/* center s in a full LCD_COLS line */
static void CenterLine(char* dst, const char* s)
{
    int len = SL(s), lpad, i, pos = 0;
    if (len > LCD_COLS) len = LCD_COLS;
    lpad = (LCD_COLS - len) / 2;
    for (i = 0; i < lpad; ++i) dst[pos++] = ' ';
    for (i = 0; i < len; ++i) dst[pos++] = s[i];
    while (pos < LCD_COLS) dst[pos++] = ' ';
    dst[LCD_COLS] = 0;
}

/* ---- US2066 driver (native I2C; control byte 0x80=cmd / 0x40=data) -------- */
static void us_wcmd(unsigned char a, BYTE c) { Con_SmbWrite8(a, 0x80, c); }
static void us_wdata(unsigned char a, BYTE d) { Con_SmbWrite8(a, 0x40, d); }

/* OLED contrast: unlock extended/OLED command set, set contrast, relock. */
static void us_contrast(unsigned char a, BYTE v)
{
    Con_SmbWrite8(a, 0x80, 0x3A);   /* function set, RE=1              */
    Con_SmbWrite8(a, 0x80, 0x79);   /* OLED command set on             */
    Con_SmbWrite8(a, 0x80, 0x81);   /* set contrast                    */
    Con_SmbWrite8(a, 0x80, v);      /* value                           */
    Con_SmbWrite8(a, 0x80, 0x78);   /* OLED command set off            */
    Con_SmbWrite8(a, 0x80, 0x38);   /* function set, RE=0 (fundamental)*/
}

static void us_init(unsigned char a)
{
    Sleep(15);
    Con_SmbWrite8(a, 0x80, CMD_FUNC);   Sleep(5);
    Con_SmbWrite8(a, 0x80, CMD_FUNC);   Sleep(1);
    Con_SmbWrite8(a, 0x80, CMD_FUNC);
    Con_SmbWrite8(a, 0x80, CMD_DISPON);
    Con_SmbWrite8(a, 0x80, CMD_CLEAR);  Sleep(2);
    Con_SmbWrite8(a, 0x80, CMD_ENTRY);
    us_contrast(a, (BYTE)s_bright);
}

static const LcdDriver k_us2066 = { "US2066", k_rowUS, us_init, us_wcmd, us_wdata, us_contrast, 1 };

/* ---- HD44780 via PCF8574 I2C backpack (the common "I2C 1602/2004") -------
   The PCF8574 is a dumb 8-bit port -- no register -- but HalWriteSMBusValue
   forces a command byte, so we write the desired port state as BOTH command
   and data (Con_SmbWrite8(a,B,B)): the expander latches B twice -> stable B,
   no glitch. The HD44780 4-bit interface is then bit-banged over the port with
   an E-strobe per nibble. Ubiquitous backpack wiring:
       P0=RS  P1=RW  P2=E  P3=Backlight  P4..P7=D4..D7  */
#define PCF_RS  0x01
#define PCF_RW  0x02
#define PCF_E   0x04
#define PCF_BL  0x08

static BYTE s_bl = PCF_BL;   /* backlight bit, carried in every port write */

static void pcf_put(unsigned char a, BYTE port) { Con_SmbWrite8(a, port, port); }

/* pulse E to latch the nibble already positioned on the port */
static void pcf_strobe(unsigned char a, BYTE port)
{
    pcf_put(a, (BYTE)(port | PCF_E));    /* E high             */
    pcf_put(a, (BYTE)(port & ~PCF_E));   /* E low -> latch     */
}

/* send the low 4 bits of 'nib' on D4..D7 with control bits (RS/BL; RW=0) */
static void pcf_nibble(unsigned char a, BYTE nib, BYTE ctrl)
{
    pcf_strobe(a, (BYTE)(((nib & 0x0F) << 4) | ctrl));
}

static void pcf_send(unsigned char a, BYTE val, BYTE rs)
{
    BYTE ctrl = (BYTE)(rs | s_bl);       /* RW = 0 (write only) */
    pcf_nibble(a, (BYTE)(val >> 4), ctrl);
    pcf_nibble(a, (BYTE)(val & 0x0F), ctrl);
}

static void hd_wcmd(unsigned char a, BYTE c) { pcf_send(a, c, 0); }
static void hd_wdata(unsigned char a, BYTE d) { pcf_send(a, d, PCF_RS); }
static void hd_contrast(unsigned char a, BYTE v) { (void)a; (void)v; }  /* pot on the module */

static void hd_init(unsigned char a)
{
    BYTE bl = s_bl;
    Sleep(50);                                   /* power-on settle          */
    pcf_nibble(a, 0x03, bl); Sleep(5);           /* force 8-bit x3 (hi nibble)*/
    pcf_nibble(a, 0x03, bl); Sleep(1);
    pcf_nibble(a, 0x03, bl); Sleep(1);
    pcf_nibble(a, 0x02, bl); Sleep(1);           /* -> 4-bit mode            */
    hd_wcmd(a, 0x28);                            /* 4-bit, 2-line, 5x8       */
    hd_wcmd(a, 0x08);                            /* display off              */
    hd_wcmd(a, CMD_CLEAR); Sleep(2);             /* clear (1.52ms)           */
    hd_wcmd(a, CMD_ENTRY);                       /* entry mode               */
    hd_wcmd(a, CMD_DISPON);                      /* display on               */
}

static const LcdDriver k_hd44780 = { "HD44780", k_rowHD, hd_init, hd_wcmd, hd_wdata, hd_contrast, 0 };
static const LcdDriver* PickDriver(int d)
{
    if (d == LCD_DRV_US2066)  return &k_us2066;
    if (d == LCD_DRV_HD44780) return &k_hd44780;
    return 0;
}

/* ---- shared panel primitives (driver-agnostic) --------------------------- */
static void LCDCmd(BYTE c)
{
    if (s_d) s_d->wcmd(s_addr8, c);
    s_hwRow = -1; s_hwCol = -1; s_lRow = -1; s_lCol = -1;
}

static void LCDGoto(int row, int col)
{
    if (row < 0 || row >= LCD_ROWS || col < 0 || col >= LCD_COLS) return;
    if (s_hwRow != row || s_hwCol != col) {
        if (s_d) s_d->wcmd(s_addr8, DDRAM(s_d->rowBase[row] + (BYTE)col));
        s_hwRow = row; s_hwCol = col;
    }
    s_lRow = row; s_lCol = col;
}

/* write one char at the logical cursor, skipping the bus if unchanged */
static void LCDChar(BYTE data)
{
    if (!s_d) return;
    if (s_lRow < 0 || s_lRow >= LCD_ROWS || s_lCol < 0 || s_lCol >= LCD_COLS) {
        s_d->wdata(s_addr8, data);
        return;
    }
    if (s_shadow[s_lRow][s_lCol] == data) {
        ++s_lCol; if (s_lCol >= LCD_COLS) { ++s_lRow; s_lCol = 0; }
        return;
    }
    if (s_hwRow != s_lRow || s_hwCol != s_lCol) {
        s_d->wcmd(s_addr8, DDRAM(s_d->rowBase[s_lRow] + (BYTE)s_lCol));
        s_hwRow = s_lRow; s_hwCol = s_lCol;
    }
    s_d->wdata(s_addr8, data);
    s_shadow[s_lRow][s_lCol] = data;
    ++s_lCol; ++s_hwCol;
    if (s_lCol >= LCD_COLS) { ++s_lRow; s_lCol = 0; ++s_hwRow; s_hwCol = 0; }
}

static void LCDPuts(const char* s, int width)
{
    int i;
    for (i = 0; i < width && i < LCD_COLS; ++i) LCDChar((BYTE)(s[i] ? s[i] : ' '));
}

static void ShadowInvalidate(void)
{
    int r, c;
    for (r = 0; r < LCD_ROWS; ++r)
        for (c = 0; c < LCD_COLS; ++c) s_shadow[r][c] = 0x00;
    s_hwRow = -1; s_hwCol = -1; s_lRow = -1; s_lCol = -1;
    for (r = 0; r < LCD_ROWS; ++r) { s_lastLine[r][0] = 1; s_lastLine[r][1] = 0; }  /* force line repaint */
}

static int Probe(unsigned char a)
{
    unsigned char v;
    Con_SmbReset();
    return Con_SmbRead8(a, 0x00, &v) ? 1 : 0;
}

/* ---- status layout -------------------------------------------------------- */
static void BuildLines(const EosLive* v, char out[LCD_ROWS][LCD_COLS + 1])
{
    char t[40];
    int  p;

    /* row 0: current menu / selected item */
    PadLine(out[0], s_ctxTop);

    /* row 1: IP */
    p = AppS(t, 0, "IP: ");
    p = AppS(t, p, Net_Ip());
    PadLine(out[1], t);

    /* row 2: CPU / MB temps */
    p = AppS(t, 0, "CPU:");
    p = AppT(t, p, v ? v->cpuTempC : -1);
    p = AppS(t, p, "c   MB:");
    p = AppT(t, p, v ? v->mbTempC : -1);
    p = AppS(t, p, "c");
    PadLine(out[2], t);

    /* row 3: RAM free (+ bank tag, right-aligned, if it fits) */
    p = AppS(t, 0, "RAM:");
    p = AppI(t, p, v ? (int)v->ramFreeMB : 0);
    p = AppS(t, p, "M free");
    PadLine(out[3], t);
    if (s_ctxBank[0]) {
        int bl = SL(s_ctxBank);
        if (bl <= LCD_COLS && (p + 1) <= (LCD_COLS - bl)) {
            int i, start = LCD_COLS - bl;
            for (i = 0; i < bl; ++i) out[3][start + i] = s_ctxBank[i];
        }
    }
}

/* ---- public API ----------------------------------------------------------- */
/* ---- config persistence: E:\Eos\lcd.dat (mirrors the theme set.dat model) --
   Enabled -> write it; Disabled -> delete it (absent = Disabled next boot).
   If the write fails (no HDD / read-only) we keep the values in RAM so the
   panel still works this session -- a graceful, silent fallback. */
static const char* DrvName(int d)
{
    if (d == LCD_DRV_US2066)  return "us2066";
    if (d == LCD_DRV_HD44780) return "hd44780";
    return "none";
}

static int hxnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void LcdCfg_Save(void)
{
    HANDLE h; DWORD wr; char b[96]; int p = 0;
    if (s_drv == LCD_DRV_NONE) { DeleteFileA("E:\\Eos\\lcd.dat"); return; }
    CreateDirectoryA("E:\\Eos", NULL);
    h = CreateFileA("E:\\Eos\\lcd.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* graceful: values stay live in RAM */
    p = AppS(b, 0, "driver=");    p = AppS(b, p, DrvName(s_drv));
    p = AppS(b, p, "\r\naddress=0x");
    {
        unsigned a7 = (unsigned)(s_addr8 >> 1); const char* hexd = "0123456789ABCDEF";
        b[p++] = hexd[(a7 >> 4) & 0xF]; b[p++] = hexd[a7 & 0xF]; b[p] = 0;
    }
    p = AppS(b, p, "\r\nbrightness="); p = AppI(b, p, s_bright);
    p = AppS(b, p, "\r\n");
    WriteFile(h, b, (DWORD)p, &wr, NULL);
    CloseHandle(h);
}

static void LcdCfg_Load(void)
{
    unsigned char buf[160]; int n, i;
    n = File_ReadInto("E:\\Eos\\lcd.dat", buf, sizeof(buf) - 1);
    if (n <= 0) { s_drv = LCD_DRV_NONE; return; }   /* absent -> Disabled */
    buf[n] = 0;
    /* tiny key=value scan (LF or CRLF); values: driver / address(0xNN) / brightness */
    i = 0;
    while (i < n) {
        int ks = i, ke, vs, ve;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') ++i;
        ke = i;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) ++i;
        {
            int eq = ks;
            while (eq < ke && buf[eq] != '=') ++eq;
            if (eq >= ke) continue;
            vs = eq + 1; ve = ke;
            if (ke - ks >= 6 && buf[ks] == 'd' && buf[ks + 1] == 'r') {          /* driver= */
                if (ve - vs >= 6 && buf[vs] == 'u') s_drv = LCD_DRV_US2066;
                else if (ve - vs >= 2 && buf[vs] == 'h') s_drv = LCD_DRV_HD44780;
                else s_drv = LCD_DRV_NONE;
            }
            else if (buf[ks] == 'a') {                                     /* address= */
                int hi, lo, base = vs;
                if (ve - vs >= 2 && buf[vs] == '0' && (buf[vs + 1] == 'x' || buf[vs + 1] == 'X')) base = vs + 2;
                hi = (base < ve) ? hxnib((char)buf[base]) : -1;
                lo = (base + 1 < ve) ? hxnib((char)buf[base + 1]) : -1;
                if (hi >= 0 && lo >= 0) s_addr8 = (unsigned char)(((hi << 4) | lo) << 1);
            }
            else if (buf[ks] == 'b') {                                     /* brightness= */
                int v = 0, j; for (j = vs; j < ve && buf[j] >= '0' && buf[j] <= '9'; ++j) v = v * 10 + (buf[j] - '0');
                if (v < 0) v = 0; if (v > 255) v = 255; s_bright = v;
            }
        }
    }
}
/* Apply the current (RAM) config: pick driver, probe, init if present. Does
   NOT touch lcd.dat -- so a config change survives in RAM even if the save
   failed (graceful fallback). */
static void LcdApply(void)
{
    s_present = 0;
    s_frozen = 0;
    s_d = PickDriver(s_drv);
    if (!s_d) return;                 /* Disabled / unsupported */
    if (!Probe(s_addr8)) return;      /* nothing answered */
    s_present = 1;
    ShadowInvalidate();
    Con_SmbReset();
    s_d->init(s_addr8);               /* status paints on the first Tick */
}

void Lcd_Init(void)
{
    LcdCfg_Load();          /* boot: pull persisted config from lcd.dat */
    LcdApply();
}

void Lcd_Tick(const EosLive* live)
{
    static DWORD s_next = 0;
    char  target[LCD_ROWS][LCD_COLS + 1];
    DWORD now;
    int   r, changed = 0;

    if (!s_d || !s_present || s_frozen) return;
    now = GetTickCount();
    if (now < s_next) return;
    s_next = now + LCD_UPDATE_MS;

    BuildLines(live, target);
    for (r = 0; r < LCD_ROWS; ++r)
        if (!SEq(target[r], s_lastLine[r])) { changed = 1; break; }
    if (!changed) return;

    Con_SmbReset();                   /* one controller reset per actual paint */
    for (r = 0; r < LCD_ROWS; ++r) {
        if (SEq(target[r], s_lastLine[r])) continue;
        LCDGoto(r, 0);
        LCDPuts(target[r], LCD_COLS);
        LCopy(s_lastLine[r], LCD_COLS + 1, target[r]);
    }
}

void Lcd_SetContext(const char* top, const char* bank)
{
    if (top) LCopy(s_ctxTop, sizeof(s_ctxTop), top);
    LCopy(s_ctxBank, sizeof(s_ctxBank), bank ? bank : "");
}

void Lcd_HandOff(const char* bankName)
{
    char l1[LCD_COLS + 1], l2[LCD_COLS + 1];
    if (!s_d || !s_present) return;
    Con_SmbReset();
    ShadowInvalidate();
    LCDCmd(CMD_CLEAR); Sleep(2);
    CenterLine(l1, "Booting");
    CenterLine(l2, bankName ? bankName : "");
    LCDGoto(1, 0); LCDPuts(l1, LCD_COLS);
    LCDGoto(2, 0); LCDPuts(l2, LCD_COLS);
    s_frozen = 1;                     /* stop status updates while the game runs */
}

void Lcd_Resume(void)
{
    if (!s_frozen) return;
    s_frozen = 0;
    ShadowInvalidate();               /* repaint status fresh on the next Tick */
}

void Lcd_SetDriver(int drv)
{
    if (drv == s_drv) return;
    s_drv = drv;
    LcdCfg_Save();
    LcdApply();
}
int Lcd_Driver(void) { return s_drv; }

void Lcd_SetAddress(unsigned char addr7)
{
    s_addr8 = (unsigned char)(addr7 << 1);
    LcdCfg_Save();
    LcdApply();
}
unsigned char Lcd_Address(void) { return (unsigned char)(s_addr8 >> 1); }

void Lcd_SetBrightness(int v)
{
    if (v < 0) v = 0; if (v > 255) v = 255;
    s_bright = v;
    LcdCfg_Save();
    if (s_present && s_d && s_d->hasContrast) {
        Con_SmbReset();
        s_d->contrast(s_addr8, (BYTE)v);
    }
}
int Lcd_Brightness(void) { return s_bright; }
int Lcd_Detected(void) { return s_present; }
int Lcd_HasContrast(void) { return s_d ? s_d->hasContrast : 0; }