// EOS DLC/update ContentMeta.xbx signer.
// Original Xbox TDATA packages: derives the title content key from XboxHDKey
// then HMAC-SHA1 signs the ContentMeta header, like PrometheOS.
#pragma once

#define DLC_SCAN_ONLY      0
#define DLC_SIGN_ALL       1
#define DLC_SIGN_INVALID   2

struct EosDlcReport {
    unsigned int found;
    unsigned int valid;
    unsigned int signedCount;
    unsigned int skipped;
    unsigned int failed;
    unsigned int simpleContent;
    unsigned int titleFolders;
};

// Scans E:\TDATA\<8-char TitleId>\$C and $U paths only.
// Runs synchronously: never touches BIOS, flash, or EEPROM.
// mode: 0=inspect, 1=re-sign all, 2=sign only invalid.
// Returns 1 when scan completed, 0 when TDATA isn't accessible.
int Dlc_Run(int mode, EosDlcReport* report);
