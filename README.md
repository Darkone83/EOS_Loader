# EOS — Loader

<div align="center">

<img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/EOS.png" width="400"><img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/Darkone83.png" width="500">

</div>

The on-console control environment for an **EOS-modded Original Xbox**. Power on and EOS gives you a controller-driven loader for launching and managing BIOS banks, booting BIOS images from SD, maintaining the console, managing EEPROM and HDD functions, controlling optional EOS hardware, and accessing the same system from a browser or FTP client.

---

## Highlights

- **Launch and manage BIOS banks** — boot Banks 1–4, the stock **TSOP**, **XbDiag Lite**, or a compatible BIOS directly from a **FAT32 SD card**.
- **256K / 512K / 1MB BIOS support** — EOS manages the dynamic bank layout and oversized-bank placement automatically.
- **Auto Boot** — mark one user bank as the automatic target and choose a **2–30 second** countdown. Press **B** or physical **EJECT** to cancel.
- **Web control panel** — bank management, SD BIOS management, EEPROM backup/restore, system information, XbDiag controls, theme editing, and loader settings from a browser.
- **FTP server** — move BIOS images and files over the network without a PC-side Xbox-specific utility.
- **EEPROM tools** — inspect, back up, restore, repair, and edit supported EEPROM fields.
- **HDD tools** — drive information, full LBA48 capacity reporting, ATA security lock/unlock, and full-drive setup/formatting.
- **Fan control** — Xbox SMC automatic control or a persistent manual setting from **20–100%** in **5%** UI steps.
- **Power controls** — dedicated **Shutdown** and **Reboot** menu with confirmation before executing either command.
- **EOS Scripts** — load expansion scripts for programmable GPIO / peripheral control.
- **Themes and music** — built-in themes plus custom themes stored on the **HDD or SD card**, including custom background images and music.
- **Compact live system card** — optional top-left CPU / motherboard temperature and RAM readout, including EOS 1.6-mode status when asserted.
- **Optional character LCD** — US2066 or HD44780/PCF8574 status display on the Xbox SMBus.
- **Optional XBOX-RGB handoff effects** — EOS can discover XBOX-RGB and send a short bank-colour effect before BIOS launch.
- **Optional EOS HDMI HUD control** — compatible EOS HDMI hardware can have its onboard HUD enabled or disabled, with the choice persisted in flash.

---

## First boot and navigation

The console cold-boots into EOS. The main menu is:

1. **Launch Bank**
2. **Bank Management**
3. **Tools**
4. **Settings**
5. **Power**
6. **About**

Use the **D-pad** to move, **A** to select, and **B** to return. Menu items are presented on the EOS 3D carousel; receding items fade before they can overlap the fixed EOS logo, title capsule, or bottom helper pill.

The optional **System Info Card** appears at the upper-left and shows live CPU temperature, motherboard temperature, and RAM information. It can be disabled under **Settings → EOS Settings** without stopping the telemetry used by other features such as the LCD.

If an Auto Boot target is configured, EOS displays a countdown after startup. Press **B** or the console **EJECT** button to cancel and remain in the Loader.

---

## BIOS banks

### Launch Bank

The launch screen exposes the active EOS BIOS layout and supported launch targets. User BIOS banks can contain **256K, 512K, or 1MB** images. EOS handles the physical placement and shadow-slot rules required by larger BIOS images.

Protected Loader and recovery regions are not exposed as ordinary writable user banks.

When **XbDiag Lite** is installed, it appears as a launchable diagnostic target. The stock onboard BIOS is available through the **TSOP** entry.

### Auto Boot

Open **Bank Management** and press **WHITE** on an occupied user BIOS in Banks 1–4 to mark it as the Auto Boot target. Only one user bank can be marked at once.

Set the countdown under **Settings → Auto Boot**. SD Card, Recovery, XbDiag Lite, TSOP, empty banks, and shadow slots are not valid Auto Boot targets.

### Flashing BIOS images

Move a BIOS image to the console through FTP or the WebUI, select a target bank, and commit the write. EOS erases, programs, and verifies the target flash region.

Supported image sizes:

- **256 KB**
- **512 KB**
- **1 MB**

The bank manager accounts for the space consumed by oversized BIOS images and prevents incompatible placement.

### Booting from SD

Choose **SD Card** from the launch list and browse a FAT32 card for a compatible BIOS image. EOS resolves and stages the selected file into SDRAM, then launches it without writing that BIOS to EOS flash.

The shared file-browser path is used for both HDD and SD browsing, so path display, selection behaviour, and scrolling remain consistent between storage sources.

---

## Web control panel

Point a browser at the Xbox IP address. The WebUI provides:

