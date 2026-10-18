/*---------------------------------------------------------------------------
    eos_format.cpp -- HDD staging (partition + format a fresh drive).

    Ported from the PrometheOS harddrive formatDrive path. Supports the
    explicitly selected Xbox kernel Harddisk0 or Harddisk1 device. Detection
    now follows PrometheOS readLbaInfo(): open Partition0, query geometry,
    query the LBA48 patch via 0xCAFEBABE, validate its magic/version, and read
    the live partition-table metadata before building a fresh layout:

        probe      : PrometheOS readLbaInfo() path
        geometry   : IOCTL_DISK_GET_DRIVE_GEOMETRY -> total sectors
        lba48      : IOCTL_CMD_LBA48_ACCESS -> patch metadata/table pointer
        table      : build standard Xbox layout (E,C,X,Y,Z,[F])
        write      : table -> Partition0 sector 0  +  backup at end of disk
        format     : XapiFormatFATVolumeEx each partition (+ F: cluster fixup)
        verify     : physical readback; no HDD1 kernel mount required

    No CRT, no heap. The partition table is exactly one sector (512 bytes).
---------------------------------------------------------------------------*/
#include "eos_format.h"
#include "eos_hdd.h"
#ifndef EOS_HOST_TEST
#include "dd_mount.h"      /* remount detected HDD1 immediately after format */
#endif

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
#define EOS_IOCTL_SUBCMD_GET_INFO          0UL

/* PrometheOS / CerBIOS LBA48 patch query. */
#define EOS_IOCTL_CMD_LBA48_ACCESS               0xCAFEBABEUL
#define EOS_LBA48_GET_INFO_MAGIC1_IDX            0
#define EOS_LBA48_GET_INFO_MAGIC1_VAL            0xCAFEBABEUL
#define EOS_LBA48_GET_INFO_MAGIC2_IDX            1
#define EOS_LBA48_GET_INFO_MAGIC2_VAL            0xBABEFACEUL
#define EOS_LBA48_GET_INFO_PATCHCODE_VERSION_IDX 2
#define EOS_LBA48_GET_INFO_LOWCODE_BASE_IDX      3
#define EOS_LBA48_GET_INFO_HIGHCODE_BASE_IDX     4
#define EOS_LBA48_GET_INFO_PATCHSEG_SIZE_IDX     5
#define EOS_LBA48_GET_INFO_PART_TABLE_OFS_IDX    6

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
static void fmtMemCopy(void* d, const void* s, int n) { unsigned char* x = (unsigned char*)d; const unsigned char* y = (const unsigned char*)s; int i; for (i = 0; i < n; ++i) x[i] = y[i]; }
static int  fmtFirstSetBit(unsigned long v) { int s = 0; if (!v) return 0; while (((v >> s) & 1UL) == 0 && s < 31) ++s; return s; }

/* "\Device\Harddisk0\PartitionN" -> dst(>=32) */
/* These paths must NEVER alias disk 1 to disk 0. The Xbox kernel exposes
   distinct Partition0 devices when a dual-drive BIOS is active. */
static void fmtDevicePath(int disk, int partNum, char* dst)
{
    const char* a = "\\Device\\Harddisk";
    const char* b = "\\Partition";
    int i = 0, j = 0;
    while (a[j]) dst[i++] = a[j++];
    dst[i++] = (char)('0' + disk);
    j = 0; while (b[j]) dst[i++] = b[j++];
    dst[i++] = (char)('0' + partNum);
    dst[i] = 0;
}
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


/* FATX metadata sizing used by both the raw formatter and host regression
   tests. This mirrors nxdk's nxFormatVolume layout with 4 KiB alignment. */
static int fmtFatxPlan(unsigned long long partSectors, unsigned long clusterBytes,
    unsigned long long* fatSectors, unsigned long* rootSectors, int* fat16)
{
    unsigned long clusterSectors;
    unsigned long long numClusters;
    unsigned long long fatBytes;
    unsigned long entryBytes;

    if (clusterBytes < 512UL || (clusterBytes & 511UL) != 0) return 0;
    clusterSectors = clusterBytes / 512UL;
    if (clusterSectors == 0) return 0;
    numClusters = partSectors / (unsigned long long)clusterSectors + 1ULL;
    entryBytes = (numClusters < 65520ULL) ? 2UL : 4UL;
    fatBytes = numClusters * (unsigned long long)entryBytes;
    fatBytes = (fatBytes + 4095ULL) & ~4095ULL;
    if (8ULL + (fatBytes / 512ULL) + (unsigned long long)clusterSectors >= partSectors)
        return 0;
    if (fatSectors) *fatSectors = fatBytes / 512ULL;
    if (rootSectors) *rootSectors = clusterSectors;
    if (fat16) *fat16 = (entryBytes == 2UL) ? 1 : 0;
    return 1;
}

