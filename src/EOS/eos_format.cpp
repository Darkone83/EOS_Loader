/*---------------------------------------------------------------------------
    eos_format.cpp -- HDD staging (partition + format a fresh drive).

    Ported from the PrometheOS harddrive formatDrive path. Staging a new drive
    does NOT need the LBA48 patch-table read (that only recovers an existing
    table); we build the standard layout from disk geometry and write it:

        geometry   : IOCTL_DISK_GET_DRIVE_GEOMETRY  -> total sectors
        table      : build standard Xbox layout (E,C,X,Y,Z,[F])
        write      : table -> Partition0 sector 0  +  backup at end of disk
        format     : XapiFormatFATVolumeEx each partition (+ F: cluster fixup)
        mount      : recreate the \??\<letter>: symlinks

    No CRT, no heap. The partition table is exactly one sector (512 bytes).
---------------------------------------------------------------------------*/
#include "eos_format.h"

#ifndef EOS_HOST_TEST
#include <xtl.h>
#include "xboxinternals.h"   /* STRING, kernel exports, FAT_VOLUME_METADATA */

/* NT object/file flags the RXDK headers don't define */
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE           0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_ALERT
#define FILE_SYNCHRONOUS_IO_ALERT      0x00000010
#endif
#ifndef FILE_NO_INTERMEDIATE_BUFFERING
#define FILE_NO_INTERMEDIATE_BUFFERING 0x00000008
#endif
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ                0x00000001
#endif
#ifndef FILE_SHARE_WRITE
#define FILE_SHARE_WRITE               0x00000002
#endif
#ifndef SYNCHRONIZE
#define SYNCHRONIZE                    0x00100000
#endif
#ifndef GENERIC_READ
#define GENERIC_READ                   0x80000000
#endif
#ifndef GENERIC_WRITE
#define GENERIC_WRITE                  0x40000000
#endif
#endif

/* ===== on-disk partition table (prom XboxPartitionTable, 512 bytes) ====== */
typedef struct {
    unsigned char  Name[16];
    unsigned int   Flags;        /* 32-bit on MSVC2003 and host (unsigned long is 64-bit on host) */
    unsigned int   LBAStartLo;
    unsigned int   LBASizeLo;
    unsigned short LBAStartHi;
    unsigned short LBASizeHi;
} EosPartEntry;                                  /* 32 bytes */

typedef struct {
    unsigned char  Magic[16];
    unsigned char  FatxMode;
    char           Res0[31];
    EosPartEntry   TableEntries[14];
    char           Res1[16];
} EosPartTable;                                  /* 16+1+31 + 14*32 + 16 = 512 */

#define EOS_PART_NOTINUSE 0x00000000UL
#define EOS_PART_INUSE    0x80000000UL

/* standard Xbox geometry (sectors) -- prom PARTITION_XBOX_* */
#define CACHE_X_START 0x00000400UL
#define CACHE_SIZE    0x00177000UL
#define CACHE_Y_START (CACHE_X_START + CACHE_SIZE)               /* 0x177400 */
#define CACHE_Z_START (CACHE_Y_START + CACHE_SIZE)               /* 0x2EE400 */
#define SHELL_C_START (CACHE_Z_START + CACHE_SIZE)               /* 0x465400 */
#define SHELL_C_SIZE  0x000FA000UL
#define DATA_E_START  (SHELL_C_START + SHELL_C_SIZE)             /* 0x55F400 */
#define DATA_E_SIZE   0x009896B0UL
#define DATA_F_START  (DATA_E_START + DATA_E_SIZE)               /* 0xEE8AB0 */

#define EOS_CLUSTER_16K 0x00004000UL
#define EOS_IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000UL

/* drive letters that make up the standard layout, in table order +1 */
static const char s_lyrLetter[6] = { 'E','C','X','Y','Z','F' };
static const int  s_lyrPartNum[6] = { 1,  2,  3,  4,  5,  6 };

