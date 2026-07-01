# Eos — Loader

The on-console app for your Eos-modded Original Xbox. Power on and you land in the Eos
loader: pick which BIOS runs, flash new BIOS images over the network, back up and manage the
EEPROM, set up a fresh hard drive, and personalise the UI — all from the couch with a
controller, or from a browser on your PC.

> Team Resurgent · Darkone83 · **Private — do not distribute**

---

## What you can do

- **Choose your BIOS** — switch between BIOS banks on the Eos board, or boot the console's
  original **TSOP** BIOS.
- **Flash BIOS images** — push a new BIOS to a bank over FTP or the web control panel, then
  commit it.
- **Web control panel** — a browser page for bank management, plus **EEPROM backup / restore**
  and a settings reset.
- **FTP server** — move banks and files to and from the console over your network.
- **EEPROM tools** — read, decode, edit, repair, back up, and restore the Xbox EEPROM.
- **Hard-drive setup** — stage a fresh drive with the standard Xbox partitions.
- **Personalise it** — themes and background music, an on-screen keyboard, plus network,
  clock, and NVRAM settings.

---

## First boot

The console cold-boots straight into the Eos loader UI. Navigate with the **D-pad**, **A**
selects, **B** goes back. The main menu gets you to **Banks**, **Tools**, and **Settings**,
and the loader shows live Eos serve state so you can see the chip working. From here you
choose what actually runs.

---

## Using it

### Banks — choosing and flashing a BIOS

The **Banks** screen lists the BIOS banks on your Eos board. Select one to make it active and
launch it — a warm reset boots into the chosen BIOS. Protected banks (the boot loader and
recovery) are marked **[LOCKED]** and can't be deleted or overwritten by accident.

To **flash a new BIOS**: get the image onto the console (FTP or the web panel), pick a target
bank, and commit — the loader writes it to the Eos flash and can serve it live.

To **boot the stock BIOS**: choose the **TSOP** entry in the bank list. Eos steps aside and
the Xbox boots its original onboard BIOS; a normal power cycle returns you to Eos.

### Web control panel

Point a browser at the console's IP address for a control panel with:

- **Bank management** — rename, delete, and flash banks from your desktop.
- **Backup EEPROM** — download your console's EEPROM as a file. **Do this once and keep it
  safe** — it's your console's identity.