/* Build the standard Xbox table for a disk of totalSectors. F: takes the
   remainder past DATA_F_START, minus one sector reserved for the backup table. */
static void fmtBuildTable(unsigned long long totalSectors, int disk, EosPartTable* t)
{
    long long fSize = (long long)totalSectors - (long long)DATA_F_START;
    int i;
    fmtMemSet(t, 0, sizeof(EosPartTable));
    for (i = 0; i < 16; ++i) t->Magic[i] = (unsigned char)"****PARTINFO****"[i];
    t->FatxMode = (unsigned char)(disk == 1 ? 2 : 1); /* Xbox primary / secondary */

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
    case FMT_ERR_TARGET:return "Drive target changed / unsupported sector size";
    case FMT_ERR_BOOTSTRAP:return "Blank-disk partition bootstrap failed";
    case FMT_ERR_VERIFY:   return "Disk written; FATX verification failed";
    default:            return "Unknown error";
    }
}

/* ---- read-only probe diagnostics --------------------------------------- */
static FormatProbeInfo s_probe[2] = { {0}, {0} };
static int s_lastFailurePartition = 0;

static void fmtProbeReset(int disk)
{
    if (disk < 0 || disk > 1) return;
    s_probe[disk].stage = FMT_PROBE_NONE;
    s_probe[disk].openStatus = 0;
    s_probe[disk].geometryStatus = 0;
    s_probe[disk].lba48Status = 0;
    s_probe[disk].lba48Magic1 = 0;
    s_probe[disk].lba48Magic2 = 0;
    s_probe[disk].lba48Version = 0;
    s_probe[disk].sectors = 0;
    s_probe[disk].bytesPerSector = 0;
    s_probe[disk].ataResult = HDD_ERR_NODISK;
    s_probe[disk].ataSectors = 0;
    s_probe[disk].ataSizeMB = 0;
}

int Format_GetProbeInfo(int disk, FormatProbeInfo* out)
{
    if (!out || disk < 0 || disk > 1) return 0;
    *out = s_probe[disk];
    return 1;
}

int Format_GetLastFailurePartition(void)
{
    return s_lastFailurePartition;
}

const char* Format_ProbeStageStr(int stage)
{
    switch (stage) {
    case FMT_PROBE_OPEN_FAIL:     return "NtOpenFile failed";
    case FMT_PROBE_GEOM_FAIL:     return "Geometry IOCTL failed";
    case FMT_PROBE_LBA48_FAIL:    return "LBA48 info IOCTL failed";
    case FMT_PROBE_LBA48_MAGIC:   return "LBA48 patch magic invalid";
    case FMT_PROBE_GEOM_OK:       return "PrometheOS probe returned";
    case FMT_PROBE_BPS_FAIL:      return "Bytes/sector rejected";
    case FMT_PROBE_CAPACITY_FAIL: return "Capacity rejected";
    case FMT_PROBE_OK:            return "Detected / geometry valid";
    case FMT_PROBE_ATA_FAIL:      return "ATA IDENTIFY failed";
    case FMT_PROBE_BOOTSTRAP:       return "Raw ATA formatter selected";
    case FMT_PROBE_VERIFY_FAIL:    return "Raw FATX verification failed";
    default:                        return "Not probed";
    }
}