#ifndef EOS_HOST_TEST
typedef struct {
    LARGE_INTEGER Cylinders;     /* total sectors */
    ULONG MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector;
} EOS_DISK_GEOMETRY;
#endif

/* ---- no-CRT helpers ----------------------------------------------------- */
static void fmtMemSet(void* d, int v, int n) { unsigned char* p = (unsigned char*)d; int i; for (i = 0; i < n; ++i) p[i] = (unsigned char)v; }
static int  fmtMemCmp(const void* a, const void* b, int n) { const unsigned char* x = (const unsigned char*)a, * y = (const unsigned char*)b; int i; for (i = 0; i < n; ++i) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }
static int  fmtFirstSetBit(unsigned long v) { int s = 0; if (!v) return 0; while (((v >> s) & 1UL) == 0 && s < 31) ++s; return s; }

/* "\Device\Harddisk0\PartitionN" -> dst(>=32) */
static void fmtDevicePath(int partNum, char* dst) { const char* b = "\\Device\\Harddisk0\\Partition"; int i = 0; while (b[i]) { dst[i] = b[i]; ++i; } dst[i++] = (char)('0' + partNum); dst[i] = 0; }
/* "\??\X:" -> dst(>=8) */
static void fmtMountPath(char c, char* dst) { if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); dst[0] = '\\'; dst[1] = '?'; dst[2] = '?'; dst[3] = '\\'; dst[4] = c; dst[5] = ':'; dst[6] = 0; }

/* ---- partition-table builders (pure, host-testable) --------------------- */
static void fmtSetName(const char* name, EosPartEntry* e)
{
    int i; fmtMemSet(e->Name, 0x20, 16);
    for (i = 0; i < 16 && name[i]; ++i) e->Name[i] = (unsigned char)name[i];
    e->Flags = (unsigned int)EOS_PART_INUSE;
}
static void fmtSetStart(unsigned long long start, EosPartEntry* e) { e->LBAStartLo = (unsigned int)(start & 0xFFFFFFFFULL); e->LBAStartHi = (unsigned short)((start >> 32) & 0xFFFFULL); }
static void fmtSetSize(unsigned long long size, EosPartEntry* e) { e->LBASizeLo = (unsigned int)(size & 0xFFFFFFFFULL); e->LBASizeHi = (unsigned short)((size >> 32) & 0xFFFFULL); }
static unsigned long long fmtGetSize(const EosPartEntry* e) { return (((unsigned long long)e->LBASizeHi) << 32) | (unsigned long long)e->LBASizeLo; }
static unsigned long long fmtGetStart(const EosPartEntry* e) { return (((unsigned long long)e->LBAStartHi) << 32) | (unsigned long long)e->LBAStartLo; }

/* prom calculateClusterSize: 16 KB until lbaSize exceeds 0x20000000 sectors. */
static unsigned long fmtCalcClusterKB(unsigned long long lbaSizeSectors)
{
    unsigned long kb = 16; unsigned long long cmp = 0x20000000ULL;
    while (lbaSizeSectors > cmp) { cmp <<= 1; kb <<= 1; }
    return kb;
}

/* Build the standard Xbox table for a disk of totalSectors. F: takes the
   remainder past DATA_F_START, minus one sector reserved for the backup table. */
