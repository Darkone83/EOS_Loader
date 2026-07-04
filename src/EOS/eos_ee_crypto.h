// eos_ee_crypto.h -- EEPROM crypto primitives for Eos.
//
// Ported from Team-Assembly / Xbox-Linux XKUtils (GPLv2) via PrometheOS:
// RC4, the Xbox HMAC-SHA1 (per-version intermediate-hash keys), and the
// EEPROM QuickCRC. These are the exact algorithms the Xbox kernel uses to
// (de)crypt and checksum the EEPROM factory/user sections, so decrypting the
// 256-byte image here yields the same plaintext the kernel would.
//
// Credits: SpeedBump, the Xbox-Linux team, Team-Assembly (UNDEAD), PrometheOS.
#pragma once
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

	// RC4 key state.
	typedef struct { UCHAR state[256]; UCHAR x, y; } EeRc4Key;
	void Ee_Rc4Init(const UCHAR* keyData, int keyLen, EeRc4Key* key);
	void Ee_Rc4Crypt(UCHAR* data, int dataLen, EeRc4Key* key);   // symmetric

	// Xbox HMAC-SHA1. version is the Xbox key generation: 9,10,11,12
	// (= 1.0, 1.1, 1.4, 1.6). Variadic (buf,len,...,NULL) like the original.
	void Ee_XboxHmacSha1(int version, UCHAR* result, ...);

	// EEPROM QuickCRC: writes a 4-byte checksum over dataLen bytes.
	void Ee_QuickCRC(UCHAR* crcOut4, const UCHAR* inData, DWORD dataLen);

#ifdef __cplusplus
}
#endif