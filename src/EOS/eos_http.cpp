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

#define HTTP_PORT      80
#define HTTP_REQ_MAX   8192
#define HTTP_RX_MAX    (1024 * 1024)   // flash upload / BMP staging
#define HTTP_JSON_MAX  8192
#define HTTP_RESP_MAX  1024
#define HTTP_POLL_BUDGET (128 * 1024)  // max body bytes moved per frame

enum { ST_IDLE = 0, ST_HDR, ST_BODY, ST_SEND };
enum { M_GET = 0, M_POST };
enum {
    R_NONE = 0, R_PAGE, R_LOGO, R_BANKS, R_RENAME, R_DELETE, R_FLASH, R_LAUNCH, R_EEPROM, R_RESET, R_SYSINFO, R_CLRXBDIAG,
    R_THEMES, R_TINI, R_TFILE, R_TDEL
};   // custom-theme web tools (Phase 4)

static SOCKET s_listen = INVALID_SOCKET;
static SOCKET s_conn = INVALID_SOCKET;
static int    s_up = 0;
static int    s_state = ST_IDLE;

static char   s_req[HTTP_REQ_MAX]; static int s_reqLen;
static int    s_method, s_route, s_bank, s_clen;
static int    s_rxRecv, s_rxStore, s_store, s_err;
static int    s_launch = -1;

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
static char   s_tFolder[64], s_tName[64];       // theme route query params
static HANDLE s_upFile = INVALID_HANDLE_VALUE;  // streaming theme-file upload

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
".modalcard input{background:#0f0f16;border:1px solid #2c2c38;border-radius:6px;color:var(--txt);padding:6px;}\n"
".modalcard input#mname,.modalcard input[type=file]{width:100%;display:block;margin-top:4px;}\n"
".modalcard input[type=color]{width:46px;height:28px;padding:2px;vertical-align:middle;cursor:pointer;}\n"
".modalcard input[type=range]{width:70%;padding:0;vertical-align:middle;}\n"
"#colors{display:grid;grid-template-columns:1fr 1fr;gap:2px 10px;}\n"
"#colors label{display:flex;align-items:center;justify-content:space-between;}\n"
"#mprog{color:var(--p);font-size:12px;min-height:16px;margin-top:8px;}\n"
"#themecard .kv .k{overflow:hidden;text-overflow:ellipsis;}\n"
"#themecard .kv button{padding:4px 9px;font-size:12px;margin-left:6px;}\n"
"</style></head><body>\n"
"<header><img src='/logo.bmp' alt=''><div><h1>EOS</h1><div class=sub>BIOS bank manager</div></div></header>\n"
"<div id=budget style='max-width:1100px;margin:0 auto;padding:8px 20px 0;color:#9a9aa8;font-size:13px;'></div>\n"
"<div class=grid id=grid></div>\n"
"<div class=grid id=sys></div><div id=msg></div>\n"
"<div id=modal class=modal><div class=modalcard>\n"
" <h2 id=mtitle>Create Theme</h2>\n"
" <label>Name<input id=mname></label>\n"
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
"}\n"
"const CK=[['bg_top','BG Top'],['bg_bottom','BG Bottom'],['panel','Panel'],['accent','Accent'],['glow','Glow'],['text','Text'],['text_dim','Text Dim']];\n"
"const CDEF={bg_top:'#0a0a0f',bg_bottom:'#05050a',panel:'#15151c',accent:'#a855f7',glow:'#c77dff',text:'#e8e8ef',text_dim:'#6a6a78'};\n"
"let editFolder=null,curBg='',curMus='';\n"
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
" else{for(const nm of t.themes){let row=document.createElement('div');row.className='kv';let k=document.createElement('span');k.className='k';k.textContent=nm;row.appendChild(k);let v=document.createElement('span');v.appendChild(btn('Edit','',function(){openEdit(nm);}));v.appendChild(btn('Delete','danger',function(){delTheme(nm);}));row.appendChild(v);info.appendChild(row);}}\n"
" c.appendChild(info);let r2=document.createElement('div');r2.className='row';r2.appendChild(btn('Create Theme','go',openCreate));c.appendChild(r2);host.appendChild(c);}\n"
"function openCreate(){editFolder=null;curBg='';curMus='';document.getElementById('mtitle').textContent='Create Theme';let mn=document.getElementById('mname');mn.value='';mn.disabled=false;document.getElementById('mdim').value=40;document.getElementById('mdimv').textContent='40';document.getElementById('mbg').value='';document.getElementById('mmus').value='';for(const kc of CK)document.getElementById('c_'+kc[0]).value=CDEF[kc[0]];document.getElementById('mprog').textContent='';showModal(true);}\n"
"async function openEdit(folder){editFolder=folder;document.getElementById('mtitle').textContent='Edit Theme';let mn=document.getElementById('mname');mn.value=folder;mn.disabled=true;document.getElementById('mbg').value='';document.getElementById('mmus').value='';document.getElementById('mprog').textContent='';\n"
" let txt='';try{let r=await fetch('/api/theme/ini?folder='+encodeURIComponent(folder));txt=await r.text();}catch(e){}let kv=parseIni(txt);curBg=kv.background||'';curMus=kv.music||'';\n"
" let dim=kv.bg_dim||'0';document.getElementById('mdim').value=dim;document.getElementById('mdimv').textContent=dim;for(const kc of CK)document.getElementById('c_'+kc[0]).value=normColor(kv[kc[0]]);showModal(true);}\n"
"function resizeImage(file){return new Promise(function(resolve){let img=new Image();img.onload=function(){let w=img.width,h=img.height,MX=1280,MY=720;if(w<=MX&&h<=MY){resolve(file);return;}let s=Math.min(MX/w,MY/h);let nw=Math.round(w*s),nh=Math.round(h*s);let cv=document.createElement('canvas');cv.width=nw;cv.height=nh;cv.getContext('2d').drawImage(img,0,0,nw,nh);let type=(file.type&&file.type.indexOf('png')>=0)?'image/png':'image/jpeg';cv.toBlob(function(b){resolve(b||file);},type,0.9);};img.onerror=function(){resolve(file);};img.src=URL.createObjectURL(file);});}\n"
"async function saveTheme(){let name=editFolder||document.getElementById('mname').value.trim().replace(/[^A-Za-z0-9 _-]/g,'');if(!name){alert('Name required');return;}\n"
" let bgFile=document.getElementById('mbg').files[0];let musFile=document.getElementById('mmus').files[0];let prog=document.getElementById('mprog');\n"
" let bgName=bgFile?('background'+extOf(bgFile.name)):curBg;let musName=musFile?('music'+extOf(musFile.name)):curMus;\n"
" let ini='version = 1\\nname = '+name+'\\n';if(bgName)ini+='background = '+bgName+'\\n';ini+='bg_dim = '+document.getElementById('mdim').value+'\\n';if(musName)ini+='music = '+musName+'\\n';\n"
" for(const kc of CK)ini+=kc[0]+' = '+document.getElementById('c_'+kc[0]).value+'\\n';\n"
" prog.textContent='Saving theme...';try{\n"
"  await fetch('/api/theme/ini?folder='+encodeURIComponent(name),{method:'POST',body:ini});\n"
"  if(bgFile){prog.textContent='Uploading background...';let blob=await resizeImage(bgFile);await fetch('/api/theme/file?folder='+encodeURIComponent(name)+'&name='+encodeURIComponent(bgName),{method:'POST',body:blob});}\n"
"  if(musFile){prog.textContent='Uploading music...';await fetch('/api/theme/file?folder='+encodeURIComponent(name)+'&name='+encodeURIComponent(musName),{method:'POST',body:musFile});}\n"
"  prog.textContent='';showModal(false);msg('Theme saved');renderThemes();\n"
" }catch(e){prog.textContent='Save failed';}}\n"
"async function delTheme(folder){if(!confirm('Delete theme \"'+folder+'\" ?'))return;try{await fetch('/api/theme/del?folder='+encodeURIComponent(folder),{method:'POST'});}catch(e){}msg('Theme deleted');renderThemes();}\n"
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
            while (i < s_reqLen && s_req[i] >= '0' && s_req[i] <= '9') { v = v * 10 + (s_req[i] - '0'); ++i; }
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

