// eos_flash.h -- Eos flash command driver (host side of the 0xEC/0xED contract).
//
// The Eos FPGA exposes a floor-guarded flash engine behind a two-port index/data
// register block: write port 0xEC = INDEX, then read/write port 0xED = DATA.
// The host never sees a physical flash address -- it passes a BANK number and a
// PAGE within that bank, and the FPGA computes phys = 0x200000 + bank_base +
// page*256, so it is impossible to address below the bitstream floor.
//
//   erase a bank   : OP=ERASE, BANK=n, GO, poll STATUS
//   program a page : fill 256-byte page buffer, OP=PROGRAM, BANK=n, PAGE=p, GO, poll
//
// All calls block until the engine reports the operation complete (the FPGA
// runs the real flash WIP poll). Return 0 on success.
//
// RXDK / MSVC2003 / C89: declarations before statements, file-scope statics,
// no CRT, inline __asm port I/O.
#pragma once

#define EOS_FLASH_OK        0
#define EOS_FLASH_REFUSED  -1   // engine rejected the op (bad op/bank, below floor)
#define EOS_FLASH_TIMEOUT  -2   // engine never reported done (bus/FPGA fault)

// bankEf: the bank's 0xEF value; only the low nibble (bank number) is used.
int Flash_EraseBank(int bankEf);
int Flash_ProgramPage(int bankEf, int page, const unsigned char* data256);

// Read one 256-byte page from a bank back into out256 (config read-back, and
// verify-after-flash). The FPGA reads flash into its page buffer, then the host
// streams it out the 0xED port. Returns 0 on success.
int Flash_ReadPage(int bankEf, int page, unsigned char* out256);

// Erase the bank, then program the image as 256-byte pages (tail padded 0xFF),
// then SYNC so the served SDRAM copy matches flash. len may be any size up to
// the bank's capacity. Returns 0 on success.
int Flash_WriteImage(int bankEf, const unsigned char* data, int len);

// Write an image WITHOUT the trailing SDRAM sync -- for the new region (0x0)
// and descriptor (0xF), which must not disturb the live serve buffer.
int Flash_WriteImageNoSync(int bankEf, const unsigned char* data, int len);

// Write `len` bytes starting at page `startPage` within the bank, no sync.
int Flash_WriteImageAtNoSync(int bankEf, int startPage, const unsigned char* data, int len);

// Erase a single 64K block (index) within a bank. For placing one new-region half.
int Flash_EraseBlock(int bankEf, int block);

// Reload a bank's flash contents into the served SDRAM copy. WriteImage calls
// this for you; exposed for a standalone "refresh after flashing" action.
int Flash_Sync(int bankEf);
int Flash_SyncNewRegion(void);   // sync bank 0x0 -> SDRAM NRGN_SD (post large-flash)
int Flash_NewRegionReady(void);  // STATUS bit5: ext region resident in SDRAM

// Raw register reads (diagnostics / progress UI).
unsigned char Flash_RawStatus(void);    // {.., refused, done, busy}
unsigned char Flash_LastFlashSR(void);  // flash status register from the last poll

// Tell the FPGA to re-read the bank-layout descriptor (bank 0xF) after writing
// it, so dynamic bank geometry updates live (no reboot needed).
void Flash_ReloadDescriptor(void);