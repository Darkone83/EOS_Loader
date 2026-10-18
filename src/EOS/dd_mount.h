/*---------------------------------------------------------------------------
    dd_mount.h -- mount primary and optional secondary Xbox HDD partitions.
---------------------------------------------------------------------------*/
#ifndef DD_MOUNT_H
#define DD_MOUNT_H

void Mount_HddPartitions(void);
void Mount_UnmountHddPartitions(void);
void Mount_UnmountHddPartitionsForDisk(int disk);

/* Secondary HDD DOS aliases. Mount_HddPartitions() applies saved EOS Drive
   Setup mode 3, probes ATA slave, and retains aliases only when
   GetDiskFreeSpaceExA confirms the corresponding FATX volume:
       P: = HDD1-C / Partition2
       Q: = HDD1-E / Partition1
       R: = HDD1-F / Partition6
       V: = HDD1-G / Partition7
   Bit 0..3 in the mask correspond to P/Q/R/V. */
unsigned int Mount_Hdd1MappedMask(void);
int          Mount_Hdd1Available(void);
int          Mount_Hdd1AtaPresent(void); /* last mount attempt: physical ATA slave */
char         Mount_Hdd1AliasForPart(char part);
char         Mount_Hdd1PartForAlias(char alias);

int Mount_SelfToD(void);
int Mount_LaunchXbe(const char* dosPath);

#endif /* DD_MOUNT_H */
