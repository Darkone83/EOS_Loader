// eos_sddiskio.cpp -- FatFs low-level disk I/O glue for the Eos onboard SD
// slot. Every physical access funnels through Sd_ReadSector() (eos_sdcard.cpp)
// -- ONE sector at a time, over the SD_BR_* LPC registers. FatFs itself never
// knows this isn't a normal block device; it just calls disk_read().
//
// Read-only: FF_FS_READONLY=1 in ffconf.h means ff.c never calls disk_write(),
// but diskio.h still declares it, so a stub is provided for link completeness
// only -- it is dead code, never reached from the read-only build.
#include "ff.h"
#include "diskio.h"
#include "eos_sdcard.h"

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return Sd_CardReady() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    // eos_sd_spi.v brings the card up autonomously at power-on; nothing to
    // kick from here. Just confirm it actually made it to card_ready.
    return Sd_CardReady() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    UINT i;

    if (pdrv != 0) return RES_PARERR;
    for (i = 0; i < count; ++i) {
        if (Sd_ReadSector((unsigned long)(sector + i), buff + (size_t)i * 512) != EOS_SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    // Never called (FF_FS_READONLY=1) -- see file header. Kept for link
    // completeness only.
    (void)pdrv; (void)buff; (void)sector; (void)count;
    return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;   // nothing buffered on the write side (read-only)
    default:
        return RES_PARERR;
    }
}

// FF_FS_NORTC=1 means ff.c never calls this (uses the fixed NORTC date
// instead), but it's declared unconditionally by some FatFs builds -- keep a
// harmless stub for link completeness.
DWORD get_fattime(void)
{
    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}