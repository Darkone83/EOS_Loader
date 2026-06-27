// eos_eeprom_crypto.h -- game-region (XBERegion) read/write for the OG Xbox
// EEPROM's RC4-encrypted security block (0x00..0x2F).
//
// The block is: HMAC-SHA1 hash [0x00..0x13] (also the RC4 key seed), Confounder
// [0x14..0x1B], and HDDKey+XBERegion [0x1C..0x2F]. The region byte lives at
// 0x2C, *inside* the encrypted block, so changing it means decrypt -> set ->
// re-encrypt with the correct per-version key (1.0 / 1.1-1.5 / 1.6, auto-
// detected). A bad re-seal corrupts the HDD-key hash and the drive won't unlock,
// so Eeprom_SetGameRegion validates a full in-memory round-trip (HDD key must
// survive) BEFORE it ever writes, and takes a mandatory backup first.
//
// Ported faithfully from PrometheOS XKEEPROM/XKSHA1/XKRC4 (hardware-validated).
#pragma once

// Decrypted XBERegion values (byte at 0x2C).
#define EOS_XBE_NA  0x01   // North America
#define EOS_XBE_JP  0x02   // Japan
#define EOS_XBE_EU  0x04   // Europe / Australia

// Detected EEPROM key version (returned for display; not usually needed).
#define EOS_XV_NONE 0
#define EOS_XV_1_0  10
#define EOS_XV_1_1  11     // covers 1.1 - 1.5
#define EOS_XV_1_6  12

// Current game region from the live EEPROM (decrypts in memory).
// Returns 0x01/0x02/0x04, or -1 if the EEPROM could not be decrypted.
int Eeprom_GetGameRegion(void);

// Detected key version of the live EEPROM (EOS_XV_*), or EOS_XV_NONE.
int Eeprom_GameRegionVersion(void);

// Set the game region. Reads the image, decrypts (auto-detecting the key
// version), changes only the region byte, then PROVES the round-trip in memory
// (re-encrypt -> re-decrypt -> HDD key + region verified) before taking a
// mandatory EEPROM backup (path -> backupOut) and writing. Read-back verified.
// Returns one of the EOS_RGN_* codes. DANGER -- the caller confirms first.
#define EOS_RGN_OK         0
#define EOS_RGN_ERR_READ  -1   // could not read the EEPROM
#define EOS_RGN_ERR_CRYPTO -2  // decrypt failed / round-trip mismatch (NOT written)
#define EOS_RGN_ERR_ARG   -3   // bad region argument
#define EOS_RGN_ERR_BACKUP -4  // mandatory backup failed (NOT written)
#define EOS_RGN_ERR_WRITE -5   // write/verify failed
int Eeprom_SetGameRegion(int region, char* backupOut, int backupLen);