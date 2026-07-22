# Eos — Loader

<div align=center>

<img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/EOS.png" width=400><img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/Darkone83.png" width=500>

</div>

The on-console app for your Eos-modded Original Xbox. Power on and you land in the Eos
loader: pick which BIOS runs, flash new BIOS images over the network, back up and manage the
EEPROM, run firmware and hard-drive tools, watch live console telemetry, and personalise the
UI — all from the couch with a controller, or from a browser on your PC.

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
- **Personalise it** — recolour the UI with built-in themes, or build your own **custom themes**
  (a background image and a music track) right in the browser and pick them on the console. Plus
  an on-screen keyboard and network / clock / NVRAM settings.
- **Keep the time** — set the console clock by hand; with an optional **X-RTC** clock module
  installed, the time is saved to the battery-backed chip and survives a full power-off.
- **Status LCD** — drive an optional **character LCD** on the Xbox SMBus (US2066 OLED or an
  HD44780 via a PCF8574 backpack) showing a live status screen: current screen, IP, temps,
  and free RAM.

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
- **Theme editor** — create, edit, and delete **custom themes** (colours, a background image, and
  a music track) from your browser. Saved themes appear on the console's theme picker straight away.

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

### Themes

The loader ships with several **built-in colour themes**. Open **Settings -> Themes** and scroll
the theme list left/right to preview them live; keep scrolling **past the built-ins** and any
**custom themes** on your drive appear in the same list.

A **custom theme** is its own look: a full-screen **background image**, a **music track**, and a
colour palette. Selecting one applies its colours and background and plays its music in place of
the global background music; switching back to a built-in theme restores the normal look.

The easy way to make one is the **web theme editor** (browser -> *Custom Themes* card -> *Create
Theme*): pick your colours, upload a background (PNG or JPG) and an optional MP3, and save. It
writes the theme to the drive under `E:\Eos\Themes\<name>\` and it shows up on the console's
theme picker with no reflash. **Edit** and **Delete** live there too. Oversized background images
are resized in the browser before upload, so uploads stay small.

### Status LCD

The loader can drive an optional **20x4 character LCD** wired to the Xbox **SMBus** (the same
bus as the X-RTC and the temperature sensors). Two controller families are supported:

- **US2066 / SSD1311** OLED modules (native I2C, addresses 0x3C / 0x3D) -- brightness
  adjustable in software.
- **HD44780** character LCDs on a **PCF8574 I2C backpack** (the common "I2C 1602/2004",
  addresses 0x27 / 0x3F) -- brightness set by the pot on the module.

It shows a fixed status screen -- current menu / selected item, IP address, CPU and
motherboard temperatures, and free RAM -- and freezes on a "Booting <bank>" message when you
launch. The screen only redraws the parts that change, so an idle loader barely touches the
bus. Configure it under **Settings -> LCD** (below); with no panel connected the feature is
simply inert.

### Settings

- **Network** — IP / DHCP and the FTP credentials.
- **Video / Audio / Region** — EEPROM-backed video standard, and game / DVD region editing.
- **Clock / NVRAM** — console time and NVRAM values. With an optional **X-RTC** clock module
  fitted, the Date & Time screen saves changes to it and restores the time from it on boot.
- **Themes** — recolour the loader UI with a built-in palette, or select a **custom theme**
  (its own background image and music). Scroll past the built-in themes to reach any on the drive.
- **Background music** — turn it on and pick a track for the loader. (A custom theme with its own
  music plays that instead while it's active.)
- **LCD** — set the **Driver** (Disabled / HD44780 / US2066), the I2C **Address**, and
  **Brightness** (US2066 only); a live **Detected** line confirms the panel is answering.
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
eos_splash / eos_theme    splash + themeable styling (built-in palettes + custom-theme backdrop)
eos_image                 image-file -> swizzled texture loader (custom-theme backgrounds; stb_image)
eos_theme_custom          disk-loaded custom themes (theme.ini parser, folder scan, set.dat select)
eos_audio                 background-music engine (minimp3 -> DirectSound)
eos_osk                   on-screen keyboard
eos_console               console read (revision, CPU MHz, temps, RAM) + SMBus access for other modules
input                     controller input

dd_ftp                    FTP server (2-session)
eos_http                  HTTP server + OTA + web EEPROM backup/restore + sysinfo + reset + theme editor
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

eos_config / eos_settings config + settings hub (video/region, theme + custom-theme picker, music)
eos_clock / eos_nvram     clock + NVRAM
eos_rtc                   optional X-RTC (DS1307-class SMBus clock): read/write + boot seed & mirror
eos_lcd                   optional SMBus status LCD (US2066 + HD44780/PCF8574 drivers, lcd.dat)
eos_ftoi                  float->int helper (MSVC2003 /GL: __ftol2_sse)

minimp3.h                 bundled MP3 decoder (background music)
stb_image.h               bundled image decoder
xboxinternals.h           kernel export declarations
Media/                    runtime media assets (background-music tracks, etc.)
```

> **minimp3.h and stb_image.h are bundled** — link `dsound.lib` for the audio engine.
>
> `eos_image.cpp`, `eos_theme_custom.cpp`, `eos_rtc.cpp`, and `eos_lcd.cpp` are separate
> translation units — make sure they're in the project's compile list.
>
> The EEPROM crypto lives in `eos_ee_crypto` + `eos_ee_data` (raw 256-byte image decode with
> HMAC-validated security block). An older `eos_eeprom_crypto.cpp` remains on disk but is **not
> part of the build** (excluded from the project) — it has been superseded.

---

## Credits

Eos loader © Team Resurgent / Darkone83. FTP engine adapted from the DarkDash codebase.