- **Bank management** — rename, delete, flash, and launch BIOS banks.
- **SD BIOS Manager** — browse the FAT32 SD card and upload or delete supported BIOS images.
- **System information** — console revision, CPU speed, RAM, video encoder, serial, MAC, video standard, region, language, and EOS bank/slot usage.
- **EEPROM backup** — download the console EEPROM. Keep a known-good copy somewhere safe.
- **EEPROM restore** — validate and restore a saved EEPROM image.
- **XbDiag controls** — detect and clear the XbDiag bank when present.
- **Reset Settings** — restore Loader settings without intentionally erasing user BIOS images.
- **Custom Theme editor** — create and edit HDD-hosted custom themes from a browser.

---

## FTP

Connect an FTP client to the Xbox IP address.

Default credentials:

- **User:** `xbox`
- **Password:** `xbox`
- **Port:** `21`

The credentials can be changed in Settings. EOS supports up to two simultaneous FTP sessions.

---

## Tools

The Tools menu groups the maintenance functions:

- **EEPROM**
- **Firmware**
- **HDD**
- **Fan Control**
- **Cerbios Config Editor**
- **EOS Scripts**
- **Format**
- **Clear Settings**

### EEPROM

EOS can read and decode the 256-byte Xbox EEPROM, verify the security block, create backups, restore validated images, and edit supported fields such as video standard and region data.

Back up the EEPROM before modifying it. The EEPROM contains console-specific identity and security information.

### Firmware backup / restore

Firmware tools can dump supported EOS flash regions to files and restore matching images. Writes are erased, programmed, and verified before completion.

### HDD tools

Drive Info reports model, serial, capacity, and ATA security state. EOS reports the full LBA48 device capacity and mounted FATX partition usage.

**Lock** binds a compatible drive to the current Xbox HDD key. **Unlock** removes ATA security where supported. Destructive or security-sensitive operations require deliberate confirmation.

### Fan Control

**Tools → Fan Control** provides:

- **Auto** — return control to the Xbox SMC thermal policy.
- **Manual** — persistent **20–100%** selection in **5%** UI increments.

The SMC hardware works at its own duty resolution, so a requested value can read back slightly differently. EOS reapplies the selected fan policy when the Loader starts.

### Hard-drive setup / Format

The Format tool stages a fresh Xbox drive with the standard partitions and assigns the remaining capacity to the large data partition. The large-drive path uses full reported disk geometry and selects FATX cluster sizing appropriate to capacity.

> **Warning:** Format is destructive and wipes the selected drive.

### Cerbios tools

EOS includes a Cerbios configuration editor and CPU/GPU overclock calculator, allowing common Cerbios configuration work directly on-console.

### EOS Scripts

EOS Scripts provide access to the expansion engine and programmable EXP pins for custom hardware and peripheral projects.

---

## Settings

The Settings hub contains the normal console and Loader configuration pages.

### Audio

Configure supported Xbox audio/NVRAM options.

### Auto Boot

Set the Auto Boot delay from **2–30 seconds**. The target BIOS itself is chosen in Bank Management.

### Date & Time

Set the Xbox clock manually. With an optional **X-RTC** installed, EOS can seed the console clock from the battery-backed RTC at boot and mirror changes back to it.

### LCD

Configure the optional 20x4 SMBus status display:

- **Disabled**
- **HD44780** through a PCF8574 I2C backpack
- **US2066 / SSD1311** native I2C OLED

Address and brightness options are exposed where supported, and EOS reports whether the configured display responds.

The live LCD layout shows:

- `EOS <spinner> <current screen>`
- `NET <IP address>`
- `CPU <temp> | MB <temp>`
- `RAM <free>/<total>  <loader version>`

During a BIOS launch the display changes to an ** EOS ** screen so the handoff state remains obvious.

### Network

Configure DHCP/static addressing and FTP credentials. Numeric entry uses the EOS on-screen keyboard with validation before values are committed.

### Region / Video

Inspect and edit the supported EEPROM/NVRAM region and video fields exposed by EOS.

### System Info

Displays decoded console and EEPROM information on-console.

### Theme

EOS supports built-in palettes plus custom themes.

Custom themes can be discovered from:

```text
E:\Eos\Themes\<theme>\
SD:\Eos\Themes\<theme>\
```

A custom theme can supply its own palette, background image, and music. EOS persists both the selected custom-theme identity and whether it came from HDD or SD, so selecting an SD theme does not depend on an HDD copy of that theme.

### EOS Settings

Loader-specific hardware/UI controls live in their own page:

- **HDMI HUD** — enable or disable the optional onboard EOS HDMI overlay. The setting is persisted in EOS flash and reapplied at Loader boot.
- **System Info Card** — show or hide the compact upper-left live telemetry card. This preference is also persisted.