#ifndef EOS_HOST_TEST
/* ---- device-side primitives -------------------------------------------- */
static int fmtReadLbaInfo(int disk, EosPartTable* liveTable,
    unsigned long long* totalSectors, unsigned long* bytesPerSector,
    unsigned int* patchVersion)
{
    STRING dev;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;
    unsigned int lbaIn[100];
    unsigned int lbaOut[100];
    unsigned int geomIn[100];
    unsigned int partitionTableAddr;
    EOS_DISK_GEOMETRY g;
    unsigned int bps;
    const char* path;

    EosHddInfo ataInfo;
    unsigned long long ataSectors;
    int ataRc;

    if (disk < 0 || disk > 1) return 0;
    fmtProbeReset(disk);
    if (liveTable) fmtMemSet(liveTable, 0, sizeof(EosPartTable));

    /* Prove the physical master/slave exists before asking for partition objects. */
    fmtMemSet(&ataInfo, 0, sizeof(ataInfo));
    ataSectors = 0;
    ataRc = Hdd_IdentifyDisk(disk, &ataInfo, &ataSectors);
    s_probe[disk].ataResult = ataRc;
    s_probe[disk].ataSectors = ataSectors;
    s_probe[disk].ataSizeMB = ataInfo.sizeMB;
    if (ataRc != HDD_OK || !ataInfo.present || ataSectors == 0) {
        s_probe[disk].stage = FMT_PROBE_ATA_FAIL;
        return 0;
    }

    /* Keep the kernel-object portion aligned with PrometheOS readLbaInfo(). */
    path = (disk == 0)
        ? "\\Device\\Harddisk0\\partition0"
        : "\\Device\\Harddisk1\\partition0";

    RtlInitAnsiString(&dev, path);
    oa.RootDirectory = 0;
    oa.ObjectName = &dev;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    st = NtOpenFile(&h, (GENERIC_READ | 0x00100000), &oa, &iosb,
        (FILE_SHARE_READ | FILE_SHARE_WRITE), 0x10);
    s_probe[disk].openStatus = (unsigned int)st;
    if (st != STATUS_SUCCESS) {
        s_probe[disk].stage = FMT_PROBE_OPEN_FAIL;
        return 0;
    }

    fmtMemSet(geomIn, 0, sizeof(geomIn));
    geomIn[0] = (unsigned int)EOS_IOCTL_SUBCMD_GET_INFO;
    fmtMemSet(&g, 0, sizeof(g));

    st = NtDeviceIoControlFile(h, 0, 0, 0, &iosb,
        EOS_IOCTL_DISK_GET_DRIVE_GEOMETRY,
        geomIn, sizeof(geomIn), &g, sizeof(g));
    s_probe[disk].geometryStatus = (unsigned int)st;
    if (st != STATUS_SUCCESS) {
        NtClose(h);
        s_probe[disk].stage = FMT_PROBE_GEOM_FAIL;
        return 0;
    }

    bps = g.BytesPerSector ? (unsigned int)g.BytesPerSector : 512U;
    s_probe[disk].sectors = (unsigned long long)g.Cylinders.QuadPart;
    s_probe[disk].bytesPerSector = bps;

    fmtMemSet(lbaIn, 0, sizeof(lbaIn));
    lbaIn[0] = (unsigned int)EOS_IOCTL_SUBCMD_GET_INFO;
    fmtMemSet(lbaOut, 0, sizeof(lbaOut));

    st = NtDeviceIoControlFile(h, 0, 0, 0, &iosb,
        EOS_IOCTL_CMD_LBA48_ACCESS,
        lbaIn, sizeof(lbaIn), lbaOut, sizeof(lbaOut));
    s_probe[disk].lba48Status = (unsigned int)st;
    NtClose(h);

    if (st != STATUS_SUCCESS) {
        s_probe[disk].stage = FMT_PROBE_LBA48_FAIL;
        return 0;
    }

    s_probe[disk].lba48Magic1 = lbaOut[EOS_LBA48_GET_INFO_MAGIC1_IDX];
    s_probe[disk].lba48Magic2 = lbaOut[EOS_LBA48_GET_INFO_MAGIC2_IDX];
    s_probe[disk].lba48Version = lbaOut[EOS_LBA48_GET_INFO_PATCHCODE_VERSION_IDX];

    if (s_probe[disk].lba48Magic1 != EOS_LBA48_GET_INFO_MAGIC1_VAL ||
        s_probe[disk].lba48Magic2 != EOS_LBA48_GET_INFO_MAGIC2_VAL) {
        s_probe[disk].stage = FMT_PROBE_LBA48_MAGIC;
        return 0;
    }

    partitionTableAddr = lbaOut[EOS_LBA48_GET_INFO_LOWCODE_BASE_IDX];
    partitionTableAddr += lbaOut[EOS_LBA48_GET_INFO_PART_TABLE_OFS_IDX];
    if (s_probe[disk].lba48Version == 2U)
        partitionTableAddr += 0x200U;

    if (liveTable && partitionTableAddr != 0U)
        fmtMemCopy(liveTable, (const void*)partitionTableAddr, sizeof(EosPartTable));

    if (totalSectors) *totalSectors = s_probe[disk].sectors;
    if (bytesPerSector) *bytesPerSector = bps;
    if (patchVersion) *patchVersion = s_probe[disk].lba48Version;
    s_probe[disk].stage = FMT_PROBE_GEOM_OK;
    return 1;
}

/* Write the table to sector 0 and a backup at the physical end of Partition0.
   PrometheOS deliberately asks the device for its allocation size here instead
   of deriving the backup offset from the earlier geometry result. */
