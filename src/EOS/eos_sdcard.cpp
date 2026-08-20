// eos_sdcard.cpp -- see eos_sdcard.h. Self-contained port-I/O, same pattern as
// eos_flash.cpp (separate register block, same 0xEC/0xED ports -- the FPGA
// multiplexes everything behind one INDEX/DATA pair).
#include "eos_sdcard.h"
#include "eos_bank.h"     // Bank_LaunchEf

// --- command interface ports (same physical ports as eos_flash.cpp) --------
#define EOS_PORT_INDEX   0x00EC
#define EOS_PORT_DATA    0x00ED

// --- register indices: bulk precache -----------------------------------
#define IDX_NR_SZC       0x13
#define IDX_SD_LBA0      0x14
#define IDX_SD_LBA1      0x15
#define IDX_SD_LBA2      0x16
#define IDX_SD_LBA3      0x17
#define IDX_SD_BLKLO     0x18
#define IDX_SD_BLKHI     0x19
#define IDX_SD_GO        0x1A
#define IDX_SD_STATUS    0x1B
#define IDX_SD_STATUS2   0x1C

// --- register indices: single-sector browse read ------------------------
#define IDX_SD_BR_LBA0     0x1D
#define IDX_SD_BR_LBA1     0x1E
#define IDX_SD_BR_LBA2     0x1F
#define IDX_SD_BR_LBA3     0x20
#define IDX_SD_BR_GO       0x21
#define IDX_SD_BR_BUF      0x22
#define IDX_SD_BR_STATUS   0x23
#define IDX_SD_BR_STATUS2  0x24

// --- register indices: single-sector write ------------------------------
#define IDX_SD_BW_LBA0     0x25
#define IDX_SD_BW_LBA1     0x26
#define IDX_SD_BW_LBA2     0x27
#define IDX_SD_BW_LBA3     0x28
#define IDX_SD_BW_BUF      0x29
#define IDX_SD_BW_GO       0x2A
#define IDX_SD_BW_STATUS   0x2B
#define IDX_SD_BW_STATUS2  0x2C

// STATUS2 bits (both precache and browse use the same layout)
#define ST2_BUSY   0x01
#define ST2_DONE   0x02
#define ST2_ERR    0x04

// Bound on busy-polls. A single sector read is well under a millisecond even
// with LPC contention; a full 1MB precache can legitimately take a couple of
// seconds, so its own poll uses a much larger bound (see Sd_PrecacheAndLaunch).
#define POLL_LIMIT_SECTOR   2000000L
#define POLL_LIMIT_WRITE    20000000L
#define POLL_LIMIT_PRECACHE 200000000L

// --- low-level port I/O (x86 IN/OUT) ----------------------------------------
static void io_out8(unsigned short port, unsigned char val)
{
    __asm
    {
        mov dx, port
        mov al, val
        out dx, al
    }
}

static unsigned char io_in8(unsigned short port)
{
    unsigned char v;
    __asm
    {
        mov dx, port
        in  al, dx
        mov v, al
    }
    return v;
}

static void regw(unsigned char idx, unsigned char val)
{
    io_out8(EOS_PORT_INDEX, idx);
    io_out8(EOS_PORT_DATA, val);
}

static unsigned char regr(unsigned char idx)
{
    io_out8(EOS_PORT_INDEX, idx);
    return io_in8(EOS_PORT_DATA);
}

static int s_lastErrCode = 0;
int Sd_LastErrCode(void) { return s_lastErrCode; }