static void fmtBuildTable(unsigned long long totalSectors, EosPartTable* t)
{
    long long fSize = (long long)totalSectors - (long long)DATA_F_START;
    int i;
    fmtMemSet(t, 0, sizeof(EosPartTable));
    for (i = 0; i < 16; ++i) t->Magic[i] = (unsigned char)"****PARTINFO****"[i];
    t->FatxMode = 1;                                 /* Harddisk0 */

    fmtSetName("XBOX DATA E", &t->TableEntries[0]);  fmtSetStart(DATA_E_START, &t->TableEntries[0]); fmtSetSize(DATA_E_SIZE, &t->TableEntries[0]);
    fmtSetName("XBOX SHELL C", &t->TableEntries[1]);  fmtSetStart(SHELL_C_START, &t->TableEntries[1]); fmtSetSize(SHELL_C_SIZE, &t->TableEntries[1]);
    fmtSetName("XBOX CACHE X", &t->TableEntries[2]);  fmtSetStart(CACHE_X_START, &t->TableEntries[2]); fmtSetSize(CACHE_SIZE, &t->TableEntries[2]);
    fmtSetName("XBOX CACHE Y", &t->TableEntries[3]);  fmtSetStart(CACHE_Y_START, &t->TableEntries[3]); fmtSetSize(CACHE_SIZE, &t->TableEntries[3]);
    fmtSetName("XBOX CACHE Z", &t->TableEntries[4]);  fmtSetStart(CACHE_Z_START, &t->TableEntries[4]); fmtSetSize(CACHE_SIZE, &t->TableEntries[4]);

    if (fSize > 0) {
        fmtSetName("DRIVE F", &t->TableEntries[5]);
        fmtSetStart(DATA_F_START, &t->TableEntries[5]);
        fmtSetSize((unsigned long long)(fSize - 1), &t->TableEntries[5]);
    }
}

const char* Format_ErrStr(int code)
{
    switch (code) {
    case FMT_OK:        return "OK";
    case FMT_ERR_GEOM:  return "No disk / geometry read failed";
    case FMT_ERR_TABLE: return "Partition table write failed";
    case FMT_ERR_FORMAT:return "Partition format failed";
    case FMT_ERR_FIXUP: return "Cluster fixup failed";
    default:            return "Unknown error";
    }
}

#ifndef EOS_HOST_TEST
/* ---- device-side primitives -------------------------------------------- */
static int fmtReadGeometry(unsigned long long* totalSectors, unsigned long* bytesPerSector)
{
    STRING dev; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; NTSTATUS st;
    EOS_DISK_GEOMETRY g; char path[40];
    fmtDevicePath(0, path); RtlInitAnsiString(&dev, path);
    oa.RootDirectory = 0; oa.ObjectName = &dev; oa.Attributes = OBJ_CASE_INSENSITIVE;
    st = NtOpenFile(&h, (GENERIC_READ | 0x00100000), &oa, &iosb, (FILE_SHARE_READ | FILE_SHARE_WRITE), 0x10);
    if (st != STATUS_SUCCESS) return 0;
    fmtMemSet(&g, 0, sizeof(g));
    st = NtDeviceIoControlFile(h, 0, 0, 0, &iosb, EOS_IOCTL_DISK_GET_DRIVE_GEOMETRY, 0, 0, &g, sizeof(g));
    NtClose(h);
    if (st != STATUS_SUCCESS) return 0;
    if (totalSectors)   *totalSectors = (unsigned long long)g.Cylinders.QuadPart;
    if (bytesPerSector) *bytesPerSector = g.BytesPerSector ? g.BytesPerSector : 512;
    return 1;
}

/* Write the table to sector 0 and a backup at the end of the disk. */
static int fmtWriteTable(const EosPartTable* t, unsigned long long totalSectors, unsigned long bps)
{
    STRING dev; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; NTSTATUS st;
    LARGE_INTEGER off; char path[40];
    unsigned long long totalBytes = totalSectors * (unsigned long long)bps;

    fmtDevicePath(0, path); RtlInitAnsiString(&dev, path);
    InitializeObjectAttributes(&oa, &dev, OBJ_CASE_INSENSITIVE, 0);
    st = NtOpenFile(&h, (SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE), &oa, &iosb,
        (FILE_SHARE_READ | FILE_SHARE_WRITE), FILE_SYNCHRONOUS_IO_ALERT);
    if (st != STATUS_SUCCESS) return 0;

    off.QuadPart = 0;
    st = NtWriteFile(h, 0, 0, 0, &iosb, (PVOID)t, sizeof(EosPartTable), &off);
    if (st != STATUS_SUCCESS) { NtClose(h); return 0; }

    if (totalBytes > sizeof(EosPartTable)) {
        off.QuadPart = (LONGLONG)(totalBytes - sizeof(EosPartTable));
        st = NtWriteFile(h, 0, 0, 0, &iosb, (PVOID)t, sizeof(EosPartTable), &off);
        if (st != STATUS_SUCCESS) { NtClose(h); return 0; }
    }
    NtClose(h);
    return 1;
}

