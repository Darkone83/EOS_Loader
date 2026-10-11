// eos_http.cpp -- see eos_http.h. Single-connection, tick-polled HTTP server.
#include <xtl.h>
#include <winsockx.h>
#include "eos_http.h"
#include "eos_bank.h"
#include "eos_descriptor.h"
#include "eos_config.h"
#include "eos_flash.h"
#include "eos_eeprom_io.h"
#include "eos_eeprom.h"
#include "eos_console.h"
#include "eos_logo_data.h"   // EOS_LOGO_W/H, EOS_LOGO_PAL[15][3], EOS_LOGO_4BPP[]
#include "eos_file.h"        // File_ListDir/Exists/ReadInto, EosFileEntry (custom themes)
#include "eos_webfiles.h"    // bounded, nonblocking Phase 5 Files WebUI
#include "eos_sdcard.h"      // FatFs-backed SD BIOS manager
#include "eos_led.h"         // Web launch LED handoff
#include "eos_xboxrgb.h"     // mirror bank launch to optional XBOX-RGB transient effect

#define HTTP_PORT      80
#define HTTP_REQ_MAX   8192
#define HTTP_RX_MAX    (1024 * 1024)   // flash upload / BMP staging
#define HTTP_JSON_MAX  8192
#define HTTP_RESP_MAX  1024
#define HTTP_POLL_BUDGET (128 * 1024)  // max body bytes moved per frame

enum { ST_IDLE = 0, ST_HDR, ST_BODY, ST_SEND, ST_FILE_SEND };
enum { M_GET = 0, M_POST };
enum {
    R_NONE = 0, R_PAGE, R_LOGO, R_BANKS, R_RENAME, R_DELETE, R_FLASH, R_LAUNCH, R_EEPROM, R_RESET, R_SYSINFO, R_CLRXBDIAG,
    R_THEMES, R_TINI, R_TFILE, R_TDEL, R_SETCOLOR,
    R_SDLIST, R_SDDEL, R_SDUP,
    R_FLIST, R_FSESSION, R_FMKDIR, R_FDELETE, R_FRENAME,
    R_FUPLOAD, R_FDOWNLOAD, R_FJOB
};   // custom-theme + SD BIOS web tools

static SOCKET s_listen = INVALID_SOCKET;
static SOCKET s_conn = INVALID_SOCKET;
static int    s_up = 0;
static int    s_state = ST_IDLE;

static char   s_req[HTTP_REQ_MAX]; static int s_reqLen;
static int    s_method, s_route, s_bank, s_clen;
static int    s_rxRecv, s_rxStore, s_store, s_err;
static int    s_launch = -1;
static int    s_filesParseError = 0;
static char   s_filesPath[EOS_WF_PATH], s_filesNew[80];
static int    s_filesPage = 0, s_filesOverwrite = 0;
static char   s_filesToken[17];              // session-scoped CSRF deterrent, NOT authentication
static char   s_fileSmall[16 * 1024];          // always safe for 64 MB consoles
static char* s_fileChunk = s_fileSmall;       // optional 64 KB on 128 MB consoles
static int    s_fileChunkCap = (int)sizeof(s_fileSmall);
static int    s_fileHave = 0, s_fileAt = 0;
static unsigned long long s_fileTotal = 0, s_fileSent = 0;

static unsigned char s_rx[HTTP_RX_MAX];
static EosLayout     g_lay;   // scratch layout for descriptor updates on flash

