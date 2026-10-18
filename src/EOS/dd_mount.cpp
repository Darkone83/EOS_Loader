/*---------------------------------------------------------------------------
    dd_mount.cpp -- map HDD partitions to drive letters via IoCreateSymbolicLink.

    Faithful port of XbDiag's FileExplorerMU.cpp::MountHDDDrives(): the same
    partition map and the same counted-string (XBOX_STRING) setup the kernel
    expects. F:/G: are the extended partitions that only exist on larger
    drives; if a partition isn't present the symlink simply leads nowhere and
    FindFirstFile on that letter fails harmlessly, so the scan skips it.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_mount.h"
#include "eos_config.h"  /* EOS-owned saved Drive Setup, bank 0xC */
#include "eos_hdd.h"     /* physical slave ATA IDENTIFY */

/* STRING (kernel counted-string) and the Io*SymbolicLink externs now come from
   xboxinternals.h. Local alias keeps the existing call sites readable. */
typedef STRING XBOX_STRING;

struct DriveMap { const char* letter; const char* device; };

static const DriveMap k_primaryDrives[] = {
    { "C", "\\Device\\Harddisk0\\Partition2" },
    { "E", "\\Device\\Harddisk0\\Partition1" },
    { "F", "\\Device\\Harddisk0\\Partition6" },
    { "G", "\\Device\\Harddisk0\\Partition7" },
    { "X", "\\Device\\Harddisk0\\Partition3" },
    { "Y", "\\Device\\Harddisk0\\Partition4" },
    { "Z", "\\Device\\Harddisk0\\Partition5" }
};

/* HDD1 aliases intentionally stay out of the normal Xbox drive-letter set.
   P/Q/R/V map the secondary C/E/F/G volumes respectively. These are also the
   aliases used by the WebUI, FTP root, Drive Info and file browser. */
static const DriveMap k_secondaryDrives[] = {
    { "P", "\\Device\\Harddisk1\\Partition2" }, /* HDD1-C */
    { "Q", "\\Device\\Harddisk1\\Partition1" }, /* HDD1-E */
    { "R", "\\Device\\Harddisk1\\Partition6" }, /* HDD1-F */
    { "V", "\\Device\\Harddisk1\\Partition7" }  /* HDD1-G */
};

#define DD_PRIMARY_COUNT   ((int)(sizeof(k_primaryDrives) / sizeof(k_primaryDrives[0])))
#define DD_SECONDARY_COUNT ((int)(sizeof(k_secondaryDrives) / sizeof(k_secondaryDrives[0])))

static unsigned int s_hdd1Mask = 0;
static unsigned int s_hdd1OwnedMask = 0;
static int s_hdd1AtaPresent = 0;

static void unmountOne(const DriveMap* m)
{
    char linkBuf[8];
    XBOX_STRING link;
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = m->letter[0]; linkBuf[5] = ':'; linkBuf[6] = 0;
    link.Length = 6; link.MaximumLength = 7; link.Buffer = linkBuf;
    IoDeleteSymbolicLink(&link);
}

/* Match PrometheOS drive::mount(): link a kernel partition object, then
   verify it with GetDiskFreeSpaceExA. Do not claim success on alias alone. */
static int mountOne(const DriveMap* m, int requirePresent, int alreadyOwned)
{
    char linkBuf[8];
    char root[4];
    int devLen;
    XBOX_STRING sLink;
    XBOX_STRING sDev;
    ULARGE_INTEGER totalBytes;

    if (!m) return 0;
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = m->letter[0]; linkBuf[5] = ':'; linkBuf[6] = 0;
    devLen = 0; while (m->device[devLen]) ++devLen;
    sLink.Length = 6; sLink.MaximumLength = 7; sLink.Buffer = linkBuf;
    sDev.Length = (USHORT)devLen; sDev.MaximumLength = (USHORT)(devLen + 1);
    sDev.Buffer = (char*)m->device;

    /* Never tear down a volume which EOS itself already mounted and which
       may have active file handles (FTP, browser, WebUI). */
    if (!alreadyOwned) {
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) {
            /* Do not replace unknown pre-existing P/Q/R/V aliases. */
            return 0;
        }
    }
    if (!requirePresent) return 1;

    root[0] = m->letter[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    totalBytes.QuadPart = 0;
    if (GetDiskFreeSpaceExA(root, 0, &totalBytes, 0) && totalBytes.QuadPart != 0)
        return 1;

    /* The alias was ours, so cleanup cannot remove a foreign link. */
    IoDeleteSymbolicLink(&sLink);
    return 0;
}

