# Eos — Loader

<div align=center>

<img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/EOS.png" width=400><img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/Darkone83.png" width=500>

</div>

The on-console app for your Eos-modded Original Xbox. Power on and you land in the Eos
loader: pick which BIOS runs, flash new BIOS images over the network, back up and manage the
EEPROM, run firmware and hard-drive tools, watch live console telemetry, and personalise the
UI — all from the couch with a controller, or from a browser on your PC.

> Team Resurgent · Darkone83 · **Private — do not distribute**

---

## What you can do

- **Choose your BIOS** — switch between BIOS banks on the Eos board, boot the console's
  original **TSOP** BIOS, or launch **XbDiag Lite** when it's installed.
- **Flash BIOS images** — push a new BIOS to a bank over FTP or the web control panel, then
  commit it. Supports **256K, 512K, and 1MB** images; oversized banks are auto-placed into
  the dynamic new-region layout.
- **Firmware backup / restore** — dump any bank to a file and restore it, per bank.
- **Web control panel** — a browser page for bank management, plus **EEPROM backup / restore**,
  system info, XbDiag management, and a settings reset.
- **FTP server** — move banks and files to and from the console over your network.
- **EEPROM tools** — read, decode, edit, repair, back up, and restore the Xbox EEPROM,
  including **video standard** (NTSC-M/J, PAL-I/M) and **game / DVD region** editing.
- **HDD tools** — drive info, plus **ATA security lock / unlock** (bind a drive to this
  console or remove security).
- **Hard-drive setup** — stage a fresh drive with the standard Xbox partitions.
- **Live telemetry HUD** — a top-right overlay shows **CPU / motherboard temperature** and
  **RAM** while you use the loader.
- **Personalise it** — themes and background music, an on-screen keyboard, plus network,
  clock, and NVRAM settings.

---

## First boot

The console cold-boots straight into the Eos loader UI. Navigate with the **D-pad**, **A**
selects, **B** goes back. The main menu gets you to **Banks**, **Tools**, and **Settings**,
and a top-right HUD shows live console telemetry (CPU / motherboard temperature and RAM). From
here you choose what actually runs.

---

## Using it

### Banks — choosing and flashing a BIOS

The **Banks** screen lists the BIOS banks on your Eos board. Select one to make it active and
launch it — a warm reset boots into the chosen BIOS. Protected banks (the boot loader and
recovery) are marked **[LOCKED]** and can't be deleted or overwritten by accident. When
**XbDiag Lite** is installed on the board, it also appears here as a launchable entry.

To **flash a new BIOS**: get the image onto the console (FTP or the web panel), pick a target
bank, and commit — the loader writes it to the Eos flash and can serve it live. **256K, 512K,
and 1MB** images are supported; larger images are automatically placed into the board's
dynamic new-region layout and the bank management screen shows the free-slot budget.

To **boot the stock BIOS**: choose the **TSOP** entry in the bank list. Eos steps aside and
the Xbox boots its original onboard BIOS; a normal power cycle returns you to Eos.

### Web control panel

Point a browser at the console's IP address for a control panel with:

- **Bank management** — rename, delete, and flash banks from your desktop.
- **System info** — console revision, CPU speed, RAM, encoder, serial, MAC, video standard,
  region, language, and bank/slot usage.
- **Backup EEPROM** — download your console's EEPROM as a file. **Do this once and keep it
  safe** — it's your console's identity.
- **Restore EEPROM** — upload a saved EEPROM image back (validated before it's written).
- **XbDiag** — when XbDiag Lite is installed, the panel shows it and can clear it from its bank.
- **Reset Settings** — return the loader's own settings to defaults; your banks are left
  untouched.

### FTP

Connect an FTP client to the console's IP — default user **`xbox`**, password **`xbox`**,
port **21** (all changeable in Settings). Up to two sessions at once; a third gets
"all sessions in use." Use it to upload BIOS images and move files.

### Tools

**Tools** groups the maintenance functions: **EEPROM**, **Firmware**, **HDD**, **Format**, and
**Clear Settings**.

### EEPROM tools

Read, decode, edit, repair, back up, and restore the Xbox EEPROM — the crypto is handled for
you (HMAC-validated). Editable fields include the **video standard** (NTSC-M, NTSC-J, PAL-I,
PAL-M) and the **game / DVD region**. **Back up before you edit.** A bad EEPROM write can stop
the console booting, and the backup is how you recover.

### Firmware tools

**Backup** dumps a chosen bank's flash to a file on the drive, and **Restore** writes a saved
firmware image back to a size-matched bank (erased, programmed, and verified page-by-page).

### HDD tools

**Drive Info** shows the model, serial, size, and ATA security state. **Lock** binds the drive
to this console's key; **Unlock** removes security. Both are armed (press twice) to prevent an
accidental change.

### Hard-drive setup (Format)

**Tools → Format** stages a fresh Xbox drive: it lays down the standard partitions
(E, C, X, Y, Z, and F spanning the rest), with correct handling for large drives.

> ⚠️ **This wipes the entire drive.** Only run it on a drive you intend to erase, and test on
> a scratch disk first.

### Clear Settings

Resets the loader's config banks (bank table + settings) to factory. Bank names and saved
settings are wiped; flashed BIOS images are not touched.

### Settings

- **Network** — IP / DHCP and the FTP credentials.
- **Video / Audio / Region** — EEPROM-backed video standard, and game / DVD region editing.
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
main.cpp                  entry point + frame loop + telemetry HUD overlay
eos_menu / eos_ui         menu system + UI
eos_gfx / eos_font        rendering + fonts (eos_font_data.h, eos_logo_data.h)
eos_splash / eos_theme    splash + themeable styling
eos_audio                 background-music engine (minimp3 -> DirectSound)
eos_osk                   on-screen keyboard
eos_console               console read (revision, CPU MHz, temps, RAM) for the HUD
input                     controller input

dd_ftp                    FTP server (2-session)
eos_http                  HTTP server + OTA + web EEPROM backup/restore + sysinfo + reset
dd_net                    networking
eos_firmware_io           firmware/loader image I/O + per-bank backup/restore

eos_bank                  bank register control + locked-bank flag + TSOP boot + XbDiag detect
eos_descriptor            dynamic bank geometry descriptor (256K/512K/1MB slot layout)
eos_flash                 flash engine bridge
eos_hdd / eos_format      drive info + ATA security + HDD staging/format
dd_mount / eos_file       mount lifecycle + file ops

eos_eeprom                EEPROM read/decode/edit/repair/restore
eos_eeprom_io             EEPROM transport
eos_ee_crypto             EEPROM SHA-1 / HMAC / RC4 / CRC (ported crypto core)
eos_ee_data               EEPROM raw-image decode/encode (video std, region, serial, MAC)

eos_config / eos_settings config + settings hub (video/region, theme, background music)
eos_clock / eos_nvram     clock + NVRAM
eos_ftoi                  float->int helper (MSVC2003 /GL: __ftol2_sse)

minimp3.h                 bundled MP3 decoder (background music)
stb_image.h               bundled image decoder
xboxinternals.h           kernel export declarations
Media/                    runtime media assets (background-music tracks, etc.)
```

> **minimp3.h and stb_image.h are bundled** — link `dsound.lib` for the audio engine.
>
> The EEPROM crypto lives in `eos_ee_crypto` + `eos_ee_data` (raw 256-byte image decode with
> HMAC-validated security block). An older `eos_eeprom_crypto.cpp` remains on disk but is **not
> part of the build** (excluded from the project) — it has been superseded.

---

## Credits

Eos loader © Team Resurgent / Darkone83. FTP engine adapted from the DarkDash codebase.