/* prom largePartitionFixup: correct on-disk SectorsPerCluster (0) for big F:. */
static int fmtLargeFixup(unsigned long long lbaStart, unsigned long clusterBytes)
{
    STRING dev; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; NTSTATUS st;
    EOS_DISK_GEOMETRY g; LARGE_INTEGER off; int sh;
    unsigned long readLen = (unsigned long)ROUND_TO_PAGES(sizeof(FAT_VOLUME_METADATA));
    FAT_VOLUME_METADATA* meta; char path[40]; int ok = 0;
    static unsigned char s_buf[0x2000];
    { unsigned long a = (unsigned long)(&s_buf[0]); a = (a + (PAGE_SIZE - 1)) & ~((unsigned long)(PAGE_SIZE - 1)); meta = (FAT_VOLUME_METADATA*)a; }

    fmtDevicePath(0, path); RtlInitAnsiString(&dev, path);
    InitializeObjectAttributes(&oa, &dev, OBJ_CASE_INSENSITIVE, 0);
    st = NtOpenFile(&h, (SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE), &oa, &iosb,
        (FILE_SHARE_READ | FILE_SHARE_WRITE), (FILE_SYNCHRONOUS_IO_ALERT | FILE_NO_INTERMEDIATE_BUFFERING));
    if (st != STATUS_SUCCESS) return 0;

    fmtMemSet(&g, 0, sizeof(g));
    st = NtDeviceIoControlFile(h, 0, 0, 0, &iosb, EOS_IOCTL_DISK_GET_DRIVE_GEOMETRY, 0, 0, &g, sizeof(g));
    if (st != STATUS_SUCCESS) { NtClose(h); return 0; }
    sh = fmtFirstSetBit(g.BytesPerSector ? g.BytesPerSector : 512);

    off.QuadPart = (LONGLONG)lbaStart; off.QuadPart <<= sh;
    st = NtReadFile(h, 0, 0, 0, &iosb, meta, readLen, &off);
    if (st != STATUS_SUCCESS) { NtClose(h); return 0; }

    if (fmtMemCmp(&meta->Signature, "FATX", 4) == 0 && meta->SectorsPerCluster == 0) {
        meta->SectorsPerCluster = (unsigned long)(clusterBytes >> sh);
        off.QuadPart = (LONGLONG)lbaStart; off.QuadPart <<= sh;
        st = NtWriteFile(h, 0, 0, 0, &iosb, meta, readLen, &off);
        ok = (st == STATUS_SUCCESS);
    }
    else ok = 1;

    NtClose(h);
    return ok;
}

static void fmtUnmount(char letter, int partNum)
{
    STRING dev, mnt; char dp[40], mp[8];
    fmtDevicePath(partNum, dp); fmtMountPath(letter, mp);
    RtlInitAnsiString(&dev, dp); RtlInitAnsiString(&mnt, mp);
    IoDeleteSymbolicLink(&mnt);          /* best effort */
    IoDismountVolumeByName(&dev);        /* best effort */
}
static void fmtMount(char letter, int partNum)
{
    STRING dev, mnt; char dp[40], mp[8];
    fmtDevicePath(partNum, dp); fmtMountPath(letter, mp);
    RtlInitAnsiString(&dev, dp); RtlInitAnsiString(&mnt, mp);
    IoCreateSymbolicLink(&mnt, &dev);
}
#endif /* !EOS_HOST_TEST */

