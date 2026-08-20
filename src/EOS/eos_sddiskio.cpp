// eos_sddiskio.cpp -- FatFs low-level disk I/O glue for the Eos onboard SD
// slot. Every physical access funnels through Sd_ReadSector()/Sd_WriteSector()
// (eos_sdcard.cpp) -- ONE sector at a time over the SD_BR_*/SD_BW_* LPC
// registers. FatFs itself just sees a normal 512-byte block device.
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
    UINT i;

    if (pdrv != 0 || count == 0) return RES_PARERR;
    for (i = 0; i < count; ++i) {
        if (Sd_WriteSector((unsigned long)(sector + i), buff + (size_t)i * 512) != EOS_SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;   // every CMD24 is completed before disk_write returns
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