/*---------------------------------------------------------------------------
    eos_format.h -- HDD staging (Tools > Format).

    Prepares a blank / new hard drive for use on the Xbox: builds the standard
    Xbox partition table, writes it to the disk (sector 0 + an end-of-disk
    backup), formats every partition, and mounts the drive letters.

    This is the PrometheOS formatDrive path, minus the LBA48 patch-table read
    (not needed when writing a fresh standard layout): geometry -> build table
    -> write table -> format C/E/X/Y/Z/F -> mount.

    Standard layout written (Harddisk0):
        Partition1  E:  data           Partition5  Z:  cache
        Partition2  C:  system/dash    Partition6  F:  extended (rest of disk)
        Partition3  X:  cache
        Partition4  Y:  cache

    DESTRUCTIVE: every partition on the drive is recreated and formatted. All
    existing data, including any installed dashboard, is lost. Intended for
    staging a new drive, not for clearing one partition.
---------------------------------------------------------------------------*/
#ifndef EOS_FORMAT_H
#define EOS_FORMAT_H

#define FMT_OK             0
#define FMT_ERR_GEOM      -1   /* could not read disk geometry (no disk?)     */
#define FMT_ERR_TABLE     -2   /* partition-table write failed                */
#define FMT_ERR_FORMAT    -3   /* a partition format failed                   */
#define FMT_ERR_FIXUP     -4   /* large-partition cluster fixup failed        */

/* Stage (partition + format + mount) the primary master drive. DESTRUCTIVE.
   Returns FMT_OK or FMT_ERR_*. On failure the drive letters are remounted so
   the system is left in a usable state where possible. */
int Format_StageDrive(void);

/* Planned layout for the detected drive, for the confirm screen.
   totalMB / dataEMB / driveFMB are filled in (driveFMB = 0 if no F partition).
   Returns 1 on success (geometry read), 0 if no disk. */
int Format_PlanInfo(unsigned long* totalMB, unsigned long* dataEMB, unsigned long* driveFMB);

const char* Format_ErrStr(int code);

#endif /* EOS_FORMAT_H */