int Format_PlanInfo(unsigned long* totalMB, unsigned long* dataEMB, unsigned long* driveFMB)
{
#ifdef EOS_HOST_TEST
    if (totalMB)  *totalMB = 0;
    if (dataEMB)  *dataEMB = (unsigned long)(DATA_E_SIZE / 2048);
    if (driveFMB) *driveFMB = 0;
    return 0;
#else
    unsigned long long total = 0; unsigned long bps = 512;
    if (!fmtReadGeometry(&total, &bps) || total == 0) return 0;
    if (totalMB)  *totalMB = (unsigned long)(total / 2048ULL);   /* sectors*512/1MB */
    if (dataEMB)  *dataEMB = (unsigned long)(DATA_E_SIZE / 2048);
    if (driveFMB) *driveFMB = (total > DATA_F_START)
        ? (unsigned long)(((total - DATA_F_START) - 1ULL) / 2048ULL) : 0;
    return 1;
#endif
}

int Format_StageDrive(void)
{
#ifdef EOS_HOST_TEST
    return FMT_OK;
#else
    EosPartTable table;
    unsigned long long total = 0; unsigned long bps = 512;
    int i, rc = FMT_OK;

    if (!fmtReadGeometry(&total, &bps) || total == 0) return FMT_ERR_GEOM;

    fmtBuildTable(total, &table);

    /* drop every drive letter so the table write + formats have the disk. */
    for (i = 0; i < 6; ++i) fmtUnmount(s_lyrLetter[i], s_lyrPartNum[i]);

    if (!fmtWriteTable(&table, total, bps)) { rc = FMT_ERR_TABLE; goto remount; }

    for (i = 0; i < 14; ++i) {
        EosPartEntry* e = &table.TableEntries[i];
        unsigned long long lbaSize;
        unsigned long clusterBytes;
        char dp[40]; STRING devStr; BOOL ok;

        if (e->Flags == EOS_PART_NOTINUSE) continue;

        lbaSize = fmtGetSize(e);
        clusterBytes = fmtCalcClusterKB(lbaSize) << 10;

        fmtDevicePath(i + 1, dp);
        RtlInitAnsiString(&devStr, dp);
        ok = XapiFormatFATVolumeEx(&devStr, clusterBytes);
        if (ok == FALSE) { rc = FMT_ERR_FORMAT; goto remount; }

        /* prom: entries at index >= 5 (F: and beyond) get the cluster fixup. */
        if (i >= 5) {
            if (!fmtLargeFixup(fmtGetStart(e), clusterBytes)) { rc = FMT_ERR_FIXUP; goto remount; }
        }
    }

remount:
    for (i = 0; i < 6; ++i) {
        EosPartEntry* e = &table.TableEntries[i];      /* only remount in-use letters */
        if (e->Flags != EOS_PART_NOTINUSE)
            fmtMount(s_lyrLetter[i], s_lyrPartNum[i]);
    }
    return rc;
#endif
}