- **Restore EEPROM** — upload a saved EEPROM image back (validated before it's written).
- **Reset Settings** — return the loader's own settings to defaults; your banks are left
  untouched.

### FTP

Connect an FTP client to the console's IP — default user **`xbox`**, password **`xbox`**,
port **21** (all changeable in Settings). Up to two sessions at once; a third gets
"all sessions in use." Use it to upload BIOS images and move files.

### Hard-drive setup (Format)

**Tools → Format** stages a fresh Xbox drive: it lays down the standard partitions
(E, C, X, Y, Z, and F spanning the rest), with correct handling for large drives.

> ⚠️ **This wipes the entire drive.** Only run it on a drive you intend to erase, and test on
> a scratch disk first.

### EEPROM tools

Read, decode, edit, repair, back up, and restore the Xbox EEPROM — the crypto is handled for
you (HMAC-validated). **Back up before you edit.** A bad EEPROM write can stop the console
booting, and the backup is how you recover.

### Settings

- **Network** — IP / DHCP and the FTP credentials.
- **Clock / NVRAM** — console time and NVRAM values.
- **Themes** — recolour the loader UI.
- **Background music** — turn it on and pick a track to play in the loader.
- **About** — version and build info.

---

## Safety

- **Format is destructive** — it wipes and re-partitions the whole drive. Validate on a
  scratch disk.
- **Back up your EEPROM** before editing it or restoring another image — it's your console's
  identity and boot health.
- **Locked banks** (boot loader, recovery) are protected from routine bank management, so you
  can't delete your way into an unbootable state by accident.

---

## For developers

### Requirements

- An Original Xbox (any revision) with an installed **Eos** board.
- **RXDK / MSVC 2003** toolchain to build the XBE.
- **`eos_pack.py`** — the BIOS packer, shipped in the **Eos firmware repo's `Tools/`
  folder** (not part of the loader source). Required to turn your built XBE into a bootable
  BIOS image. Needs **Python 3 + `lz4`** (`pip install lz4`).

> The codebase targets RXDK / MSVC 2003 (C89-era constraints, no CRT/heap, `/GL` on all
> units). It is not expected to build under a modern toolchain unchanged.

### Building

Build with the RXDK / MSVC 2003 toolchain to produce the loader `default.xbe`.

### Packing the BIOS image

Packing is done with **`eos_pack.py`, which lives in the Eos firmware repo's `Tools/`
folder** — it is a required, separate tool, not part of this loader source. Building the
loader produces `default.xbe`; `eos_pack.py` is what turns that into a flashable image.

The Eos board boots a **2 MB Xenium-style BIOS image** with the loader XBE embedded
(LZ4-compressed) in the XeniumOS bank, and a borrowed Cerbios kernel + bank geometry kept
byte-for-byte. `eos_pack.py` swaps **only** the embedded XBE into a known-good template, so
the kernel's XBE-location expectations stay satisfied.

```bash
python3 eos_pack.py pack <template.bin> default.xbe eos.bin   # build the image
python3 eos_pack.py verify eos.bin                             # round-trip check
python3 eos_pack.py unpack eos.bin out.xbe                     # pull an XBE back out
```

- `<template.bin>` must be the **2 MB Xenium image** (e.g. `Xenium_Prometheos_V1_5_0.bin`) —
  not the 256 K Cerbios `.bin` or the 1 MB RP2040 image.
- The XBE is placed at the XeniumOS bank (`0x100000`); descriptor is `u32 decompressed_size,
  u32 compressed_size` then the raw LZ4 block. Kernel sits at `0x180000`.
- Budget: descriptor + compressed XBE must fit **0x80000 (512 KB)** — `pack` errors on
  overflow and self-verifies the round-trip before writing.

> **Flashing the packed image to the board is chip prep, not loader software** — see the
> firmware README ("Flashing / chip prep"). The loader's job ends at producing `eos.bin`.
> Stock BIOS images flash directly; packing is only for embedding a custom loader XBE.

### Source layout

```
main.cpp                  entry point + frame loop
eos_menu / eos_ui         menu system + UI
eos_gfx / eos_font        rendering + fonts (eos_font_data.h, eos_logo_data.h)
eos_splash / eos_theme    splash + themeable styling
eos_audio                 background-music engine (minimp3 -> DirectSound)
eos_osk / eos_console     on-screen keyboard + console/log
input                     controller input

dd_ftp                    FTP server (2-session)
eos_http                  HTTP server + OTA + web EEPROM backup/restore + reset
dd_net                    networking
eos_firmware_io           firmware/loader image I/O

eos_bank                  bank register control + locked-bank flag + TSOP boot
eos_flash                 flash engine bridge
eos_hdd / eos_fotmat      drive info + HDD staging/format  (+ eos_format.h)
dd_mount / eos_file       mount lifecycle + file ops

eos_eeprom                EEPROM read/decode/edit/repair/restore
eos_eeprom_io             EEPROM transport
eos_eeprom_crypto         EEPROM HMAC/crypto

eos_config / eos_settings config + settings hub (theme, background music)
eos_clock / eos_nvram     clock + NVRAM
eos_fotl                  float->int helper (MSVC2003 /GL: __ftol2_sse)

minimp3.h                 bundled MP3 decoder (background music)
stb_image.h               bundled image decoder
xboxinternals.h           kernel export declarations
Media/                    runtime media assets (background-music tracks, etc.)
```

> **minimp3.h and stb_image.h are bundled** — link `dsound.lib` for the audio engine.
>
> **Heads-up — two filenames are typo'd** (the code inside is correct): `eos_fotmat.cpp` is
> the HDD format unit (`eos_format.cpp` internally) and `eos_fotl.cpp` is the ftol shim
> (`eos_ftol.cpp` internally). Rename them if you want the filenames to match; the build just
> needs them included either way.

---

## Credits

Eos loader © Team Resurgent / Darkone83. FTP engine adapted from the DarkDash codebase.