// Map a bank table index to a descriptor slot (0..3) or -1. User banks have
// EF 0x3..0x6 -> slot 0..3. (Same mapping as the loader UI.)
static int httpDescSlot(int idx)
{
    unsigned char ef = Bank_Ef(idx);
    if (ef >= 0x3 && ef <= 0x6) return (int)(ef - 0x3);
    return -1;
}
static char   s_json[HTTP_JSON_MAX];
static char   s_resp[HTTP_RESP_MAX];
static char   s_name[80];
static char   s_tFolder[64], s_tName[64], s_tLoc[8]; // theme route query params
static char   s_sdPath[160];                    // SD manager path (root-relative)
static char   s_colorStr[10];                   // /api/setcolor ?c=RRGGBB
static HANDLE s_upFile = INVALID_HANDLE_VALUE;  // streaming theme-file upload
static FIL    s_sdUpFile;                       // SD theme-file streaming upload
static int    s_sdUpOpen = 0;

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
"header{display:flex;align-items:center;gap:16px;padding:20px;border-bottom:1px solid #222;max-width:1100px;margin:0 auto;}\n"
"header img{width:72px;height:72px;}\n"
"h1{font-size:22px;margin:0;letter-spacing:4px;color:var(--p);}\n"
".sub{color:var(--dim);font-size:12px;}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px;padding:20px;max-width:1100px;margin:0 auto;}\n"
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
"#cmodal{position:fixed;inset:0;background:rgba(0,0,0,.66);display:none;align-items:center;justify-content:center;z-index:50;}\n"
"#cmodal.show{display:flex;}\n"
"#cbox{background:var(--card);border:1px solid #2c2c38;border-radius:12px;padding:20px;max-width:520px;width:90%;}\n"
"#cbox h3{margin:0 0 4px;color:var(--p);font-size:16px;letter-spacing:2px;}\n"
"#cbox .csub{color:var(--dim);font-size:12px;margin-bottom:14px;}\n"
"#cgrid{display:grid;grid-template-columns:repeat(6,1fr);gap:8px;}\n"
"#cgrid .sw{border-radius:8px;padding:12px 4px;text-align:center;font-size:11px;cursor:pointer;color:#1a1a1a;border:2px solid transparent;user-select:none;}\n"
"#cgrid .sw:hover{border-color:var(--p);}\n"
"#cgrid .sw.off{background:#3a3a3a;color:#c8c8c8;}\n"
"#cbox .crow{display:flex;justify-content:flex-end;margin-top:14px;}\n"
".info{margin:10px 0 4px;}\n"
".kv{display:flex;justify-content:space-between;gap:12px;padding:5px 0;border-bottom:1px solid #1e1e28;font-size:13px;}\n"
".kv:last-child{border-bottom:none;}\n"
".kv .k{color:var(--dim);}\n"
".kv .v{color:var(--txt);font-variant-numeric:tabular-nums;}\n"
"@media(max-width:520px){.grid{grid-template-columns:1fr;padding:14px;}header{padding:14px;}header img{width:56px;height:56px;}h1{font-size:19px;}.row{gap:6px;}button{flex:1 1 auto;}}\n"
"#msg{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#1d1d27;border:1px solid var(--p);padding:10px 18px;border-radius:8px;opacity:0;transition:.2s;pointer-events:none;}\n"
"#msg.show{opacity:1;}\n"
".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);align-items:center;justify-content:center;z-index:50;padding:16px;}\n"
".modalcard{background:var(--card);border:1px solid var(--p);border-radius:10px;padding:20px;max-width:460px;width:100%;max-height:90vh;overflow:auto;}\n"
".modalcard h2{margin:0 0 12px;color:var(--p);font-size:18px;}\n"
".modalcard label{display:block;margin:9px 0;font-size:12px;color:var(--dim);}\n"
".modalcard input,.modalcard select{background:#0f0f16;border:1px solid #2c2c38;border-radius:6px;color:var(--txt);padding:6px;}\n"
".modalcard input#mname,.modalcard input[type=file],.modalcard select#mloc{width:100%;display:block;margin-top:4px;}\n"
".modalcard input[type=color]{width:46px;height:28px;padding:2px;vertical-align:middle;cursor:pointer;}\n"
".modalcard input[type=range]{width:70%;padding:0;vertical-align:middle;}\n"
"#colors{display:grid;grid-template-columns:1fr 1fr;gap:2px 10px;}\n"
"#colors label{display:flex;align-items:center;justify-content:space-between;}\n"
"#mprog{color:var(--p);font-size:12px;min-height:16px;margin-top:8px;}\n"
"#themecard .kv .k{overflow:hidden;text-overflow:ellipsis;}\n"
"#themecard .kv button{padding:4px 9px;font-size:12px;margin-left:6px;}\n"
".files-nav{max-width:1100px;margin:0 auto;padding:10px 20px;display:flex;gap:12px;align-items:center;flex-wrap:wrap}\n"
".files-nav button{border-color:#584074;color:#d7c0ee}\n"
"#filesapp[hidden]{display:none}\n"
"#filesapp{max-width:1060px;margin:0 auto 12px;background:var(--card);border:1px solid #322841;border-radius:12px;padding:16px}\n"
".files-header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px}\n"
".files-header h2{font-size:17px;color:var(--p);margin:0}\n"
".files-breadcrumb{display:flex;flex-wrap:wrap;gap:5px;align-items:center;margin:13px 0;color:#b1a3c3;font-size:12px}\n"
".files-breadcrumb button{padding:3px 6px;border:0;background:transparent;color:#c5a9e7}\n"
".files-toolbar{display:flex;gap:7px;align-items:center;flex-wrap:wrap;margin:8px 0 12px}\n"
".files-toolbar button:disabled,.files-row button:disabled{opacity:.35;cursor:not-allowed}\n"
".files-row{display:flex;align-items:center;gap:9px;border-top:1px solid #282332;padding:8px 2px;min-height:45px}\n"
".files-name{min-width:0;flex:1;overflow-wrap:anywhere;font-size:13px;color:var(--txt);text-align:left;background:transparent;border:0;padding:2px}\n"
".files-dir{color:#d3b4f6;font-weight:600}\n"
".files-meta{min-width:80px;text-align:right;color:var(--dim);font-size:11px}\n"
".files-ops{display:flex;gap:4px;flex-wrap:wrap}\n"
".files-ops button{padding:5px 8px;font-size:11px}\n"
".files-ops button.danger{color:#d88989}\n"
".files-status{font-size:12px;color:#afa2bd;min-height:18px;margin:8px 0}\n"
".files-warning{border:1px solid #6b5833;background:#211c15;color:#f0d1a1;padding:9px;border-radius:7px;font-size:12px}\n"
".files-bar{height:7px;border-radius:8px;background:#272130;overflow:hidden;margin:10px 0}\n"
".files-bar>div{height:100%;background:var(--p);width:0;transition:width .1s}\n"
".files-page{display:flex;gap:8px;align-items:center;justify-content:flex-end}\n"
".files-page button{font-size:12px;padding:5px 10px}\n"
".files-muted{font-size:11px;color:var(--dim)}\n"
"@media(max-width:640px){#filesapp{margin:0 12px 12px;padding:12px}.files-row{flex-wrap:wrap}.files-name{flex:1 1 55%}.files-ops{width:100%;justify-content:flex-end}.files-meta{min-width:58px}}\n"
".files-navline{display:flex;align-items:center;gap:9px;min-width:0}\n"
".files-navline>button{padding:4px 12px;font-size:12px;flex:none}\n"
".files-navline>button:disabled{opacity:.35;cursor:not-allowed}\n"
".files-breadcrumb{min-width:0}\n"
".files-dir:hover,.files-name:hover{color:var(--p)}\n"
".files-ops button.danger{color:#ea8b8b;border-color:#76444b}\n"
".files-ops button.danger:hover{background:#342024;color:#fff}\n"
"#wf-confirm[hidden]{display:none}\n"
".files-confirm-overlay{position:fixed;inset:0;z-index:90;background:rgba(0,0,0,.75);display:flex;align-items:center;justify-content:center;padding:14px}\n"
".files-confirm-card{background:var(--card);border:1px solid #6b4050;border-radius:12px;max-width:480px;width:100%;padding:20px;color:var(--txt);box-shadow:0 12px 36px rgba(0,0,0,.45)}\n"
".files-confirm-card h3{margin:0 0 12px;color:#efb3be;font-size:17px}\n"
".files-confirm-card p{font-size:13px;overflow-wrap:anywhere;line-height:1.5}\n"
".files-delete-warning{color:#f5c2ad}\n"
".files-confirm-buttons{display:flex;justify-content:flex-end;gap:10px;margin-top:18px}\n"
".files-confirm-buttons button.danger{background:#722e3b;border-color:#9f4051;color:#fff}\n"
".files-confirm-buttons button.danger:hover{background:#973749}\n"
"</style></head><body>\n"
"<header><img src='/logo.bmp' alt=''><div><h1>EOS</h1><div class=sub>BIOS bank manager</div></div></header>\n"
"<div id=budget style='max-width:1100px;margin:0 auto;padding:8px 20px 0;color:#9a9aa8;font-size:13px;'></div>\n"
"<div class=files-nav><button id=wf-open type=button>File Manager</button>\n"
"<span class=files-muted>Browse mounted HDD and SD volumes. Upload, rename, and delete files.</span></div>\n"
"<section id=filesapp hidden>\n"
"<div class=files-header><h2>File Manager</h2><span id=wf-rights class=files-muted>Select a volume to browse</span></div>\n"
"<div class=files-navline><button id=wf-up type=button disabled>Up</button><div id=wf-crumb class=files-breadcrumb></div></div>\n"
"<div class=files-toolbar>\n"
"<button id=wf-mkdir type=button disabled>New Folder</button><button id=wf-upload type=button disabled>Upload Files</button>\n"
"<button id=wf-folder type=button disabled>Upload Folder</button><button id=wf-refresh type=button>Refresh</button>\n"
"<button id=wf-cancel type=button hidden>Cancel Upload</button>\n"
"<input id=wf-file type=file multiple hidden><input id=wf-folderinput type=file webkitdirectory multiple hidden>\n"
"</div>\n"
"<div class=files-warning>Large uploads may take a while. Keep EOS running until the transfer completes. Deleting a file or folder requires confirmation and cannot be undone.</div>\n"
"<div class=files-bar><div id=wf-progress></div></div>\n"
"<div id=wf-status role=status class=files-status></div>\n"
"<div id=wf-list></div>\n"
"<div class=files-page><button id=wf-prev type=button disabled>Previous</button><span id=wf-pageno class=files-muted>Page 1</span><button id=wf-next type=button disabled>Next</button></div>\n"
"</section>\n"
"<div id=wf-confirm hidden class=files-confirm-overlay role=dialog aria-modal=true aria-labelledby=wf-confirm-title>\n"
"<div class=files-confirm-card>\n"
"<h3 id=wf-confirm-title>Confirm deletion</h3>\n"
"<p id=wf-confirm-detail></p>\n"
"<p id=wf-confirm-warning class=files-delete-warning></p>\n"
"<div class=files-confirm-buttons><button id=wf-confirm-cancel type=button>Cancel</button><button id=wf-confirm-yes type=button class=danger>Delete</button></div>\n"
"</div></div>\n"
"<div class=grid id=grid></div>\n"
"<div class=grid id=sys></div><div id=msg></div>\n"
"<div id=cmodal><div id=cbox>\n"
" <h3 id=ctitle>LED Color</h3>\n"
" <div class=csub>Pick a color for this bank's status LED. Off = LED dark.</div>\n"
" <div id=cgrid></div>\n"
" <div class=crow><button onclick='closeColor()'>Close</button></div>\n"
"</div></div>\n"
"<div id=modal class=modal><div class=modalcard>\n"
" <h2 id=mtitle>Create Theme</h2>\n"
" <label>Name<input id=mname></label>\n"
" <label>Storage<select id=mloc><option value=hdd>HDD</option><option value=sd>SD Card</option></select></label>\n"
" <div id=colors></div>\n"
" <label>Background dim <input id=mdim type=range min=0 max=100 value=40> <span id=mdimv>40</span></label>\n"
" <label>Background image (png or jpg)<input id=mbg type=file accept=image/png,image/jpeg></label>\n"
" <label>Music (mp3, optional)<input id=mmus type=file accept=audio/mpeg,.mp3></label>\n"
" <div id=mprog></div>\n"
" <div class=row><button id=msave class=go>Save</button><button id=mcancel class=danger>Cancel</button></div>\n"
"</div></div>\n"
"<script>\n"
"const SZ=['256K','512K','1MB'];\n"
"function msg(t){let m=document.getElementById('msg');m.textContent=t;m.classList.add('show');setTimeout(function(){m.classList.remove('show');},2500);}\n"
"function btn(label,cls,fn){let e=document.createElement('button');e.textContent=label;if(cls)e.className=cls;e.addEventListener('click',fn);return e;}\n"
"function card(b){\n"
" let c=document.createElement('div');c.className='card';\n"
" let h=document.createElement('h2');h.textContent=b.name;c.appendChild(h);\n"
" let bd=document.createElement('span');\n"
" let shadow=(b.slot===3);\n"
" if(b.boot){bd.className='badge boot';bd.textContent='BOOT';}\n"
" else if(shadow){bd.className='badge';bd.textContent='UNAVAILABLE';}\n"
" else if(b.slot===2){bd.className='badge ready';bd.textContent=SZ[b.dsize>=0?b.dsize:b.size]+' READY';}\n"
" else if(b.occ){bd.className='badge ready';bd.textContent=SZ[b.size]+' READY';}\n"
" else{bd.className='badge';bd.textContent='EMPTY';}\n"
" c.appendChild(bd);\n"
" if(shadow){c.style.opacity='0.45';c.appendChild(document.createElement('div'));return c;}\n"
" let row=document.createElement('div');row.className='row';\n"
" if(!b.boot){\n"
"  row.appendChild(btn('Flash','',function(){pick(b.i);}));\n"
"  row.appendChild(btn('Rename','',function(){ren(b.i,b.name);}));\n"
"  if(b.ef>=3&&b.ef<=6)row.appendChild(btn('LED Color','',function(){pickColor(b.i,b.name);}));\n"
"  if(b.occ){row.appendChild(btn('Delete','danger',function(){del(b.i);}));\n"
"            row.appendChild(btn('Launch','go',function(){go(b.i);}));}\n"
" }\n"
" c.appendChild(row);return c;\n"
"}\n"
"async function load(){\n"
" let r=await fetch('/api/banks');let j=await r.json();\n"
" let g=document.getElementById('grid');g.innerHTML='';\n"
" let hdr=document.getElementById('budget');\n"
" if(hdr)hdr.textContent='User banks: '+(j.freeSlots!==undefined?j.freeSlots:4)+' of 4 free  (1MB budget)';\n"
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
"const PAL=[['Off','ffffff'],['Red','ff0000'],['Orange','ff6000'],['Amber','ffd000'],['Green','30ff00'],['Teal','00ffc0'],['Cyan','00c0ff'],['Blue','0040ff'],['Purple','a855f7'],['Magenta','ff00e0'],['Pink','ff3080'],['White','fefefe']];\n"
"function pickColor(i,name){\n"
" let m=document.getElementById('cmodal');\n"
" document.getElementById('ctitle').textContent='LED Color \u2014 '+name;\n"
" let g=document.getElementById('cgrid');g.innerHTML='';\n"
" for(const p of PAL){\n"
"  let s=document.createElement('div');s.className='sw'+(p[1]=='ffffff'?' off':'');\n"
"  if(p[1]!='ffffff')s.style.background='#'+p[1];\n"
"  s.textContent=p[0];\n"
"  s.addEventListener('click',(function(hex){return async function(){\n"
"    await fetch('/api/setcolor?b='+i+'&c='+hex,{method:'POST'});\n"
"    m.classList.remove('show');msg('Color set');\n"
"  };})(p[1]));\n"
"  g.appendChild(s);\n"
" }\n"
" m.classList.add('show');\n"
"}\n"
"function closeColor(){document.getElementById('cmodal').classList.remove('show');}\n"
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
"async function clearXbdiag(){\n"
" if(!confirm('Erase XbDiag Lite from bank 0xD?'))return;\n"
" let r=await fetch('/api/clrxbdiag');msg(r.ok?'XbDiag cleared':'Clear failed');loadSys();\n"
"}\n"
"function kv(k,v){let d=document.createElement('div');d.className='kv';\n"
" let a=document.createElement('span');a.className='k';a.textContent=k;\n"
" let b=document.createElement('span');b.className='v';b.textContent=v;\n"
" d.appendChild(a);d.appendChild(b);return d;}\n"
"async function loadSys(){\n"
" let host=document.getElementById('sys');host.innerHTML='';\n"
" let s={};try{let r=await fetch('/api/sysinfo');s=await r.json();}catch(e){}\n"
" let c=document.createElement('div');c.className='card';\n"
" let h=document.createElement('h2');h.textContent='System';c.appendChild(h);\n"
" let info=document.createElement('div');info.className='info';\n"
" info.appendChild(kv('Loader',s.loader||'?'));\n"
" info.appendChild(kv('Console',(s.rev||'?')+(s.cpuMhz?'  '+s.cpuMhz+' MHz':'')));\n"
" info.appendChild(kv('RAM',(s.ramMB?s.ramMB+' MB':'?')));\n"
" info.appendChild(kv('Encoder',s.encoder||'?'));\n"
" info.appendChild(kv('Serial',s.serial||'?'));\n"
" info.appendChild(kv('MAC',s.mac||'?'));\n"
" info.appendChild(kv('Video',s.video||'?'));\n"
" info.appendChild(kv('Region',(s.region||'?')+(s.dvd?'  /  '+s.dvd:'')));\n"
" info.appendChild(kv('Language',s.lang||'?'));\n"
" info.appendChild(kv('Banks used',(s.usedBanks!==undefined?s.usedBanks:'?')+' / 4'));\n"
" info.appendChild(kv('Free slots',s.freeSlots!==undefined?s.freeSlots:'?'));\n"
" info.appendChild(kv('Ext region',s.extReady?'Resident':'Not loaded'));\n"
" c.appendChild(info);\n"
" let row=document.createElement('div');row.className='row';\n"
" row.appendChild(btn('Backup EEPROM','',eeBackup));\n"
" row.appendChild(btn('Restore EEPROM','',eeRestore));\n"
" c.appendChild(row);\n"
" let row2=document.createElement('div');row2.className='row';\n"
" row2.appendChild(btn('Reset Settings','danger',resetSettings));\n"
" c.appendChild(row2);\n"
" if(s.xbdiag){\n"
"  info.appendChild(kv('XbDiag Lite','Installed (bank 0xD)'));\n"
"  let row3=document.createElement('div');row3.className='row';\n"
"  row3.appendChild(btn('Clear XbDiag','danger',clearXbdiag));\n"
"  c.appendChild(row3);\n"
" }\n"
" host.appendChild(c);\n"
" await renderThemes();\n"
" await renderSd('/');\n"
"}\n"
"const CK=[['bg_top','BG Top'],['bg_bottom','BG Bottom'],['panel','Panel'],['accent','Accent'],['glow','Glow'],['text','Text'],['text_dim','Text Dim']];\n"
"const CDEF={bg_top:'#0a0a0f',bg_bottom:'#05050a',panel:'#15151c',accent:'#a855f7',glow:'#c77dff',text:'#e8e8ef',text_dim:'#6a6a78'};\n"
"let editFolder=null,editLoc='hdd',curBg='',curMus='';\n"
"function showModal(on){document.getElementById('modal').style.display=on?'flex':'none';}\n"
"function extOf(fn){let i=fn.lastIndexOf('.');return i>=0?fn.slice(i).toLowerCase():'';}\n"
"function normColor(c){c=(c||'').trim().toLowerCase();if(c.length==7&&c[0]=='#'){let ok=true;for(let i=1;i<7;i++){let h=c[i];if(!((h>='0'&&h<='9')||(h>='a'&&h<='f')))ok=false;}if(ok)return c;}return '#888888';}\n"
"function parseIni(t){let o={};for(const ln of t.split('\\n')){let s=ln.trim();if(!s||s[0]=='#'||s[0]==';')continue;let e=s.indexOf('=');if(e<0)continue;o[s.slice(0,e).trim().toLowerCase()]=s.slice(e+1).trim();}return o;}\n"
"function buildColorInputs(){let w=document.getElementById('colors');w.innerHTML='';for(const kc of CK){let l=document.createElement('label');l.textContent=kc[1];let ci=document.createElement('input');ci.type='color';ci.id='c_'+kc[0];ci.value=CDEF[kc[0]]||'#888888';l.appendChild(ci);w.appendChild(l);}}\n"
"function initModal(){document.getElementById('msave').addEventListener('click',saveTheme);document.getElementById('mcancel').addEventListener('click',function(){showModal(false);});let dm=document.getElementById('mdim');dm.addEventListener('input',function(){document.getElementById('mdimv').textContent=dm.value;});}\n"
"async function renderThemes(){let host=document.getElementById('sys');let old=document.getElementById('themecard');if(old)old.remove();let t={themes:[]};try{let r=await fetch('/api/themes');t=await r.json();}catch(e){}\n"
" let c=document.createElement('div');c.className='card';c.id='themecard';let h=document.createElement('h2');h.textContent='Custom Themes';c.appendChild(h);\n"
" let info=document.createElement('div');info.className='info';\n"
" if(!t.themes||!t.themes.length){let e=document.createElement('div');e.className='kv';e.textContent='No custom themes yet';info.appendChild(e);}\n"
" else{for(const id of t.themes){let q=id.indexOf('|'),loc=(q>=0?id.slice(0,q):'HDD').toLowerCase(),nm=q>=0?id.slice(q+1):id;let row=document.createElement('div');row.className='kv';let k=document.createElement('span');k.className='k';k.textContent=nm+'  ['+loc.toUpperCase()+']';row.appendChild(k);let v=document.createElement('span');v.appendChild(btn('Edit','',function(){openEdit(nm,loc);}));v.appendChild(btn('Delete','danger',function(){delTheme(nm,loc);}));row.appendChild(v);info.appendChild(row);}}\n"
" c.appendChild(info);let r2=document.createElement('div');r2.className='row';r2.appendChild(btn('Create Theme','go',openCreate));c.appendChild(r2);host.appendChild(c);}\n"
"function openCreate(){editFolder=null;editLoc='hdd';curBg='';curMus='';document.getElementById('mtitle').textContent='Create Theme';let mn=document.getElementById('mname');mn.value='';mn.disabled=false;let ml=document.getElementById('mloc');ml.value='hdd';ml.disabled=false;document.getElementById('mdim').value=40;document.getElementById('mdimv').textContent='40';document.getElementById('mbg').value='';document.getElementById('mmus').value='';for(const kc of CK)document.getElementById('c_'+kc[0]).value=CDEF[kc[0]];document.getElementById('mprog').textContent='';showModal(true);}\n"
"async function openEdit(folder,loc){editFolder=folder;editLoc=loc||'hdd';document.getElementById('mtitle').textContent='Edit Theme';let mn=document.getElementById('mname');mn.value=folder;mn.disabled=true;let ml=document.getElementById('mloc');ml.value=editLoc;ml.disabled=true;document.getElementById('mbg').value='';document.getElementById('mmus').value='';document.getElementById('mprog').textContent='';\n"
" let txt='';try{let r=await fetch('/api/theme/ini?folder='+encodeURIComponent(folder)+'&loc='+encodeURIComponent(editLoc));txt=await r.text();}catch(e){}let kv=parseIni(txt);curBg=kv.background||'';curMus=kv.music||'';\n"
" let dim=kv.bg_dim||'0';document.getElementById('mdim').value=dim;document.getElementById('mdimv').textContent=dim;for(const kc of CK)document.getElementById('c_'+kc[0]).value=normColor(kv[kc[0]]);showModal(true);}\n"
"function resizeImage(file){return new Promise(function(resolve){let img=new Image();img.onload=function(){let w=img.width,h=img.height,MX=1280,MY=720;if(w<=MX&&h<=MY){resolve(file);return;}let s=Math.min(MX/w,MY/h);let nw=Math.round(w*s),nh=Math.round(h*s);let cv=document.createElement('canvas');cv.width=nw;cv.height=nh;cv.getContext('2d').drawImage(img,0,0,nw,nh);let type=(file.type&&file.type.indexOf('png')>=0)?'image/png':'image/jpeg';cv.toBlob(function(b){resolve(b||file);},type,0.9);};img.onerror=function(){resolve(file);};img.src=URL.createObjectURL(file);});}\n"
"async function saveTheme(){let name=editFolder||document.getElementById('mname').value.trim().replace(/[^A-Za-z0-9 _-]/g,'');if(!name){alert('Name required');return;}let loc=editFolder?editLoc:document.getElementById('mloc').value;\n"
" let bgFile=document.getElementById('mbg').files[0];let musFile=document.getElementById('mmus').files[0];let prog=document.getElementById('mprog');\n"
" let bgName=bgFile?('background'+extOf(bgFile.name)):curBg;let musName=musFile?('music'+extOf(musFile.name)):curMus;\n"
" let ini='version = 1\\nname = '+name+'\\n';if(bgName)ini+='background = '+bgName+'\\n';ini+='bg_dim = '+document.getElementById('mdim').value+'\\n';if(musName)ini+='music = '+musName+'\\n';\n"
" for(const kc of CK)ini+=kc[0]+' = '+document.getElementById('c_'+kc[0]).value+'\\n';\n"
" prog.textContent='Saving theme...';try{\n"
"  await fetch('/api/theme/ini?folder='+encodeURIComponent(name)+'&loc='+encodeURIComponent(loc),{method:'POST',body:ini});\n"
"  if(bgFile){prog.textContent='Uploading background...';let blob=await resizeImage(bgFile);await fetch('/api/theme/file?folder='+encodeURIComponent(name)+'&name='+encodeURIComponent(bgName)+'&loc='+encodeURIComponent(loc),{method:'POST',body:blob});}\n"
"  if(musFile){prog.textContent='Uploading music...';await fetch('/api/theme/file?folder='+encodeURIComponent(name)+'&name='+encodeURIComponent(musName)+'&loc='+encodeURIComponent(loc),{method:'POST',body:musFile});}\n"
"  prog.textContent='';showModal(false);msg('Theme saved');renderThemes();\n"
" }catch(e){prog.textContent='Save failed';}}\n"
"async function delTheme(folder,loc){if(!confirm('Delete theme \"'+folder+'\" from '+loc.toUpperCase()+'?'))return;try{await fetch('/api/theme/del?folder='+encodeURIComponent(folder)+'&loc='+encodeURIComponent(loc),{method:'POST'});}catch(e){}msg('Theme deleted');renderThemes();}\n"
"let sdPath='/';let sdUpload=document.createElement('input');sdUpload.type='file';sdUpload.accept='.bin';\n"
"function sdFmt(n){if(n==1048576)return '1 MB';if(n==524288)return '512 KB';if(n==262144)return '256 KB';return n+' B';}\n"
"function sdParent(p){if(p==='/'||!p)return '/';let q=p.replace(/\\/$/,'');let i=q.lastIndexOf('/');return i<=0?'/':q.slice(0,i+1);}\n"
"async function renderSd(path){sdPath=path||'/';let host=document.getElementById('sys');let old=document.getElementById('sdcard');if(old)old.remove();let d={ok:false,entries:[]};try{let r=await fetch('/api/sd/list?path='+encodeURIComponent(sdPath));d=await r.json();}catch(e){}\n"
" let c=document.createElement('div');c.className='card';c.id='sdcard';let h=document.createElement('h2');h.textContent='SD BIOS Manager';c.appendChild(h);let info=document.createElement('div');info.className='info';info.appendChild(kv('Path',sdPath));\n"
" if(!d.ok){let e=document.createElement('div');e.className='kv';e.textContent=d.error||'SD card unavailable';info.appendChild(e);}else{if(sdPath!=='/'){let up=document.createElement('div');up.className='kv';let k=document.createElement('span');k.className='k';k.textContent='[..]';up.appendChild(k);let v=document.createElement('span');v.appendChild(btn('Up','',function(){renderSd(sdParent(sdPath));}));up.appendChild(v);info.appendChild(up);}\n"
"  if(!d.entries||!d.entries.length){let e=document.createElement('div');e.className='kv';e.textContent='No BIOS files';info.appendChild(e);}else{for(const f of d.entries){let row=document.createElement('div');row.className='kv';let k=document.createElement('span');k.className='k';k.textContent=(f.d?'[D] ':'')+f.n+(f.d?'/':'  ('+sdFmt(f.s)+')');row.appendChild(k);let v=document.createElement('span');if(f.d){v.appendChild(btn('Open','',function(){let b=sdPath==='/'?'':sdPath.replace(/\\/$/,'');renderSd(b+'/'+f.n+'/');}));}else{v.appendChild(btn('Delete','danger',function(){sdDelete(f.n);}));}row.appendChild(v);info.appendChild(row);}}}\n"
" c.appendChild(info);let rr=document.createElement('div');rr.className='row';rr.appendChild(btn('Upload BIOS','go',function(){sdUpload.value='';sdUpload.click();}));rr.appendChild(btn('Refresh','',function(){renderSd(sdPath);}));c.appendChild(rr);host.appendChild(c);}\n"
"async function sdDelete(name){if(!confirm('Delete BIOS \"'+name+'\" from SD card?'))return;let b=sdPath==='/'?'':sdPath.replace(/\\/$/,'');let p=b+'/'+name;let r=await fetch('/api/sd/delete?path='+encodeURIComponent(p),{method:'POST'});msg(r.ok?'BIOS deleted':'Delete failed: '+(await r.text()));renderSd(sdPath);}\n"
"sdUpload.onchange=async function(){let f=sdUpload.files[0];if(!f)return;if(!(f.size==262144||f.size==524288||f.size==1048576)){msg('BIOS must be exactly 256K, 512K, or 1MB');return;}let b=sdPath==='/'?'':sdPath.replace(/\\/$/,'');let p=b+'/'+f.name;msg('Uploading '+f.name+'...');let r=await fetch('/api/sd/upload?path='+encodeURIComponent(p),{method:'POST',body:await f.arrayBuffer()});msg(r.ok?'BIOS uploaded':'Upload failed: '+(await r.text()));renderSd(sdPath);};\n"
"// Phase 5: file manager UI. All destructive work remains server-validated.\n"
"// Phase 5: file manager UI. All destructive work remains server-validated.\n"
"let wfPath='/',wfPage=0,wfKey='',wfRunning=false,wfXhr=null,wfCancel=false,wfQueue=[];\n"
"let wfN=0,wfTotal=0,wfFinished=0,wfConfirmActive=false;\n"
"const wfId=s=>document.getElementById(s);\n"
"function wfText(s){wfId('wf-status').textContent=s||'';}\n"
"function wfProgress(p){wfId('wf-progress').style.width=Math.max(0,Math.min(100,p))+'%';}\n"
"function wfJoin(base,name){return(base==='/'?'':base.replace(/\\/$/,''))+'/'+name;}\n"
"function wfParent(path){if(path==='/')return '/';const x=path.lastIndexOf('/');return x<=0?'/':path.slice(0,x);}\n"
"function wfByte(n){if(n>=1048576)return(n/1048576).toFixed(1)+' MB';if(n>=1024)return(n/1024).toFixed(1)+' KB';return n+' B';}\n"
"function wfElement(tag,text,cls){const e=document.createElement(tag);if(text!==null&&text!==undefined)e.textContent=text;if(cls)e.className=cls;return e;}\n"
"function wfButton(label,fn,cls){const b=wfElement('button',label,cls||'');b.type='button';b.onclick=fn;return b;}\n"
"async function wfSession(){if(wfKey)return;const r=await fetch('/api/files/session');if(!r.ok)throw Error('File manager unavailable');const j=await r.json();if(!j.key)throw Error('Missing file session');wfKey=j.key;}\n"
"async function wfCall(url,method){await wfSession();const r=await fetch(url,{method:method||'POST',headers:{'X-EOS-Files-Key':wfKey}});let body={};try{body=await r.json();}catch(e){}if(!r.ok||body.ok===false)throw Error(body.error||('HTTP '+r.status));return{data:body,status:r.status};}\n"
"async function wfOpen(){const el=wfId('filesapp');if(!el.hidden){if(wfRunning||wfConfirmActive)return;el.hidden=true;return;}el.hidden=false;try{await wfSession();await wfLoad('/',0);}catch(e){wfText('File Manager error: '+e.message);}el.scrollIntoView({block:'start'});}\n"
"function wfCrumb(){const c=wfId('wf-crumb');c.replaceChildren();const pieces=wfPath.split('/').filter(Boolean);c.appendChild(wfButton('Drives',()=>wfLoad('/',0)));\n"
"let path='';for(const p of pieces){path+='/'+p;c.appendChild(wfElement('span','/'));const dest=path;c.appendChild(wfButton(p,()=>wfLoad(dest,0)));}\n"
"wfId('wf-up').disabled=(wfPath==='/');}\n"
"async function wfLoad(path,page){if(wfRunning||wfConfirmActive)return;const items=wfId('wf-list');items.replaceChildren();wfText('Loading...');\n"
"try{const r=await fetch('/api/files/list?path='+encodeURIComponent(path)+'&page='+(page||0));const j=await r.json();if(!r.ok||!j.ok)throw Error(j.error||'Listing failed');\n"
"wfPath=path;wfPage=page||0;wfCrumb();const editable=!!j.writable;\n"
"wfId('wf-mkdir').disabled=!editable;wfId('wf-upload').disabled=!editable;wfId('wf-folder').disabled=!editable;\n"
"wfId('wf-rights').textContent=editable?'Mounted folder — upload, create, rename or delete':'Select a mounted volume to modify files';\n"
"let dirs=0,files=0;\n"
"for(const f of j.entries){if(f.d)dirs++;else files++;const row=wfElement('div',null,'files-row');const dst=wfJoin(wfPath,f.n);\n"
"const name=wfButton((f.d?'[Folder] ':'')+f.n,()=>f.d?wfLoad(dst,0):wfDownload(dst),'files-name'+(f.d?' files-dir':''));name.title=f.d?'Open folder':'Download file';row.appendChild(name);\n"
"row.appendChild(wfElement('span',f.d?'Folder':wfByte(f.s),'files-meta'));const ops=wfElement('div',null,'files-ops');\n"
"if(f.d)ops.appendChild(wfButton('Open',()=>wfLoad(dst,0)));else ops.appendChild(wfButton('Download',()=>wfDownload(dst)));\n"
"if(editable){ops.appendChild(wfButton('Rename',()=>wfRename(dst,f.n)));ops.appendChild(wfButton('Delete',()=>wfDelete(dst,f),'danger'));}\n"
"row.appendChild(ops);items.appendChild(row);}\n"
"if(!j.entries.length)items.appendChild(wfElement('div','This folder is empty.','files-status'));\n"
"wfId('wf-prev').disabled=wfPage<=0;wfId('wf-next').disabled=!j.more;wfId('wf-pageno').textContent='Page '+(wfPage+1);\n"
"wfText(dirs+' folder(s), '+files+' file(s) — folders first, A–Z'+(editable?'':' — open a mounted volume to modify files'));\n"
"}catch(e){wfText('Browse failed: '+e.message);}}\n"
"function wfDownload(path){window.location.href='/api/files/download?path='+encodeURIComponent(path);}\n"
"async function wfMk(){if(wfRunning||wfConfirmActive)return;const name=prompt('New folder name:');if(!name||!name.trim())return;try{await wfCall('/api/files/mkdir?path='+encodeURIComponent(wfJoin(wfPath,name.trim())));await wfLoad(wfPath,wfPage);}catch(e){wfText('Create folder failed: '+e.message);}}\n"
"async function wfRename(path,oldName){if(wfRunning||wfConfirmActive)return;const newName=prompt('Rename to:',oldName);if(!newName||!newName.trim()||newName===oldName)return;try{await wfCall('/api/files/rename?path='+encodeURIComponent(path)+'&name='+encodeURIComponent(newName.trim()));await wfLoad(wfPath,wfPage);}catch(e){wfText('Rename failed: '+e.message);}}\n"
"function wfConfirmDelete(path,item){return new Promise(resolve=>{\n"
"if(wfConfirmActive){resolve(false);return;}wfConfirmActive=true;\n"
"const overlay=wfId('wf-confirm'),accept=wfId('wf-confirm-yes'),cancel=wfId('wf-confirm-cancel');\n"
"wfId('wf-confirm-title').textContent=item.d?'Delete folder and contents?':'Delete file?';\n"
"wfId('wf-confirm-detail').textContent='Target: '+path;\n"
"wfId('wf-confirm-warning').textContent=item.d?'This permanently deletes this folder and all its contents.':'This permanently deletes this file.';\n"
"accept.disabled=false;\n"
"accept.textContent=item.d?'Delete Folder and Contents':'Delete File';overlay.hidden=false;cancel.focus();\n"
"function finish(approved){overlay.hidden=true;wfConfirmActive=false;accept.onclick=null;cancel.onclick=null;document.removeEventListener('keydown',key);resolve(approved);}\n"
"function key(e){if(e.key==='Escape'){e.preventDefault();finish(false);}}\n"
"accept.onclick=()=>finish(true);cancel.onclick=()=>finish(false);document.addEventListener('keydown',key);\n"
"});}\n"
"async function wfDelete(path,item){if(wfRunning||wfConfirmActive)return;if(!await wfConfirmDelete(path,item))return;\n"
"try{const result=await wfCall('/api/files/delete?path='+encodeURIComponent(path));let resultMessage='File deleted';\n"
"if(result.status===202){wfRunning=true;resultMessage=await wfWaitJob();wfRunning=false;}\n"
"const targetPath=wfPath,targetPage=wfPage;await wfLoad(targetPath,targetPage);wfText(resultMessage);\n"
"}catch(e){wfRunning=false;wfText('Delete failed: '+e.message+' — some items may already be deleted');}}\n"
"async function wfWaitJob(){for(;;){const r=await fetch('/api/files/job');const d=await r.json();if(!r.ok)throw Error('Could not read deletion progress');\n"
"if(d.state==='done')return 'Folder and '+d.removed+' item(s) deleted';\n"
"if(d.state==='failed')throw Error(d.error||'Folder deletion failed');\n"
"wfText('Deleting folder contents... '+d.removed+' item(s) removed');await new Promise(resolve=>setTimeout(resolve,350));}}\n"
"async function wfQueueFiles(list){if(wfRunning||wfConfirmActive)return;const arr=Array.from(list||[]);if(!arr.length)return;\n"
"wfQueue=[];wfTotal=0;for(const file of arr){const rel=file.webkitRelativePath||file.name;\n"
"const bits=rel.replace(/\\\\/g,'/').split('/').filter(Boolean);if(!bits.length||bits.some(x=>x==='.'||x==='..')){wfText('Invalid folder path');return;}\n"
"if(file.size>2147483647){wfText('File exceeds 2 GB HTTP transfer limit');return;}\n"
"wfQueue.push({file,rel:bits.join('/'),bits});wfTotal+=file.size;}\n"
"if(wfTotal>=32*1048576||arr.some(f=>f.size>=32*1048576)){\n"
"if(!confirm('Large upload ('+wfByte(wfTotal)+'). This may take a while. Keep EOS running and do not reboot or power off the Xbox until finished. Continue?'))return;}\n"
"wfCancel=false;wfFinished=0;wfRunning=true;wfId('wf-cancel').hidden=false;wfProgress(0);\n"
"let finalMessage='';try{for(wfN=0;wfN<wfQueue.length;wfN++){\n"
"if(wfCancel)throw Error('Transfer cancelled');const job=wfQueue[wfN];let dir=wfPath;\n"
"for(let i=0;i<job.bits.length-1;i++){if(wfCancel)throw Error('Transfer cancelled');dir=wfJoin(dir,job.bits[i]);await wfCall('/api/files/mkdir?path='+encodeURIComponent(dir));}\n"
"const dest=wfJoin(dir,job.bits[job.bits.length-1]);wfText('Uploading '+job.rel+' ('+(wfN+1)+'/'+wfQueue.length+')');\n"
"let overwrite=false;while(true){try{await wfSend(dest,job.file,overwrite);break;}catch(e){\n"
"if(e.status===409&&confirm('Replace existing file \"'+job.rel+'\"?')){overwrite=true;continue;}throw e;}}\n"
"wfFinished+=job.file.size;wfProgress(wfTotal?100*wfFinished/wfTotal:100);}\n"
"finalMessage='Finished: '+wfQueue.length+' file(s) uploaded and finalized';\n"
"}catch(e){finalMessage='Upload stopped: '+e.message+'. Check the destination before retrying.';}\n"
"wfRunning=false;wfId('wf-cancel').hidden=true;wfXhr=null;wfId('wf-file').value='';wfId('wf-folderinput').value='';\n"
"await wfLoad(wfPath,wfPage);wfText(finalMessage);}\n"
"function wfSend(path,file,overwrite){return new Promise(async(resolve,reject)=>{\n"
"try{await wfSession();if(wfCancel)throw Error('Cancelled');const x=new XMLHttpRequest();wfXhr=x;\n"
"x.open('POST','/api/files/upload?path='+encodeURIComponent(path)+'&overwrite='+(overwrite?'1':'0'));\n"
"x.setRequestHeader('X-EOS-Files-Key',wfKey);x.setRequestHeader('Content-Type','application/octet-stream');\n"
"x.upload.onprogress=function(e){if(e.lengthComputable){const part=wfFinished+e.loaded;wfProgress(wfTotal?100*part/wfTotal:100);wfText('Uploading '+file.name+' — '+wfByte(e.loaded)+' / '+wfByte(file.size));}};\n"
"x.onload=function(){let d={};try{d=JSON.parse(x.responseText);}catch(e){}if(x.status>=200&&x.status<300&&d.ok)resolve();else{const e=Error(d.error||('HTTP '+x.status));e.status=x.status;reject(e);}};\n"
"x.onerror=function(){reject(Error('Connection lost'));};x.onabort=function(){reject(Error('Cancelled'));};x.send(file);\n"
"}catch(e){reject(e);}});}\n"
"function wfStop(){wfCancel=true;if(wfXhr)wfXhr.abort();}\n"
"wfId('wf-open').onclick=wfOpen;\n"
"wfId('wf-up').onclick=()=>wfLoad(wfParent(wfPath),0);\n"
"wfId('wf-mkdir').onclick=wfMk;\n"
"wfId('wf-prev').onclick=()=>wfLoad(wfPath,wfPage-1);\n"
"wfId('wf-next').onclick=()=>wfLoad(wfPath,wfPage+1);\n"
"wfId('wf-refresh').onclick=()=>wfLoad(wfPath,wfPage);\n"
"wfId('wf-upload').onclick=()=>wfId('wf-file').click();\n"
"wfId('wf-folder').onclick=()=>wfId('wf-folderinput').click();\n"
"wfId('wf-file').onchange=e=>wfQueueFiles(e.target.files);\n"
"wfId('wf-folderinput').onchange=e=>wfQueueFiles(e.target.files);\n"
"wfId('wf-cancel').onclick=wfStop;\n"
"loadSys();\n"
"load();\n"
"buildColorInputs();initModal();\n"
"</script></body></html>\n";

