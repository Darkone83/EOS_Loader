// eos_http.cpp -- see eos_http.h. Single-connection, tick-polled HTTP server.
#include <xtl.h>
#include <winsockx.h>
#include "eos_http.h"
#include "eos_bank.h"
#include "eos_config.h"
#include "eos_flash.h"
#include "eos_eeprom_io.h"
#include "eos_logo_data.h"   // EOS_LOGO_W/H, EOS_LOGO_PAL[15][3], EOS_LOGO_4BPP[]

#define HTTP_PORT      80
#define HTTP_REQ_MAX   8192
#define HTTP_RX_MAX    (1024 * 1024)   // flash upload / BMP staging
#define HTTP_JSON_MAX  8192
#define HTTP_RESP_MAX  1024
#define HTTP_POLL_BUDGET (128 * 1024)  // max body bytes moved per frame

enum { ST_IDLE = 0, ST_HDR, ST_BODY, ST_SEND };
enum { M_GET = 0, M_POST };
enum { R_NONE = 0, R_PAGE, R_LOGO, R_BANKS, R_RENAME, R_DELETE, R_FLASH, R_LAUNCH, R_EEPROM, R_RESET };

static SOCKET s_listen = INVALID_SOCKET;
static SOCKET s_conn = INVALID_SOCKET;
static int    s_up = 0;
static int    s_state = ST_IDLE;

static char   s_req[HTTP_REQ_MAX]; static int s_reqLen;
static int    s_method, s_route, s_bank, s_clen;
static int    s_rxRecv, s_rxStore, s_store, s_err;
static int    s_launch = -1;

static unsigned char s_rx[HTTP_RX_MAX];
static char   s_json[HTTP_JSON_MAX];
static char   s_resp[HTTP_RESP_MAX];
static char   s_name[80];

// send segments: headers then body
static const char* s_txH; static int s_txHLen, s_txHOff;
static const char* s_txB; static int s_txBLen, s_txBOff;