void Mount_UnmountHddPartitionsForDisk(int disk)
{
    int i;
    if (disk == 0) {
        for (i = 0; i < DD_PRIMARY_COUNT; ++i)
            unmountOne(&k_primaryDrives[i]);
        return;
    }
    if (disk != 1) return;
    /* Leave HDD0 mounted while an explicitly selected HDD1 is formatted. */
    for (i = 0; i < DD_SECONDARY_COUNT; ++i)
        if (s_hdd1OwnedMask & (1u << i)) unmountOne(&k_secondaryDrives[i]);
    s_hdd1OwnedMask = 0;
    s_hdd1Mask = 0;
}

void Mount_UnmountHddPartitions(void)
{
    Mount_UnmountHddPartitionsForDisk(0);
    Mount_UnmountHddPartitionsForDisk(1);
}

void Mount_HddPartitions(void)
{
    int i;
    EosHddInfo info;
    unsigned long long sectors = 0;
    unsigned int nextMask = 0;

    /* Existing primary handling is preserved, but active aliases are no
       longer torn down on every refresh. */
    for (i = 0; i < DD_PRIMARY_COUNT; ++i)
        mountOne(&k_primaryDrives[i], 0, 0);

    /* Apply the EOS flash-backed Drive Setup value, rather than merely
       displaying/saving it. Mode 3 enables probing the IDE slave. */
    if (Config_GetDriveSetup() != EOS_DRIVE_DUAL_HDD) {
        for (i = 0; i < DD_SECONDARY_COUNT; ++i)
            if (s_hdd1OwnedMask & (1u << i)) unmountOne(&k_secondaryDrives[i]);
        s_hdd1Mask = 0;
        s_hdd1OwnedMask = 0;
        s_hdd1AtaPresent = 0;
        return;
    }

    s_hdd1AtaPresent = (Hdd_IdentifyDisk(1, &info, &sectors) == HDD_OK && sectors > 0);
    if (!s_hdd1AtaPresent) {
        for (i = 0; i < DD_SECONDARY_COUNT; ++i)
            if (s_hdd1OwnedMask & (1u << i)) unmountOne(&k_secondaryDrives[i]);
        s_hdd1Mask = 0;
        s_hdd1OwnedMask = 0;
        return;
    }

    for (i = 0; i < DD_SECONDARY_COUNT; ++i) {
        unsigned int bit = 1u << i;
        if (mountOne(&k_secondaryDrives[i], 1, (s_hdd1OwnedMask & bit) != 0)) {
            nextMask |= bit;
            s_hdd1OwnedMask |= bit;
        }
        else s_hdd1OwnedMask &= ~bit;
    }
    s_hdd1Mask = nextMask;
}

int Mount_Hdd1AtaPresent(void) { return s_hdd1AtaPresent; }

unsigned int Mount_Hdd1MappedMask(void)
{
    return s_hdd1Mask;
}

int Mount_Hdd1Available(void)
{
    return s_hdd1Mask ? 1 : 0;
}

char Mount_Hdd1AliasForPart(char part)
{
    if (part >= 'a' && part <= 'z') part = (char)(part - 32);
    if (part == 'C') return 'P';
    if (part == 'E') return 'Q';
    if (part == 'F') return 'R';
    if (part == 'G') return 'V';
    return 0;
}

char Mount_Hdd1PartForAlias(char alias)
{
    if (alias >= 'a' && alias <= 'z') alias = (char)(alias - 32);
    if (alias == 'P') return 'C';
    if (alias == 'Q') return 'E';
    if (alias == 'R') return 'F';
    if (alias == 'V') return 'G';
    return 0;
}

static const char* deviceForDrive(char drive)
{
    int i;
    for (i = 0; i < DD_PRIMARY_COUNT; ++i)
        if (k_primaryDrives[i].letter[0] == drive) return k_primaryDrives[i].device;
    for (i = 0; i < DD_SECONDARY_COUNT; ++i)
        if (k_secondaryDrives[i].letter[0] == drive) return k_secondaryDrives[i].device;
    return 0;
}