// ---- JSON: every bank ------------------------------------------------------
static int buildBanks(char* o)
{
    int p = 0, i, n = Bank_Count();
    EosLayout lay; int layOk = Desc_Load(&lay);

    // DISPLAY-ONLY heal: correct the in-memory layout so the bank list and free
    // count reflect real occupancy, but NEVER write the descriptor to flash --
    // doing so flips the FPGA to descriptor_valid and routes static 256K banks
    // through the dynamic path, which regressed bank boot. Only an actual
    // ext-bank flash persists a descriptor.
    if (!layOk) { Desc_InitEmpty(&lay); layOk = 1; }
    for (i = 0; i < n; ++i) {
        int slot = httpDescSlot(i);
        if (slot >= 0 && lay.slot[slot].state == EOS_SLOT_FREE && Bank_Occupied(i)) {
            lay.slot[slot].state = EOS_SLOT_NATIVE;
            lay.slot[slot].sizeCode = EOS_SZC_256K;
            lay.slot[slot].physBase = 0;
        }
    }

    p += appS(o + p, "{\"freeSlots\":");
    p += appI(o + p, Desc_FreeSlots(&lay));
    p += appS(o + p, ",\"banks\":[");
    for (i = 0; i < n; ++i) {
        if (i) o[p++] = ',';
        p += appS(o + p, "{\"i\":");    p += appI(o + p, i);
        p += appS(o + p, ",\"name\":\""); p += appJson(o + p, Bank_Name(i));
        p += appS(o + p, "\",\"ef\":");  p += appI(o + p, Bank_Ef(i));
        p += appS(o + p, ",\"occ\":");  p += appI(o + p, Bank_Occupied(i));
        p += appS(o + p, ",\"size\":"); p += appI(o + p, Bank_SizeCode(i));
        p += appS(o + p, ",\"boot\":"); p += appI(o + p, Bank_IsBoot(i));
        p += appS(o + p, ",\"slot\":");
        {
            int ds = httpDescSlot(i);
            p += appI(o + p, (layOk && ds >= 0) ? lay.slot[ds].state : -1);
        }
        p += appS(o + p, ",\"dsize\":");
        {
            int ds = httpDescSlot(i);
            p += appI(o + p, (layOk && ds >= 0) ? lay.slot[ds].sizeCode : -1);
        }
        o[p++] = '}';
    }
    p += appS(o + p, "]}");
    o[p] = 0; return p;
}

