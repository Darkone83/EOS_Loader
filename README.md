# EOS_Loader
BIOS management loader for EOS
# Eos — Loader

The Xbox-side application for the Eos modchip. It runs on the Original Xbox, manages BIOS
banks, flashes BIOS images onto the Eos board, stages hard drives, and exposes EEPROM /
NVRAM / settings tooling — all from an on-console UI with FTP and HTTP access.

> Team Resurgent · Darkone83 · **Private — do not distribute**

---

## Features

- **Bank management** — select and manage BIOS banks via the Eos bank register (`0xEF`).
- **BIOS flashing** — write images to the Eos QSPI banks through the flash command bridge.
- **FTP server** — upload/download banks and files over the network. Default `xbox` /
  `xbox`, port 21, up to 2 concurrent sessions.
- **HTTP / OTA** — web access and over-the-air firmware/loader updates.
- **HDD Format (drive staging)** — full standard-partition staging for a fresh Xbox drive
  (E/C/X/Y/Z + F spanning the remainder), with large-drive cluster handling.
- **EEPROM tools** — read / decode / edit / repair / restore, with HMAC-validated crypto.
- **NVRAM, Clock, Settings hub, Network editor** — on-console configuration.
- **Themes + on-screen keyboard + console/log** — themeable UI with text entry.
- **Serve HUD bridge** — surfaces Eos serve state on-console.

---

## Requirements

- An Original Xbox (any revision) with an installed **Eos** board.
- **RXDK / MSVC 2003** toolchain to build the XBE.
- **eos_packer** to package the build into a flashable/distributable image.

> The codebase targets the RXDK / MSVC 2003 environment (C89-era constraints, no CRT/heap,
> `/GL` on all units). It is not expected to build under a modern toolchain unchanged.

---

## Building

1. Build the project with the RXDK / MSVC 2003 toolchain to produce the loader `default.xbe`.
2. Package with **eos_packer** (the known-good packer) to produce the final image:

   ```
   eos_packer <built default.xbe + assets>  ->  <eos loader image>
   ```

   > Use your established eos_packer invocation/output target — the build flow is unchanged
   > from the current working setup. Fill in the exact command line for this repo here.

3. Flash / install the resulting image (see below).

---

## Install

The loader is served by the Eos board as the cold-boot default, so the Xbox boots straight
into it. Update it either by:

- **eos_packer + flash** — repackage and write the new image to the board, or
- **OTA** — use the loader's HTTP/OTA updater to push a new build to a running unit.

---

## Usage

On boot the Xbox lands in the Eos loader UI. From there:

- **Banks** — pick the active BIOS bank; flash new images via FTP/HTTP then commit.
- **Tools → Format** — stage a fresh hard drive. This is a **whole-drive** operation
  (wipes and re-partitions); test on a scratch drive first.
- **FTP** — connect to the console's IP (user `xbox`, pass `xbox`, port 21) to move banks
  and files. Credentials are configurable in settings.
- **Settings** — network, clock, EEPROM, NVRAM, themes, About.

---

## Source layout

```
main.cpp              entry point + frame loop
eos_menu / eos_ui     menu system + UI
eos_gfx / eos_font    rendering + fonts (eos_font_data.h, eos_logo_data.h)
eos_splash / eos_theme  splash + themeable styling
eos_osk / eos_console   on-screen keyboard + console/log
input                 controller input

dd_ftp                FTP server (2-session)
eos_http              HTTP server + OTA updater
dd_net                networking
eos_firmware_io       firmware/loader image I/O

eos_bank              bank register control
eos_flash             flash engine bridge
eos_hdd / eos_format  drive info + HDD staging/format
dd_mount / eos_file   mount lifecycle + file ops

eos_eeprom            EEPROM read/decode/edit/repair/restore
eos_eeprom_io         EEPROM transport
eos_eeprom_crypto     EEPROM HMAC/crypto

eos_config / eos_settings   config + settings hub
eos_clock / eos_nvram       clock + NVRAM
eos_ftol              float->int helper (MSVC2003 /GL: __ftol2_sse)
xboxinternals.h       kernel export declarations
```

---

## Notes

- **Format is destructive.** `Tools → Format` stages an entire drive (full wipe + standard
  partition table). Always validate on a scratch disk.
- **FTP** is single-session-per-slot with a 2-slot pool; a 3rd connection gets `421 All
  sessions in use`.

---

## Credits

Eos loader © Team Resurgent / Darkone83. FTP engine adapted from the DarkDash codebase.