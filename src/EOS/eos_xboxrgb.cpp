// eos_xboxrgb.cpp -- optional XBOX-RGB discovery + transient EOS bank effects.
//
// RXDK/MSVC2003 friendly: non-blocking UDP, no STL/CRT formatting, no threads.
// XBOX-RGB remains a passive SMBus sniffer; this path is LAN-only on UDP 7777.
#include <xtl.h>
#include <winsockx.h>
#include "dd_net.h"
#include "eos_xboxrgb.h"

#define XRGB_PORT             7777
#define XRGB_DISC_FAST_MS     3000UL
#define XRGB_DISC_SLOW_MS    15000UL
#define XRGB_STALE_MS        45000UL
#define XRGB_RX_MAX            512

static SOCKET        s_sock = INVALID_SOCKET;
static int           s_wasUp = 0;
static int           s_present = 0;
static unsigned long s_rgbIp = 0;
static DWORD         s_nextDisc = 0;
static DWORD         s_lastSeen = 0;

static int strContainsN(const char* hay, int hayLen, const char* needle)
{
    int i, j, nl;
    nl = 0; while (needle[nl]) ++nl;
    if (nl == 0 || hayLen < nl) return 0;
    for (i = 0; i <= hayLen - nl; ++i) {
        for (j = 0; j < nl && hay[i + j] == needle[j]; ++j) {}
        if (j == nl) return 1;
    }
    return 0;
}

static void closeSock(void)
{
    if (s_sock != INVALID_SOCKET) closesocket(s_sock);
    s_sock = INVALID_SOCKET;
}

static int openSock(void)
{
    SOCKET s;
    struct sockaddr_in a;
    unsigned long nb;
    int one;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));

    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;                 // ephemeral source port; discovery replies here
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return 0;
    }

    nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    s_sock = s;
    return 1;
}

static int sendTo(unsigned long ip, const char* msg, int len)
{
    struct sockaddr_in to;
    int r;
    if (s_sock == INVALID_SOCKET) return 0;

    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(XRGB_PORT);
    to.sin_addr.s_addr = ip;
    r = sendto(s_sock, msg, len, 0, (struct sockaddr*)&to, sizeof(to));
    return (r == len) ? 1 : 0;
}

static void sendDiscovery(void)
{
    static const char q[] = "RGBDISC?";
    sendTo(0xFFFFFFFFUL, q, (int)(sizeof(q) - 1));
}

void XboxRgb_Init(void)
{
    closeSock();
    s_wasUp = 0;
    s_present = 0;
    s_rgbIp = 0;
    s_nextDisc = 0;
    s_lastSeen = 0;
}

void XboxRgb_Tick(void)
{
    DWORD now;
    int up, i;
    char rx[XRGB_RX_MAX];

    up = Net_IsUp();
    if (!up) {
        if (s_wasUp) closeSock();
        s_wasUp = 0;
        s_present = 0;
        s_rgbIp = 0;
        return;
    }

    if (!s_wasUp) {
        closeSock();
        if (!openSock()) return;
        s_wasUp = 1;
        s_present = 0;
        s_rgbIp = 0;
        s_nextDisc = 0;
    }
    if (s_sock == INVALID_SOCKET && !openSock()) return;

    now = GetTickCount();
    if ((LONG)(now - s_nextDisc) >= 0) {
        sendDiscovery();
        s_nextDisc = now + (s_present ? XRGB_DISC_SLOW_MS : XRGB_DISC_FAST_MS);
    }

    // A few non-blocking receives per frame are enough; discovery is tiny.
    for (i = 0; i < 4; ++i) {
        struct sockaddr_in from;
        int fromLen, n;
        ZeroMemory(&from, sizeof(from));
        fromLen = sizeof(from);
        n = recvfrom(s_sock, rx, XRGB_RX_MAX - 1, 0,
            (struct sockaddr*)&from, &fromLen);
        if (n <= 0) break;
        rx[n] = 0;

        // Existing XBOX-RGB discovery is enough. Older firmware simply ignores
        // the EOSB event packet.
        if (strContainsN(rx, n, "XBOX RGB") &&
            strContainsN(rx, n, "\"op\":\"discover\"")) {
            s_rgbIp = from.sin_addr.s_addr;
            s_present = 1;
            s_lastSeen = now;
        }
    }

    if (s_present && (DWORD)(now - s_lastSeen) > XRGB_STALE_MS) {
        s_present = 0;
        s_rgbIp = 0;
    }
}

int XboxRgb_Present(void) { return s_present; }

int XboxRgb_BankEvent(int bank, unsigned int rgb, unsigned long durationMs)
{
    unsigned char pkt[9];
    unsigned long dst;
    unsigned int secs;
    int ok;

    if (!Net_IsUp()) return 0;
    if (s_sock == INVALID_SOCKET && !openSock()) return 0;

    if (bank < 0) bank = 0;
    if (bank > 15) bank = 15;
    rgb &= 0x00FFFFFFu;
    if (durationMs < 3000UL) durationMs = 3000UL;
    if (durationMs > 10000UL) durationMs = 10000UL;
    secs = (unsigned int)((durationMs + 500UL) / 1000UL);
    if (secs < 3) secs = 3;
    if (secs > 10) secs = 10;

    pkt[0] = 'E'; pkt[1] = 'O'; pkt[2] = 'S'; pkt[3] = 'B';
    pkt[4] = (unsigned char)bank;
    pkt[5] = (unsigned char)((rgb >> 16) & 0xFF);
    pkt[6] = (unsigned char)((rgb >> 8) & 0xFF);
    pkt[7] = (unsigned char)(rgb & 0xFF);
    pkt[8] = (unsigned char)secs;

    dst = s_present ? s_rgbIp : 0xFFFFFFFFUL;
    ok = sendTo(dst, (const char*)pkt, sizeof(pkt));
    if (!ok && s_present) {
        s_present = 0;
        s_rgbIp = 0;
        ok = sendTo(0xFFFFFFFFUL, (const char*)pkt, sizeof(pkt));
    }
    return ok;
}