// Build "E:\Eos\Themes\<folder>" (+ "\<leaf>" when leaf != 0).
static void themePath(char* out, int cap, const char* folder, const char* leaf)
{
    const char* pre = "E:\\Eos\\Themes\\";
    int p = 0, i = 0;
    while (pre[i] && p < cap - 1) out[p++] = pre[i++];
    for (i = 0; folder[i] && p < cap - 1; ++i) out[p++] = folder[i];
    if (leaf) {
        if (p < cap - 1) out[p++] = '\\';
        for (i = 0; leaf[i] && p < cap - 1; ++i) out[p++] = leaf[i];
    }
    out[p] = 0;
}

// {"themes":["Name",...]} -- folders under E:\Eos\Themes with a theme.ini.
static int buildThemesJson(char* o)
{
    static EosFileEntry ents[64];
    char ini[256];
    int  p = 0, n, i, first = 1;
    n = File_ListDir("E:\\Eos\\Themes", ents, 64);
    p += appS(o + p, "{\"themes\":[");
    for (i = 0; i < n; ++i) {
        if (!ents[i].is_dir) continue;
        themePath(ini, sizeof(ini), ents[i].name, "theme.ini");
        if (!File_Exists(ini)) continue;
        if (!first) o[p++] = ',';
        first = 0;
        o[p++] = '"'; p += appJson(o + p, ents[i].name); o[p++] = '"';
    }
    p += appS(o + p, "]}");
    return p;
}

