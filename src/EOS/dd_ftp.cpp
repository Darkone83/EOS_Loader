// FtpServ.cpp
// XbDiag FTP server — split from FileExplorer.cpp.
//
// All sockets non-blocking (FIONBIO), polled once per frame via FtpServ_Tick().
// FileExplorer calls FtpServ_Start/Stop/Tick/DrawWidget and reads g_ftp.state
// for the status widget.
//
// Credentials : xbox / xbox   Port: 21   Mode: passive (PASV) only
// Commands    : USER PASS SYST TYPE PWD CWD CDUP LIST RETR STOR PASV QUIT
//               DELE MKD RMD RNFR RNTO SIZE NOOP FEAT MDTM
//               AUTH PBSZ PROT STAT REST ALLO MODE EPSV
//               (AUTH/PBSZ/PROT/EPSV/REST/ALLO acknowledged but not fully implemented)

#include "dd_ftp.h"
#include "dd_mount.h"     /* primary + detected HDD1 aliases */
#include "dd_net.h"       /* Net_Ip / Net_IsUp (deferred start) */
#include <xtl.h>
#include <winsockx.h>

/* local copy of XbDiag StrCopy (was in DiagCommon) */
static void StrCopy(char* dst, int dstLen, const char* src) {
    int i = 0;
    if (dstLen <= 0) return;
    while (src[i] && i < dstLen - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* local copies of XbDiag StrCat2 / IntToStr (were in DiagCommon) */
static void StrCat2(char* buf, int bufLen, const char* a, const char* b) {
    int i = 0;
    while (*a && i < bufLen - 1) buf[i++] = *a++;
    while (*b && i < bufLen - 1) buf[i++] = *b++;
    buf[i] = '\0';
}

static void IntToStr(int v, char* buf, int bufLen) {
    char tmp[16];
    int  out = 0, n = 0, i;
    unsigned u;
    if (bufLen <= 1) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    if (v < 0 && out < bufLen - 1) { buf[out++] = '-'; v = -v; }
    u = (unsigned)v;
    while (u > 0 && n < 15) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    for (i = n - 1; i >= 0 && out < bufLen - 1; --i) buf[out++] = tmp[i];
    buf[out] = '\0';
}

// ============================================================================
// Constants
// ============================================================================

#define FTP_DATA_PORT_BASE   2024
#define FTP_DATA_PORT_COUNT  32
#define FTP_MAX_PATH         256    // max path length used in FTP operations
#define FTP_PATH_PATTERN_MAX (FTP_MAX_PATH + 4)
#define FTP_IDLE_TIMEOUT_MS  300000UL  // drop an idle control connection after 5 min

// ============================================================================
// Internal string helpers
// ============================================================================

void FtpServ_AppendStr(char* out, int outLen, const char* src)
{
    int i = 0; while (out[i]) i++;
    while (*src && i < outLen - 1) out[i++] = *src++;
    out[i] = '\0';
}

// Copy src into dst, truncating to maxChars display characters.
// Appends "~" if truncated. dst must be >= dstLen bytes.
void FtpServ_TruncName(const char* src, char* dst, int maxChars, int dstLen)
{
    int len = 0; while (src[len]) len++;
    if (len <= maxChars)
    {
        int i = 0;
        while (*src && i < dstLen - 1) dst[i++] = *src++;
        dst[i] = '\0';
    }
    else
    {
        int i = 0;
        while (i < maxChars - 1 && i < dstLen - 2) dst[i++] = src[i];
        dst[i++] = '~';
        dst[i] = '\0';
    }
}

// Local bounded path helpers. Kept here so FTP does not depend on the
// DarkDash dd_path/dd_fileops modules that are not part of EOS Loader.
static int FtpPathLen(const char* s)
{
    int n = 0; if (!s) return 0; while (s[n]) ++n; return n;
}

static int FtpPathCopy(char* dst, int cap, const char* src)
{
    int i = 0;
    if (!dst || cap <= 0 || !src) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
    return src[i] == 0;
}

static int FtpPathJoin(char* dst, int cap, const char* a, const char* b)
{
    int n, i = 0;
    if (!FtpPathCopy(dst, cap, a)) return 0;
    n = FtpPathLen(dst);
    if (n > 0 && dst[n - 1] != '\\' && dst[n - 1] != '/') {
        if (n >= cap - 1) return 0;
        dst[n++] = '\\'; dst[n] = 0;
    }
    while (b && b[i]) {
        if (n >= cap - 1) return 0;
        dst[n++] = b[i++];
    }
    dst[n] = 0;
    return 1;
}

// ============================================================================
// Module state
// ============================================================================

FtpCtx g_ftp;

static int  s_nextDataPort = 0;
static char s_ipStr[20];
static bool s_ipOK = false;
static char s_ftpUser[32] = "xbox";
static char s_ftpPass[32] = "xbox";
static int  s_ftpPort = 21;

// ============================================================================
// FTP helpers
// ============================================================================

static void FtpSendStr(SOCKET s, const char* str)
{
    // Queue into ctrl send buffer; FtpTick drains it each frame.
    if (s != g_ftp.ctrlSock) return;  // only used for ctrl replies
    int len = 0; while (str[len]) len++;

    // If the reply won't fit, first compact the buffer by shifting out the
    // already-sent prefix. sendOff bytes at the front are gone — reclaim them.
    int space = (int)sizeof(g_ftp.sendBuf) - g_ftp.sendLen;
    if (len > space && g_ftp.sendOff > 0)
    {
        int remaining = g_ftp.sendLen - g_ftp.sendOff;
        for (int i = 0; i < remaining; ++i)
            g_ftp.sendBuf[i] = g_ftp.sendBuf[g_ftp.sendOff + i];
        g_ftp.sendLen = remaining;
        g_ftp.sendOff = 0;
        space = (int)sizeof(g_ftp.sendBuf) - g_ftp.sendLen;
    }

    // After compaction, if it still won't fit the buffer is genuinely exhausted.
    // This should be unreachable in normal operation (8192 bytes, command intake
    // paused at threshold) — but if it happens, stay connected and discard rather
    // than drop the session. A missed reply is recoverable; a disconnect is not.
    if (len > space) return;

    for (int i = 0; i < len; ++i)
        g_ftp.sendBuf[g_ftp.sendLen++] = str[i];
}

static void FtpReply(int code, const char* msg)
{
    char line[128];
    char codeBuf[8];
    IntToStr(code, codeBuf, sizeof(codeBuf));
    line[0] = '\0';
    StrCat2(line, sizeof(line), line, codeBuf);
    StrCat2(line, sizeof(line), line, " ");
    StrCat2(line, sizeof(line), line, msg);
    StrCat2(line, sizeof(line), line, "\r\n");
    FtpSendStr(g_ftp.ctrlSock, line);
}

// Open a passive data listen socket and return the port
static bool FtpOpenPassive()
{
    if (g_ftp.dataListen != INVALID_SOCKET)
    {
        closesocket(g_ftp.dataListen);
        g_ftp.dataListen = INVALID_SOCKET;
    }
    if (g_ftp.dataSock != INVALID_SOCKET)
    {
        closesocket(g_ftp.dataSock);
        g_ftp.dataSock = INVALID_SOCKET;
    }

    g_ftp.dataListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ftp.dataListen == INVALID_SOCKET) return false;

    // SO_REUSEADDR so rebind succeeds immediately after prior connection closes
    DWORD reuse = 1;
    setsockopt(g_ftp.dataListen, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(g_ftp.dataListen, FIONBIO, &nb);

    // Rotate through port range to avoid TIME_WAIT on rapid successive transfers
    int port = FTP_DATA_PORT_BASE + (s_nextDataPort % FTP_DATA_PORT_COUNT);
    s_nextDataPort++;

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((u_short)port);

    if (bind(g_ftp.dataListen, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(g_ftp.dataListen, 1) != 0)
    {
        closesocket(g_ftp.dataListen);
        g_ftp.dataListen = INVALID_SOCKET;
        return false;
    }
    g_ftp.dataPort = (WORD)port;
    return true;
}

// Build PASV response using our IP and data port
static void FtpSendPasv()
{
    // Parse IP octets from s_ipStr
    BYTE oct[4] = { 127, 0, 0, 1 };
    if (s_ipOK)
    {
        const char* p = s_ipStr;
        for (int i = 0; i < 4; ++i)
        {
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            oct[i] = (BYTE)v;
            if (*p == '.') p++;
        }
    }

    WORD port = g_ftp.dataPort;
    char reply[64];
    char nums[8];
    reply[0] = '\0';
    StrCat2(reply, sizeof(reply), reply, "227 Entering Passive Mode (");

    char o[4];
    for (int i = 0; i < 4; ++i)
    {
        IntToStr((int)oct[i], o, sizeof(o));
        StrCat2(reply, sizeof(reply), reply, o);
        StrCat2(reply, sizeof(reply), reply, ",");
    }
    IntToStr((int)(port >> 8), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ",");
    IntToStr((int)(port & 0xFF), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ").\r\n");
    FtpSendStr(g_ftp.ctrlSock, reply);
}

// Append one LIST line to g_ftp.listBuf. Returns false if buffer full.
// line format: "drwxr-xr-x  1 xbox xbox  <size> Jan 01 00:00 <n>\r\n"
static bool FtpAppendListLine(const char* name, bool isDir, DWORD sizeLow)
{
    char line[FTP_MAX_PATH + 64];
    char* p = line;

    const char* perm = isDir ? "drwxr-xr-x" : "-rw-r--r--";
    while (*perm) *p++ = *perm++;
    *p++ = ' ';

    const char* owner = "  1 xbox xbox ";
    while (*owner) *p++ = *owner++;

    char szBuf[12];
    IntToStr((int)sizeLow, szBuf, sizeof(szBuf));
    int szLen = 0; while (szBuf[szLen]) szLen++;
    for (int i = szLen; i < 10; ++i) *p++ = ' ';
    const char* sp = szBuf; while (*sp) *p++ = *sp++;
    *p++ = ' ';

    const char* dt = "Jan 01 00:00 ";
    while (*dt) *p++ = *dt++;

    const char* nm = name; while (*nm) *p++ = *nm++;
    *p++ = '\r'; *p++ = '\n'; *p = '\0';

    int lineLen = (int)(p - line);
    int space = (int)sizeof(g_ftp.listBuf) - g_ftp.listBufLen;
    if (lineLen > space) return false;  // buffer full — truncate listing
    for (int i = 0; i < lineLen; ++i)
        g_ftp.listBuf[g_ftp.listBufLen++] = line[i];
    return true;
}

void FtpServ_Start(const char* ipStr, bool ipOK)
{
    // Refresh primary and secondary HDD aliases before exposing the FTP root.
    Mount_HddPartitions();

    // Store network state from FileExplorer
    s_ipOK = ipOK;
    if (ipStr) { int _i = 0; while (ipStr[_i] && _i < 19) { s_ipStr[_i] = ipStr[_i]; ++_i; } s_ipStr[_i] = '\0'; }

    ZeroMemory(&g_ftp, sizeof(g_ftp));
    g_ftp.listenSock = INVALID_SOCKET;
    g_ftp.ctrlSock = INVALID_SOCKET;
    g_ftp.dataListen = INVALID_SOCKET;
    g_ftp.dataSock = INVALID_SOCKET;
    g_ftp.xferFile = INVALID_HANDLE_VALUE;
    s_nextDataPort = 0;  // reset port rotation on each server start

    if (!s_ipOK) return;  // no network

    g_ftp.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ftp.listenSock == INVALID_SOCKET) return;

    DWORD reuse = 1;
    setsockopt(g_ftp.listenSock, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(g_ftp.listenSock, FIONBIO, &nb);

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    {
        int port = s_ftpPort;
        if (port < 1 || port > 65535) port = 21;
        sa.sin_port = htons((u_short)port);
    }

    if (bind(g_ftp.listenSock, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(g_ftp.listenSock, 5) != 0)
    {
        closesocket(g_ftp.listenSock);
        g_ftp.listenSock = INVALID_SOCKET;
        return;
    }

    // Always start in virtual root — MapFtpPath converts at filesystem boundaries
    g_ftp.atVirtualRoot = true;
    StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), "/");
    g_ftp.state = FTP_LISTEN;
}

// Stop FTP server — close all sockets
void FtpServ_Stop()
{
    if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
    }
    if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
    if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
    if (g_ftp.ctrlSock != INVALID_SOCKET) { closesocket(g_ftp.ctrlSock);   g_ftp.ctrlSock = INVALID_SOCKET; }
    if (g_ftp.listenSock != INVALID_SOCKET) { closesocket(g_ftp.listenSock); g_ftp.listenSock = INVALID_SOCKET; }
    g_ftp.state = FTP_OFF;
}

// Clean a virtual FTP path.
// - Splits on '/' and '\\', processes each component.
// - Skips empty components and ".".
// - ".." pops the last component (clamped at root, cannot escape).
// - Rejects components containing Windows-illegal chars (* ? " < > |).
// - Accepts ANY other name: _folder, .hidden, __temp, F:, etc.
// - Result always starts with '/' and has no trailing '/'.
static int CleanVirtualPath(const char* virtualPath, char* out, int outLen)
{
    int  oi = 0;
    const char* p = virtualPath;

    if (!out || outLen < 2) return 0;
    out[0] = '\0';

    while (*p)
    {
        // Skip separators
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;

        // Extract next component. Refuse an overlong component rather than
        // silently converting it into a different path.
        char tok[FTP_MAX_PATH];
        int  ti = 0;
        while (*p && *p != '/' && *p != '\\')
        {
            if (ti >= (int)sizeof(tok) - 1) return 0;
            tok[ti++] = *p++;
        }
        tok[ti] = '\0';

        if (tok[0] == '\0')
            continue;                               // empty — skip

        if (tok[0] == '.' && tok[1] == '\0')
            continue;                               // "." — skip

        if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0')
        {
            // ".." — pop last component, clamped at root
            if (oi > 0)
            {
                oi--;
                while (oi > 0 && out[oi] != '/') oi--;
            }
            continue;
        }

        // Reject Windows-illegal chars (colon is allowed: needed for "F:")
        for (int i = 0; tok[i]; i++)
        {
            char c = tok[i];
            if (c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
                return 0;
        }

        // Append /component exactly; never truncate.
        if (oi + 1 + ti >= outLen) return 0;
        out[oi++] = '/';
        for (int i = 0; tok[i]; i++) out[oi++] = tok[i];
    }

    if (oi == 0) { out[0] = '/'; out[1] = '\0'; return 1; }
    out[oi] = '\0';
    return 1;
}


static int ResolveRelative(const char* cwd, const char* arg, char* out, int outLen)
{
    if (arg[0] != '/') {
        // Max accepted cwd + separator + arg fits exactly in this scratch buffer.
        char tmp[FTP_MAX_PATH * 2];
        int need = FtpPathLen(cwd) + 1 + FtpPathLen(arg);
        if (need >= (int)sizeof(tmp)) return 0;
        StrCopy(tmp, sizeof(tmp), cwd);
        FtpServ_AppendStr(tmp, sizeof(tmp), "\\");
        FtpServ_AppendStr(tmp, sizeof(tmp), arg);
        return CleanVirtualPath(tmp, out, outLen);
    }
    // absolute virtual path — clean as-is
    return CleanVirtualPath(arg, out, outLen);
}

// Port of PrometheOS mapFtpPath (simplified for bare drive-letter virtual paths).
// "/F:/Apps/Type D" -> "F:\\Apps\\Type D"
// "/" -> "/"  (virtual root — caller must check)
static int MapFtpPath(const char* virt, char* out, int outLen)
{
    const char* a = (virt[0] == '/') ? virt + 1 : virt;
    int alen = FtpPathLen(a);

    if (!out || outLen <= 0) return 0;
    out[0] = '\0';

    // Translate secondary HDD friendly names to the loader's reserved aliases.
    // /HDD1-C/... -> P:\\..., /HDD1-E/... -> Q:\\..., etc. Direct P:/Q:/R:/V:
    // paths remain accepted for compatibility, but the FTP root advertises the
    // descriptive HDD1-* names.
    if (a[0] == 'H' && a[1] == 'D' && a[2] == 'D' && a[3] == '1' &&
        a[4] == '-' && (a[5] == 'C' || a[5] == 'E' || a[5] == 'F' || a[5] == 'G') &&
        (a[6] == '\0' || a[6] == '/' || a[6] == '\\'))
    {
        char alias = Mount_Hdd1AliasForPart(a[5]);
        const char* rest = a + 6;
        int restLen;
        if (!alias) return 0;
        if (*rest == '/' || *rest == '\\') ++rest;
        restLen = FtpPathLen(rest);
        if (3 + restLen >= outLen) return 0;
        out[0] = alias; out[1] = ':'; out[2] = '\\'; out[3] = '\0';
        if (*rest)
        {
            int i = 3;
            while (*rest)
            {
                out[i++] = (*rest == '/') ? '\\' : *rest;
                ++rest;
            }
            out[i] = '\0';
        }
        return 1;
    }

    // Normal drive mapping is length preserving except "F:" -> "F:\\".
    if (alen >= outLen) return 0;
    if (!FtpPathCopy(out, outLen, a)) return 0;
    for (int i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
    if (alen == 2 && out[1] == ':')
    {
        if (3 >= outLen) { out[0] = '\0'; return 0; }
        out[2] = '\\'; out[3] = '\0';
    }
    return 1;
}

// Convenience: resolve arg against cwd then map to real Windows path.
static int FtpResolvePath(const char* arg, char* out, int outLen)
{
    char virt[FTP_MAX_PATH];
    if (!ResolveRelative(g_ftp.cwd, arg, virt, sizeof(virt))) return 0;
    return MapFtpPath(virt, out, outLen);
}

/* A DOS drive root is considered present when the filesystem can report its
   volume size.  This intentionally matches Drive Info and PrometheOS instead
   of requiring FILE_ATTRIBUTE_DIRECTORY on X:\\, which is unreliable for
   FATX volume roots/secondary aliases on RXDK. */
static int FtpDriveRootAvailable(char letter)
{
    char root[4];
    ULARGE_INTEGER freeAvail, totalBytes, totalFree;
    DWORD attr;

    root[0] = letter; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    if (GetDiskFreeSpaceExA(root, &freeAvail, &totalBytes, &totalFree) &&
        totalBytes.QuadPart != 0)
        return 1;

    /* Keep a conservative fallback for non-FATX mappings such as D:. */
    attr = GetFileAttributesA(root);
    return attr != 0xFFFFFFFFUL;
}
// Execute a pending LIST — enumerate and send directly to dataSock, reply 226.
// Build listing into listBuf then start async drain via XFER_LIST.
// Called from FtpTick once dataSock is accepted.
static void FtpDoListing()
{
    g_ftp.listBufLen = 0;
    g_ftp.listBufOff = 0;

    if (g_ftp.listVirtualRoot)
    {
        const char* driveLetters[] = { "C", "D", "E", "F", "G", "X", "Y", "Z", NULL };
        for (int di = 0; driveLetters[di]; ++di)
        {
            if (FtpDriveRootAvailable(driveLetters[di][0]))
            {
                char driveName[4] = { driveLetters[di][0], ':', '\0' };
                FtpAppendListLine(driveName, true, 0);
            }
        }
        {
            static const char secAlias[4] = { 'P','Q','R','V' };
            static const char* secName[4] = { "HDD1-C","HDD1-E","HDD1-F","HDD1-G" };
            unsigned int mask = Mount_Hdd1MappedMask();
            for (int si = 0; si < 4; ++si)
            {
                if ((mask & (1u << si)) && FtpDriveRootAvailable(secAlias[si]))
                    FtpAppendListLine(secName[si], true, 0);
            }
        }
    }
    else
    {
        char pat[FTP_MAX_PATH + 4];
        StrCopy(pat, sizeof(pat), g_ftp.listDir);
        int pl = 0; while (pat[pl]) pl++;
        if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
        pat[pl] = '*'; pat[pl + 1] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                FtpAppendListLine(fd.cFileName, isDir, isDir ? 0 : fd.nFileSizeLow);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
    }

    // Listing built — send 150 and start async drain
    FtpReply(150, "Opening data connection.");
    g_ftp.listPending = false;
    g_ftp.xferType = XFER_LIST;
    g_ftp.state = FTP_TRANSFER;
}

// Recursively delete a directory and all its contents
static bool FtpDeleteDir(const char* path)
{
    char pat[FTP_PATH_PATTERN_MAX];
    if (!FtpPathJoin(pat, sizeof(pat), path, "*")) return false;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;

            // Build full child path
            char child[FTP_MAX_PATH];
            if (!FtpPathJoin(child, sizeof(child), path, fd.cFileName)) {
                FindClose(h);
                return false;
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!FtpDeleteDir(child)) { FindClose(h); return false; }
            }
            else
            {
                if (!DeleteFileA(child)) { FindClose(h); return false; }
            }
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) != 0;
}

// Process one complete FTP command line (null-terminated, no \r\n)
static void FtpHandleCommand(char* cmd)
{
    // Split verb and argument
    char verb[16] = {};
    char arg[1024] = {};
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < 15) { verb[i] = cmd[i]; i++; }
    verb[i] = '\0';
    if (cmd[i] == ' ') {
        int ai = 0;
        i++;
        while (cmd[i + ai]) {
            if (ai >= (int)sizeof(arg) - 1) { FtpReply(501, "Argument too long."); return; }
            arg[ai] = cmd[i + ai];
            ai++;
        }
        arg[ai] = '\0';
    }

    // Strip surrounding quotes — some clients quote paths containing spaces
    // e.g. CWD "F:\Apps\Type D"
    {
        int alen = 0; while (arg[alen]) alen++;
        if (alen >= 2 && arg[0] == '"' && arg[alen - 1] == '"')
        {
            for (int k = 0; k < alen - 2; ++k) arg[k] = arg[k + 1];
            arg[alen - 2] = '\0';
        }
    }

    // Uppercase verb
    for (int k = 0; verb[k]; ++k)
        if (verb[k] >= 'a' && verb[k] <= 'z') verb[k] -= 32;

    if (!g_ftp.authed)
    {
        // Only USER and PASS allowed before auth
        if (verb[0] == 'U' && verb[1] == 'S' && verb[2] == 'E' && verb[3] == 'R')
        {
            // Check username: "xbox"
            bool ok = (lstrcmpA(arg, s_ftpUser) == 0);
            g_ftp.gotUser = ok;
            FtpReply(331, "Password required.");
        }
        else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'S')
        {
            bool ok = g_ftp.gotUser &&
                (lstrcmpA(arg, s_ftpPass) == 0);
            if (ok) { g_ftp.authed = true; g_ftp.atVirtualRoot = true; StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), "/"); FtpReply(230, "Logged in."); }
            else { FtpReply(530, "Login incorrect."); }
        }
        else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
        {
            FtpReply(221, "Bye.");
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            g_ftp.state = FTP_LISTEN;
        }
        else { FtpReply(530, "Not logged in."); }
        return;
    }

    // ---- Authenticated commands ----
    if (verb[0] == 'S' && verb[1] == 'Y' && verb[2] == 'S' && verb[3] == 'T')
    {
        FtpReply(215, "Windows_NT");
    }
    else if (verb[0] == 'T' && verb[1] == 'Y' && verb[2] == 'P' && verb[3] == 'E')
    {
        FtpReply(200, "Type set.");
    }
    else if ((verb[0] == 'P' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'P' && verb[2] == 'W' && verb[3] == 'D'))
    {
        // PWD returns virtual path exactly — same as PrometheOS
        char reply[FTP_MAX_PATH + 8];
        reply[0] = '"';
        int ri = 1;
        const char* pp = g_ftp.cwd;
        while (*pp && ri < (int)sizeof(reply) - 4) reply[ri++] = *pp++;
        reply[ri++] = '"'; reply[ri++] = '\0';
        FtpReply(257, reply);
    }
    else if ((verb[0] == 'C' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'W' && verb[3] == 'D') ||
        (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U' && verb[3] == 'P') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U' && verb[3] == 'P'))
    {
        // Exact port of PrometheOS CWD/CDUP logic.
        bool isCdup = (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U') ||
            (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U');
        const char* param = isCdup ? ".." : arg;

        char newVirtual[FTP_MAX_PATH];
        if (!ResolveRelative(g_ftp.cwd, param, newVirtual, sizeof(newVirtual))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        bool isFolder = false;

        // Case 1: resolved to virtual root "/"
        if (newVirtual[0] == '/' && newVirtual[1] == '\0')
        {
            isFolder = true;
        }
        else
        {
            char ftpPath[FTP_MAX_PATH];
            if (!MapFtpPath(newVirtual, ftpPath, sizeof(ftpPath))) {
                FtpReply(553, "Path too long or invalid."); return;
            }

            // Case 2: drive root — ftpPath is "F:" or "F:\"
            int fplen = 0; while (ftpPath[fplen]) fplen++;
            if (fplen >= 2 && ftpPath[1] == ':' && (ftpPath[2] == '\0' || (ftpPath[2] == '\\' && ftpPath[3] == '\0')))
            {
                isFolder = FtpDriveRootAvailable(ftpPath[0]) ? true : false;
            }
            else
            {
                // Case 3: subdirectory.
                // GetFileAttributesA first — works on non-empty dirs.
                // Fallback: try CreateDirectoryA; if it fails with
                // ERROR_ALREADY_EXISTS the path exists and is a directory.
                // This handles empty directories on RXDK where GetFileAttributes
                // may return INVALID_FILE_ATTRIBUTES.
                DWORD attr = GetFileAttributesA(ftpPath);
                if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    isFolder = true;
                }
                else
                {
                    if (!CreateDirectoryA(ftpPath, NULL) &&
                        GetLastError() == ERROR_ALREADY_EXISTS)
                        isFolder = true;
                }
            }
        }

        if (isFolder)
        {
            g_ftp.atVirtualRoot = (newVirtual[0] == '/' && newVirtual[1] == '\0');
            StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), newVirtual);
            FtpReply(250, "Directory changed.");
        }
        else
        {
            FtpReply(550, "No such directory.");
        }
    }
    else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'V')
    {
        if (FtpOpenPassive()) FtpSendPasv();
        else FtpReply(425, "Cannot open data connection.");
    }
    else if ((verb[0] == 'L' && verb[1] == 'I' && verb[2] == 'S' && verb[3] == 'T') ||
        (verb[0] == 'N' && verb[1] == 'L' && verb[2] == 'S' && verb[3] == 'T'))
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        // Strip all leading flag groups (e.g. "-a", "-la", "-al", "-a -l")
        // Some clients send multiple flag tokens before the path or nothing.
        char* listArg = arg;
        while (listArg[0] == '-')
        {
            while (*listArg && *listArg != ' ') listArg++;  // skip flag token
            while (*listArg == ' ') listArg++;              // skip spaces
        }

        // Resolve listing target.
        // Empty arg or bare "/" after flag stripping always means current directory.
        // At virtual root that produces the drive letter listing.
        bool isRootArg = (listArg[0] == '\0') ||
            (listArg[0] == '/' && listArg[1] == '\0');
        bool listVirtualRoot = g_ftp.atVirtualRoot && isRootArg;
        char listDir[FTP_MAX_PATH] = {};
        if (!listVirtualRoot)
        {
            if (listArg[0] != '\0')
            {
                if (!FtpResolvePath(listArg, listDir, sizeof(listDir))) {
                    FtpReply(553, "Path too long or invalid."); return;
                }
            }
            else
            {
                if (!MapFtpPath(g_ftp.cwd, listDir, sizeof(listDir))) {
                    FtpReply(553, "Path too long or invalid."); return;
                }
            }
        }

        // Store as pending then dispatch immediately if dataSock is already
        // accepted (fast clients connect to the data port before LIST arrives).
        // Otherwise FtpTick dispatches it when accept fires.
        g_ftp.listPending = true;
        g_ftp.listVirtualRoot = listVirtualRoot;
        StrCopy(g_ftp.listDir, sizeof(g_ftp.listDir), listDir);

        if (g_ftp.dataSock != INVALID_SOCKET)
            FtpDoListing();
    }
    else if (verb[0] == 'R' && verb[1] == 'E' && verb[2] == 'T' && verb[3] == 'R')
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        HANDLE hf = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE)
        {
            FtpReply(550, "File not found."); return;
        }

        g_ftp.xferFile = hf;
        g_ftp.xferTotal = GetFileSize(hf, NULL);
        g_ftp.xferDone = 0;
        g_ftp.retrBufLen = 0;
        g_ftp.retrBufOff = 0;
        // REST resume: seek the source file to the requested offset so the
        // client can continue an interrupted download.
        if (g_ftp.restOffset > 0)
        {
            if (g_ftp.restOffset <= g_ftp.xferTotal)
            {
                SetFilePointer(hf, (LONG)g_ftp.restOffset, NULL, FILE_BEGIN);
                g_ftp.xferDone = g_ftp.restOffset;
            }
            g_ftp.restOffset = 0;   // consumed
        }
        FtpServ_TruncName(arg, g_ftp.xferName, 18, sizeof(g_ftp.xferName));
        StrCopy(g_ftp.xferPath, sizeof(g_ftp.xferPath), fullPath);
        // Defer 150 until dataSock is accepted — mirrors LIST's listPending pattern.
        // Sending 150 before the data connection exists starts the client's transfer
        // timeout prematurely; on back-to-back transfers the port may not be ready yet.
        g_ftp.retrPending = true;
        g_ftp.xferType = XFER_RETR;
        // Do not set FTP_TRANSFER yet — tick promotes state when dataSock accepted
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            FtpReply(150, "Opening data connection.");
            g_ftp.retrPending = false;
            g_ftp.state = FTP_TRANSFER;
        }
    }
    else if (verb[0] == 'S' && verb[1] == 'T' && verb[2] == 'O' && verb[3] == 'R')
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        // ALLO pre-check: if the client declared a size and the target volume
        // can't hold it, refuse now rather than filling the disk and failing
        // mid-upload. Best-effort (only when ALLO was sent and the path has a
        // drive root we can query).
        if (g_ftp.alloSize > 0 && fullPath[1] == ':')
        {
            char root[4]; ULARGE_INTEGER freeAvail, totalB, totalFree;
            root[0] = fullPath[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            if (GetDiskFreeSpaceExA(root, &freeAvail, &totalB, &totalFree))
            {
                if (freeAvail.QuadPart < (unsigned __int64)g_ftp.alloSize)
                {
                    g_ftp.alloSize = 0;
                    FtpReply(552, "Insufficient storage for upload.");
                    return;
                }
            }
        }
        g_ftp.alloSize = 0;   // consumed (or unusable)

        // REST resume on upload: open existing + seek; otherwise truncate.
        HANDLE hf;
        if (g_ftp.restOffset > 0)
        {
            hf = CreateFile(fullPath, GENERIC_WRITE, 0,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
                SetFilePointer(hf, (LONG)g_ftp.restOffset, NULL, FILE_BEGIN);
        }
        else
        {
            hf = CreateFile(fullPath, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (hf == INVALID_HANDLE_VALUE)
        {
            g_ftp.restOffset = 0;
            FtpReply(550, "Cannot create file."); return;
        }

        g_ftp.xferFile = hf;
        g_ftp.xferTotal = 0;   // unknown until complete
        g_ftp.xferDone = (g_ftp.restOffset > 0) ? g_ftp.restOffset : 0;
        g_ftp.restOffset = 0;   // consumed
        FtpServ_TruncName(arg, g_ftp.xferName, 18, sizeof(g_ftp.xferName));
        StrCopy(g_ftp.xferPath, sizeof(g_ftp.xferPath), fullPath);
        // Defer 150 until dataSock is accepted — same pattern as RETR/LIST.
        g_ftp.storPending = true;
        g_ftp.xferType = XFER_STOR;
        // Do not set FTP_TRANSFER yet — tick promotes state when dataSock accepted
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            FtpReply(150, "Opening data connection.");
            g_ftp.storPending = false;
            g_ftp.state = FTP_TRANSFER;
        }
    }
    else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
    {
        FtpReply(221, "Bye.");
        if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
        }
        if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
        if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
        closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
        g_ftp.state = FTP_LISTEN;
        g_ftp.authed = false;
        g_ftp.gotUser = false;
        g_ftp.gotRnfr = false;
        g_ftp.atVirtualRoot = false;
        g_ftp.ctrlHalfClosed = false;
        g_ftp.listPending = false;
        g_ftp.retrPending = false;
        g_ftp.storPending = false;
        g_ftp.xferType = XFER_NONE;
        g_ftp.recvLen = 0;
        g_ftp.listBufLen = 0;
        g_ftp.listBufOff = 0;
        g_ftp.sendLen = 0;
        g_ftp.sendOff = 0;
    }
    else if (verb[0] == 'D' && verb[1] == 'E' && verb[2] == 'L' && verb[3] == 'E')
    {
        // Delete a file
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (DeleteFileA(fullPath))
            FtpReply(250, "File deleted.");
        else
            FtpReply(550, "Delete failed.");
    }
    else if ((verb[0] == 'M' && verb[1] == 'K' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'M' && verb[2] == 'K' && verb[3] == 'D'))
    {
        // Create directory
        char newVirt[FTP_MAX_PATH];
        if (!ResolveRelative(g_ftp.cwd, arg, newVirt, sizeof(newVirt))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        char fullPath[FTP_MAX_PATH];
        if (!MapFtpPath(newVirt, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (CreateDirectoryA(fullPath, NULL))
        {
            // RFC 959: 257 response quotes the virtual path
            char reply[FTP_MAX_PATH + 4];
            reply[0] = '"';
            int ri = 1;
            const char* pp = newVirt;
            while (*pp && ri < (int)sizeof(reply) - 3) reply[ri++] = *pp++;
            reply[ri++] = '"'; reply[ri] = '\0';
            FtpReply(257, reply);
        }
        else
            FtpReply(550, "Cannot create directory.");
    }
    else if ((verb[0] == 'R' && verb[1] == 'M' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'R' && verb[2] == 'M' && verb[3] == 'D'))
    {
        // Remove directory and all contents recursively
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (FtpDeleteDir(fullPath))
            FtpReply(250, "Directory removed.");
        else
            FtpReply(550, "Remove failed.");
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'F' && verb[3] == 'R')
    {
        // Rename from — verify source exists before storing
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            g_ftp.gotRnfr = false;
            FtpReply(553, "Path too long or invalid."); return;
        }

        // Check as file first, then as directory
        DWORD attr = GetFileAttributesA(fullPath);
        if (attr == 0xFFFFFFFF)
        {
            g_ftp.gotRnfr = false;
            FtpReply(550, "No such file or directory.");
        }
        else
        {
            StrCopy(g_ftp.rnfrPath, sizeof(g_ftp.rnfrPath), fullPath);
            g_ftp.gotRnfr = true;
            FtpReply(350, "Waiting for RNTO.");
        }
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'T' && verb[3] == 'O')
    {
        if (!g_ftp.gotRnfr)
        {
            FtpReply(503, "Send RNFR first."); return;
        }
        g_ftp.gotRnfr = false;

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (MoveFileA(g_ftp.rnfrPath, fullPath))
            FtpReply(250, "Renamed.");
        else
            FtpReply(550, "Rename failed.");
    }
    else if (verb[0] == 'F' && verb[1] == 'E' && verb[2] == 'A' && verb[3] == 'T')
    {
        // Advertise supported features. Do NOT advertise MLST/MLSD — FileZilla
        // will prefer MLSD over LIST if advertised, and we don't implement it.
        // EPSV deliberately omitted — clients probe it anyway but we decline it.
        FtpSendStr(g_ftp.ctrlSock,
            "211-Features:\r\n"
            " SIZE\r\n"
            " MDTM\r\n"
            " REST STREAM\r\n"
            " UTF8\r\n"
            " TVFS\r\n"
            "211 END\r\n");
    }
    else if (verb[0] == 'O' && verb[1] == 'P' && verb[2] == 'T' && verb[3] == 'S')
    {
        // FileZilla sends "OPTS UTF8 ON" — just accept it
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'C' && verb[1] == 'L' && verb[2] == 'N' && verb[3] == 'T')
    {
        // FileZilla identifies itself with CLNT — accept silently
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'M' && verb[1] == 'L' && verb[2] == 'S' && verb[3] == 'D')
    {
        // FileZilla prefers MLSD over LIST — not implemented, but reply 502
        // so FileZilla retries with LIST rather than treating it as fatal.
        FtpReply(502, "MLSD not implemented, use LIST.");
    }
    else if (verb[0] == 'E' && verb[1] == 'P' && verb[2] == 'S' && verb[3] == 'V')
    {
        // FileZilla tries EPSV before PASV — decline with 500 so it falls back cleanly.
        // 522 means "network protocol not supported" which confuses some clients.
        FtpReply(500, "EPSV not supported, use PASV.");
    }
    else if (verb[0] == 'A' && verb[1] == 'B' && verb[2] == 'O' && verb[3] == 'R')
    {
        // Abort current transfer if any
        if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
        }
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
        }
        g_ftp.xferType = XFER_NONE;
        g_ftp.listPending = false;
        g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
        g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
        if (g_ftp.state == FTP_TRANSFER) g_ftp.state = FTP_CONNECTED;
        FtpReply(226, "Abort successful.");
    }
    else if (verb[0] == 'N' && verb[1] == 'O' && verb[2] == 'O' && verb[3] == 'P')
    {
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'S' && verb[1] == 'I' && verb[2] == 'Z' && verb[3] == 'E')
    {
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(fullPath, GetFileExInfoStandard, &fad) &&
            !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            // Report low 32 bits — files over 4GB are uncommon on Xbox FAT
            char szBuf[32];
            IntToStr((int)fad.nFileSizeLow, szBuf, sizeof(szBuf));
            FtpReply(213, szBuf);
        }
        else { FtpReply(550, "File not found."); }
    }
    else if (verb[0] == 'A' && verb[1] == 'U' && verb[2] == 'T' && verb[3] == 'H')
    {
        // WinSCP sends AUTH TLS — we are plain FTP only
        FtpReply(500, "AUTH not supported, plain FTP only.");
    }
    else if (verb[0] == 'P' && verb[1] == 'B' && verb[2] == 'S' && verb[3] == 'Z')
    {
        // WinSCP sends PBSZ 0 before PROT — acknowledge silently
        FtpReply(200, "PBSZ 0 OK.");
    }
    else if (verb[0] == 'P' && verb[1] == 'R' && verb[2] == 'O' && verb[3] == 'T')
    {
        // WinSCP sends PROT P — we only support clear data (PROT C)
        FtpReply(200, "PROT C assumed.");
    }
    else if (verb[0] == 'S' && verb[1] == 'T' && verb[2] == 'A' && verb[3] == 'T')
    {
        // WinSCP uses STAT as a connection keep-alive
        FtpReply(211, "DarkDash FTP Server ready.");
    }
    else if (verb[0] == 'M' && verb[1] == 'D' && verb[2] == 'T' && verb[3] == 'M')
    {
        // Return a dummy timestamp — Xbox FAT doesn't carry reliable mtime
        FtpReply(213, "19700101000000");
    }
    else if (verb[0] == 'R' && verb[1] == 'E' && verb[2] == 'S' && verb[3] == 'T')
    {
        // Resume support: remember the byte offset; the next RETR/STOR seeks to
        // it before transferring. Cleared after it's consumed (or on abort).
        DWORD off = 0;
        const char* p = arg;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { off = off * 10 + (DWORD)(*p - '0'); p++; }
        g_ftp.restOffset = off;
        FtpReply(350, "Restart position accepted, send RETR or STOR.");
    }
    else if (verb[0] == 'A' && verb[1] == 'L' && verb[2] == 'L' && verb[3] == 'O')
    {
        // Pre-allocation hint: capture the declared size so STOR can pre-check
        // free space and reject an upload that won't fit, rather than filling
        // the disk and failing mid-transfer. "ALLO <n>".
        DWORD sz = 0;
        const char* p = arg;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { sz = sz * 10 + (DWORD)(*p - '0'); p++; }
        g_ftp.alloSize = sz;
        FtpReply(200, "ALLO OK.");
    }
    else if (verb[0] == 'M' && verb[1] == 'O' && verb[2] == 'D' && verb[3] == 'E')
    {
        // MODE S (stream) is the only mode we support
        FtpReply(200, "MODE S OK.");
    }
    else if (verb[0] == 'S' && verb[1] == 'I' && verb[2] == 'T' && verb[3] == 'E')
    {
        // SITE subcommands — not implemented, 202 = "not implemented but accepted"
        // Use 202 not 502 so clients don't treat it as a fatal error
        FtpReply(202, "SITE not implemented.");
    }
    else if ((verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'R' && verb[3] == 'C') ||
        (verb[0] == 'X' && verb[1] == 'M' && verb[2] == 'D' && verb[3] == '5'))
    {
        // FlashFXP checksum commands — not supported
        FtpReply(502, "Checksum commands not supported.");
    }
    else
    {
        FtpReply(502, "Command not implemented.");
    }
}