// ---- JSON: system info -----------------------------------------------------
static int buildSysInfo(char* o)
{
    int p = 0, i, occ = 0;
    EosLayout lay; int layOk = Desc_Load(&lay);
    EosEeprom ee;
    EosConsole con;
    char macbuf[20];

    // Console (SMC/SMBus + kernel) and EEPROM (kernel decrypted) reads. Neither
    // touches the FPGA flash-command engine, so this is safe while banks serve.
    Console_Read(&con);
    Eeprom_Read(&ee);
    if (ee.macValid) Eeprom_MacStr(&ee, macbuf); else { macbuf[0] = '?'; macbuf[1] = 0; }

    for (i = 0; i < Bank_Count(); ++i)
        if (httpDescSlot(i) >= 0 && Bank_Occupied(i)) ++occ;

    p += appS(o + p, "{\"loader\":\"");
    p += appS(o + p, EOS_LOADER_VERSION);

    // console identity
    p += appS(o + p, "\",\"rev\":\"");
    p += appJson(o + p, con.revStr ? con.revStr : "?");
    p += appS(o + p, "\",\"cpuMhz\":");
    p += appI(o + p, con.cpuMhz);
    p += appS(o + p, ",\"ramMB\":");
    p += appI(o + p, (int)con.ramMB);
    p += appS(o + p, ",\"encoder\":\"");
    p += appJson(o + p, con.encStr ? con.encStr : "?");

    // eeprom identity
    p += appS(o + p, "\",\"serial\":\"");
    p += appJson(o + p, ee.valid ? ee.serial : "?");
    p += appS(o + p, "\",\"mac\":\"");
    p += appJson(o + p, macbuf);
    p += appS(o + p, "\",\"video\":\"");
    p += appJson(o + p, Eeprom_VideoStandardStr(&ee));
    p += appS(o + p, "\",\"region\":\"");
    p += appJson(o + p, Eeprom_GameRegionStr(&ee));
    p += appS(o + p, "\",\"dvd\":\"");
    p += appJson(o + p, Eeprom_DvdRegionStr(&ee));
    p += appS(o + p, "\",\"lang\":\"");
    p += appJson(o + p, Eeprom_LanguageStr(&ee));

    // loader/bank state
    p += appS(o + p, "\",\"usedBanks\":");
    p += appI(o + p, occ);
    p += appS(o + p, ",\"freeSlots\":");
    p += appI(o + p, layOk ? Desc_FreeSlots(&lay) : 4);
    p += appS(o + p, ",\"extReady\":");
    p += appI(o + p, Flash_NewRegionReady());
    // XbDiag Lite presence -- cached probe (primed at boot), NOT a live flash
    // read here. Drives the web UI's XbDiag row + Clear button.
    p += appS(o + p, ",\"xbdiag\":");
    p += appI(o + p, Bank_XbDiagPresent());
    p += appS(o + p, "}");
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
            if (i >= s_reqLen || s_req[i] < '0' || s_req[i]>'9')return -1;
            while (i < s_reqLen && s_req[i] >= '0' && s_req[i] <= '9') {
                int digit = s_req[i] - '0';
                if (v > 214748364 || (v == 214748364 && digit > 7))return -1;
                v = v * 10 + digit; ++i;
            }
            return v;
        }
        while (i < s_reqLen && s_req[i] != '\n') ++i;
        ++i;
    }
    return -1;
}

