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
- **Python 3 + `lz4`** and `eos_pack.py` to pack the XBE into a bootable BIOS image.

> The codebase targets the RXDK / MSVC 2003 environment (C89-era constraints, no CRT/heap,
> `/GL` on all units). It is not expected to build under a modern toolchain unchanged.

---

## Building

Build the project with the RXDK / MSVC 2003 toolchain to produce the loader `default.xbe`.

## Packing the BIOS image

The Eos board boots a **2 MB Xenium-style BIOS image** with the loader XBE embedded
(LZ4-compressed) in the XeniumOS bank, and a borrowed Cerbios kernel + bank geometry kept
byte-for-byte. `eos_pack.py` swaps **only** the embedded XBE into a known-good template, so
the kernel's XBE-location expectations stay satisfied — your launcher XBE is the only
variable.

Requires Python 3 + the `lz4` package (`pip install lz4`).

```bash
# pack your built loader into a bootable image
python3 eos_pack.py pack <template.bin> default.xbe eos.bin

# confirm the payload round-trips byte-identical
python3 eos_pack.py verify eos.bin

# (pull an XBE back out of an image)
python3 eos_pack.py unpack eos.bin out.xbe
```

- `<template.bin>` must be the **2 MB Xenium image** (e.g. `Xenium_Prometheos_V1_5_0.bin`) —
  not the 256 K Cerbios `.bin` or the 1 MB RP2040 image.
- The XBE is placed at the XeniumOS bank (`0x100000`); descriptor is `u32 decompressed_size,
  u32 compressed_size` then the raw LZ4 block. Kernel sits at `0x180000`.
- Budget: descriptor + compressed XBE must fit **0x80000 (512 KB)** — `pack` errors on
  overflow and self-verifies the round-trip before writing.

> Stock BIOS images (Cerbios, etc.) flash directly — packing is only for embedding a custom
> loader XBE.

> **Flashing the packed image to the board is chip prep, not loader software** — see the
> firmware README ("Flashing / chip prep"). The loader's job ends at producing `eos.bin`.

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