static int fmtWriteTable(int disk, const EosPartTable* t)
{
    STRING dev; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; NTSTATUS st;
    LARGE_INTEGER off; FILE_FS_SIZE_INFORMATION fsi; char path[40];

    fmtDevicePath(disk, 0, path); RtlInitAnsiString(&dev, path);
    InitializeObjectAttributes(&oa, &dev, OBJ_CASE_INSENSITIVE, 0);
    st = NtOpenFile(&h, (SYNCHRONIZE | GENERIC_READ | GENERIC_WRITE), &oa, &iosb,
        (FILE_SHARE_READ | FILE_SHARE_WRITE), FILE_SYNCHRONOUS_IO_ALERT);
    if (st != STATUS_SUCCESS) return 0;

    off.QuadPart = 0;
    st = NtWriteFile(h, 0, 0, 0, &iosb, (PVOID)t, sizeof(EosPartTable), &off);
    if (st != STATUS_SUCCESS) { NtClose(h); return 0; }

    fmtMemSet(&fsi, 0, sizeof(fsi));
    st = NtQueryVolumeInformationFile(h, &iosb, &fsi, sizeof(fsi), FileFsSizeInformation);
    if (st != STATUS_SUCCESS) { NtClose(h); return 0; }

    off.QuadPart = fsi.TotalAllocationUnits.QuadPart *
        (LONGLONG)(fsi.BytesPerSector * fsi.SectorsPerAllocationUnit);
    if (off.QuadPart < (LONGLONG)sizeof(EosPartTable)) { NtClose(h); return 0; }
    off.QuadPart -= (LONGLONG)sizeof(EosPartTable);
    st = NtWriteFile(h, 0, 0, 0, &iosb, (PVOID)t, sizeof(EosPartTable), &off);
    NtClose(h);
    return (st == STATUS_SUCCESS);
}

/* prom largePartitionFixup: correct on-disk SectorsPerCluster (0) for big F:. */
static int fmtLargeFixup(int disk, unsigned long long lbaStart, unsigned long clusterBytes)
{
    STRING dev; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE h; NTSTATUS st;
    EOS_DISK_GEOMETRY g; LARGE_INTEGER off; int sh;
    unsigned long readLen = (unsigned long)ROUND_TO_PAGES(sizeof(FAT_VOLUME_METADATA));
    FAT_VOLUME_METADATA* meta; char path[40]; int ok = 0;
    static unsigned char s_buf[0x2000];
    { unsigned long a = (unsigned long)(&s_buf[0]); a = (a + (PAGE_SIZE - 1)) & ~((unsigned long)(PAGE_SIZE - 1)); meta = (FAT_VOLUME_METADATA*)a; }

    fmtDevicePath(disk, 0, path); RtlInitAnsiString(&dev, path);
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


static void fmtPut16(unsigned char* p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
}

static void fmtPut32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFFUL);
    p[1] = (unsigned char)((v >> 8) & 0xFFUL);
    p[2] = (unsigned char)((v >> 16) & 0xFFUL);
    p[3] = (unsigned char)((v >> 24) & 0xFFUL);
}

/* Raw FATX formatter for a partition whose kernel device object does not yet
   exist. Layout is intentionally the same as nxdk's nxFormatVolume:
     4 KiB superblock, aligned FAT, one empty root-directory cluster.
   Only filesystem metadata is written; the full partition is not zeroed. */
static int fmtRawFatxPartition(int disk, unsigned long long start,
    unsigned long long sizeSectors, unsigned long clusterBytes, unsigned long volumeId)
{
    unsigned char sec[512];
    unsigned char verify[512];
    unsigned long long fatSectors;
    unsigned long rootSectors;
    unsigned long long fatStart;
    unsigned long long rootStart;
    int fat16;

    if (!fmtFatxPlan(sizeSectors, clusterBytes, &fatSectors, &rootSectors, &fat16))
        return 0;

    /* Superblock: first 4 KiB is 0xFF except the FATX header. */
    fmtMemSet(sec, 0xFF, 512);
    sec[0] = 'F'; sec[1] = 'A'; sec[2] = 'T'; sec[3] = 'X';
    fmtPut32(sec + 4, volumeId);
    fmtPut32(sec + 8, clusterBytes / 512UL);
    fmtPut32(sec + 12, 1UL);
    fmtPut16(sec + 16, 0);
    if (Hdd_WriteSector(disk, start, sec) != HDD_OK) return 0;
    fmtMemSet(sec, 0xFF, 512);
    if (Hdd_WriteRepeatSectors(disk, start + 1ULL, 7UL, sec) != HDD_OK) return 0;

    /* FAT begins immediately after the 4 KiB superblock. */
    fatStart = start + 8ULL;
    fmtMemSet(sec, 0, 512);
    if (fat16) {
        sec[0] = 0xF8; sec[1] = 0xFF;
        sec[2] = 0xFF; sec[3] = 0xFF;
    }
    else {
        sec[0] = 0xF8; sec[1] = 0xFF; sec[2] = 0xFF; sec[3] = 0xFF;
        sec[4] = 0xFF; sec[5] = 0xFF; sec[6] = 0xFF; sec[7] = 0xFF;
    }
    if (Hdd_WriteSector(disk, fatStart, sec) != HDD_OK) return 0;
    fmtMemSet(sec, 0, 512);
    if (fatSectors > 1ULL) {
        if (fatSectors - 1ULL > 0xFFFFFFFFULL) return 0;
        if (Hdd_WriteRepeatSectors(disk, fatStart + 1ULL,
            (unsigned long)(fatSectors - 1ULL), sec) != HDD_OK) return 0;
    }

    /* Empty root directory cluster follows the FAT and is all 0xFF. */
    rootStart = fatStart + fatSectors;
    fmtMemSet(sec, 0xFF, 512);
    if (Hdd_WriteRepeatSectors(disk, rootStart, rootSectors, sec) != HDD_OK) return 0;

    /* Cheap readback catches addressing/drive-selection mistakes immediately. */
    if (Hdd_ReadSector(disk, start, verify) != HDD_OK) return 0;
    if (verify[0] != 'F' || verify[1] != 'A' || verify[2] != 'T' || verify[3] != 'X') return 0;
    if (((unsigned long)verify[8] | ((unsigned long)verify[9] << 8) |
        ((unsigned long)verify[10] << 16) | ((unsigned long)verify[11] << 24)) !=
        clusterBytes / 512UL) return 0;
    return 1;
}

