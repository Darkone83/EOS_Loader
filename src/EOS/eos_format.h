/*---------------------------------------------------------------------------
    eos_format.h -- HDD staging (Tools > Format).

    Recreates the standard Xbox partition table and FATX partitions on the
    explicitly chosen physical device: Harddisk0 (primary) or Harddisk1
    (secondary, only when the current BIOS exposes that device).

    This is the PrometheOS staging layout: geometry -> build table -> write
    table -> format C/E/X/Y/Z/F. Primary letter links are remounted; secondary
    partitions use native Harddisk1 paths and are NOT linked over primary E:.

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

/* Stage (partition + format + mount) the primary master drive. DESTRUCTIVE.
   Returns FMT_OK or FMT_ERR_*. On failure the drive letters are remounted so
   the system is left in a usable state where possible. */
int Format_StageDrive(void);

/* Select physical disk 0 (primary) or 1 (secondary). PlanInfoForDisk fails
   closed when the target is missing or uses non-512B sectors; outSectors is
   captured for final-confirm identity/geometry rechecking. */
int Format_PlanInfoForDisk(int disk, unsigned long* totalMB,
    unsigned long* dataEMB, unsigned long* driveFMB, unsigned long long* outSectors);
/* Stage only the explicitly selected drive if geometry still matches plan. */
int Format_StageDriveChecked(int disk, unsigned long long expectedSectors);

/* Planned layout for the detected drive, for the confirm screen.
   totalMB / dataEMB / driveFMB are filled in (driveFMB = 0 if no F partition).
   Returns 1 on success (geometry read), 0 if no disk. */
int Format_PlanInfo(unsigned long* totalMB, unsigned long* dataEMB, unsigned long* driveFMB);

const char* Format_ErrStr(int code);

#endif /* EOS_FORMAT_H */