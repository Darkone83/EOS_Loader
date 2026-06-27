/*---------------------------------------------------------------------------
    dd_ftp.h -- DarkDash FTP service.

    A passive-mode FTP server, frame-polled from the main loop (the Xbox is a
    single-core P3, so a background thread would only time-slice against the
    render loop -- polling is the correct model). The proven engine is XbDiag's
    FtpServ, refactored here as a DarkDash service: enable/disable, configurable
    control port, and configurable credentials all come from dd_data.

    Defaults : xbox / xbox   Port 21   Passive (PASV) only, one client at a time.
    Clients  : tested against FlashFXP, FileZilla, WinSCP.

    Lifecycle (call from main):
        Ftp_Init()  once at boot
        if (settings.ftpEnabled && link) Ftp_Start(ip, ipOK);
        Ftp_Tick()  every frame
        Ftp_Stop()  / Ftp_Start() on the Settings toggle
---------------------------------------------------------------------------*/
#ifndef DD_FTP_H
#define DD_FTP_H

/* winsockx.h pulls windows.h unless _INC_WINDOWS is already defined, which
   xtl.h does -- so xtl.h must come first. Include both here so this header is
   self-sufficient no matter where it's pulled in. */
#include <xtl.h>
#include <winsockx.h>

   /* ---- the underlying engine's state/context (kept from FtpServ) ---------- */

#define FTP_MAX_SESSIONS 2    /* concurrent clients (shared listener, pooled contexts) */

enum FtpState { FTP_OFF = 0, FTP_LISTEN, FTP_CONNECTED, FTP_TRANSFER };
enum FtpXfer { XFER_NONE = 0, XFER_LIST, XFER_RETR, XFER_STOR };

struct FtpCtx {
    FtpState state;
    SOCKET   ctrlSock, dataListen, dataSock;   /* listener is shared (module-level) */
    WORD     dataPort;
    bool     authed, gotUser, atVirtualRoot, ctrlHalfClosed;
    char     cwd[256];
    FtpXfer  xferType;
    char     xferName[64];
    char     xferPath[256];
    HANDLE   xferFile;
    DWORD    xferTotal, xferDone;
    bool     gotRnfr;
    char     rnfrPath[256];
    char     recvBuf[1024]; int recvLen;
    char     sendBuf[2048]; int sendLen, sendOff;
    char     retrBuf[65536]; int retrBufLen, retrBufOff;
    bool     listPending, listVirtualRoot;
    char     listDir[256];
    bool     retrPending, storPending;
    char     listBuf[65536]; int listBufLen, listBufOff;
    DWORD    restOffset;        /* REST: byte offset to resume next RETR/STOR */
    DWORD    alloSize;          /* ALLO: client-declared size of next upload (0 = unknown) */
    DWORD    lastActivityMs;    /* GetTickCount of last control activity (idle timeout) */
};

/* engine entry points (defined in dd_ftp.cpp, were FtpServ_*) */
void FtpServ_Start(const char* ipStr, bool ipOK);
void FtpServ_Stop(void);
void FtpServ_Tick(void);
void FtpServ_AppendStr(char* out, int outLen, const char* src);
void FtpServ_TruncName(const char* src, char* dst, int maxChars, int dstLen);

/* ---- DarkDash service API ----------------------------------------------- */

void Ftp_SetCredentials(const char* user, const char* pass, int port); /* defaults xbox/xbox/21 */
int  Ftp_SessionCount(void);   /* active client connections (0..FTP_MAX_SESSIONS) */

void Ftp_Init(void);
void Ftp_Want(int want);                        /* enable/disable; start deferred to Tick */
void Ftp_Start(const char* ipStr, int ipOK);   /* current network IP + up flag */
void Ftp_Stop(void);
void Ftp_Tick(void);                            /* call every frame             */

int  Ftp_IsRunning(void);   /* 1 if listening/connected/transferring */
int  Ftp_Wanted(void);      /* 1 if the user enabled it              */
int  Ftp_Status(void);      /* 0 off / 1 listen / 2 connected / 3 xfer */
void Ftp_Progress(unsigned long* done, unsigned long* total);

#endif /* DD_FTP_H */