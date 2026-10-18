/*---------------------------------------------------------------------------
    eos_format.h -- HDD staging (Tools > Format).

    Recreates the standard Xbox partition table and FATX partitions on the
    explicitly chosen physical device: Harddisk0 (primary) or Harddisk1
    (secondary). Physical presence/capacity is established by ATA IDENTIFY, so
    a completely blank drive can be initialized even before Partition0 exists.

    Uses the PrometheOS partition layout (E/C/X/Y/Z/F). HDD1 is prepared and
    checked directly through physical ATA, without requiring any kernel disk
    object or drive-letter alias. HDD0 retains its existing kernel-backed path.

    Standard layout (disk 0 FATX mode 1; disk 1 FATX mode 2):
        Partition1  E:  data           Partition5  Z:  cache
        Partition2  C:  system/dash    Partition6  F:  extended (rest of disk)
        Partition3  X:  cache
        Partition4  Y:  cache

    DESTRUCTIVE: every partition on the SELECTED drive is recreated and
    formatted. Confirm the physical target before performing any write.
---------------------------------------------------------------------------*/
#ifndef EOS_FORMAT_H
#define EOS_FORMAT_H

#define FMT_OK             0
#define FMT_ERR_GEOM      -1   /* could not read disk geometry (no disk?)     */
#define FMT_ERR_TABLE     -2   /* partition-table write failed                */
#define FMT_ERR_FORMAT    -3   /* a partition format failed                   */
#define FMT_ERR_FIXUP     -4   /* large-partition cluster fixup failed        */
#define FMT_ERR_TARGET    -5   /* changed/unavailable target or unsupported geometry */
#define FMT_ERR_BOOTSTRAP -6   /* raw sector-0 bootstrap write/readback failed        */
#define FMT_ERR_VERIFY    -7  /* written data did not pass raw FATX verification */

/* Read-only HDD probe diagnostics. These describe the most recent
   PrometheOS-style readLbaInfo() probe for each physical disk. */
#define FMT_PROBE_NONE          0
#define FMT_PROBE_OPEN_FAIL     1
#define FMT_PROBE_GEOM_FAIL     2
#define FMT_PROBE_LBA48_FAIL    3
#define FMT_PROBE_LBA48_MAGIC   4
#define FMT_PROBE_GEOM_OK       5
#define FMT_PROBE_BPS_FAIL      6
#define FMT_PROBE_CAPACITY_FAIL 7
#define FMT_PROBE_OK            8
#define FMT_PROBE_ATA_FAIL      9
#define FMT_PROBE_BOOTSTRAP      10
#define FMT_PROBE_VERIFY_FAIL    11

typedef struct {
    int stage;
    unsigned int openStatus;
    unsigned int geometryStatus;
    unsigned int lba48Status;
    unsigned int lba48Magic1;
    unsigned int lba48Magic2;
    unsigned int lba48Version;
    unsigned long long sectors;
    unsigned int bytesPerSector;
    int ataResult;
    unsigned long long ataSectors;
    unsigned long ataSizeMB;
} FormatProbeInfo;

int Format_GetProbeInfo(int disk, FormatProbeInfo* out);
const char* Format_ProbeStageStr(int stage);

/* If FATX formatting/fixup fails, returns the 1-based partition number that
   failed. Zero means the failure was not partition-specific. */
int Format_GetLastFailurePartition(void);

/* Stage (partition + format) the primary master drive. DESTRUCTIVE.
   Returns FMT_OK or FMT_ERR_*. On failure the drive letters are remounted so
   the system is left in a usable state where possible. */
int Format_StageDrive(void);

/* Select physical disk 0 (primary) or 1 (secondary). PlanInfoForDisk fails
   closed when the target is missing or uses non-512B sectors; outSectors is
   captured for final-confirm identity/geometry rechecking. */
int Format_PlanInfoForDisk(int disk, unsigned long* totalMB,
    unsigned long* dataEMB, unsigned long* driveFMB, unsigned long long* outSectors);
/* Stage only the explicitly selected drive if geometry still matches plan.
   HDD1 uses raw ATA/LBA and verifies its FATX headers; native mounts are not
   required and are the responsibility of the later DarkDash upgrade. */
int Format_StageDriveChecked(int disk, unsigned long long expectedSectors);

/* Planned layout for the detected drive, for the confirm screen.
   totalMB / dataEMB / driveFMB are filled in (driveFMB = 0 if no F partition).
   Returns 1 on success (geometry read), 0 if no disk. */
int Format_PlanInfo(unsigned long* totalMB, unsigned long* dataEMB, unsigned long* driveFMB);

const char* Format_ErrStr(int code);

#endif /* EOS_FORMAT_H */