// ---- single-sector browse read ---------------------------------------------
int Sd_ReadSector(unsigned long lba, unsigned char* buf512)
{
    volatile long t;
    unsigned char st2;
    int i;

    regw(IDX_SD_BR_LBA0, (unsigned char)(lba & 0xFF));
    regw(IDX_SD_BR_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    regw(IDX_SD_BR_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    regw(IDX_SD_BR_LBA3, (unsigned char)((lba >> 24) & 0xFF));
    regw(IDX_SD_BR_GO, 1);

    io_out8(EOS_PORT_INDEX, IDX_SD_BR_STATUS2);
    for (t = 0; t < POLL_LIMIT_SECTOR; ++t) {
        st2 = io_in8(EOS_PORT_DATA);
        if (!(st2 & ST2_BUSY) && (st2 & ST2_DONE)) break;
    }
    if (t == POLL_LIMIT_SECTOR) { s_lastErrCode = 14; return EOS_SD_TIMEOUT; }

    if (st2 & ST2_ERR) {
        s_lastErrCode = regr(IDX_SD_BR_STATUS) & 0x0F;
        return EOS_SD_CARDERR;
    }
    s_lastErrCode = 0;

    // selecting SD_BR_BUF resets the streaming read pointer; 512 reads then
    // stream the sector, auto-incrementing (same shape as flash's PBUF).
    io_out8(EOS_PORT_INDEX, IDX_SD_BR_BUF);
    for (i = 0; i < 512; ++i)
        buf512[i] = io_in8(EOS_PORT_DATA);

    return EOS_SD_OK;
}

// ---- single-sector write --------------------------------------------------
// Fill the gateware's 512-byte staging buffer, then issue one CMD24 write.
// The write path deliberately mirrors Sd_ReadSector(): one raw sector per
// command keeps FatFs simple and avoids needing a multi-block SD protocol.
int Sd_WriteSector(unsigned long lba, const unsigned char* buf512)
{
    volatile long t;
    unsigned char st2;
    int i;

    regw(IDX_SD_BW_LBA0, (unsigned char)(lba & 0xFF));
    regw(IDX_SD_BW_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    regw(IDX_SD_BW_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    regw(IDX_SD_BW_LBA3, (unsigned char)((lba >> 24) & 0xFF));

    // Selecting SD_BW_BUF resets its write pointer. Each data write pushes
    // one byte and auto-increments, exactly like the flash page buffer.
    io_out8(EOS_PORT_INDEX, IDX_SD_BW_BUF);
    for (i = 0; i < 512; ++i)
        io_out8(EOS_PORT_DATA, buf512[i]);

    regw(IDX_SD_BW_GO, 1);

    io_out8(EOS_PORT_INDEX, IDX_SD_BW_STATUS2);
    for (t = 0; t < POLL_LIMIT_WRITE; ++t) {
        st2 = io_in8(EOS_PORT_DATA);
        if (!(st2 & ST2_BUSY) && (st2 & ST2_DONE)) break;
    }
    if (t == POLL_LIMIT_WRITE) { s_lastErrCode = 11; return EOS_SD_TIMEOUT; }

    if (st2 & ST2_ERR) {
        s_lastErrCode = regr(IDX_SD_BW_STATUS) & 0x0F;
        return EOS_SD_CARDERR;
    }
    s_lastErrCode = 0;
    return EOS_SD_OK;
}

int Sd_CardReady(void)
{
    // Cheapest available probe: read sector 0 into a scratch buffer. err_code
    // 14 specifically means "card not ready yet" (still bringing itself up,
    // or init failed) -- any other outcome means the card answered.
    static unsigned char scratch[512];
    int rc = Sd_ReadSector(0, scratch);
    return (rc == EOS_SD_OK) ? 1 : 0;
}

// ---- FatFs mount ------------------------------------------------------------
static FATFS s_fatfs;
static int   s_mounted = 0;

int Sd_Mount(void)
{
    FRESULT fr = f_mount(&s_fatfs, "", 1);   // opt=1: mount now, not deferred
    if (fr != FR_OK) { s_mounted = 0; return EOS_SD_MOUNTFAIL; }
    s_mounted = 1;
    return EOS_SD_OK;
}

void Sd_Unmount(void)
{
    if (s_mounted) { f_mount(0, "", 0); s_mounted = 0; }
}

// ---- resolve an open file to a raw contiguous LBA run ----------------------
// CLMT layout per FatFs's own f_lseek(CREATE_LINKMAP): clmt[0] is the table
// size on input / items-used on output; a single contiguous run produces
// exactly 4 items {size=4, run_len_clusters, start_cluster, 0-terminator}.
// More than 4 means more than one fragment. See ff.c's f_lseek for the exact
// semantics this mirrors -- not reimplemented by hand, just consumed as
// documented, so we inherit FatFs's own tested cluster-chain walk rather than
// re-deriving it.
#define CLMT_SIZE 8
static DWORD s_clmt[CLMT_SIZE];

int Sd_ResolveFile(FIL* fp, unsigned long* outLba, unsigned int* outSectors, int* outSzc)
{
    FRESULT fr;
    DWORD startCluster, clusterRun;
    unsigned long sectors;
    unsigned long lba;

    s_clmt[0] = CLMT_SIZE;
    fp->cltbl = s_clmt;
    fr = f_lseek(fp, CREATE_LINKMAP);
    if (fr != FR_OK) return EOS_SD_FRAGMENTED;   // FR_NOT_ENOUGH_CORE -> table too
    // small -> more than 1 fragment
    if (s_clmt[0] != 4) return EOS_SD_FRAGMENTED;   // more than one run

    clusterRun = s_clmt[1];
    startCluster = s_clmt[2];

    lba = (unsigned long)(s_fatfs.database + (LBA_t)(startCluster - 2) * s_fatfs.csize);
    sectors = (unsigned long)clusterRun * s_fatfs.csize;

    if (sectors == 512)       *outSzc = EOS_SD_SZC_256K;
    else if (sectors == 1024) *outSzc = EOS_SD_SZC_512K;
    else if (sectors == 2048) *outSzc = EOS_SD_SZC_1MB;
    else return EOS_SD_TOOBIG;   // not one of the three standard BIOS sizes

    *outLba = lba;
    *outSectors = (unsigned int)sectors;
    return EOS_SD_OK;
}

// ---- bulk precache + launch -------------------------------------------------
int Sd_PrecacheAndLaunch(unsigned long lba, unsigned int sectors, int szc)
{
    volatile long t;
    unsigned char st2;

    regw(IDX_NR_SZC, (unsigned char)(szc & 0x03));
    regw(IDX_SD_LBA0, (unsigned char)(lba & 0xFF));
    regw(IDX_SD_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    regw(IDX_SD_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    regw(IDX_SD_LBA3, (unsigned char)((lba >> 24) & 0xFF));
    regw(IDX_SD_BLKLO, (unsigned char)(sectors & 0xFF));
    regw(IDX_SD_BLKHI, (unsigned char)((sectors >> 8) & 0x0F));
    regw(IDX_SD_GO, 1);

    io_out8(EOS_PORT_INDEX, IDX_SD_STATUS2);
    for (t = 0; t < POLL_LIMIT_PRECACHE; ++t) {
        st2 = io_in8(EOS_PORT_DATA);
        if (!(st2 & ST2_BUSY) && (st2 & ST2_DONE)) break;
    }
    if (t == POLL_LIMIT_PRECACHE) { s_lastErrCode = 14; return EOS_SD_TIMEOUT; }

    if (st2 & ST2_ERR) {
        s_lastErrCode = regr(IDX_SD_STATUS) & 0x0F;
        return EOS_SD_CARDERR;
    }

    // Precache landed cleanly -- launch bank 0x0 exactly like any other
    // resident bank. Does not return.
    Bank_LaunchEf(0x00);
    return EOS_SD_OK;   // unreachable
}