/* Complete a physically present drive in one pass: table at LBA0, FATX
   metadata for every active partition, backup table at the final physical
   sector. No kernel volume mount or reboot is required. */
static int fmtRawFormatDisk(int disk, unsigned long long total, const EosPartTable* table)
{
    unsigned char verify[512];
    FILETIME ft;
    unsigned long volumeBase;
    int i;

    if (!table || total < (unsigned long long)DATA_F_START) return FMT_ERR_TARGET;

    if (Hdd_WriteSector(disk, 0, (const unsigned char*)table) != HDD_OK)
        return FMT_ERR_TABLE;
    if (Hdd_ReadSector(disk, 0, verify) != HDD_OK || fmtMemCmp(verify, table, 512) != 0)
        return FMT_ERR_TABLE;

    KeQuerySystemTime(&ft);
    volumeBase = ft.dwLowDateTime;

    for (i = 0; i < 14; ++i) {
        const EosPartEntry* e = &table->TableEntries[i];
        unsigned long long size;
        unsigned long clusterBytes;
        if (e->Flags == EOS_PART_NOTINUSE) continue;
        size = fmtGetSize(e);
        clusterBytes = fmtCalcClusterKB(size) << 10;
        if (!fmtRawFatxPartition(disk, fmtGetStart(e), size, clusterBytes,
            volumeBase ^ (0x9E3779B9UL * (unsigned long)(i + 1)))) {
            s_lastFailurePartition = i + 1;
            return FMT_ERR_FORMAT;
        }
    }

    /* PrometheOS keeps a backup copy at the physical end of the disk. */
    if (Hdd_WriteSector(disk, total - 1ULL, (const unsigned char*)table) != HDD_OK)
        return FMT_ERR_TABLE;
    if (Hdd_ReadSector(disk, total - 1ULL, verify) != HDD_OK ||
        fmtMemCmp(verify, table, 512) != 0) return FMT_ERR_TABLE;

    /* Best effort cache flush. Readback above has already validated addressing. */
    Hdd_FlushDisk(disk);
    return FMT_OK;
}

#endif /* !EOS_HOST_TEST */

int Format_PlanInfoForDisk(int disk, unsigned long* totalMB,
    unsigned long* dataEMB, unsigned long* driveFMB, unsigned long long* sectors)
{
#ifdef EOS_HOST_TEST
    if (disk >= 0 && disk <= 1) fmtProbeReset(disk);
    if (totalMB) *totalMB = 0;
    if (dataEMB) *dataEMB = (unsigned long)(DATA_E_SIZE / 2048);
    if (driveFMB) *driveFMB = 0;
    if (sectors) *sectors = 0;
    return 0;
#else
    unsigned long long total = 0;
    unsigned long bps = 0;
    unsigned int patchVersion = 0;
    EosPartTable liveTable;
    if (disk != 0 && disk != 1) return 0;
    if (disk == 1) {
        EosHddInfo info;
        unsigned long long ataSectors = 0;
        fmtProbeReset(1);
        fmtMemSet(&info, 0, sizeof(info));
        s_probe[1].ataResult = Hdd_IdentifyDisk(1, &info, &ataSectors);
        s_probe[1].ataSectors = ataSectors;
        s_probe[1].ataSizeMB = info.sizeMB;
        if (s_probe[1].ataResult != HDD_OK || !ataSectors) {
            s_probe[1].stage = FMT_PROBE_ATA_FAIL;
            return 0;
        }
        total = ataSectors;
        bps = 512; // ATA PIO interface addresses 512-byte logical sectors.
        s_probe[1].sectors = total;
        s_probe[1].bytesPerSector = bps;
        s_probe[1].stage = FMT_PROBE_BOOTSTRAP;
    }
    else if (!fmtReadLbaInfo(disk, &liveTable, &total, &bps, &patchVersion)) {
        /* A blank/unregistered drive may have no Partition0 object. ATA
           IDENTIFY is authoritative; formatting uses raw FATX metadata writes.
           Mounting is attempted immediately after the completed format. */
        if (s_probe[disk].ataResult != HDD_OK || s_probe[disk].ataSectors == 0) return 0;
        total = s_probe[disk].ataSectors;
        bps = 512;
        s_probe[disk].stage = FMT_PROBE_BOOTSTRAP;
    }
    /* PrometheOS permits a stock-size drive whose total sector count ends
       exactly where the optional F partition would begin. */
    if (bps != 512) {
        s_probe[disk].stage = FMT_PROBE_BPS_FAIL;
        return 0;
    }
    if (total < (unsigned long long)DATA_F_START) {
        s_probe[disk].stage = FMT_PROBE_CAPACITY_FAIL;
        return 0;
    }
    if (s_probe[disk].stage != FMT_PROBE_BOOTSTRAP)
        s_probe[disk].stage = FMT_PROBE_OK;
    if (totalMB) *totalMB = (unsigned long)(total / 2048ULL);
    if (dataEMB) *dataEMB = (unsigned long)(DATA_E_SIZE / 2048);
    if (driveFMB) *driveFMB = (total > DATA_F_START + 1ULL)
        ? (unsigned long)((total - DATA_F_START - 1ULL) / 2048ULL) : 0;
    if (sectors) *sectors = total;
    return 1;
#endif
}

