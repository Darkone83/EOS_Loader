#pragma once
// eos_hdd.h -- ATA hard-disk tools for the OG Xbox (primary master).
//
// Raw ATA over the IDE ports (0x1F0), same in/out style as the flash driver.
// IDENTIFY reads model/serial/size/security; the lock/unlock password is
// HMAC-SHA1(XboxHDKey, model || serial) -- the kernel's decrypted HDD key, so
// no EEPROM crypto is needed here. Unlock removes security (unlock + disable);
// Lock enables it with this console's key. Both are guarded in the UI; Lock can
// lock a drive to this console, so it confirms first.
//
// If the normal unlock path leaves the drive locked, EOS can fall back to the
// supported vendor-specific recovery flows in eos_vsc.cpp. Those flows are an
// EOS adaptation of PrometheOS hddVscUnlocker behavior by Team Resurgent.
//
// ATA command-send logic ported from PrometheOS XKHDD (hardware-validated).

#define HDD_OK                0
#define HDD_OK_VSC            1   // VSC recovery succeeded; passwords saved
#define HDD_OK_VSC_NOSAVE     2   // VSC recovery succeeded; credential write failed
#define HDD_ERR_NODISK       -1   // IDENTIFY failed (no master drive / timeout)
#define HDD_ERR_UNSUPP       -2   // drive doesn't support ATA security
#define HDD_ERR_STATE        -3   // wrong precondition (e.g. lock when already enabled)
#define HDD_ERR_ATA          -4   // a security command didn't take

#define HDD_VSC_CRED_PATH "E:\\Eos\\unlock.txt"

// ATA IDENTIFY word-128 security-status bits.
#define HDD_SEC_SUPPORTED 0x0001
#define HDD_SEC_ENABLED   0x0002
#define HDD_SEC_LOCKED    0x0004
#define HDD_SEC_FROZEN    0x0008

#define HDD_PART_MAX 4

typedef struct EosHddInfo {
    int            present;        // IDENTIFY succeeded
    char           model[44];      // trimmed, byte-swap corrected
    char           serial[24];     // trimmed
    unsigned short security;       // raw word 128
    unsigned long  sizeMB;         // physical drive capacity in MB
} EosHddInfo;

typedef struct EosHddPartitionInfo {
    char           drive;          // HDD0: C/E/F/G; HDD1 aliases: P/Q/R/V
    unsigned long  totalMB;
    unsigned long  usedMB;
    unsigned long  freeMB;
    unsigned int   usedPercent;    // 0..100
} EosHddPartitionInfo;

// ATA IDENTIFY on physical disk 0 (master) or 1 (slave). This does not
// depend on FATX, a partition table, or any \Device\Harddisk# object.
// outSectors receives the exact ATA-reported 512-byte sector count.
int Hdd_IdentifyDisk(int disk, EosHddInfo* out, unsigned long long* outSectors);

// Raw ATA PIO sector access used by the blank-disk formatter before the Xbox
// kernel has created \Device\Harddisk#\Partition0. Generic one-sector access
// supports LBA28 and LBA48; repeated writes are chunked for fast FATX metadata
// initialization. None of these functions depend on an existing filesystem.
int Hdd_ReadSector(int disk, unsigned long long lba, unsigned char out512[512]);
int Hdd_WriteSector(int disk, unsigned long long lba, const unsigned char in512[512]);
int Hdd_WriteRepeatSectors(int disk, unsigned long long lba, unsigned long count,
    const unsigned char sector512[512]);
int Hdd_FlushDisk(int disk);

// Compatibility wrappers retained for the original bootstrap callers.
int Hdd_ReadSector0(int disk, unsigned char out512[512]);
int Hdd_WriteSector0(int disk, const unsigned char in512[512]);

// Backward-compatible primary-master wrapper used by the existing HDD tools.
int Hdd_Identify(EosHddInfo* out);

// Mounted Xbox partition usage. Only partitions that actually resolve are
// returned; stock drives therefore do not get fake F/G rows. disk 0 uses
// C/E/F/G; disk 1 uses the loader aliases P/Q/R/V for HDD1 C/E/F/G.
int Hdd_GetPartitionsForDisk(int disk, EosHddPartitionInfo* out, int maxEntries);
int Hdd_GetPartitions(EosHddPartitionInfo* out, int maxEntries);

// Read-only FATX partition inspection using physical ATA/LBA, not kernel mounts.
// Used by EOS Drive Info for HDD1; P/Q/R/V below are display labels only.
// They do NOT imply that a DOS alias or FATX kernel filesystem is mounted.
#define HDD_RAW_TABLE_READ_ERROR  0
#define HDD_RAW_TABLE_INVALID     1
#define HDD_RAW_TABLE_VALID       2
#define HDD_RAW_FATX_READ_ERROR   0
#define HDD_RAW_FATX_MISSING      1
#define HDD_RAW_FATX_VALID        2

typedef struct EosHddRawPartitionInfo {
    char partition;               // C, E, F, or G (logical Xbox partition)
    char suggestedAlias;          // P, Q, R, or V (for future DarkDash use)
    int  tablePartitionNumber;    // Xbox kernel partition index (2,1,6,7)
    unsigned long long startLBA;
    unsigned long long sectorCount;
    unsigned long sizeMB;
    unsigned long sectorsPerCluster; // read from FATX header if present
    int fatxStatus;               // HDD_RAW_FATX_*
} EosHddRawPartitionInfo;

// Parse LBA0 and check each user partition's FATX sector by raw ATA.
// 'totalSectors' must come from ATA IDENTIFY for the same physical disk.
// Reports tableState even when no user entries are returned.
int Hdd_GetRawPartitionsForDisk(int disk, unsigned long long totalSectors,
    EosHddRawPartitionInfo* out, int maxEntries, int* tableState, int* fatxMode);

// Remove security from the primary master. EOS first tries the normal
// console-derived user password. If the drive remains locked, a supported VSC
// recovery is attempted automatically. No-op (HDD_OK) if already unlocked.
int Hdd_Unlock(void);

// Enable security on the primary master, locking it to THIS console's key
// (sets master + user passwords). VSC is never involved in the lock path.
int Hdd_Lock(void);