// ============================================================================
// FtpTick — called once per frame, non-blocking throughout
// ============================================================================

void FtpServ_Tick()
{
    if (g_ftp.state == FTP_OFF) return;

    // Idle timeout: drop a control connection that's sat silent too long, so a
    // dead/half-gone client doesn't hold the single session forever (the server
    // is single-connection, so a stuck client locks everyone else out). Only
    // while CONNECTED -- never during a transfer, where the control channel is
    // legitimately quiet for the whole download/upload.
    if (g_ftp.state == FTP_CONNECTED && g_ftp.ctrlSock != INVALID_SOCKET &&
        g_ftp.xferType == XFER_NONE)
    {
        if (GetTickCount() - g_ftp.lastActivityMs > FTP_IDLE_TIMEOUT_MS)
        {
            FtpSendStr(g_ftp.ctrlSock, "421 Idle timeout, closing control connection.\r\n");
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            g_ftp.authed = false; g_ftp.gotUser = false; g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false; g_ftp.listPending = false;
            g_ftp.ctrlHalfClosed = false; g_ftp.retrPending = false; g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE; g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0; g_ftp.sendOff = 0;
            g_ftp.restOffset = 0; g_ftp.alloSize = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    }

    // ---- Drain ctrl send buffer ------------------------------------------
    // FtpSendStr() queues into sendBuf; we push bytes here each tick so
    // WSAEWOULDBLOCK never silently drops part of a reply.
    if (g_ftp.ctrlSock != INVALID_SOCKET && g_ftp.sendLen > g_ftp.sendOff)
    {
        int n = send(g_ftp.ctrlSock,
            g_ftp.sendBuf + g_ftp.sendOff,
            g_ftp.sendLen - g_ftp.sendOff, 0);
        if (n > 0)
        {
            g_ftp.sendOff += n;
            if (g_ftp.sendOff >= g_ftp.sendLen)
                g_ftp.sendOff = g_ftp.sendLen = 0;  // buffer fully drained
        }
        else if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            // Hard send error on ctrl socket — treat as disconnect
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
            }
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            g_ftp.authed = false; g_ftp.gotUser = false; g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false; g_ftp.listPending = false;
            g_ftp.ctrlHalfClosed = false; g_ftp.retrPending = false; g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE; g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0; g_ftp.sendOff = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    }

    // ---- Drain secondary connection attempts --------------------------------
    // FileZilla opens a second ctrl connection for transfers while keeping the
    // browsing connection open. Our server is single-session, so we accept and
    // immediately reject any connection that arrives while already connected.
    // This is critical: leaving it stuck in the listen backlog causes FileZilla
    // to wait until its own connection timeout fires, which it reports as a
    // disconnect. Sending 421 immediately tells FileZilla to reuse the existing
    // session instead of waiting on the stalled second connection.
    if ((g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER) &&
        g_ftp.listenSock != INVALID_SOCKET)
    {
        // Drain ALL pending secondary connections this tick (FileZilla may open
        // several at once). The accepted socket INHERITS the listen socket's
        // non-blocking mode, so a plain send() can return WSAEWOULDBLOCK and the
        // 421 never goes out before we close -- FileZilla then sees an abortive
        // close and reports a failed session instead of reusing the existing one.
        // Force the socket blocking and linger-on-close so the reply is actually
        // delivered, then FileZilla falls back to the single live session.
        const char* k_busy = "421 Only one session permitted.\r\n";
        int len = 0; while (k_busy[len]) len++;
        for (;;)
        {
            SOCKADDR_IN ca; int caLen = sizeof(ca);
            SOCKET cs = accept(g_ftp.listenSock, (SOCKADDR*)&ca, &caLen);
            struct linger lg;
            u_long blk = 0;
            if (cs == INVALID_SOCKET) break;
            ioctlsocket(cs, FIONBIO, &blk);                 /* -> blocking          */
            lg.l_onoff = 1; lg.l_linger = 1;                /* flush on close       */
            setsockopt(cs, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
            send(cs, k_busy, len, 0);
            closesocket(cs);
        }
    }

    // ---- Accept new control connection ------------------------------------
    if (g_ftp.state == FTP_LISTEN && g_ftp.listenSock != INVALID_SOCKET)
    {
        SOCKADDR_IN ca; int caLen = sizeof(ca);
        SOCKET cs = accept(g_ftp.listenSock, (SOCKADDR*)&ca, &caLen);
        if (cs != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(cs, FIONBIO, &nb);
            g_ftp.ctrlSock = cs;
            g_ftp.authed = false;
            g_ftp.gotUser = false;
            g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false;
            g_ftp.ctrlHalfClosed = false;
            g_ftp.xferType = XFER_NONE;
            g_ftp.recvLen = 0;
            g_ftp.restOffset = 0;
            g_ftp.alloSize = 0;
            g_ftp.lastActivityMs = GetTickCount();
            g_ftp.state = FTP_CONNECTED;
            // Multi-line 220 banner — FTP spec: "NNN-" for continuation, "NNN " for final line.
            // FlashFXP and FileZilla display the continuation lines in their message/log panel.
            FtpSendStr(g_ftp.ctrlSock, "220-DarkDash FTP Server\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Original Xbox Dashboard\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Team Resurgent  /  Darkone83\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Mode : Passive (PASV) only\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220 Ready.\r\n");
            // Fall through — don't return. The send-drain at the top of the
            // next-tick pass needs the banner to be flushed promptly. Returning
            // here would defer it one full frame on every new connection.
        }
        else
        {
            // No pending connection this tick — nothing else to do in LISTEN state.
            return;
        }
    }

    // ---- Accept passive data connection ----------------------------------
    // Try to accept whenever dataListen is open and we don't have dataSock yet.
    if (g_ftp.dataSock == INVALID_SOCKET &&
        g_ftp.dataListen != INVALID_SOCKET &&
        (g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER))
    {
        SOCKADDR_IN da; int daLen = sizeof(da);
        SOCKET ds = accept(g_ftp.dataListen, (SOCKADDR*)&da, &daLen);
        if (ds != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(ds, FIONBIO, &nb);

            // Expand TCP send buffer to 256KB — Xbox default is ~8KB which causes
            // constant WSAEWOULDBLOCK even with the spin loop, capping throughput.
            // 256KB gives the kernel enough runway to keep the wire saturated.
            int sndbuf = 256 * 1024;
            setsockopt(ds, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

            g_ftp.dataSock = ds;

            // If a LIST was waiting for a data connection, execute it now
            if (g_ftp.listPending)
                FtpDoListing();

            // If a RETR or STOR was waiting, send the deferred 150 now
            if (g_ftp.retrPending)
            {
                FtpReply(150, "Opening data connection.");
                g_ftp.retrPending = false;
                g_ftp.state = FTP_TRANSFER;
            }
            else if (g_ftp.storPending)
            {
                FtpReply(150, "Opening data connection.");
                g_ftp.storPending = false;
                g_ftp.state = FTP_TRANSFER;
            }
        }
    }

    // Fallback: listPending but dataSock already accepted (fast client connected
    // to data port before LIST command arrived — dispatch was handled inline in
    // FtpHandleCommand, but catch any edge case where it was missed).
    if (g_ftp.listPending &&
        g_ftp.dataSock != INVALID_SOCKET &&
        g_ftp.xferType == XFER_NONE)
    {
        FtpDoListing();
    }

    // Deferred RETR/STOR 150: dataSock just became available — send 150 now and
    // promote to FTP_TRANSFER so the data pump engages this tick.
    if (g_ftp.retrPending && g_ftp.dataSock != INVALID_SOCKET)
    {
        FtpReply(150, "Opening data connection.");
        g_ftp.retrPending = false;
        g_ftp.state = FTP_TRANSFER;
    }
    if (g_ftp.storPending && g_ftp.dataSock != INVALID_SOCKET)
    {
        FtpReply(150, "Opening data connection.");
        g_ftp.storPending = false;
        g_ftp.state = FTP_TRANSFER;
    }

    // ---- Control socket: receive and command dispatch --------------------
    if ((g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER) &&
        g_ftp.ctrlSock != INVALID_SOCKET)
    {
        // Always try to recv — don't gate on send buffer fullness.
        // A 8KB send buffer gives enough headroom; pausing recv causes
        // FileZilla to time out waiting for command acknowledgement.
        //
        // recv == 0 is a TCP half-close: the peer (FileZilla) shut down its write
        // side after its last command but still expects our pending replies (e.g.
        // 226 Transfer complete) on the ctrl socket.  Set ctrlHalfClosed and stop
        // reading; the send-drain at the top of tick will flush the buffer, and we
        // tear down only once it is empty.  Treating half-close as a hard error was
        // the root cause of random FileZilla disconnects before 226 was delivered.
        {
            int space = (int)sizeof(g_ftp.recvBuf) - g_ftp.recvLen - 1;
            if (space > 0 && !g_ftp.ctrlHalfClosed)
            {
                int n = recv(g_ftp.ctrlSock,
                    g_ftp.recvBuf + g_ftp.recvLen, space, 0);
                if (n > 0) { g_ftp.recvLen += n; g_ftp.lastActivityMs = GetTickCount(); }
                else if (n == 0)
                {
                    // Graceful half-close — stop receiving, keep sending
                    g_ftp.ctrlHalfClosed = true;
                }
                else if (WSAGetLastError() != WSAEWOULDBLOCK)
                    goto ctrl_disconnect;  // hard socket error
            }
            else
            {
                // recvBuf full — discard up to next newline, keep remainder
                int nl = -1;
                for (int i = 0; i < g_ftp.recvLen; ++i)
                    if (g_ftp.recvBuf[i] == '\n') { nl = i; break; }
                if (nl >= 0)
                {
                    int remaining = g_ftp.recvLen - nl - 1;
                    for (int i = 0; i < remaining; ++i)
                        g_ftp.recvBuf[i] = g_ftp.recvBuf[nl + 1 + i];
                    g_ftp.recvLen = remaining;
                }
                else g_ftp.recvLen = 0;
            }
        }

        // Parse and dispatch all complete commands already in recvBuf.
        // This runs regardless of send buffer state — commands buffered before
        // the gate closed still need to be processed.
        {
            g_ftp.recvBuf[g_ftp.recvLen] = '\0';
            char* buf = g_ftp.recvBuf;
            while (true)
            {
                int pos = -1;
                for (int i = 0; i < g_ftp.recvLen; ++i)
                    if (buf[i] == '\n') { pos = i; break; }
                if (pos < 0) break;

                if (pos > 0 && buf[pos - 1] == '\r') buf[pos - 1] = '\0';
                buf[pos] = '\0';

                FtpHandleCommand(buf);

                int remaining = g_ftp.recvLen - pos - 1;
                for (int i = 0; i < remaining; ++i) buf[i] = buf[pos + 1 + i];
                g_ftp.recvLen = remaining;
                buf[remaining] = '\0';
            }
        }
        // Half-close teardown: peer shut write side; once our send buffer is fully
        // drained (all replies delivered) we can close the session cleanly.
        // We do NOT dispatch any further commands once ctrlHalfClosed is set.
        if (g_ftp.ctrlHalfClosed && g_ftp.sendLen == g_ftp.sendOff)
            goto ctrl_disconnect;

        goto skip_ctrl_disconnect;

    ctrl_disconnect:
        {
            // Client disconnected or hard socket error — full session teardown
            if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
            }
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            g_ftp.authed = false;
            g_ftp.gotUser = false;
            g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false;
            g_ftp.ctrlHalfClosed = false;
            g_ftp.listPending = false;
            g_ftp.retrPending = false;
            g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE;
            g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0;
            g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0;
            g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0;
            g_ftp.sendOff = 0;
            g_ftp.restOffset = 0;
            g_ftp.alloSize = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    skip_ctrl_disconnect:;
    }

    // ---- Data transfer ---------------------------------------------------
    if (g_ftp.state == FTP_TRANSFER && g_ftp.dataSock != INVALID_SOCKET)
    {
        static char ioBuf[65536];  // 64KB — matches retrBuf for symmetric up/down speed

        if (g_ftp.xferType == XFER_LIST)
        {
            // Drain listBuf into dataSock non-blockingly, same pattern as RETR
            if (g_ftp.listBufOff < g_ftp.listBufLen)
            {
                int toSend = g_ftp.listBufLen - g_ftp.listBufOff;
                int sent = send(g_ftp.dataSock,
                    g_ftp.listBuf + g_ftp.listBufOff, toSend, 0);
                if (sent > 0)
                {
                    g_ftp.listBufOff += sent;
                }
                else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    // Data socket died mid-listing
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    FtpReply(426, "Connection closed; transfer aborted.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    return;
                }
                // WSAEWOULDBLOCK — just wait for next tick
            }
            else
            {
                // All listing data sent — close data socket and reply 226
                closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                if (g_ftp.dataListen != INVALID_SOCKET)
                {
                    closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                }
                FtpReply(226, "Transfer complete.");
                g_ftp.xferType = XFER_NONE;
                g_ftp.state = FTP_CONNECTED;
            }
        }
        else if (g_ftp.xferType == XFER_RETR)
        {
            // Time-bounded pump: keep reading + sending for up to 30ms per tick.
            // On WSAEWOULDBLOCK we spin within the time window rather than bailing
            // for a full frame — this is the key to hitting max throughput since
            // the TCP send buffer drains in microseconds and we'd waste the rest
            // of the budget if we broke out immediately.
            DWORD retrStart = GetTickCount();
            while (GetTickCount() - retrStart < 30)
            {
                // Drain any unsent remainder from the previous read first
                if (g_ftp.retrBufOff < g_ftp.retrBufLen)
                {
                    int toSend = g_ftp.retrBufLen - g_ftp.retrBufOff;
                    int sent = send(g_ftp.dataSock,
                        g_ftp.retrBuf + g_ftp.retrBufOff, toSend, 0);
                    if (sent > 0)
                    {
                        g_ftp.retrBufOff += sent;
                        g_ftp.xferDone += (DWORD)sent;
                        // Keep looping — may have drained fully, can read next chunk
                        continue;
                    }
                    else if (sent < 0)
                    {
                        if (WSAGetLastError() == WSAEWOULDBLOCK)
                            break;  // TCP send buffer full — yield to next tick
                        // Hard error
                        closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                        CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                        FtpReply(426, "Transfer aborted.");
                        g_ftp.xferType = XFER_NONE;
                        g_ftp.state = FTP_CONNECTED;
                        goto retr_done;
                    }
                    continue;
                }

                // Buffer fully drained — read the next chunk from disk
                g_ftp.retrBufLen = 0;
                g_ftp.retrBufOff = 0;
                DWORD bytesRead = 0;
                if (ReadFile(g_ftp.xferFile, g_ftp.retrBuf, sizeof(g_ftp.retrBuf),
                    &bytesRead, NULL) && bytesRead > 0)
                {
                    g_ftp.retrBufLen = (int)bytesRead;
                    // Fall through to drain path at top of loop next iteration
                }
                else
                {
                    // EOF or read error — transfer complete
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    if (g_ftp.dataListen != INVALID_SOCKET)
                    {
                        closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                    }
                    CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(226, "Transfer complete.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto retr_done;
                }
            }
        retr_done:;
        }
        else if (g_ftp.xferType == XFER_STOR)
        {
            // Time-bounded pump: keep receiving for up to 15ms per tick,
            // same pattern as RETR, so uploads match download speed.
            DWORD storStart = GetTickCount();
            while (GetTickCount() - storStart < 15)
            {
                int n = recv(g_ftp.dataSock, ioBuf, sizeof(ioBuf), 0);
                if (n > 0)
                {
                    DWORD written = 0;
                    if (!WriteFile(g_ftp.xferFile, ioBuf, (DWORD)n, &written, NULL) ||
                        written != (DWORD)n)
                    {
                        closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                        CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                        FtpReply(452, "Write error - disk full?");
                        g_ftp.xferType = XFER_NONE;
                        g_ftp.state = FTP_CONNECTED;
                        return;
                    }
                    g_ftp.xferDone += written;
                    // continue draining while socket has data and time remains
                }
                else if (n == 0)
                {
                    // Client closed data connection = transfer complete
                    FlushFileBuffers(g_ftp.xferFile);
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    if (g_ftp.dataListen != INVALID_SOCKET)
                    {
                        closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                    }
                    CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(226, "Transfer complete.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto stor_done;
                }
                else
                {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                        break;  // nothing queued, yield until next tick
                    // Hard error — client aborted
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(426, "Transfer aborted.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto stor_done;
                }
            }
        stor_done:;
        }
    }
}

/* ============================================================================
   DarkDash service wrappers. The dash is single-threaded, so the server is
   frame-polled: Ftp_Tick() runs once per main-loop frame. These thin shims
   keep the proven FtpServ_* engine intact while giving DarkDash a clean
   service API and status for the Settings panel.
   ============================================================================ */

static int s_ftpWanted = 0;   /* 1 if the user has FTP enabled */

void Ftp_SetCredentials(const char* user, const char* pass, int port)
{
    int i;
    if (user) {
        i = 0; while (user[i] && i < (int)sizeof(s_ftpUser) - 1) { s_ftpUser[i] = user[i]; ++i; }
        s_ftpUser[i] = 0;
    }
    if (pass) {
        i = 0; while (pass[i] && i < (int)sizeof(s_ftpPass) - 1) { s_ftpPass[i] = pass[i]; ++i; }
        s_ftpPass[i] = 0;
    }
    if (port > 0 && port < 65536) s_ftpPort = port;
}

void Ftp_Init(void) {
    g_ftp.state = FTP_OFF;
    s_ftpWanted = 0;
}

/* Mark the service wanted/unwanted. Actual bind is deferred to Ftp_Tick once
   the network has resolved (at boot DHCP isn't done yet, so we can't bind). */
void Ftp_Want(int want) {
    s_ftpWanted = want ? 1 : 0;
    if (!s_ftpWanted) FtpServ_Stop();
}

void Ftp_Start(const char* ipStr, int ipOK) {
    s_ftpWanted = 1;
    FtpServ_Start(ipStr, ipOK != 0);
}

void Ftp_Stop(void) {
    s_ftpWanted = 0;
    FtpServ_Stop();
}

void Ftp_Tick(void) {
    /* Deferred start: if wanted but not yet listening, bring it up as soon as
       the network is ready. If the link drops while running, stop so we retry
       cleanly when it returns. */
    if (s_ftpWanted) {
        if (g_ftp.state == FTP_OFF) {
            if (Net_IsUp())
                FtpServ_Start(Net_Ip(), 1);
        }
    }
    if (g_ftp.state != FTP_OFF) FtpServ_Tick();
}

int  Ftp_IsRunning(void) { return g_ftp.state != FTP_OFF; }
int  Ftp_Wanted(void) { return s_ftpWanted; }

/* 0 off, 1 listening, 2 connected, 3 transferring -- mirrors FtpState */
int  Ftp_Status(void) { return (int)g_ftp.state; }

/* transfer progress for the status panel (bytes); 0/0 when idle */
void Ftp_Progress(unsigned long* done, unsigned long* total) {
    if (done)  *done = g_ftp.xferDone;
    if (total) *total = g_ftp.xferTotal;
}