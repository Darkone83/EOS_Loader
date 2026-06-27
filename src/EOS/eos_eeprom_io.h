// eos_eeprom_io.h -- full 256-byte EEPROM image backup/restore.
//
// The whole EEPROM is read/written through the kernel (ExQueryNonVolatileSetting
// / ExSaveNonVolatileSetting with the magic 0xFFFF index), so this needs no
// SMBus access and links cleanly in the loader. Backup is read-only on the
// console and carries no risk; the write path is guarded separately.
#pragma once

#define EOS_EEPROM_SIZE 256

#define EOS_EE_OK          0
#define EOS_EE_ERR_READ   -1
#define EOS_EE_ERR_WRITE  -2
#define EOS_EE_ERR_FILE   -3
#define EOS_EE_ERR_IMAGE  -4   // image failed the sanity check
#define EOS_EE_ERR_SIZE   -5   // file not exactly 256 bytes

// Read the live 256-byte EEPROM image. Safe (read-only). Returns EOS_EE_OK.
int Eeprom_ReadImage(unsigned char* img256);

// Write a 256-byte image to the EEPROM. DANGER -- a bad image bricks the
// console. Callers MUST validate + back up first. Returns EOS_EE_OK.
int Eeprom_WriteImage(const unsigned char* img256);

// Basic sanity: reject blank/uniform images (all 0x00 / all 0xFF) so a restore
// can refuse obvious garbage before touching the chip. Returns EOS_EE_OK if the
// image looks like real EEPROM content.
int Eeprom_ImageValid(const unsigned char* img256);

// Back up the live EEPROM to E:\Eos\Backups\eeprom_<serial>_NN.bin (NN auto-
// increments so it never overwrites). outPath receives the chosen path.
int Eeprom_BackupToHdd(char* outPath, int outPathLen);

// Load a .bin image (must be exactly 256 bytes) from a DOS path.
int Eeprom_LoadBin(const char* path, unsigned char* img256);


// Enumerate backup .bin basenames in E:\Eos\Backups (for the restore picker).
int  Eeprom_ListBackups(char names[][64], int maxN);
// Read the printable serial (12 chars) from an image into out13.
void Eeprom_ImageSerial(const unsigned char* img256, char* out13);

// --- video standard (factory region) ----------------------------------------
// Raw EEPROM values at offset 0x58 (Checksum2-protected), matching the encoding
// PrometheOS writes. The console reads these at boot to pick NTSC vs PAL timing.
#define EOS_VS_NTSC 0x00014000u
#define EOS_VS_PAL  0x00038000u

// Current raw video standard from the live image (offset 0x58); 0 on read error.
unsigned int Eeprom_GetVideoStandardRaw(void);

// Set the EEPROM video standard (offset 0x58) and recompute Checksum2, after a
// MANDATORY backup of the current EEPROM (path -> backupOut). Verifies the
// written bytes. DANGER (factory write) -- the caller confirms first.
int Eeprom_SetVideoStandard(unsigned int rawVal, char* backupOut, int backupLen);