// ---- string helpers (no CRT) ----------------------------------------------
static int aLen(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static int appS(char* d, const char* s) { int i = 0; while (s[i]) { d[i] = s[i]; ++i; } return i; }
static int appI(char* d, int v)
{
    char t[12]; int n = 0, p = 0; unsigned u;
    if (v < 0) { d[p++] = '-'; u = (unsigned)(-v); }
    else u = (unsigned)v;
    if (u == 0) { d[p++] = '0'; d[p] = 0; return p; }
    while (u && n < 11) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (n > 0) d[p++] = t[--n];
    d[p] = 0; return p;
}
static int appJson(char* d, const char* s)
{
    int i = 0, p = 0; char c;
    while ((c = s[i++]) != 0) {
        if (c == '"' || c == '\\') { d[p++] = '\\'; d[p++] = c; }
        else if ((unsigned char)c >= 0x20) d[p++] = c;
    }
    return p;
}
static void putLE32(unsigned char* o, unsigned v)
{
    o[0] = (unsigned char)v; o[1] = (unsigned char)(v >> 8);
    o[2] = (unsigned char)(v >> 16); o[3] = (unsigned char)(v >> 24);
}
static int strEqN(const char* a, const char* b, int n)
{
    int i; for (i = 0; i < n; ++i) { if (a[i] != b[i]) return 0; if (!a[i]) return 0; } return 1;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// ---- web UI ----------------------------------------------------------------
// DOM-built (no inline handlers) so there is no quote nesting to escape.
static const char* k_page =
"<!doctype html><html><head><meta charset=utf-8>\n"
"<meta name=viewport content='width=device-width,initial-scale=1'>\n"
"<title>EOS</title>\n"
"<style>\n"
":root{--p:rgb(168,85,247);--bg:#0a0a0f;--card:#15151c;--dim:#6a6a78;--txt:#e8e8ef;}\n"
"*{box-sizing:border-box;font-family:system-ui,sans-serif;}\n"
"body{margin:0;background:var(--bg);color:var(--txt);}\n"
"header{display:flex;align-items:center;gap:16px;padding:20px;border-bottom:1px solid #222;}\n"
"header img{width:72px;height:72px;}\n"
"h1{font-size:22px;margin:0;letter-spacing:4px;color:var(--p);}\n"
".sub{color:var(--dim);font-size:12px;}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px;padding:20px;}\n"
".card{background:var(--card);border:1px solid #24242e;border-radius:10px;padding:16px;}\n"
".card h2{font-size:16px;margin:0 0 8px;word-break:break-all;}\n"
".badge{display:inline-block;font-size:11px;padding:2px 9px;border-radius:20px;border:1px solid #333;color:var(--dim);}\n"
".badge.ready{color:var(--p);border-color:var(--p);}\n"
".badge.boot{color:#39d98a;border-color:#39d98a;}\n"
".row{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap;}\n"
"button{background:#1d1d27;color:var(--txt);border:1px solid #2c2c38;border-radius:7px;padding:7px 12px;cursor:pointer;font-size:13px;}\n"
"button:hover{border-color:var(--p);}\n"
"button.danger:hover{border-color:#e05050;color:#e05050;}\n"
"button.go:hover{border-color:#39d98a;color:#39d98a;}\n"
"#msg{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#1d1d27;border:1px solid var(--p);padding:10px 18px;border-radius:8px;opacity:0;transition:.2s;pointer-events:none;}\n"
"#msg.show{opacity:1;}\n"
"</style></head><body>\n"
"<header><img src='/logo.bmp' alt=''><div><h1>EOS</h1><div class=sub>BIOS bank manager</div></div></header>\n"
"<div class=grid id=grid></div>\n"
"<div class=grid id=sys></div><div id=msg></div>\n"
"<script>\n"
"const SZ=['256K','512K','1MB'];\n"
"function msg(t){let m=document.getElementById('msg');m.textContent=t;m.classList.add('show');setTimeout(function(){m.classList.remove('show');},2500);}\n"
"function btn(label,cls,fn){let e=document.createElement('button');e.textContent=label;if(cls)e.className=cls;e.addEventListener('click',fn);return e;}\n"
"function card(b){\n"
" let c=document.createElement('div');c.className='card';\n"
" let h=document.createElement('h2');h.textContent=b.name;c.appendChild(h);\n"
" let bd=document.createElement('span');\n"
" if(b.boot){bd.className='badge boot';bd.textContent='BOOT';}\n"
" else if(b.occ){bd.className='badge ready';bd.textContent=SZ[b.size]+' READY';}\n"
" else{bd.className='badge';bd.textContent='EMPTY';}\n"
" c.appendChild(bd);\n"
" let row=document.createElement('div');row.className='row';\n"
" if(!b.boot){\n"
"  row.appendChild(btn('Flash','',function(){pick(b.i);}));\n"
"  row.appendChild(btn('Rename','',function(){ren(b.i,b.name);}));\n"
"  if(b.occ){row.appendChild(btn('Delete','danger',function(){del(b.i);}));\n"
"            row.appendChild(btn('Launch','go',function(){go(b.i);}));}\n"
" }\n"
" c.appendChild(row);return c;\n"
"}\n"
"async function load(){\n"
" let r=await fetch('/api/banks');let j=await r.json();\n"
" let g=document.getElementById('grid');g.innerHTML='';\n"
" for(const b of j.banks)g.appendChild(card(b));\n"
"}\n"
"let fi=document.createElement('input');fi.type='file';fi.accept='.bin';let fb=-1;\n"
"function pick(i){fb=i;fi.value='';fi.click();}\n"
"fi.onchange=async function(){\n"
" let f=fi.files[0];if(!f)return;msg('Flashing '+f.name+' ...');\n"
" let buf=await f.arrayBuffer();\n"
" let r=await fetch('/api/flash?b='+fb,{method:'POST',body:buf});\n"
" if(r.ok){await fetch('/api/rename?b='+fb,{method:'POST',body:f.name.replace(/\\.[^.]*$/,'')});msg('Flashed');}\n"
" else msg('Flash failed: '+(await r.text()));\n"
" load();\n"
"};\n"
"async function ren(i,cur){let n=prompt('Bank name:',cur);if(n==null)return;await fetch('/api/rename?b='+i,{method:'POST',body:n});msg('Renamed');load();}\n"
"async function del(i){if(!confirm('Delete this bank?'))return;await fetch('/api/delete?b='+i,{method:'POST'});msg('Deleted');load();}\n"
"async function go(i){if(!confirm('Launch this bank? The console will reboot.'))return;fetch('/api/launch?b='+i,{method:'POST'});msg('Launching...');}\n"
"async function eeBackup(){\n"
" let r=await fetch('/api/eeprom');\n"
" if(!r.ok){msg('EEPROM read failed');return;}\n"
" let b=await r.blob();let u=URL.createObjectURL(b);\n"
" let a=document.createElement('a');a.href=u;a.download='eeprom.bin';a.click();\n"
" URL.revokeObjectURL(u);msg('EEPROM backed up');\n"
"}\n"
"let ei=document.createElement('input');ei.type='file';ei.accept='.bin';\n"
"ei.onchange=async function(){\n"
" let f=ei.files[0];if(!f)return;\n"
" if(!confirm('Restore this EEPROM? It overwrites the console EEPROM.'))return;\n"
" let buf=await f.arrayBuffer();\n"
" let r=await fetch('/api/eeprom',{method:'POST',body:buf});\n"
" msg(r.ok?'EEPROM restored':'Restore failed: '+(await r.text()));\n"
"};\n"
"function eeRestore(){ei.value='';ei.click();}\n"
"async function resetSettings(){\n"
" if(!confirm('Reset loader settings to defaults? Banks are not touched.'))return;\n"
" await fetch('/api/reset',{method:'POST'});msg('Settings reset');\n"
"}\n"
"function sysCard(){\n"
" let c=document.createElement('div');c.className='card';\n"
" let h=document.createElement('h2');h.textContent='System';c.appendChild(h);\n"
" let bd=document.createElement('span');bd.className='badge';bd.textContent='EEPROM / SETTINGS';c.appendChild(bd);\n"
" let row=document.createElement('div');row.className='row';\n"
" row.appendChild(btn('Backup EEPROM','',eeBackup));\n"
" row.appendChild(btn('Restore EEPROM','',eeRestore));\n"
" row.appendChild(btn('Reset Settings','danger',resetSettings));\n"
" c.appendChild(row);return c;\n"
"}\n"
"document.getElementById('sys').appendChild(sysCard());\n"
"load();\n"
"</script></body></html>\n";

// ---- JSON: every bank ------------------------------------------------------
static int buildBanks(char* o)
{
    int p = 0, i, n = Bank_Count();
    p += appS(o + p, "{\"banks\":[");
    for (i = 0; i < n; ++i) {
        if (i) o[p++] = ',';
        p += appS(o + p, "{\"i\":");    p += appI(o + p, i);
        p += appS(o + p, ",\"name\":\""); p += appJson(o + p, Bank_Name(i));
        p += appS(o + p, "\",\"ef\":");  p += appI(o + p, Bank_Ef(i));
        p += appS(o + p, ",\"occ\":");  p += appI(o + p, Bank_Occupied(i));
        p += appS(o + p, ",\"size\":"); p += appI(o + p, Bank_SizeCode(i));
        p += appS(o + p, ",\"boot\":"); p += appI(o + p, Bank_IsBoot(i));
        o[p++] = '}';
    }
    p += appS(o + p, "]}");
    o[p] = 0; return p;
}

// ---- logo -> 24-bit BMP (idx0 mapped to the page background) ---------------
static int buildLogoBmp(unsigned char* o)
{
    int W = EOS_LOGO_W, H = EOS_LOGO_H, rowB = W * 3, sz = 54 + rowB * H, x, y, i;
    for (i = 0; i < 54; ++i) o[i] = 0;
    o[0] = 'B'; o[1] = 'M';
    putLE32(o + 2, (unsigned)sz);
    putLE32(o + 10, 54);
    putLE32(o + 14, 40);
    putLE32(o + 18, (unsigned)W);
    putLE32(o + 22, (unsigned)H);
    o[26] = 1; o[28] = 24;                 // planes=1, bpp=24
    putLE32(o + 34, (unsigned)(rowB * H));
    for (y = 0; y < H; ++y) {
        unsigned char* d = o + 54 + (H - 1 - y) * rowB;   // BMP rows are bottom-up
        for (x = 0; x < W; ++x) {
            int idx;
            unsigned char r, g, b, byte;
            i = y * W + x;
            byte = EOS_LOGO_4BPP[i >> 1];
            idx = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            if (idx == 0) { r = 10; g = 10; b = 15; }     // page --bg #0a0a0f
            else { const unsigned char* c = EOS_LOGO_PAL[idx - 1]; r = c[0]; g = c[1]; b = c[2]; }
            d[x * 3 + 0] = b; d[x * 3 + 1] = g; d[x * 3 + 2] = r;   // BGR
        }
    }
    return sz;
}

// ---- request line / headers parse -----------------------------------------
static int qBank(const char* q)   // find "b=" in the query, return int or -1
{
    int i = 0, v;
    while (q[i] && q[i] != ' ' && q[i] != '\r') {
        if (q[i] == 'b' && q[i + 1] == '=') {
            i += 2; v = 0;
            if (q[i] < '0' || q[i] > '9') return -1;
            while (q[i] >= '0' && q[i] <= '9') { v = v * 10 + (q[i] - '0'); ++i; }
            return v;
        }
        ++i;
    }
    return -1;
}

static int findCLen(void)   // scan headers for Content-Length, -1 if absent
{
    int i = 0, v;
    const char* k = "content-length:";
    while (i < s_reqLen) {
        int j = 0;
        while (k[j] && i + j < s_reqLen && lc(s_req[i + j]) == k[j]) ++j;
        if (!k[j]) {
            i += j;
            while (i < s_reqLen && (s_req[i] == ' ' || s_req[i] == '\t')) ++i;
            v = 0;
            while (i < s_reqLen && s_req[i] >= '0' && s_req[i] <= '9') { v = v * 10 + (s_req[i] - '0'); ++i; }
            return v;
        }
        while (i < s_reqLen && s_req[i] != '\n') ++i;
        ++i;
    }
    return -1;
}

static void parseReq(void)
{
    int i = 0; const char* path;
    s_method = M_GET; s_route = R_NONE; s_bank = -1; s_clen = -1;

    if (strEqN(s_req, "POST ", 5)) { s_method = M_POST; i = 5; }
    else if (strEqN(s_req, "GET ", 4)) { s_method = M_GET; i = 4; }
    else return;

    path = s_req + i;
    if (strEqN(path, "/api/banks", 10)) s_route = R_BANKS;
    else if (strEqN(path, "/api/rename", 11)) s_route = R_RENAME;
    else if (strEqN(path, "/api/delete", 11)) s_route = R_DELETE;
    else if (strEqN(path, "/api/flash", 10)) s_route = R_FLASH;
    else if (strEqN(path, "/api/launch", 11)) s_route = R_LAUNCH;
    else if (strEqN(path, "/api/eeprom", 11)) s_route = R_EEPROM;
    else if (strEqN(path, "/api/reset", 10))  s_route = R_RESET;
    else if (strEqN(path, "/logo.bmp", 9)) s_route = R_LOGO;
    else if (path[0] == '/' && (path[1] == ' ' || path[1] == '?')) s_route = R_PAGE;

    s_bank = qBank(path);
    if (s_method == M_POST) s_clen = findCLen();
}

// ---- response setup --------------------------------------------------------
static void respond(const char* status, const char* ctype, const char* body, int blen)
{
    int p = 0;
    p += appS(s_resp + p, "HTTP/1.1 ");      p += appS(s_resp + p, status);
    p += appS(s_resp + p, "\r\nContent-Type: "); p += appS(s_resp + p, ctype);
    p += appS(s_resp + p, "\r\nContent-Length: "); p += appI(s_resp + p, blen);
    p += appS(s_resp + p, "\r\nConnection: close\r\n\r\n");
    s_txH = s_resp; s_txHLen = p; s_txHOff = 0;
    s_txB = body;   s_txBLen = blen; s_txBOff = 0;
    s_state = ST_SEND;
}
static void respondText(const char* status, const char* msg)
{
    respond(status, "text/plain", msg, aLen(msg));
}

// ---- act on a fully-received request --------------------------------------
static void process(void)
{
    int n = Bank_Count();

    if (s_route == R_PAGE) { respond("200 OK", "text/html", k_page, aLen(k_page)); return; }
    if (s_route == R_BANKS) { int len = buildBanks(s_json); respond("200 OK", "application/json", s_json, len); return; }
    if (s_route == R_LOGO) { int len = buildLogoBmp(s_rx); respond("200 OK", "image/bmp", (const char*)s_rx, len); return; }

    if (s_route == R_RENAME) {
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        s_name[s_rxStore] = 0;
        if (s_name[0]) { Bank_SetName(s_bank, s_name); Config_Save(); }
        respondText("200 OK", "ok"); return;
    }
    if (s_route == R_DELETE) {
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank) || !Bank_Occupied(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        if (Flash_EraseBank(Bank_Ef(s_bank)) == EOS_FLASH_OK) {
            Bank_ClearEntry(s_bank); Config_Save();
            respondText("200 OK", "ok");
        }
        else respondText("500 Error", "erase failed");
        return;
    }
    if (s_route == R_FLASH) {
        int sc;
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        if (s_err == 413) { respondText("413 Too Large", "image exceeds bank capacity"); return; }
        if (s_clen <= 0) { respondText("400 Bad Request", "no image"); return; }
        if (Flash_WriteImage(Bank_Ef(s_bank), s_rx, s_clen) != EOS_FLASH_OK) { respondText("500 Error", "flash failed"); return; }
        sc = (s_clen <= 256 * 1024) ? EOS_BANK_SIZE_256K : (s_clen <= 512 * 1024) ? EOS_BANK_SIZE_512K : EOS_BANK_SIZE_1MB;
        Bank_SetOccupied(s_bank, 1, sc); Config_Save();
        respondText("200 OK", "ok"); return;
    }
    if (s_route == R_LAUNCH) {
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank) || !Bank_Occupied(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        s_launch = s_bank;                 // warm-reset after the response is flushed
        respondText("200 OK", "launching"); return;
    }
    if (s_route == R_EEPROM) {
        if (s_method == M_GET) {
            if (Eeprom_ReadImage(s_rx) != EOS_EE_OK) { respondText("500 Error", "eeprom read failed"); return; }
            respond("200 OK", "application/octet-stream", (const char*)s_rx, EOS_EEPROM_SIZE);
            return;
        }
        if (s_err == 400) { respondText("400 Bad Request", "eeprom must be 256 bytes"); return; }
        if (s_rxStore != EOS_EEPROM_SIZE) { respondText("400 Bad Request", "short image"); return; }
        if (Eeprom_ImageValid(s_rx) != EOS_EE_OK) { respondText("400 Bad Request", "invalid eeprom image"); return; }
        if (Eeprom_WriteImage(s_rx) != EOS_EE_OK) { respondText("500 Error", "eeprom write failed"); return; }
        respondText("200 OK", "ok"); return;
    }
    if (s_route == R_RESET) {
        Config_ResetSettings();
        respondText("200 OK", "settings reset"); return;
    }
    respondText("404 Not Found", "not found");
}

// ---- connection lifecycle --------------------------------------------------
static void closeConn(void)
{
    if (s_conn != INVALID_SOCKET) { closesocket(s_conn); s_conn = INVALID_SOCKET; }
    s_state = ST_IDLE; s_launch = -1;
}

// route a POST body to the right buffer; called once headers are parsed
static void beginBody(void)
{
    s_rxRecv = 0; s_rxStore = 0; s_store = 1; s_err = 0;
    if (s_route == R_FLASH) {
        int cap = Bank_CapacityBytes(s_bank >= 0 ? s_bank : 0);
        if (cap > HTTP_RX_MAX) cap = HTTP_RX_MAX;
        if (s_clen > cap) { s_err = 413; s_store = 0; }   // drain then 413
    }
    else if (s_route == R_EEPROM) {
        if (s_clen != EOS_EEPROM_SIZE) { s_err = 400; s_store = 0; }   // EEPROM image is exactly 256B
    }
    else if (s_route == R_RENAME) {
        // cap to the name buffer; extra drained
    }
    else {
        s_store = 0;                                       // delete/launch: no body kept
    }
}

// stash up to `cap` body bytes into the route's target; rest is dropped
static void stashBody(const char* src, int len)
{
    int cap, room, i;
    if (!s_store) { s_rxRecv += len; return; }
    if (s_route == R_FLASH || s_route == R_EEPROM) {
        cap = (s_route == R_EEPROM) ? EOS_EEPROM_SIZE : HTTP_RX_MAX;
        room = cap - s_rxStore; if (room > len) room = len;
        for (i = 0; i < room; ++i) s_rx[s_rxStore + i] = (unsigned char)src[i];
        s_rxStore += room;
    }
    else if (s_route == R_RENAME) {
        cap = (int)sizeof(s_name) - 1;
        room = cap - s_rxStore; if (room > len) room = len;
        for (i = 0; i < room; ++i) s_name[s_rxStore + i] = src[i];
        s_rxStore += room;
    }
    s_rxRecv += len;
}

void Http_Poll(void)
{
    char buf[2048];
    int  moved = 0, r;

    if (!s_up) return;

    // accept one client if idle
    if (s_conn == INVALID_SOCKET) {
        unsigned long nb = 1;
        SOCKET c = accept(s_listen, NULL, NULL);
        if (c == INVALID_SOCKET) return;          // WOULDBLOCK -> nobody waiting
        ioctlsocket(c, FIONBIO, &nb);
        s_conn = c; s_state = ST_HDR; s_reqLen = 0;
    }

    if (s_state == ST_HDR) {
        for (;;) {
            int room = HTTP_REQ_MAX - s_reqLen;
            int e, he;
            if (room <= 1) { respondText("400 Bad Request", "header too large"); break; }
            r = recv(s_conn, s_req + s_reqLen, room, 0);
            if (r == 0) { closeConn(); return; }
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) return;   // wait next frame
                closeConn(); return;
            }
            s_reqLen += r;
            // find end of headers
            he = -1;
            for (e = 3; e < s_reqLen; ++e)
                if (s_req[e - 3] == '\r' && s_req[e - 2] == '\n' && s_req[e - 1] == '\r' && s_req[e] == '\n') { he = e + 1; break; }
            if (he < 0) { if ((moved += r) > HTTP_POLL_BUDGET) return; continue; }

            parseReq();
            if (s_method == M_POST && s_clen > 0) {
                int after = s_reqLen - he;
                beginBody();
                if (after > 0) stashBody(s_req + he, after);
                if (s_rxRecv >= s_clen) { process(); }
                else { s_state = ST_BODY; }
            }
            else {
                process();
            }
            break;
        }
    }

    if (s_state == ST_BODY) {
        for (;;) {
            r = recv(s_conn, buf, (int)sizeof(buf), 0);
            if (r == 0) { closeConn(); return; }
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) return;
                closeConn(); return;
            }
            stashBody(buf, r);
            if (s_rxRecv >= s_clen) { process(); break; }
            if ((moved += r) > HTTP_POLL_BUDGET) return;          // yield, resume next frame
        }
    }

    if (s_state == ST_SEND) {
        // headers
        while (s_txHOff < s_txHLen) {
            r = send(s_conn, s_txH + s_txHOff, s_txHLen - s_txHOff, 0);
            if (r == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) return; closeConn(); return; }
            s_txHOff += r;
        }
        // body
        while (s_txBOff < s_txBLen) {
            r = send(s_conn, s_txB + s_txBOff, s_txBLen - s_txBOff, 0);
            if (r == SOCKET_ERROR) { if (WSAGetLastError() == WSAEWOULDBLOCK) return; closeConn(); return; }
            s_txBOff += r;
        }
        // fully sent
        if (s_launch >= 0) {
            int bank = s_launch;
            closeConn();
            Bank_Launch(bank);            // 0xEF + SMC warm reset -- does not return
            return;
        }
        closeConn();
    }
}

// ---- start / stop ----------------------------------------------------------
void Http_Start(void)
{
    SOCKET s;
    struct sockaddr_in a;
    unsigned long nb = 1;

    if (s_up) return;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;

    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(HTTP_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return; }
    if (listen(s, 4) != 0) { closesocket(s); return; }
    ioctlsocket(s, FIONBIO, &nb);

    s_listen = s; s_conn = INVALID_SOCKET; s_state = ST_IDLE; s_launch = -1; s_up = 1;
}

void Http_Stop(void)
{
    if (s_conn != INVALID_SOCKET) { closesocket(s_conn);   s_conn = INVALID_SOCKET; }
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
    s_up = 0; s_state = ST_IDLE; s_launch = -1;
}

int Http_IsUp(void) { return s_up; }