int Format_PlanInfo(unsigned long* totalMB, unsigned long* dataEMB, unsigned long* driveFMB)
{
    return Format_PlanInfoForDisk(0, totalMB, dataEMB, driveFMB, 0);
}

int Format_StageDriveChecked(int disk, unsigned long long expectedSectors)
{
#ifdef EOS_HOST_TEST
    (void)disk; (void)expectedSectors;
    return FMT_ERR_GEOM; /* never simulate success for destructive operations */
#else
    EosPartTable table;
    EosPartTable liveTable;
    unsigned long long total = 0;
    unsigned long bps = 0;
    unsigned int patchVersion = 0;
    int i, rc = FMT_OK;

    s_lastFailurePartition = 0;
    if (disk != 0 && disk != 1) return FMT_ERR_TARGET;
    if (!expectedSectors) return FMT_ERR_TARGET;

    /* HDD1 preparation is strictly physical ATA/LBA. Do not open any
       \Device\Harddisk1\PartitionN path: the kernel may not expose it.
       This is the same raw path which previously prepared the test drive. */
    if (disk == 1) {
        EosHddInfo info;
        unsigned long long sectors = 0;
        EosHddRawPartitionInfo checked[HDD_PART_MAX];
        int tableState = 0, mode = 0, count, j, foundC = 0, foundE = 0, foundF = 0;
        int rawRc;
        fmtMemSet(&info, 0, sizeof(info));
        if (Hdd_IdentifyDisk(1, &info, &sectors) != HDD_OK ||
            sectors != expectedSectors || sectors < (unsigned long long)DATA_F_START)
            return FMT_ERR_TARGET;
        fmtBuildTable(sectors, 1, &table);
        s_probe[1].stage = FMT_PROBE_BOOTSTRAP;
        rawRc = fmtRawFormatDisk(1, sectors, &table);
        if (rawRc != FMT_OK) return rawRc;
        // Validate freshly-written on-disk state without a kernel mount.
        count = Hdd_GetRawPartitionsForDisk(1, sectors, checked,
            HDD_PART_MAX, &tableState, &mode);
        for (j = 0; j < count; ++j) {
            if (checked[j].fatxStatus != HDD_RAW_FATX_VALID) continue;
            if (checked[j].partition == 'C') foundC = 1;
            if (checked[j].partition == 'E') foundE = 1;
            if (checked[j].partition == 'F') foundF = 1;
        }
        if (tableState != HDD_RAW_TABLE_VALID || mode != 2 ||
            !foundC || !foundE ||
            (sectors > (unsigned long long)DATA_F_START + 1ULL && !foundF)) {
            s_probe[1].stage = FMT_PROBE_VERIFY_FAIL;
            return FMT_ERR_VERIFY;
        }
        s_probe[1].stage = FMT_PROBE_OK;
        return FMT_OK;
    }

    if (!fmtReadLbaInfo(disk, &liveTable, &total, &bps, &patchVersion)) {
        int rawRc;
        /* Primary-drive compatibility fallback: when the kernel lacks
           Partition0, revalidate the physical ATA target and write/verify
           the standard Xbox table/FATX metadata directly. */
        if (s_probe[disk].ataResult != HDD_OK || s_probe[disk].ataSectors != expectedSectors ||
            expectedSectors < (unsigned long long)DATA_F_START) return FMT_ERR_TARGET;
        total = s_probe[disk].ataSectors;
        fmtBuildTable(total, disk, &table);
        rawRc = fmtRawFormatDisk(disk, total, &table);
        if (rawRc != FMT_OK) return rawRc;

        /* The raw formatter has written/verified the partition layout. A
           missing kernel mount is NOT a formatting failure. */
        if (disk == 0) Mount_HddPartitions();
        s_probe[disk].stage = FMT_PROBE_OK;
        return FMT_OK;
    }
    /* Revalidate the same disk/size on the final confirm. No implicit fallback
       to the other physical device if the selected target changes. */
    if (total != expectedSectors || bps != 512 ||
        total < (unsigned long long)DATA_F_START) return FMT_ERR_TARGET;

    fmtBuildTable(total, disk, &table);
    /* PrometheOS semantics: release DOS aliases only, not HDD volume objects.
       Reset EOS-owned mount state before the table/volume rewrite. */
    Mount_UnmountHddPartitionsForDisk(disk);

    if (!fmtWriteTable(disk, &table)) { rc = FMT_ERR_TABLE; goto remount; }

    for (i = 0; i < 14; ++i) {
        EosPartEntry* e = &table.TableEntries[i];
        unsigned long long lbaSize;
        unsigned long clusterBytes;
        char dp[40]; STRING devStr; BOOL ok;
        if (e->Flags == EOS_PART_NOTINUSE) continue;

        lbaSize = fmtGetSize(e);
        clusterBytes = fmtCalcClusterKB(lbaSize) << 10;
        fmtDevicePath(disk, i + 1, dp);
        RtlInitAnsiString(&devStr, dp);
        ok = XapiFormatFATVolumeEx(&devStr, clusterBytes);
        if (ok == FALSE) { s_lastFailurePartition = i + 1; rc = FMT_ERR_FORMAT; goto remount; }
        if (i >= 5 && !fmtLargeFixup(disk, fmtGetStart(e), clusterBytes)) {
            s_lastFailurePartition = i + 1;
            rc = FMT_ERR_FIXUP; goto remount;
        }
    }
remount:
    /* Restore existing HDD0 aliases on the kernel-backed primary path. */
    Mount_HddPartitions();
    return rc;
#endif
}

