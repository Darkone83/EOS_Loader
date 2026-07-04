// eos_flash.cpp -- Eos flash command driver (host side of the 0xEC/0xED contract).
// See eos_flash.h. Self-contained: own port-I/O, no kernel imports.
#include "eos_flash.h"

// --- command interface ports ------------------------------------------------
#define EOS_PORT_INDEX   0x00EC
#define EOS_PORT_DATA    0x00ED

// --- register indices (select via 0xEC, then r/w via 0xED) ------------------
#define IDX_OP           0x00
#define IDX_BANK         0x01
#define IDX_PAGELO       0x02
#define IDX_PAGEHI       0x03
#define IDX_PBUF         0x04
#define IDX_GO           0x05
#define IDX_STATUS       0x06
#define IDX_LASTSTAT     0x07
#define IDX_DESCRELOAD   0x0D
#define IDX_ERASEBLK     0x0E

// --- ops --------------------------------------------------------------------
#define OP_ERASE         0
#define OP_PROGRAM       1
#define OP_READ          2
#define OP_SYNC          3   // reload the whole selected bank flash->SDRAM

// --- STATUS bits ------------------------------------------------------------
#define ST_BUSY          0x01
#define ST_DONE          0x02
#define ST_REFUSED       0x04
#define ST_RELOAD        0x08   // SDRAM reload in progress (post-flash)
#define ST_NRGN_READY    0x20   // ext-region resident in SDRAM (bit5)

// Bound on the busy-poll. Each iteration is an LPC I/O read (~1us); a worst-case
// 1MB bank erase is a few seconds, so this is generous headroom, not a tuned value.
#define POLL_LIMIT       60000000L

// --- low-level port I/O (x86 IN/OUT) ----------------------------------------
// Ports 0xEC/0xED are unclaimed by the MCPX and forwarded to the LPC bus, where
// the Eos FPGA decodes them. Identical pattern to eos_bank.cpp's 0xEF access.
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

// --- register helpers -------------------------------------------------------
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

// Wait for the engine to finish the in-flight op.
// GO clears the sticky 'done', so we wait for (!busy && done); 'refused' is
// sticky until the next GO, so it is safe to test here too.
static int waitDone(void)
{
    volatile long t;
    unsigned char st;

    io_out8(EOS_PORT_INDEX, IDX_STATUS);   // park index on STATUS; poll DATA
    for (t = 0; t < POLL_LIMIT; ++t) {
        st = io_in8(EOS_PORT_DATA);
        if (st & ST_REFUSED) return EOS_FLASH_REFUSED;
        if (!(st & ST_BUSY) && (st & ST_DONE)) return EOS_FLASH_OK;
    }
    return EOS_FLASH_TIMEOUT;
}

// After a SYNC, wait for the backend's flash->SDRAM reload to actually finish.
// 'done' only means the command was accepted -- the reload runs on in the
// backend and is visible as STATUS bit3. A full-bank reload holds that bit high
// for many milliseconds, so phase 1 reliably catches the rising edge; if it
// never rises (nothing to copy) we simply fall through as already-current.
static int waitReload(void)
{
    volatile long t;
    unsigned char st;
    int started = 0;

    io_out8(EOS_PORT_INDEX, IDX_STATUS);
    for (t = 0; t < POLL_LIMIT; ++t) {        // phase 1: reload begins
        st = io_in8(EOS_PORT_DATA);
        if (st & ST_REFUSED) return EOS_FLASH_REFUSED;
        if (st & ST_RELOAD) { started = 1; break; }
        if (t > 100000L) break;               // never started -> nothing to reload
    }
    if (!started) return EOS_FLASH_OK;
    for (t = 0; t < POLL_LIMIT; ++t) {        // phase 2: reload completes
        st = io_in8(EOS_PORT_DATA);
        if (!(st & ST_RELOAD)) return EOS_FLASH_OK;
    }
    return EOS_FLASH_TIMEOUT;
}

int Flash_EraseBank(int bankEf)
{
    regw(IDX_OP, (unsigned char)OP_ERASE);
    regw(IDX_BANK, (unsigned char)(bankEf & 0x0F));
    regw(IDX_GO, 1);
    return waitDone();
}

