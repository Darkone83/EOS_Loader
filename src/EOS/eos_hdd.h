#pragma once
// eos_hdd.h -- ATA hard-disk tools for the OG Xbox (primary master).
//
// Normal lock/unlock uses the console-derived HDD password. If normal unlock
// fails and security remains enabled, Hdd_Unlock() transparently falls back to
// eos_vsc (PrometheOS-derived WD/Seagate vendor recovery).

#define HDD_OK              0
#define HDD_OK_VSC          1   // VSC fallback succeeded + credentials saved
#define HDD_OK_VSC_NOSAVE   2   // VSC fallback succeeded, unlock.txt write failed
#define HDD_ERR_NODISK     -1
#define HDD_ERR_UNSUPP     -2
#define HDD_ERR_STATE      -3
#define HDD_ERR_ATA        -4

#define HDD_SEC_SUPPORTED 0x0001
#define HDD_SEC_ENABLED   0x0002
#define HDD_SEC_LOCKED    0x0004
#define HDD_SEC_FROZEN    0x0008

#define HDD_PART_MAX 4

typedef struct EosHddInfo {
    int            present;
    char           model[44];
    char           serial[24];
    unsigned short security;
    unsigned long  sizeMB;
} EosHddInfo;

typedef struct EosPartitionInfo {
    char          drive;        // C/E/F/G
    int           present;
    unsigned long totalMB;
    unsigned long freeMB;
    unsigned long usedMB;
    unsigned int  usedPercent;
} EosPartitionInfo;

int Hdd_Identify(EosHddInfo* out);
int Hdd_Unlock(void);
int Hdd_Lock(void);

// Query user-facing Xbox partitions C/E/F/G. Missing volumes are skipped.
// Returns the number of valid entries written to out (0..maxCount).
int Hdd_GetPartitions(EosPartitionInfo* out, int maxCount);