// ---- custom-theme web helpers (Phase 4) -----------------------------------
static int hx(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// Extract the query value for 'key' into out (minimal %XX / '+' decode). The
// key must sit at the query start or just after '?' or '&'. Returns length.
static int qStr(const char* q, const char* key, char* out, int cap)
{
    int i = 0, kl = 0;
    while (key[kl]) ++kl;
    out[0] = 0;
    while (q[i] && q[i] != ' ' && q[i] != '\r') {
        if (i == 0 || q[i - 1] == '?' || q[i - 1] == '&') {
            int j = 0;
            while (j < kl && q[i + j] == key[j]) ++j;
            if (j == kl && q[i + j] == '=') {
                int k = i + kl + 1, p = 0;
                while (q[k] && q[k] != '&' && q[k] != ' ' && q[k] != '\r' && p < cap - 1) {
                    char c = q[k];
                    if (c == '%' && q[k + 1] && q[k + 2]) { c = (char)((hx(q[k + 1]) << 4) | hx(q[k + 2])); k += 2; }
                    else if (c == '+') c = ' ';
                    out[p++] = c; ++k;
                }
                out[p] = 0;
                return p;
            }
        }
        ++i;
    }
    return 0;
}

// Validate one path component: non-empty, not hidden, no separators / traversal.
static int safeName(const char* s)
{
    int i;
    if (!s || !s[0] || s[0] == '.') return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return 0;
        if ((unsigned char)c < 0x20) return 0;
        if (i >= 63) return 0;
    }
    return 1;
}

// Validate a root-relative FatFs path. Directories may contain spaces/dots,
// but traversal, drive prefixes and backslashes are never accepted.
static int safeSdPath(const char* s)
{
    int i;
    if (!s || s[0] != '/') return 0;
    for (i = 0; s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return 0;
        if (s[i] == '.' && s[i + 1] == '.') return 0;
        if (i >= 158) return 0;
    }
    return 1;
}

static int sdBiosSize(FSIZE_t n)
{
    return n == (FSIZE_t)(256 * 1024) || n == (FSIZE_t)(512 * 1024) || n == (FSIZE_t)(1024 * 1024);
}

// JSON listing for the WebUI. Directories remain navigable, but non-BIOS files
// are intentionally hidden: this is a BIOS manager, not a general SD editor.
static int buildSdJson(char* o, const char* path)
{
    DIR dir; FILINFO fno; FRESULT fr; int p = 0, first = 1, shown = 0;
    if (Sd_Mount() != EOS_SD_OK) return -1;
    fr = f_opendir(&dir, (path[0] == '/' && path[1] == 0) ? "/" : path);
    if (fr != FR_OK) return -2;
    p += appS(o + p, "{\"ok\":true,\"entries\":[");
    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.') continue;
        if (!(fno.fattrib & AM_DIR) && !sdBiosSize(fno.fsize)) continue;
        if (shown++ >= 48 || p > HTTP_JSON_MAX - 256) break;
        if (!first) o[p++] = ','; first = 0;
        p += appS(o + p, "{\"n\":\""); p += appJson(o + p, fno.fname);
        p += appS(o + p, "\",\"d\":"); p += appI(o + p, (fno.fattrib & AM_DIR) ? 1 : 0);
        p += appS(o + p, ",\"s\":"); p += appI(o + p, (int)fno.fsize);
        o[p++] = '}';
    }
    f_closedir(&dir);
    p += appS(o + p, "]}");
    return p;
}

static int themeLocSd(void)
{
    return s_tLoc[0] == 's' || s_tLoc[0] == 'S';
}

// Build HDD or virtual SD theme path. File_* understands the SD:\ prefix.
static void themePath(char* out, int cap, const char* folder, const char* leaf, int sd)
{
    const char* pre = sd ? "SD:\\Eos\\Themes\\" : "E:\\Eos\\Themes\\";
    int p = 0, i = 0;
    while (pre[i] && p < cap - 1) out[p++] = pre[i++];
    for (i = 0; folder[i] && p < cap - 1; ++i) out[p++] = folder[i];
    if (leaf) {
        if (p < cap - 1) out[p++] = '\\';
        for (i = 0; leaf[i] && p < cap - 1; ++i) out[p++] = leaf[i];
    }
    out[p] = 0;
}

// Native FatFs path for SD theme writes/deletes.
static void sdThemePath(char* out, int cap, const char* folder, const char* leaf)
{
    const char* pre = "/Eos/Themes/";
    int p = 0, i = 0;
    while (pre[i] && p < cap - 1) out[p++] = pre[i++];
    for (i = 0; folder[i] && p < cap - 1; ++i) out[p++] = folder[i];
    if (leaf) {
        if (p < cap - 1) out[p++] = '/';
        for (i = 0; leaf[i] && p < cap - 1; ++i) out[p++] = leaf[i];
    }
    out[p] = 0;
}

static int sdEnsureThemeDir(const char* folder)
{
    char dir[160];
    FRESULT fr;
    if (Sd_Mount() != EOS_SD_OK) return 0;
    fr = f_mkdir("/Eos"); if (fr != FR_OK && fr != FR_EXIST) return 0;
    fr = f_mkdir("/Eos/Themes"); if (fr != FR_OK && fr != FR_EXIST) return 0;
    sdThemePath(dir, sizeof(dir), folder, 0);
    fr = f_mkdir(dir); if (fr != FR_OK && fr != FR_EXIST) return 0;
    return 1;
}

// {"themes":["HDD|Name","SD|Name",...]} -- both theme roots.
static int buildThemesJson(char* o)
{
    static EosFileEntry ents[64];
    char ini[256];
    int  p = 0, n, i, first = 1;
    n = File_ListDir("E:\\Eos\\Themes", ents, 64);
    p += appS(o + p, "{\"themes\":[");
    for (i = 0; i < n; ++i) {
        if (!ents[i].is_dir) continue;
        themePath(ini, sizeof(ini), ents[i].name, "theme.ini", 0);
        if (!File_Exists(ini)) continue;
        if (!first) o[p++] = ',';
        first = 0;
        p += appS(o + p, "\"HDD|"); p += appJson(o + p, ents[i].name); o[p++] = '"';
    }
    n = File_ListDir("SD:\\Eos\\Themes", ents, 64);
    for (i = 0; i < n && p < HTTP_JSON_MAX - 128; ++i) {
        if (!ents[i].is_dir) continue;
        themePath(ini, sizeof(ini), ents[i].name, "theme.ini", 1);
        if (!File_Exists(ini)) continue;
        if (!first) o[p++] = ',';
        first = 0;
        p += appS(o + p, "\"SD|"); p += appJson(o + p, ents[i].name); o[p++] = '"';
    }
    p += appS(o + p, "]}");
    return p;
}