int Flash_ProgramPage(int bankEf, int page, const unsigned char* data256)
{
    int i;

    // Selecting PBUF resets the engine's page-buffer pointer; 256 DATA writes
    // then fill the page (auto-incrementing).
    io_out8(EOS_PORT_INDEX, IDX_PBUF);
    for (i = 0; i < 256; ++i)
        io_out8(EOS_PORT_DATA, data256[i]);

    regw(IDX_OP, (unsigned char)OP_PROGRAM);
    regw(IDX_BANK, (unsigned char)(bankEf & 0x0F));
    regw(IDX_PAGELO, (unsigned char)(page & 0xFF));
    regw(IDX_PAGEHI, (unsigned char)((page >> 8) & 0x0F));
    regw(IDX_GO, 1);
    return waitDone();
}

int Flash_ReadPage(int bankEf, int page, unsigned char* out256)
{
    int rc, i;

    regw(IDX_OP, (unsigned char)OP_READ);
    regw(IDX_BANK, (unsigned char)(bankEf & 0x0F));
    regw(IDX_PAGELO, (unsigned char)(page & 0xFF));
    regw(IDX_PAGEHI, (unsigned char)((page >> 8) & 0x0F));
    regw(IDX_GO, 1);
    rc = waitDone();
    if (rc != EOS_FLASH_OK) return rc;

    // Engine has the page in its buffer; selecting PBUF resets the read pointer,
    // then each DATA read streams the next byte (auto-incrementing).
    io_out8(EOS_PORT_INDEX, IDX_PBUF);
    for (i = 0; i < 256; ++i)
        out256[i] = io_in8(EOS_PORT_DATA);

    return EOS_FLASH_OK;
}

int Flash_Sync(int bankEf)
{
    int rc;
    regw(IDX_OP, (unsigned char)OP_SYNC);
    regw(IDX_BANK, (unsigned char)(bankEf & 0x0F));
    regw(IDX_GO, 1);
    rc = waitDone();                 // command accepted
    if (rc != EOS_FLASH_OK) return rc;
    return waitReload();             // SDRAM copy now matches flash
}

// Sync the NEW REGION (bank 0x0) flash -> SDRAM. The backend maps this bank's
// reload to the dedicated new-region SDRAM home (NRGN_SD, 0x400000), which is
// clear of the loader's own serve window -- so unlike the old assumption, this
// is safe and is REQUIRED: oversized banks are served from that SDRAM copy, so
// without this sync a freshly-flashed large bank isn't resident until a cold
// boot re-runs the preload. Issuing it here makes the bank launchable at once.
int Flash_SyncNewRegion(void)
{
    return Flash_Sync(0x00);         // bank 0x0 -> backend routes to NRGN_SD
}

// Read STATUS bit5: 1 = the ext region is resident in SDRAM (a large bank can
// be served). Lets the loader confirm the post-flash sync actually completed.
int Flash_NewRegionReady(void)
{
    unsigned char st;
    io_out8(EOS_PORT_INDEX, IDX_STATUS);
    st = io_in8(EOS_PORT_DATA);
    return (st & ST_NRGN_READY) ? 1 : 0;
}

int Flash_WriteImage(int bankEf, const unsigned char* data, int len)
{
    int           rc, pages, page, off, i;
    unsigned char pg[256];

    rc = Flash_EraseBank(bankEf);
    if (rc != EOS_FLASH_OK) return rc;

    pages = (len + 255) / 256;
    for (page = 0; page < pages; ++page) {
        off = page * 256;
        for (i = 0; i < 256; ++i)
            pg[i] = (off + i < len) ? data[off + i] : 0xFF;   // pad tail with 0xFF

        rc = Flash_ProgramPage(bankEf, page, pg);
        if (rc != EOS_FLASH_OK) return rc;
    }

    // Programs write flash but no longer self-refresh SDRAM (per-page reloads
    // were dropped back-to-back). One full-bank SYNC makes the served copy match.
    return Flash_Sync(bankEf);
}