int Format_StageDrive(void)
{
    unsigned long long sectors = 0;
    if (!Format_PlanInfoForDisk(0, 0, 0, 0, &sectors)) return FMT_ERR_GEOM;
    return Format_StageDriveChecked(0, sectors);
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
        fmtBuildTable(total, 0, &t);
        if (fmtMemCmp(t.Magic, "****PARTINFO****", 16) != 0) { printf("FAIL magic\n"); ++fails; }
        if (t.FatxMode != 1) { printf("FAIL fatxmode\n"); ++fails; }
        fmtBuildTable(total, 1, &t);
        if (t.FatxMode != 2) { printf("FAIL secondary fatxmode\n"); ++fails; }
        fmtBuildTable(total, 0, &t);
        fails += chkEntry(&t, 0, "XBOX DATA E", DATA_E_START, DATA_E_SIZE);
        fails += chkEntry(&t, 1, "XBOX SHELL C", SHELL_C_START, SHELL_C_SIZE);
        fails += chkEntry(&t, 2, "XBOX CACHE X", CACHE_X_START, CACHE_SIZE);
        fails += chkEntry(&t, 3, "XBOX CACHE Y", CACHE_Y_START, CACHE_SIZE);
        fails += chkEntry(&t, 4, "XBOX CACHE Z", CACHE_Z_START, CACHE_SIZE);
        fails += chkEntry(&t, 5, "DRIVE F", DATA_F_START, fSize);
        if (t.TableEntries[6].Flags != EOS_PART_NOTINUSE) { printf("FAIL e6 should be unused\n"); ++fails; }
    }

    /* full-capacity regression checks: F always consumes every sector after
       the fixed Xbox partitions, except the final backup-table sector. */
    {
        static const unsigned long long totals[] = {
            1953125000ULL,   /* 1 TB decimal */
            3906250000ULL,   /* 2 TB decimal */
            7812500000ULL,   /* 4 TB decimal */
            15625000000ULL   /* 8 TB decimal */
        };
        static const unsigned long clusters[] = { 64, 128, 256, 512 };
        int n;
        for (n = 0; n < 4; ++n) {
            unsigned long long want = totals[n] - DATA_F_START - 1ULL;
            fmtBuildTable(totals[n], 0, &t);
            if (fmtGetSize(&t.TableEntries[5]) != want) {
                printf("FAIL full-capacity %d size %llX want %llX\n", n,
                    fmtGetSize(&t.TableEntries[5]), want); ++fails;
            }
            if (fmtCalcClusterKB(want) != clusters[n]) {
                printf("FAIL full-capacity %d cluster %lu want %lu\n", n,
                    fmtCalcClusterKB(want), clusters[n]); ++fails;
            }
        }
    }

    /* Secondary large-drive regression: FATX mode 2 retains the full 48-bit
       F size; this specifically exercises LBASizeHi above the 2 TiB boundary. */
    {
        unsigned long long total = 7812500000ULL; /* 4 TB decimal */
        unsigned long long want = total - DATA_F_START - 1ULL;
        fmtBuildTable(total, 1, &t);
        if (t.FatxMode != 2) { printf("FAIL 4TB secondary fatxmode\n"); ++fails; }
        if (fmtGetSize(&t.TableEntries[5]) != want) {
            printf("FAIL 4TB secondary F size %llX want %llX\n",
                fmtGetSize(&t.TableEntries[5]), want); ++fails;
        }
        if (t.TableEntries[5].LBASizeHi == 0) {
            printf("FAIL 4TB secondary high LBA size not encoded\n"); ++fails;
        }
    }

    /* cluster doubling threshold is 0x20000000 sectors (256 GB) */
    if (fmtCalcClusterKB(0x20000000ULL) != 16) { printf("FAIL cluster @256G\n"); ++fails; }
    if (fmtCalcClusterKB(0x20000001ULL) != 32) { printf("FAIL cluster >256G\n"); ++fails; }
    if (fmtCalcClusterKB(0x40000001ULL) != 64) { printf("FAIL cluster >512G\n"); ++fails; }
    if (fmtCalcClusterKB(0x80000001ULL) != 128) { printf("FAIL cluster >1T\n"); ++fails; }

    /* stock-size 8 GB geometry: total ends exactly at DATA_F_START.
       This is a valid C/E/cache-only layout and must not invent F:. */
    {
        unsigned long long total = (unsigned long long)DATA_F_START;
        fmtBuildTable(total, 1, &t);
        if (t.FatxMode != 2) { printf("FAIL stock secondary fatxmode\n"); ++fails; }
        if (t.TableEntries[5].Flags != EOS_PART_NOTINUSE) { printf("FAIL stock-size F present\n"); ++fails; }
        fails += chkEntry(&t, 0, "XBOX DATA E", DATA_E_START, DATA_E_SIZE);
    }

    /* small drive below F start -> no F entry (builder-only regression;
       runtime validation correctly rejects a disk this small). */
    {
        unsigned long long total = DATA_F_START - 100ULL;
        fmtBuildTable(total, 0, &t);
        if (t.TableEntries[5].Flags != EOS_PART_NOTINUSE) { printf("FAIL small-drive F present\n"); ++fails; }
        fails += chkEntry(&t, 0, "XBOX DATA E", DATA_E_START, DATA_E_SIZE);  /* E still there */
    }

    /* Raw FATX metadata plan regression. Even on an 8 TB F partition the
       metadata region stays below LBA28, while the end-of-disk backup uses the
       generic LBA48 single-sector writer. */
    {
        static const unsigned long long totals[] = {
            1953125000ULL, 3906250000ULL, 7812500000ULL, 15625000000ULL
        };
        int n;
        for (n = 0; n < 4; ++n) {
            unsigned long long fs = totals[n] - DATA_F_START - 1ULL;
            unsigned long cb = fmtCalcClusterKB(fs) << 10;
            unsigned long long fatSec = 0;
            unsigned long rootSec = 0;
            int fat16 = 0;
            if (!fmtFatxPlan(fs, cb, &fatSec, &rootSec, &fat16)) {
                printf("FAIL raw FATX plan %d\n", n); ++fails;
            }
            else if ((unsigned long long)DATA_F_START + 8ULL + fatSec + rootSec >= 0x10000000ULL) {
                printf("FAIL raw FATX metadata exceeds LBA28 %d\n", n); ++fails;
            }
            if (fat16) { printf("FAIL large F unexpectedly FATX16 %d\n", n); ++fails; }
        }
    }

    /* device/mount path building */
    {
        char d[40], m[8]; fmtDevicePath(0, 6, d); fmtMountPath('f', m);
        if (fmtMemCmp(d, "\\Device\\Harddisk0\\Partition6", 28) != 0) { printf("FAIL devpath '%s'\n", d); ++fails; }
        fmtDevicePath(1, 6, d);
        if (fmtMemCmp(d, "\\Device\\Harddisk1\\Partition6", 28) != 0) { printf("FAIL secondary devpath '%s'\n", d); ++fails; }
        if (fmtMemCmp(m, "\\??\\F:", 7) != 0) { printf("FAIL mntpath '%s'\n", m); ++fails; }
    }

    /* bit scan */
    if (fmtFirstSetBit(512) != 9 || fmtFirstSetBit(4096) != 12) { printf("FAIL bitscan\n"); ++fails; }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL STAGING LOGIC TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
#endif