// Delete every file in a theme folder (flat), then the folder. 1 on rmdir ok.
static int deleteThemeFolder(const char* folder)
{
    static EosFileEntry ents[64];
    char dir[256], fp[256];
    int  n, i;
    themePath(dir, sizeof(dir), folder, 0);
    n = File_ListDir(dir, ents, 64);
    for (i = 0; i < n; ++i) {
        if (ents[i].is_dir) continue;
        themePath(fp, sizeof(fp), folder, ents[i].name);
        DeleteFileA(fp);
    }
    return RemoveDirectoryA(dir) ? 1 : 0;
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
    else if (strEqN(path, "/api/sysinfo", 12)) s_route = R_SYSINFO;
    else if (strEqN(path, "/api/clrxbdiag", 14)) s_route = R_CLRXBDIAG;
    else if (strEqN(path, "/api/themes", 11)) s_route = R_THEMES;
    else if (strEqN(path, "/api/theme/ini", 14)) s_route = R_TINI;
    else if (strEqN(path, "/api/theme/file", 15)) s_route = R_TFILE;
    else if (strEqN(path, "/api/theme/del", 14)) s_route = R_TDEL;
    else if (strEqN(path, "/logo.bmp", 9)) s_route = R_LOGO;
    else if (path[0] == '/' && (path[1] == ' ' || path[1] == '?')) s_route = R_PAGE;

    s_bank = qBank(path);
    if (s_route == R_THEMES || s_route == R_TINI || s_route == R_TFILE || s_route == R_TDEL) {
        qStr(path, "folder", s_tFolder, sizeof(s_tFolder));
        qStr(path, "name", s_tName, sizeof(s_tName));
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

// ---- act on a fully-received request --------------------------------------
static void process(void)
{
    int n = Bank_Count();

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

    // ---- custom-theme web tools (Phase 4) ----
    if (s_route == R_THEMES) { int len = buildThemesJson(s_json); respond("200 OK", "application/json", s_json, len); return; }
    if (s_route == R_TINI) {
        char fp[256];
        if (!safeName(s_tFolder)) { respondText("400 Bad Request", "bad folder"); return; }
        if (s_method == M_GET) {
            int rd;
            themePath(fp, sizeof(fp), s_tFolder, "theme.ini");
            rd = File_ReadInto(fp, s_rx, HTTP_RX_MAX - 1);
            if (rd < 0) { respondText("404 Not Found", "no theme.ini"); return; }
            respond("200 OK", "text/plain", (const char*)s_rx, rd);
            return;
        }
        {   // POST: write the body as theme.ini (folder auto-created)
            char dir[256]; HANDLE h; DWORD wr;
            themePath(dir, sizeof(dir), s_tFolder, 0);
            CreateDirectoryA("E:\\Eos", NULL);
            CreateDirectoryA("E:\\Eos\\Themes", NULL);
            CreateDirectoryA(dir, NULL);
            themePath(fp, sizeof(fp), s_tFolder, "theme.ini");
            h = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { respondText("500 Error", "open failed"); return; }
            WriteFile(h, s_rx, (DWORD)s_rxStore, &wr, NULL);
            CloseHandle(h);
            respondText("200 OK", "ini saved");
            return;
        }
    }
    if (s_route == R_TFILE) {
        if (s_upFile != INVALID_HANDLE_VALUE) { CloseHandle(s_upFile); s_upFile = INVALID_HANDLE_VALUE; }
        if (s_err) { respondText("500 Error", "upload failed"); return; }
        respondText("200 OK", "file saved");
        return;
    }
    if (s_route == R_TDEL) {
        if (!safeName(s_tFolder)) { respondText("400 Bad Request", "bad folder"); return; }
        deleteThemeFolder(s_tFolder);
        respondText("200 OK", "theme deleted");
        return;
    }


    if (s_route == R_RENAME) {
        if (s_bank < 0 || s_bank >= n || Bank_IsBoot(s_bank)) { respondText("400 Bad Request", "bad bank"); return; }
        s_name[s_rxStore] = 0;
        if (s_name[0]) { Bank_SetName(s_bank, s_name); Config_Save(); }
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
    if (s_upFile != INVALID_HANDLE_VALUE) { CloseHandle(s_upFile); s_upFile = INVALID_HANDLE_VALUE; }
    if (s_conn != INVALID_SOCKET) { closesocket(s_conn); s_conn = INVALID_SOCKET; }
    s_state = ST_IDLE; s_launch = -1;
}

// route a POST body to the right buffer; called once headers are parsed
static void beginBody(void)
{
    s_rxRecv = 0; s_rxStore = 0; s_store = 1; s_err = 0;
    if (s_route == R_FLASH) {
        // Accept up to the full 1MB budget; an oversized image is routed to the
        // new region and the slot-fit (free-run) check happens in the handler.
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
        if (!safeName(s_tFolder) || !safeName(s_tName)) { s_err = 400; s_store = 0; }
        else {
            char dir[256], fp[256];
            themePath(dir, sizeof(dir), s_tFolder, 0);
            CreateDirectoryA("E:\\Eos", NULL);
            CreateDirectoryA("E:\\Eos\\Themes", NULL);
            CreateDirectoryA(dir, NULL);
            themePath(fp, sizeof(fp), s_tFolder, s_tName);
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
    else if (s_route == R_TINI) {
        cap = HTTP_RX_MAX;
        room = cap - s_rxStore; if (room > len) room = len;
        for (i = 0; i < room; ++i) s_rx[s_rxStore + i] = (unsigned char)src[i];
        s_rxStore += room;
    }
    else if (s_route == R_TFILE) {
        if (s_upFile != INVALID_HANDLE_VALUE) {
            DWORD wr; WriteFile(s_upFile, src, (DWORD)len, &wr, NULL);
            s_rxStore += len;
        }
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
            // Every bank launches normally; the FPGA redirects an oversized anchor
            // to the ext-region SDRAM copy.
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