// Delete every file in a theme folder (flat), then the folder.
static int deleteThemeFolder(const char* folder, int sd)
{
    static EosFileEntry ents[64];
    char dir[256], fp[256];
    int  n, i;
    if (sd) {
        DIR d; FILINFO fno; FRESULT fr;
        if (Sd_Mount() != EOS_SD_OK) return 0;
        sdThemePath(dir, sizeof(dir), folder, 0);
        fr = f_opendir(&d, dir);
        if (fr == FR_OK) {
            for (;;) {
                fr = f_readdir(&d, &fno);
                if (fr != FR_OK || fno.fname[0] == 0) break;
                if (fno.fattrib & AM_DIR) continue;
                sdThemePath(fp, sizeof(fp), folder, fno.fname);
                f_unlink(fp);
            }
            f_closedir(&d);
        }
        return f_unlink(dir) == FR_OK;
    }
    themePath(dir, sizeof(dir), folder, 0, 0);
    n = File_ListDir(dir, ents, 64);
    for (i = 0; i < n; ++i) {
        if (ents[i].is_dir) continue;
        themePath(fp, sizeof(fp), folder, ents[i].name, 0);
        DeleteFileA(fp);
    }
    return RemoveDirectoryA(dir) ? 1 : 0;
}
// File-manager request routing deliberately requires an exact endpoint name.
// Existing bank/BIOS route matching remains untouched.
static int matchRoute(const char* path, const char* name)
{
    int i = 0; while (name[i]) { if (path[i] != name[i])return 0; ++i; }
    return path[i] == 0 || path[i] == '?' || path[i] == ' ' || path[i] == '\r';
}
static int filesTokenOk(void)
{
    const char* k = "x-eos-files-key:"; int kl = aLen(k);
    for (int i = 0; i < s_reqLen;) {
        int end = i; while (end < s_reqLen && s_req[end] != '\n')++end;
        if (end <= i + 2)break;
        int matched = 1;
        for (int x = 0; x < kl; ++x)if (i + x >= end || lc(s_req[i + x]) != k[x]) { matched = 0; break; }
        if (matched) {
            int pos = i + kl; while (pos < end && (s_req[pos] == ' ' || s_req[pos] == '\t'))++pos;
            for (int x = 0; x < 16; ++x)if (pos + x >= end || s_req[pos + x] != s_filesToken[x])return 0;
            return pos + 16 >= end || s_req[pos + 16] == '\r' || s_req[pos + 16] == '\n';
        }
        i = end + 1;
    }
    return 0;
}
// Unlike the legacy theme query parser, this fails closed on truncation,
// malformed percent escapes and encoded NUL bytes. Never act on a truncated path.
static int fileParam(const char* request, const char* key, char* out, int cap)
{
    int kl = aLen(key); out[0] = 0;
    const char* q = request; while (*q && *q != '?' && *q != ' ' && *q != '\r')++q;
    if (*q == '?')++q; else return 1; // absent (caller can default to '/')
    while (*q && *q != ' ' && *q != '\r') {
        const char* start = q; const char* eq = q;
        while (*eq && *eq != '=' && *eq != '&' && *eq != ' ' && *eq != '\r')++eq;
        int match = (eq - start == kl && *eq == '=');
        for (int i = 0; i < kl && match; i++)if (start[i] != key[i])match = 0;
        if (match) {
            const char* p = eq + 1; int n = 0;
            while (*p && *p != '&' && *p != ' ' && *p != '\r') {
                unsigned char c = (unsigned char)*p++;
                if (c == '%') {
                    int hi = -1, lo = -1;
                    if (*p) { char x = *p++; hi = (x >= '0' && x <= '9') ? x - '0' : (x >= 'a' && x <= 'f') ? x - 'a' + 10 : (x >= 'A' && x <= 'F') ? x - 'A' + 10 : -1; }
                    if (*p) { char x = *p++; lo = (x >= '0' && x <= '9') ? x - '0' : (x >= 'a' && x <= 'f') ? x - 'a' + 10 : (x >= 'A' && x <= 'F') ? x - 'A' + 10 : -1; }
                    if (hi < 0 || lo < 0)return 0; c = (unsigned char)((hi << 4) | lo);
                }
                else if (c == '+')c = ' ';
                if (c < 32 || c == 127 || n >= cap - 1)return 0;
                out[n++] = (char)c;
            }
            out[n] = 0; return 1;
        }
        q = eq; while (*q && *q != '&' && *q != ' ' && *q != '\r')++q;
        if (*q == '&')++q;
    }
    return 1;
}
static int filesInt(const char* q, const char* key);
static void parseReq(void)
{
    int i = 0; const char* path;
    s_method = M_GET; s_route = R_NONE; s_bank = -1; s_clen = -1;

    if (strEqN(s_req, "POST ", 5)) { s_method = M_POST; i = 5; }
    else if (strEqN(s_req, "GET ", 4)) { s_method = M_GET; i = 4; }
    else return;

    path = s_req + i;
    if (matchRoute(path, "/api/files/list")) s_route = R_FLIST;
    else if (matchRoute(path, "/api/files/session")) s_route = R_FSESSION;
    else if (matchRoute(path, "/api/files/mkdir")) s_route = R_FMKDIR;
    else if (matchRoute(path, "/api/files/delete")) s_route = R_FDELETE;
    else if (matchRoute(path, "/api/files/rename")) s_route = R_FRENAME;
    else if (matchRoute(path, "/api/files/upload")) s_route = R_FUPLOAD;
    else if (matchRoute(path, "/api/files/download")) s_route = R_FDOWNLOAD;
    else if (matchRoute(path, "/api/files/job")) s_route = R_FJOB;
    else if (strEqN(path, "/api/banks", 10)) s_route = R_BANKS;
    else if (strEqN(path, "/api/rename", 11)) s_route = R_RENAME;
    else if (strEqN(path, "/api/delete", 11)) s_route = R_DELETE;
    else if (strEqN(path, "/api/flash", 10)) s_route = R_FLASH;
    else if (strEqN(path, "/api/launch", 11)) s_route = R_LAUNCH;
    else if (strEqN(path, "/api/eeprom", 11)) s_route = R_EEPROM;
    else if (strEqN(path, "/api/reset", 10))  s_route = R_RESET;
    else if (strEqN(path, "/api/sysinfo", 12)) s_route = R_SYSINFO;
    else if (strEqN(path, "/api/clrxbdiag", 14)) s_route = R_CLRXBDIAG;
    else if (strEqN(path, "/api/themes", 11)) s_route = R_THEMES;
    else if (strEqN(path, "/api/theme/ini", 14)) s_route = R_TINI;
    else if (strEqN(path, "/api/theme/file", 15)) s_route = R_TFILE;
    else if (strEqN(path, "/api/theme/del", 14)) s_route = R_TDEL;
    else if (strEqN(path, "/api/setcolor", 13)) s_route = R_SETCOLOR;
    else if (strEqN(path, "/api/sd/list", 12)) s_route = R_SDLIST;
    else if (strEqN(path, "/api/sd/delete", 14)) s_route = R_SDDEL;
    else if (strEqN(path, "/api/sd/upload", 14)) s_route = R_SDUP;
    else if (strEqN(path, "/logo.bmp", 9)) s_route = R_LOGO;
    else if (path[0] == '/' && (path[1] == ' ' || path[1] == '?')) s_route = R_PAGE;

    s_bank = qBank(path);
    if (s_route == R_THEMES || s_route == R_TINI || s_route == R_TFILE || s_route == R_TDEL) {
        qStr(path, "folder", s_tFolder, sizeof(s_tFolder));
        qStr(path, "name", s_tName, sizeof(s_tName));
        qStr(path, "loc", s_tLoc, sizeof(s_tLoc));
    }
    if (s_route == R_SETCOLOR) {
        qStr(path, "c", s_colorStr, sizeof(s_colorStr));   // hex RRGGBB (no '#')
    }
    if (s_route == R_SDLIST || s_route == R_SDDEL || s_route == R_SDUP) {
        qStr(path, "path", s_sdPath, sizeof(s_sdPath));
    }
    s_filesParseError = 0;
    if (s_route >= R_FLIST && s_route <= R_FJOB) {
        if (!fileParam(path, "path", s_filesPath, sizeof(s_filesPath)))s_filesParseError = 1;
        if (!s_filesPath[0]) { s_filesPath[0] = '/'; s_filesPath[1] = 0; }
        if (!fileParam(path, "name", s_filesNew, sizeof(s_filesNew)))s_filesParseError = 1;
        s_filesPage = filesInt(path, "page");
        s_filesOverwrite = filesInt(path, "overwrite") == 1;
    }
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

static int filesInt(const char* q, const char* key)
{
    char tmp[14]; qStr(q, key, tmp, sizeof(tmp)); int val = 0;
    for (int i = 0; tmp[i]; ++i) {
        if (tmp[i] < '0' || tmp[i]>'9')return -1;
        if (val > 100000)return -1; val = val * 10 + (tmp[i] - '0');
    }
    return val;
}
static int fileStatus(int r)
{
    if (r == WF_BADPATH)return 400;
    if (r == WF_DENIED)return 403;
    if (r == WF_NOTFOUND)return 404;
    if (r == WF_STALE)return 423;
    if (r == WF_EXISTS || r == WF_BUSY)return 409;
    if (r == WF_UNAVAILABLE)return 503;
    if (r == WF_DEPTH)return 422;
    return 500;
}
static void fileAnswer(int r)
{
    if (r == WF_OK) { respond("200 OK", "application/json", "{\"ok\":true}", 11); return; }
    int p = 0; const char* err = Wf_Error(r);
    p += appS(s_json + p, "{\"ok\":false,\"error\":\""); p += appJson(s_json + p, err); p += appS(s_json + p, "\"}");
    const char* status = "500 Internal Server Error"; int code = fileStatus(r);
    if (code == 400)status = "400 Bad Request"; else if (code == 403)status = "403 Forbidden";
    else if (code == 404)status = "404 Not Found"; else if (code == 409)status = "409 Conflict";
    else if (code == 422)status = "422 Unprocessable Entity"; else if (code == 423)status = "423 Locked";
    else if (code == 503)status = "503 Service Unavailable";
    respond(status, "application/json", s_json, p);
}
static void sendFileHeader(unsigned long long length, const char* name)
{
    int p = 0; char num[24]; int j = 0; unsigned long long v = length;
    if (!v)num[j++] = '0'; while (v && j < 23) { num[j++] = (char)('0' + v % 10); v /= 10; }
    p += appS(s_resp + p, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=\"");
    p += appS(s_resp + p, name); p += appS(s_resp + p, "\"\r\nContent-Length: ");
    while (j > 0)s_resp[p++] = num[--j];
    p += appS(s_resp + p, "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    s_txH = s_resp; s_txHLen = p; s_txHOff = 0;
    s_fileHave = 0; s_fileAt = 0; s_fileTotal = length; s_fileSent = 0; s_state = ST_FILE_SEND;
}

// ---- act on a fully-received request --------------------------------------
static void process(void)
{
    int n = Bank_Count();

    // Phase 5 file manager: only read endpoints accept GET; every mutation
    // requires POST and the session-scoped header, including empty-body POSTs.
    if (s_route >= R_FLIST && s_route <= R_FJOB) {
        if (s_filesParseError) { fileAnswer(WF_BADPATH); return; }
        int write = (s_route == R_FMKDIR || s_route == R_FDELETE || s_route == R_FRENAME || s_route == R_FUPLOAD);
        if ((write && s_method != M_POST) || (!write && s_method != M_GET)) {
            respondText("405 Method Not Allowed", "bad method"); return;
        }
        if (write && !filesTokenOk()) { respondText("403 Forbidden", "file session required"); return; }
        if (s_route == R_FSESSION) {
            int p = 0; p += appS(s_json + p, "{\"key\":\""); p += appS(s_json + p, s_filesToken); p += appS(s_json + p, "\"}");
            respond("200 OK", "application/json", s_json, p); return;
        }
        if (s_route == R_FLIST) {
            int r = Wf_List(s_filesPath, s_filesPage, s_json, HTTP_JSON_MAX);
            if (r < 0)fileAnswer(r); else respond("200 OK", "application/json", s_json, r); return;
        }
        if (s_route == R_FJOB) {
            int r = Wf_JobInfo(s_json, HTTP_JSON_MAX);
            if (r < 0)fileAnswer(r); else respond("200 OK", "application/json", s_json, r); return;
        }
        if (s_route == R_FMKDIR) { fileAnswer(Wf_Mkdir(s_filesPath)); return; }
        if (s_route == R_FRENAME) { fileAnswer(Wf_Rename(s_filesPath, s_filesNew)); return; }
        if (s_route == R_FDELETE) {
            int r = Wf_Delete(s_filesPath);
            if (r == 1) {
                int p = Wf_JobInfo(s_json, HTTP_JSON_MAX);
                respond("202 Accepted", "application/json", s_json, p); return;
            }
            fileAnswer(r); return;
        }
        if (s_route == R_FUPLOAD) {
            if (s_err) { fileAnswer(s_err == 403 ? WF_DENIED : s_err == 409 ? WF_EXISTS : s_err == 400 ? WF_BADPATH : WF_IOERROR); return; }
            if (s_clen < 0 || s_rxRecv != s_clen) { Wf_UploadAbort(); fileAnswer(WF_INCOMPLETE); return; }
            fileAnswer(Wf_UploadFinish()); return;
        }
        if (s_route == R_FDOWNLOAD) {
            unsigned long long length = 0; char name[80];
            int r = Wf_DownloadOpen(s_filesPath, &length, name, sizeof(name));
            if (r < 0) { fileAnswer(r); return; }sendFileHeader(length, name); return;
        }
    }
    if (s_route == R_PAGE) { respond("200 OK", "text/html", k_page, aLen(k_page)); return; }
    if (s_route == R_BANKS) { int len = buildBanks(s_json); respond("200 OK", "application/json", s_json, len); return; }
    if (s_route == R_SYSINFO) { int len = buildSysInfo(s_json); respond("200 OK", "application/json", s_json, len); return; }
    if (s_route == R_CLRXBDIAG) {
        int i, xd = -1;
        for (i = 0; i < Bank_Count(); ++i) if ((Bank_Ef(i) & 0x0F) == 0x0D) { xd = i; break; }
        if (xd < 0) { respondText("404 Not Found", "no XbDiag bank"); return; }
        if (Flash_EraseBank(0x0D) != EOS_FLASH_OK) { respondText("500 Error", "erase failed"); return; }
        Bank_ClearEntry(xd); Config_Save();
        respondText("200 OK", "XbDiag cleared"); return;
    }
    if (s_route == R_LOGO) { int len = buildLogoBmp(s_rx); respond("200 OK", "image/bmp", (const char*)s_rx, len); return; }

    // ---- SD BIOS manager ---------------------------------------------------
    if (s_route == R_SDLIST) {
        int len;
        if (!safeSdPath(s_sdPath)) { respondText("400 Bad Request", "bad SD path"); return; }
        len = buildSdJson(s_json, s_sdPath);
        if (len == -1) { respondText("503 Unavailable", "SD card not mounted"); return; }
        if (len == -2) { respondText("404 Not Found", "directory not found"); return; }
        respond("200 OK", "application/json", s_json, len); return;
    }
    if (s_route == R_SDDEL) {
        FILINFO fi; FRESULT fr;
        if (!safeSdPath(s_sdPath)) { respondText("400 Bad Request", "bad SD path"); return; }
        if (Sd_Mount() != EOS_SD_OK) { respondText("503 Unavailable", "SD card not mounted"); return; }
        fr = f_stat(s_sdPath, &fi);
        if (fr != FR_OK) { respondText("404 Not Found", "file not found"); return; }
        if ((fi.fattrib & AM_DIR) || !sdBiosSize(fi.fsize)) { respondText("403 Forbidden", "not a BIOS file"); return; }
        fr = f_unlink(s_sdPath);
        if (fr != FR_OK) { respondText("500 Error", "delete failed"); return; }
        respondText("200 OK", "deleted"); return;
    }
    if (s_route == R_SDUP) {
        FIL fp; FRESULT fr; UINT bw = 0;
        if (!safeSdPath(s_sdPath)) { respondText("400 Bad Request", "bad SD path"); return; }
        if (s_err == 413) { respondText("413 Too Large", "BIOS exceeds 1MB"); return; }
        if (!sdBiosSize((FSIZE_t)s_clen) || s_rxStore != s_clen) {
            respondText("400 Bad Request", "BIOS must be exactly 256K, 512K, or 1MB"); return;
        }
        if (Sd_Mount() != EOS_SD_OK) { respondText("503 Unavailable", "SD card not mounted"); return; }
        fr = f_open(&fp, s_sdPath, FA_WRITE | FA_CREATE_ALWAYS);
        if (fr != FR_OK) { respondText("500 Error", "open failed"); return; }
        // Reserve one contiguous run up front so the existing raw-LBA launcher
        // can precache the uploaded BIOS without fragmentation stitching.
        fr = f_expand(&fp, (FSIZE_t)s_clen, 1);
        if (fr == FR_OK) fr = f_lseek(&fp, 0);
        if (fr == FR_OK) fr = f_write(&fp, s_rx, (UINT)s_clen, &bw);
        if (fr == FR_OK && bw == (UINT)s_clen) fr = f_sync(&fp);
        f_close(&fp);
        if (fr != FR_OK || bw != (UINT)s_clen) {
            f_unlink(s_sdPath);
            respondText("500 Error", "SD write failed or no contiguous space"); return;
        }
        respondText("200 OK", "uploaded"); return;
    }

    // ---- custom-theme web tools (Phase 4) ----
    if (s_route == R_THEMES) { int len = buildThemesJson(s_json); respond("200 OK", "application/json", s_json, len); return; }
    if (s_route == R_TINI) {
        char fp[256];
        if (!safeName(s_tFolder)) { respondText("400 Bad Request", "bad folder"); return; }
        if (s_method == M_GET) {
            int rd;
            themePath(fp, sizeof(fp), s_tFolder, "theme.ini", themeLocSd());
            rd = File_ReadInto(fp, s_rx, HTTP_RX_MAX - 1);
            if (rd < 0) { respondText("404 Not Found", "no theme.ini"); return; }
            respond("200 OK", "text/plain", (const char*)s_rx, rd);
            return;
        }
        {   // POST: write the body as theme.ini (folder auto-created)
            if (themeLocSd()) {
                FIL f; FRESULT fr; UINT bw = 0; int opened = 0;
                if (!sdEnsureThemeDir(s_tFolder)) { respondText("503 Unavailable", "SD card unavailable"); return; }
                sdThemePath(fp, sizeof(fp), s_tFolder, "theme.ini");
                fr = f_open(&f, fp, FA_WRITE | FA_CREATE_ALWAYS);
                if (fr == FR_OK) opened = 1;
                if (fr == FR_OK) fr = f_write(&f, s_rx, (UINT)s_rxStore, &bw);
                if (fr == FR_OK) fr = f_sync(&f);
                if (opened) f_close(&f);
                if (fr != FR_OK || bw != (UINT)s_rxStore) { respondText("500 Error", "SD write failed"); return; }
            }
            else {
                char dir[256]; HANDLE h; DWORD wr;
                themePath(dir, sizeof(dir), s_tFolder, 0, 0);
                CreateDirectoryA("E:\\Eos", NULL);
                CreateDirectoryA("E:\\Eos\\Themes", NULL);
                CreateDirectoryA(dir, NULL);
                themePath(fp, sizeof(fp), s_tFolder, "theme.ini", 0);
                h = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE) { respondText("500 Error", "open failed"); return; }
                WriteFile(h, s_rx, (DWORD)s_rxStore, &wr, NULL);
                CloseHandle(h);
            }
            respondText("200 OK", "ini saved");
            return;
        }
    }
    if (s_route == R_TFILE) {
        if (s_upFile != INVALID_HANDLE_VALUE) { CloseHandle(s_upFile); s_upFile = INVALID_HANDLE_VALUE; }
        if (s_sdUpOpen) {
            if (f_sync(&s_sdUpFile) != FR_OK) s_err = 500;
            f_close(&s_sdUpFile); s_sdUpOpen = 0;
        }
        if (s_err) { respondText("500 Error", "upload failed"); return; }
        respondText("200 OK", "file saved");
        return;
    }
    if (s_route == R_TDEL) {
        if (!safeName(s_tFolder)) { respondText("400 Bad Request", "bad folder"); return; }
        deleteThemeFolder(s_tFolder, themeLocSd());
        respondText("200 OK", "theme deleted");
        return;
    }


    if (s_route == R_RENAME) {
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        s_name[s_rxStore] = 0;
        if (s_name[0]) { Bank_SetName(s_bank, s_name); Config_Save(); }
        respondText("200 OK", "ok"); return;
    }
    if (s_route == R_SETCOLOR) {
        // /api/setcolor?b=N&c=RRGGBB  -> set the user bank's LED color.
        unsigned int rgb; int i; char ch;
        // validate it's a user bank; Desc_SetColor takes a BANK INDEX and does the
        // slot conversion internally, so pass s_bank (not a pre-converted slot).
        if (s_bank < 0 || s_bank >= n || httpDescSlot(s_bank) < 0) { respondText("400 Bad Request", "bad bank"); return; }
        // parse 6 hex digits (RRGGBB) -> 0x00RRGGBB
        rgb = 0;
        for (i = 0; i < 6; ++i) {
            ch = s_colorStr[i];
            if (ch >= '0' && ch <= '9')      rgb = (rgb << 4) | (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') rgb = (rgb << 4) | (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') rgb = (rgb << 4) | (unsigned)(ch - 'A' + 10);
            else { respondText("400 Bad Request", "bad color"); return; }
        }
        Desc_SetColor(s_bank, rgb & 0xFFFFFFu);  // bank index; slot mapping is internal
        respondText("200 OK", "ok"); return;
    }
    if (s_route == R_DELETE) {
        int dslot = httpDescSlot(s_bank);
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank) || !Bank_Occupied(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }

        // Large bank (anchor): erase its new-region blocks + clear descriptor.
        // Otherwise a normal 256K bank in the default range -- erase as before.
        if (dslot >= 0 && Desc_Load(&g_lay) && g_lay.valid &&
            g_lay.slot[dslot].state == EOS_SLOT_ANCHOR) {
            int span = Desc_SlotsFor(g_lay.slot[dslot].sizeCode);
            unsigned int base = g_lay.slot[dslot].physBase;
            int nblk = (span == 4) ? 16 : 8;
            int bk, j;
            if (base >= EOS_NEWRGN_BASE && base < (EOS_NEWRGN_BASE + 0x100000)) {
                int firstBlk = (int)((base - EOS_NEWRGN_BASE) / 0x10000);
                for (bk = 0; bk < nblk && (firstBlk + bk) < 16; ++bk)
                    if (Flash_EraseBlock(EOS_BANK_NEWREGION, firstBlk + bk) != EOS_FLASH_OK) { respondText("500 Error", "erase failed"); return; }
            }
            for (j = 0; j < span && (dslot + j) < EOS_DESC_SLOTS; ++j) {
                int tbl;
                g_lay.slot[dslot + j].state = EOS_SLOT_FREE;
                g_lay.slot[dslot + j].sizeCode = EOS_SZC_256K;
                g_lay.slot[dslot + j].physBase = 0;
                tbl = Bank_IndexForEf((unsigned char)(0x3 + dslot + j));
                if (tbl >= 0) Bank_ClearEntry(tbl);
            }
            Desc_Save(&g_lay); Config_Save();
            respondText("200 OK", "ok"); return;
        }

        if (Flash_EraseBank(Bank_Ef(s_bank)) == EOS_FLASH_OK) {
            if (dslot >= 0 && Desc_Load(&g_lay) && g_lay.valid && g_lay.slot[dslot].state == EOS_SLOT_NATIVE) {
                g_lay.slot[dslot].state = EOS_SLOT_FREE; g_lay.slot[dslot].sizeCode = EOS_SZC_256K; g_lay.slot[dslot].physBase = 0;
                Desc_Save(&g_lay);
            }
            Bank_ClearEntry(s_bank); Config_Save();
            respondText("200 OK", "ok");
        }
        else respondText("500 Error", "erase failed");
        return;
    }
    if (s_route == R_FLASH) {
        int sc;
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        if (s_err == 413) { respondText("413 Too Large", "image exceeds 1MB budget"); return; }
        if (s_clen <= 0) { respondText("400 Bad Request", "no image"); return; }
        sc = (s_clen <= 256 * 1024) ? EOS_BANK_SIZE_256K : (s_clen <= 512 * 1024) ? EOS_BANK_SIZE_512K : EOS_BANK_SIZE_1MB;

        if (sc == EOS_BANK_SIZE_256K) {
            // 256K: DEFAULT range, exactly as before. No descriptor.
            int dslot = httpDescSlot(s_bank);
            if (dslot >= 0 && Desc_Load(&g_lay) && g_lay.valid &&
                (g_lay.slot[dslot].state == EOS_SLOT_SHADOW || g_lay.slot[dslot].state == EOS_SLOT_ANCHOR)) {
                respondText("409 Conflict", "bank used by an oversized BIOS - delete it first"); return;
            }
            if (Flash_WriteImage(Bank_Ef(s_bank), s_rx, s_clen) != EOS_FLASH_OK) { respondText("500 Error", "flash failed"); return; }
            Bank_SetOccupied(s_bank, 1, sc);
            // Record NATIVE in the descriptor so auto-place won't overwrite it.
            if (dslot >= 0) {
                if (!Desc_Load(&g_lay) || !g_lay.valid) Desc_InitEmpty(&g_lay);
                g_lay.slot[dslot].state = EOS_SLOT_NATIVE;
                g_lay.slot[dslot].sizeCode = EOS_SZC_256K;
                g_lay.slot[dslot].physBase = 0;
                Desc_Save(&g_lay);
            }
            Config_Save();
            respondText("200 OK", "ok"); return;
        }

        // large BIOS -> new region, auto-placed into a free half
        {
            int szc = (sc == EOS_BANK_SIZE_1MB) ? EOS_SZC_1MB : EOS_SZC_512K;
            int need = Desc_SlotsFor(szc);
            int slot = -1, cand, allFree;
            unsigned int nrbase;
            int startPage, j;
            if (httpDescSlot(s_bank) < 0) { respondText("400 Bad Request", "not a user slot"); return; }
            if (!Desc_Load(&g_lay) || !g_lay.valid) Desc_InitEmpty(&g_lay);

            // auto-place: 1MB needs all 4 free; 512K takes the first free even pair
            if (szc == EOS_SZC_1MB) {
                allFree = 1;
                for (j = 0; j < EOS_DESC_SLOTS; ++j)
                    if (g_lay.slot[j].state != EOS_SLOT_FREE) { allFree = 0; break; }
                if (allFree) slot = 0;
            }
            else {
                for (cand = 0; cand <= 2; cand += 2)
                    if (g_lay.slot[cand].state == EOS_SLOT_FREE && g_lay.slot[cand + 1].state == EOS_SLOT_FREE) { slot = cand; break; }
            }
            if (slot < 0) { respondText("409 Conflict", (szc == EOS_SZC_1MB) ? "need all banks free" : "no free pair - free some banks"); return; }

            nrbase = (szc == EOS_SZC_1MB) ? EOS_NEWRGN_BASE
                : (slot >= 2) ? (EOS_NEWRGN_BASE + EOS_NEWRGN_HALF)
                : EOS_NEWRGN_BASE;
            startPage = (int)((nrbase - EOS_NEWRGN_BASE) / 256);
            if (Flash_WriteImageAtNoSync(EOS_BANK_NEWREGION, startPage, s_rx, s_clen) != EOS_FLASH_OK) { respondText("500 Error", "flash failed (new region)"); return; }
            Flash_SyncNewRegion();   // page into SDRAM so it's launchable now
            g_lay.slot[slot].state = EOS_SLOT_ANCHOR;
            g_lay.slot[slot].sizeCode = (unsigned char)szc;
            g_lay.slot[slot].physBase = nrbase;
            for (j = 1; j < need; ++j) {
                g_lay.slot[slot + j].state = EOS_SLOT_SHADOW; g_lay.slot[slot + j].sizeCode = EOS_SZC_256K; g_lay.slot[slot + j].physBase = 0;
            }
            if (Desc_Save(&g_lay) != EOS_FLASH_OK) { respondText("500 Error", "descriptor write failed"); return; }
            {
                int anchorTbl = Bank_IndexForEf((unsigned char)(0x3 + slot));
                if (anchorTbl >= 0) Bank_SetOccupied(anchorTbl, 1, sc);
            }
            Config_Save();
            respondText("200 OK", "ok"); return;
        }
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
    Wf_UploadAbort(); Wf_DownloadClose();
    if (s_upFile != INVALID_HANDLE_VALUE) { CloseHandle(s_upFile); s_upFile = INVALID_HANDLE_VALUE; }
    if (s_sdUpOpen) { f_close(&s_sdUpFile); s_sdUpOpen = 0; }
    if (s_conn != INVALID_SOCKET) { closesocket(s_conn); s_conn = INVALID_SOCKET; }
    s_state = ST_IDLE; s_launch = -1;
}

// route a POST body to the right buffer; called once headers are parsed
static void beginBody(void)
{
    s_rxRecv = 0; s_rxStore = 0; s_store = 1; s_err = 0;
    if (s_route == R_FUPLOAD) {
        if (s_filesParseError) { s_err = 400; s_store = 0; return; }
        if (!filesTokenOk()) { s_err = 403; s_store = 0; return; }
        if (s_clen < 0) { s_err = 400; s_store = 0; return; }
        int result = Wf_UploadBegin(s_filesPath, s_clen, s_filesOverwrite);
        if (result != WF_OK) { s_err = (result == WF_EXISTS ? 409 : (result == WF_BADPATH ? 400 : (result == WF_DENIED ? 403 : 500))); s_store = 0; }
        return;
    }
    if (s_route == R_FLASH || s_route == R_SDUP) {
        // Both flash and SD BIOS uploads share the existing 1MB receive buffer.
        int cap = HTTP_RX_MAX;
        if (s_clen > cap) { s_err = 413; s_store = 0; }   // drain then 413
    }
    else if (s_route == R_EEPROM) {
        if (s_clen != EOS_EEPROM_SIZE) { s_err = 400; s_store = 0; }   // EEPROM image is exactly 256B
    }
    else if (s_route == R_RENAME) {
        // cap to the name buffer; extra drained
    }
    else if (s_route == R_TINI) {
        // POST theme.ini text -> buffer into s_rx (tiny)
    }
    else if (s_route == R_TFILE) {
        // stream the upload straight to disk (mp3 far exceeds s_rx)
        if (s_upFile != INVALID_HANDLE_VALUE) { CloseHandle(s_upFile); s_upFile = INVALID_HANDLE_VALUE; }
        if (s_sdUpOpen) { f_close(&s_sdUpFile); s_sdUpOpen = 0; }
        if (!safeName(s_tFolder) || !safeName(s_tName)) { s_err = 400; s_store = 0; }
        else if (themeLocSd()) {
            char fp[256]; FRESULT fr;
            if (!sdEnsureThemeDir(s_tFolder)) { s_err = 503; s_store = 0; }
            else {
                sdThemePath(fp, sizeof(fp), s_tFolder, s_tName);
                fr = f_open(&s_sdUpFile, fp, FA_WRITE | FA_CREATE_ALWAYS);
                if (fr != FR_OK) { s_err = 500; s_store = 0; }
                else s_sdUpOpen = 1;
            }
        }
        else {
            char dir[256], fp[256];
            themePath(dir, sizeof(dir), s_tFolder, 0, 0);
            CreateDirectoryA("E:\\Eos", NULL);
            CreateDirectoryA("E:\\Eos\\Themes", NULL);
            CreateDirectoryA(dir, NULL);
            themePath(fp, sizeof(fp), s_tFolder, s_tName, 0);
            s_upFile = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_upFile == INVALID_HANDLE_VALUE) { s_err = 500; s_store = 0; }
        }
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
    if (s_route == R_FUPLOAD) {
        int remaining = s_clen - s_rxRecv;
        int use = len < remaining ? len : remaining;
        if (use > 0) { int r = Wf_UploadWrite(src, use); if (r != WF_OK) { s_err = 500; s_store = 0; } }
        s_rxRecv += len; return;
    }
    if (s_route == R_FLASH || s_route == R_EEPROM || s_route == R_SDUP) {
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
    else if (s_route == R_TINI) {
        cap = HTTP_RX_MAX;
        room = cap - s_rxStore; if (room > len) room = len;
        for (i = 0; i < room; ++i) s_rx[s_rxStore + i] = (unsigned char)src[i];
        s_rxStore += room;
    }
    else if (s_route == R_TFILE) {
        if (s_sdUpOpen) {
            UINT wr = 0;
            FRESULT fr = f_write(&s_sdUpFile, src, (UINT)len, &wr);
            if (fr != FR_OK || wr != (UINT)len) { s_err = 500; s_store = 0; }
            else s_rxStore += len;
        }
        else if (s_upFile != INVALID_HANDLE_VALUE) {
            DWORD wr; WriteFile(s_upFile, src, (DWORD)len, &wr, NULL);
            if (wr != (DWORD)len) { s_err = 500; s_store = 0; }
            else s_rxStore += len;
        }
    }
    s_rxRecv += len;
}

void Http_Poll(void)
{
    char buf[2048];
    int  moved = 0, r;

    if (!s_up) return;
    Wf_Tick(); // at most eight HDD/SD entries per frame during folder deletion

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
            if (s_method == M_POST && s_route == R_FUPLOAD) {
                int after = s_reqLen - he; beginBody();
                // Reject inaccessible destinations without consuming large bodies.
                if (s_err) { process(); break; }
                if (after > 0)stashBody(s_req + he, after);
                if (s_err || s_rxRecv >= s_clen)process(); else s_state = ST_BODY;
            }
            else if (s_method == M_POST && s_clen > 0) {
                int after = s_reqLen - he;
                beginBody();
                if (after > 0) stashBody(s_req + he, after);
                if (s_rxRecv >= s_clen) { process(); }
                else { s_state = ST_BODY; }
            }
            else { process(); }
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
            // Abort a failed streamed write immediately rather than making the
            // browser finish transmitting an already-doomed large upload.
            if (s_route == R_FUPLOAD && s_err) { process(); break; }
            if (s_rxRecv >= s_clen) { process(); break; }
            if ((moved += r) > HTTP_POLL_BUDGET) return;          // yield, resume next frame
        }
    }

    if (s_state == ST_FILE_SEND) {
        int budget = HTTP_POLL_BUDGET;
        while (s_txHOff < s_txHLen) {
            r = send(s_conn, s_txH + s_txHOff, s_txHLen - s_txHOff, 0);
            if (r <= 0) { if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)return; closeConn(); return; }
            s_txHOff += r;
        }
        while (budget > 0) {
            if (s_fileAt >= s_fileHave) {
                if (s_fileSent >= s_fileTotal) { closeConn(); return; }
                unsigned long long remaining = s_fileTotal - s_fileSent;
                int want = s_fileChunkCap; if (remaining < (unsigned long long)want)want = (int)remaining;
                s_fileHave = Wf_DownloadRead(s_fileChunk, want); s_fileAt = 0;
                if (s_fileHave <= 0) { closeConn(); return; }
            }
            int want = s_fileHave - s_fileAt; if (want > budget)want = budget;
            r = send(s_conn, s_fileChunk + s_fileAt, want, 0);
            if (r <= 0) { if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)return; closeConn(); return; }
            s_fileAt += r; s_fileSent += (unsigned)r; budget -= r;
        }
        return;
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

            // Match the local launch-menu LED handoff and mirror the same
            // bank color into XBOX-RGB's temporary EOS effect.
            {
                unsigned char ef = Bank_Ef(bank);
                unsigned int rgb;
                int eventBank;
                if (ef == 0x0A) {
                    rgb = 0xFEFEFEu;
                    eventBank = 5;
                    Led_Show(EOS_LED_WHITE, 0);
                }
                else {
                    rgb = Desc_GetColor(bank);
                    eventBank = (ef >= 0x3 && ef <= 0x6) ? (int)(ef - 0x2) : 0;
                    Led_Show(EOS_LED_SOLID, rgb);
                }
                if (XboxRgb_BankEvent(eventBank, rgb, 7000UL)) Sleep(15);
            }

            // Bank_Launch performs the clean firmware warm-reset handoff.
            Bank_Launch(bank);            // does not return
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

    s_listen = s; s_conn = INVALID_SOCKET; s_state = ST_IDLE; s_launch = -1;
    // Optional 128 MB speed path. Stock consoles retain the fixed 16 KB buffer.
    MEMORYSTATUS mem; ZeroMemory(&mem, sizeof(mem)); mem.dwLength = sizeof(mem); GlobalMemoryStatus(&mem);
    if (mem.dwTotalPhys >= 112UL * 1024UL * 1024UL && mem.dwAvailPhys >= 24UL * 1024UL * 1024UL) {
        char* large = (char*)GlobalAlloc(GMEM_FIXED, 64 * 1024);
        if (large) { s_fileChunk = large; s_fileChunkCap = 64 * 1024; }
    }
    unsigned long seed = GetTickCount() ^ (unsigned long)(size_t)&s_req ^ (unsigned long)(size_t)&s_conn;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { seed = seed * 1664525UL + 1013904223UL; s_filesToken[i] = hex[(seed >> 24) & 15]; }
    s_filesToken[16] = 0;
    s_up = 1;
}

void Http_Stop(void)
{
    closeConn();
    Wf_Shutdown();
    if (s_fileChunk != s_fileSmall) { GlobalFree(s_fileChunk); s_fileChunk = s_fileSmall; s_fileChunkCap = sizeof(s_fileSmall); }
    if (s_listen != INVALID_SOCKET) { closesocket(s_listen); s_listen = INVALID_SOCKET; }
    s_up = 0; s_state = ST_IDLE; s_launch = -1;
}

int Http_IsUp(void) { return s_up; }