/*---------------------------------------------------------------------------
    Mount_SelfToD -- re-point D: at this XBE's own directory.

    XeImageFileName holds the device path the running image was loaded from,
    e.g. "\Device\Harddisk0\Partition2\xboxdash.xbe". We strip the trailing
    filename to get the directory device path and symlink "\??\D:" to it, so
    every "D:\..." asset path resolves to our install folder no matter how we
    were launched (title vs dashboard-from-Cerbios).
---------------------------------------------------------------------------*/
int Mount_SelfToD(void) {
    char  devPath[300];
    char  linkBuf[8];
    const char* src;
    int   srcLen, lastBs, i, j;

    if (!XeImageFileName || !XeImageFileName->Buffer) return 0;
    src = XeImageFileName->Buffer;
    srcLen = (int)XeImageFileName->Length;          /* counted, may not be NUL-terminated */
    if (srcLen <= 0 || srcLen > (int)sizeof(devPath) - 1) return 0;

    /* find the last backslash -> end of the directory part */
    lastBs = -1;
    for (i = 0; i < srcLen; ++i) if (src[i] == '\\') lastBs = i;
    if (lastBs <= 0) return 0;                       /* no directory component */

    /* copy "\Device\...\PartitionN<...dirs...>" up to (not incl) last backslash */
    for (j = 0; j < lastBs && j < (int)sizeof(devPath) - 1; ++j) devPath[j] = src[j];
    devPath[j] = '\0';

    /* remap "\??\D:" -> devPath */
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = 'D';  linkBuf[5] = ':';  linkBuf[6] = '\0';
    {
        int devLen = 0; while (devPath[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };
        IoDeleteSymbolicLink(&sLink);               /* drop any existing D: mapping */
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) return 0;
    }
    return 1;
}

/*---------------------------------------------------------------------------
    Mount_LaunchXbe -- boot the XBE at a DOS path (e.g. "E:\Apps\Foo\default.xbe").

    XLaunchNewImage only launches reliably from D: on RXDK, so we remap D: to
    the device path of the partition holding the XBE, then launch
    "D:\<filename>" -- the same pattern XbDiag's file explorer uses. On success
    this never returns. Returns non-zero only on failure (unknown drive, bad
    path, remap failure, or a launch that bounced back).
---------------------------------------------------------------------------*/
int Mount_LaunchXbe(const char* dosPath) {
    char        drive;
    const char* device = NULL;
    int         i, len, lastBs, j, k;
    char        devPath[300];
    char        launchPath[64];
    char        linkBuf[8];

    if (!dosPath || !dosPath[0]) return 1;
    len = 0; while (dosPath[len]) ++len;
    if (len <= 3 || dosPath[1] != ':' || (dosPath[2] != '\\' && dosPath[2] != '/')) return 3;

    drive = (dosPath[0] >= 'a' && dosPath[0] <= 'z')
        ? (char)(dosPath[0] - 32) : dosPath[0];

    /* D: is the running title's own device -- launch directly, no remap. */
    if (drive == 'D') { XLaunchNewImage((char*)dosPath, NULL); return 1; }

    /* The optical disc (S:) isn't an HDD partition, so it's not in k_drives;
       it lives at \Device\Cdrom0. Resolve it explicitly so a game disc can be
       launched (remap D: -> Cdrom0, then XLaunchNewImage D:\default.xbe). */
    if (drive == 'S') {
        device = "\\Device\\Cdrom0";
    }
    else {
        device = deviceForDrive(drive);
    }
    if (!device) return 2;

    lastBs = -1;
    for (i = 0; i < len; ++i) if (dosPath[i] == '\\') lastBs = i;
    if (lastBs < 2 || lastBs >= len - 1) return 3; /* need at least "X:\file" */

    /* devPath = device prefix + directory part after "X:". Refuse a path that
       does not fit rather than mounting a silently truncated directory. */
    j = 0;
    while (device[j]) {
        if (j >= (int)sizeof(devPath) - 1) return 3;
        devPath[j] = device[j];
        j++;
    }
    for (k = 2; k < lastBs; ++k) {
        if (j >= (int)sizeof(devPath) - 1) return 3;
        devPath[j++] = dosPath[k];
    }
    devPath[j] = '\0';

    /* remap "\??\D:" -> devPath */
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = 'D';  linkBuf[5] = ':';  linkBuf[6] = '\0';
    {
        int devLen = 0; while (devPath[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };
        IoDeleteSymbolicLink(&sLink);
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) {
            /* D: was removed before the failed remap; put DarkDash back on its
               own asset directory before returning to the caller. */
            Mount_SelfToD();
            return 4;
        }
    }

    /* build "D:\<filename>" from the part after the last backslash */
    /* A FATX leaf should already be small, but keep this exact: never turn an
       overlong launch request into a different/truncated filename. */
    if ((len - (lastBs + 1)) + 3 >= (int)sizeof(launchPath)) {
        Mount_SelfToD();
        return 3;
    }
    k = 0;
    launchPath[k++] = 'D'; launchPath[k++] = ':'; launchPath[k++] = '\\';
    for (i = lastBs + 1; dosPath[i]; ++i)
        launchPath[k++] = dosPath[i];
    launchPath[k] = '\0';

    XLaunchNewImage(launchPath, NULL);

    /* A successful launch never returns. If it does, immediately restore the
       dashboard's asset mount before the caller resumes rendering. */
    Mount_SelfToD();
    return 5;
}