Both options default to **On** when no saved 1.0.6 setting exists.

---

## Power

The main-menu **Power** page provides:

- **Shutdown**
- **Reboot**

Both require a second **A** press before EOS sends the command to the Xbox SMC. Reboot uses a full power-cycle path and explicitly returns EOS to the Loader boot bank before the cycle begins.

---

## UI and themes

EOS 1.0.6 keeps the 3D carousel presentation while tightening the visual rules around it:

- selected and receding capsules share consistent geometry;
- pill text is visually centred;
- title/header capsules use a single-draw shape to avoid visible body/end-cap seams;
- receding carousel items fade before entering the top logo/title area or bottom helper area;
- fixed helper/status pills use the same visual language throughout the Loader;
- the compact System Info Card avoids consuming unnecessary screen space over image/model themes.

The built-in **Darkone83** presentation uses the EOS plasma/model background path. Other built-in and custom themes continue to use the same shared colour tokens and UI geometry.

---

## Safety

- **Format wipes the entire selected drive.**
- **Back up the EEPROM** before changing or restoring EEPROM data.
- EOS keeps Loader/recovery areas protected from routine bank-management operations.
- BIOS and firmware writes are verified; do not intentionally interrupt power during a flash operation.

---

## For developers

### Requirements

- Original Xbox with an EOS board.
- **RXDK / MSVC 2003** toolchain for the Loader XBE.
- **Python 3 + `lz4`** for `eos_pack.py`.

The codebase targets the RXDK / MSVC 2003 environment and retains C89-era/compiler constraints. It is not expected to compile unchanged under a modern desktop compiler.

### Building

Build the Loader project to produce `default.xbe`.

### Packing a bootable EOS image

`eos_pack.py` lives with the EOS firmware/tooling rather than in this Loader repository. It replaces the embedded XeniumOS XBE inside a known-good 2 MB Xenium/PrometheOS-style template while preserving the rest of the flash geometry.

```bash
python3 eos_pack.py pack <template.bin> default.xbe eos.bin
python3 eos_pack.py verify eos.bin
python3 eos_pack.py unpack eos.bin out.xbe
```

The embedded XBE region begins at `0x100000`; the kernel region begins at `0x180000`. The XBE descriptor plus raw LZ4 block must fit the 512 KB XeniumOS region.

Packing prepares a flash image. Actual chip programming/firmware preparation belongs to the EOS firmware project.

### Source layout

```text
main.cpp                  phase/frame loop, system card, tools and power UI
eos_menu                  main-menu logic and EOS logo/menu presentation
eos_ui                    shared title/footer/status pills + 3D carousel
eos_gfx / eos_font        rendering, capsule geometry and fonts
eos_splash                EOS logo/splash
eos_theme                  built-in palettes
eos_theme_custom          HDD/SD custom-theme discovery and loading
eos_image                 image -> swizzled Xbox texture loader
eos_model / eos_plasma    Darkone83 embedded model/plasma background
eos_audio                 background-music engine
input                      controller input
eos_osk                    on-screen keyboard

dd_net / dd_ftp           network and FTP services
eos_http                  WebUI / HTTP services
dd_mount / eos_file       mounted filesystem and shared file-browser I/O
eos_sdcard / eos_sddiskio onboard SD + FatFs integration
ff / ffunicode / diskio   vendored FatFs

eos_bank                  bank selection / launch / TSOP / XbDiag control
eos_descriptor            dynamic bank geometry
eos_flash                 flash engine bridge
eos_firmware_io           firmware image backup/restore

eos_hdd / eos_format      HDD information, security and formatting
eos_fan                   SMC fan control
eos_cerbios               Cerbios configuration tools

eos_eeprom                EEPROM UI/operations
eos_eeprom_io             EEPROM transport
eos_ee_crypto             active EEPROM SHA-1/HMAC/RC4/CRC implementation
eos_ee_data               raw EEPROM decode/encode helpers

eos_config                persistent EOS bank/settings storage
eos_settings              Settings hub including EOS Settings
eos_nvram                  Xbox OS-section settings
eos_clock / eos_rtc       console clock and optional X-RTC
eos_lcd                    optional SMBus status LCD
eos_xboxrgb               optional XBOX-RGB launch effects
```

---

## Credits

EOS Loader © Team Resurgent / Darkone83.

FTP functionality is adapted from the DarkDash codebase.

VSC HDD unlock support is based on the PrometheOS implementation by Team Resurgent and its contributors. Western Digital and Seagate vendor-specific recovery flows were adapted for EOS while preserving the original PrometheOS behaviour.