/* ---- host self-test ----------------------------------------------------- */
#ifdef EOS_HOST_TEST
#include <stdio.h>
static int chkEntry(EosPartTable* t, int idx, const char* name, unsigned long long start, unsigned long long size)
{
    EosPartEntry* e = &t->TableEntries[idx]; int f = 0;
    char nm[17]; int i; for (i = 0; i < 16; ++i) nm[i] = (char)e->Name[i]; nm[16] = 0;
    if (e->Flags != EOS_PART_INUSE) { printf("FAIL e%d flags %08X\n", idx, e->Flags); ++f; }
    if (fmtGetStart(e) != start) { printf("FAIL e%d start %llX want %llX\n", idx, fmtGetStart(e), start); ++f; }
    if (fmtGetSize(e) != size) { printf("FAIL e%d size %llX want %llX\n", idx, fmtGetSize(e), size); ++f; }
    /* name is space-padded; compare the meaningful prefix */
    { int n = 0; while (name[n]) ++n; if (fmtMemCmp(nm, name, n) != 0) { printf("FAIL e%d name '%s'\n", idx, nm); ++f; } }
    return f;
}
int main(void)
{
    int fails = 0;
    EosPartTable t;

    /* struct must be exactly one sector */
    if (sizeof(EosPartEntry) != 32) { printf("FAIL entry size %d\n", (int)sizeof(EosPartEntry)); ++fails; }
    if (sizeof(EosPartTable) != 512) { printf("FAIL table size %d\n", (int)sizeof(EosPartTable)); ++fails; }

    /* derived layout constants */
    if (DATA_F_START != 0xEE8AB0UL) { printf("FAIL F_START %lX\n", DATA_F_START); ++fails; }
    if (SHELL_C_START != 0x465400UL) { printf("FAIL C_START %lX\n", SHELL_C_START); ++fails; }
    if (DATA_E_START != 0x55F400UL) { printf("FAIL E_START %lX\n", DATA_E_START); ++fails; }

    /* large drive (500 GB ~ 976,562,500 sectors) -> F present */
    {
        unsigned long long total = 976562500ULL;
        unsigned long long fSize = total - DATA_F_START - 1ULL;
        fmtBuildTable(total, &t);
        if (fmtMemCmp(t.Magic, "****PARTINFO****", 16) != 0) { printf("FAIL magic\n"); ++fails; }
        if (t.FatxMode != 1) { printf("FAIL fatxmode\n"); ++fails; }
        fails += chkEntry(&t, 0, "XBOX DATA E", DATA_E_START, DATA_E_SIZE);
        fails += chkEntry(&t, 1, "XBOX SHELL C", SHELL_C_START, SHELL_C_SIZE);
        fails += chkEntry(&t, 2, "XBOX CACHE X", CACHE_X_START, CACHE_SIZE);
        fails += chkEntry(&t, 3, "XBOX CACHE Y", CACHE_Y_START, CACHE_SIZE);
        fails += chkEntry(&t, 4, "XBOX CACHE Z", CACHE_Z_START, CACHE_SIZE);
        fails += chkEntry(&t, 5, "DRIVE F", DATA_F_START, fSize);
        if (t.TableEntries[6].Flags != EOS_PART_NOTINUSE) { printf("FAIL e6 should be unused\n"); ++fails; }
    }

    /* cluster doubling threshold is 0x20000000 sectors (256 GB) */
    if (fmtCalcClusterKB(0x20000000ULL) != 16) { printf("FAIL cluster @256G\n"); ++fails; }
    if (fmtCalcClusterKB(0x20000001ULL) != 32) { printf("FAIL cluster >256G\n"); ++fails; }
    if (fmtCalcClusterKB(0x40000001ULL) != 64) { printf("FAIL cluster >512G\n"); ++fails; }
    if (fmtCalcClusterKB(0x80000001ULL) != 128) { printf("FAIL cluster >1T\n"); ++fails; }

    /* small drive below F start -> no F entry */
    {
        unsigned long long total = DATA_F_START - 100ULL;
        fmtBuildTable(total, &t);
        if (t.TableEntries[5].Flags != EOS_PART_NOTINUSE) { printf("FAIL small-drive F present\n"); ++fails; }
        fails += chkEntry(&t, 0, "XBOX DATA E", DATA_E_START, DATA_E_SIZE);  /* E still there */
    }

    /* device/mount path building */
    {
        char d[40], m[8]; fmtDevicePath(6, d); fmtMountPath('f', m);
        if (fmtMemCmp(d, "\\Device\\Harddisk0\\Partition6", 28) != 0) { printf("FAIL devpath '%s'\n", d); ++fails; }
        if (fmtMemCmp(m, "\\??\\F:", 7) != 0) { printf("FAIL mntpath '%s'\n", m); ++fails; }
    }

    /* bit scan */
    if (fmtFirstSetBit(512) != 9 || fmtFirstSetBit(4096) != 12) { printf("FAIL bitscan\n"); ++fails; }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL STAGING LOGIC TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
#endif