// Same as Flash_WriteImage but WITHOUT the trailing SDRAM sync. Used for the
// new region (bank 0x0) and the descriptor block (bank 0xF): these are NOT part
// of the live serve buffer, and syncing them would reload their contents over
// the SDRAM window the running loader is served from -> loader hang. They are
// served fresh from flash on the warm-reset that selects them.
int Flash_WriteImageNoSync(int bankEf, const unsigned char* data, int len)
{
    return Flash_WriteImageAtNoSync(bankEf, 0, data, len);
}

// Write `len` bytes starting at page `startPage` within the bank, no sync. Used
// to place an oversized bank at a specific half of the new region (e.g. the 2nd
// 512K starts at page 0x800 = byte 0x80000). Erases only the 64K blocks the
// image spans, starting at startPage's block, so a write to half 1 does not
// disturb half 0.
int Flash_WriteImageAtNoSync(int bankEf, int startPage, const unsigned char* data, int len)
{
    int           rc, pages, page, off, i;
    unsigned char pg[256];
    int           firstBlock, lastByte, blk;

    // Erase the 64K blocks this image covers (block = 256 pages). We erase per
    // 64K block within the bank via targeted page-0-of-block programming is not
    // possible, so we rely on the bank erase for full-region banks. For the new
    // region we must erase only the covered half; do it by erasing each covered
    // 64K block through the engine's block-erase using the block's base page.
    firstBlock = startPage / 256;                 // 256 pages per 64K block
    lastByte = startPage * 256 + len - 1;
    for (blk = firstBlock; blk <= lastByte / 65536; ++blk) {
        rc = Flash_EraseBlock(bankEf, blk);
        if (rc != EOS_FLASH_OK) return rc;
    }

    pages = (len + 255) / 256;
    for (page = 0; page < pages; ++page) {
        off = page * 256;
        for (i = 0; i < 256; ++i)
            pg[i] = (off + i < len) ? data[off + i] : 0xFF;
        rc = Flash_ProgramPage(bankEf, startPage + page, pg);
        if (rc != EOS_FLASH_OK) return rc;
    }
    return EOS_FLASH_OK;
}

unsigned char Flash_RawStatus(void) { return regr(IDX_STATUS); }
unsigned char Flash_LastFlashSR(void) { return regr(IDX_LASTSTAT); }

// Trigger the FPGA to re-read the bank-layout descriptor block (bank 0xF) after
// the loader has written it. Writing IDX_DESCRELOAD pulses a re-read in the FPGA
// bank engine; geometry (base/size for user slots) updates without a reboot.
// Erase a single 64K block within a bank. `block` is the block index (0 = the
// bank's first 64K, 1 = next, ...). Used to place one 512K half of the new
// region without erasing the other half. Arms block-erase mode, sets the page
// to the block's base page (block*256), fires ERASE (which erases just that
// one block), then disarms the mode.
int Flash_EraseBlock(int bankEf, int block)
{
    int page = block * 256;   // 256 pages per 64K block
    int rc;
    regw(IDX_ERASEBLK, 1);                                  // arm block-erase
    regw(IDX_OP, (unsigned char)OP_ERASE);
    regw(IDX_BANK, (unsigned char)(bankEf & 0x0F));
    regw(IDX_PAGELO, (unsigned char)(page & 0xFF));
    regw(IDX_PAGEHI, (unsigned char)((page >> 8) & 0x1F));
    regw(IDX_GO, 1);
    rc = waitDone();
    regw(IDX_ERASEBLK, 0);                                  // disarm
    return rc;
}

void Flash_ReloadDescriptor(void)
{
    volatile long t;
    unsigned char st;

    regw(IDX_DESCRELOAD, 1);   // pulse: FPGA re-reads the descriptor block

    // The re-read grabs the flash bus (the engine goes busy for ~64 bytes of
    // SPI). We MUST wait for it to finish before any following flash op, or that
    // op races the in-progress reload on the shared engine and the console hangs.
    // Poll STATUS until busy clears (bounded). The reload starts a cycle or two
    // after the pulse, so give it a moment to assert busy, then wait for idle.
    for (t = 0; t < 2000; ++t) {}                 // let the reload assert busy
    io_out8(EOS_PORT_INDEX, IDX_STATUS);
    for (t = 0; t < POLL_LIMIT; ++t) {
        st = io_in8(EOS_PORT_DATA);
        if (!(st & ST_BUSY)) break;                // engine idle -> reload done
    }
}