#pragma once
// eos_sdcard.h -- Eos SD card driver (host side of the 0xEC/0xED SD_*/SD_BR_*
// register contract; see eos_flash_cmd.v / eos_sd_precache.v / eos_sd_spi.v).
//
// Three jobs, mirroring the gateware commands:
//   1) Single-sector browse reads, used by our FatFs diskio glue (eos_sddiskio.cpp)
//      to walk the FAT32 volume -- directories, FAT table, cluster chains.
//   2) Single-sector writes, used by FatFs for the WebUI BIOS manager.
//   3) The bulk precache: given a file's raw starting LBA + sector count
//      (resolved via FatFs, see Sd_ResolveFile below), copy it into NRGN_SD in
//      hardware and launch it as bank 0x0. This remains entirely separate from
//      the TSOP/user-bank flashing path in eos_flash.cpp.
#include <xtl.h>
#include "ff.h"

// FatFs error surface for card-level problems (mirrors eos_sd_spi's err_code):
//   0=none 1=cmd0 2=cmd8 3=acmd41_timeout 4=cmd58 6=cmd17(R1) 7=token_timeout
//   8=legacy_card 9=cmd24(R1) 10=write_rejected 11=write_busy_timeout
//   13=refused,busy_with_other_op 14=card_not_ready
#define EOS_SD_OK          0
#define EOS_SD_TIMEOUT    -1
#define EOS_SD_CARDERR    -2   // see Sd_LastErrCode() for the specific reason
#define EOS_SD_FRAGMENTED -3   // Sd_ResolveFile: file is not one contiguous run
#define EOS_SD_TOOBIG     -4   // Sd_ResolveFile: file doesn't match a size class
#define EOS_SD_MOUNTFAIL  -5

// Size classes, matching NR_SZC and the existing flash bank size codes.
#define EOS_SD_SZC_256K   0
#define EOS_SD_SZC_512K   1
#define EOS_SD_SZC_1MB    2

// --- raw single-sector read, used ONLY by eos_sddiskio.cpp's disk_read() ----
// Reads exactly one 512-byte sector at 'lba' into buf512. Returns EOS_SD_OK or
// EOS_SD_TIMEOUT/EOS_SD_CARDERR. Never touches NRGN_SD or the precache path.
int Sd_ReadSector(unsigned long lba, unsigned char* buf512);

// Writes exactly one 512-byte sector through the gateware CMD24 path. Used by
// eos_sddiskio.cpp only; callers above FatFs should use f_write/f_unlink.
int Sd_WriteSector(unsigned long lba, const unsigned char* buf512);

// 1 if the card responded to init (cheap: reads the browse-path's card_ready
// status; does not itself touch the card). Call after Sd_Mount()/first use.
int Sd_CardReady(void);

// Last card-level error code from either path (see the encoding above), for
// UI display. 0 if the last operation succeeded.
int Sd_LastErrCode(void);

// --- FatFs mount ------------------------------------------------------------
// Mounts the (only) logical drive against our diskio glue. Call once before
// any f_opendir/f_open. Returns EOS_SD_OK or EOS_SD_MOUNTFAIL.
int Sd_Mount(void);
void Sd_Unmount(void);

// --- resolve an OPEN file to a raw, contiguous LBA run ----------------------
// fp must already be f_open()'d read-only on the mounted SD volume. On
// success, fills outLba/outSectors with the file's raw starting sector and
// sector count, and outSzc with the matching NR_SZC size class. Requires the
// file to be a SINGLE contiguous run on disk (checked via FatFs's official
// fast-seek/CREATE_LINKMAP mechanism, not a hand-rolled FAT walk) and to be
// exactly 256K, 512K, or 1MB -- BIOS images always are. Returns EOS_SD_OK,
// EOS_SD_FRAGMENTED, or EOS_SD_TOOBIG.
//
// A fragmented file is refused outright rather than stitched from multiple
// runs -- see eos_sd_support_spec.md for why this is a deliberate v1 scope
// line, not an oversight: BIOS files are small, freshly-copied, single files
// on (usually freshly-formatted) cards, so contiguity is the expected case,
// and stitching multi-run precache calls is real added complexity for an
// edge case that's cheap to just tell the user to fix (copy fresh).
int Sd_ResolveFile(FIL* fp, unsigned long* outLba, unsigned int* outSectors, int* outSzc);

// --- precache + launch -------------------------------------------------------
// Copies 'sectors' 512-byte blocks starting at 'lba' into NRGN_SD (bulk
// hardware precache -- see eos_sd_precache.v), sets NR_SZC to 'szc' first,
// then launches bank 0x0 via Bank_LaunchEf(0x00) exactly like any other
// resident bank. Does not return on success (same contract as Bank_Launch).
// On failure (card error before launch), returns EOS_SD_TIMEOUT/CARDERR and
// does NOT launch -- the caller stays in the loader.
int Sd_PrecacheAndLaunch(unsigned long lba, unsigned int sectors, int szc);