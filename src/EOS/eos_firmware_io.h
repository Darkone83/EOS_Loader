#pragma once
// eos_firmware_io.h -- per-bank firmware (BIOS image) backup/restore.
//
// Each flash bank is dumped to / restored from its own .bin under
// E:\Eos\Backups\Firmware, streamed a page at a time (no multi-MB buffer).
// Restore is size-matched to the target bank, erases, programs, verifies every
// page, then SYNCs the served SDRAM copy. Per-bank (not a bulk chip image) so a
// restore only ever touches the one slot -- the loader/config/recovery blocks
// are never in the blast radius.
#pragma once

#define FW_OK          0
#define FW_ERR_FILE   -1   // file open/read/write failure
#define FW_ERR_FLASH  -2   // flash engine refused/timed out
#define FW_ERR_SIZE   -3   // bank size invalid, or image != target bank size
#define FW_ERR_VERIFY -4   // read-back mismatch after programming

// Bytes in a bank for an EOS_BANK_SIZE_* code (0 if the code is unknown).
int Firmware_BankBytes(int sizeCode);

// Dump bank[bankIdx]'s flash to E:\Eos\Backups\Firmware\fw_<name>_<ef>_<sz>_NN.bin
// (NN auto-increments). outPath receives the chosen path. Returns FW_OK.
int Firmware_BackupBank(int bankIdx, char* outPath, int outLen);

// List *.bin firmware backups into names[][64]. Returns count.
int Firmware_ListBackups(char names[][64], int maxN);

// Size in bytes of a backup file (for the size-match display). <0 on error.
int Firmware_BinBytes(const char* fileName);   // fileName = basename in the FW folder

// Restore a firmware .bin (basename in the FW folder) to bank[bankIdx]: the file
// must equal the bank's size; erases, programs streamed, verifies each page,
// then SYNCs. Returns FW_OK or an FW_ERR_*.
int Firmware_RestoreBank(const char* fileName, int bankIdx);