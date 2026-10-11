# EOS — Loader

<div align="center">

<img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/EOS.png" width="400"><img src="https://github.com/Darkone83/EOS_Loader/blob/main/images/Darkone83.png" width="500">

</div>

The on-console control environment for an **EOS-modded Original Xbox**. Power on and EOS gives you a controller-driven loader for launching and managing BIOS banks, booting compatible BIOS images from SD, maintaining the console, controlling optional EOS hardware, and using the WebUI or FTP from another device on your network.

**Current release: 1.0.7.** This release adds selectable menu layouts, a contextual Bank Details Card, controller-button glyphs, a DLC / Update Signer, primary/secondary HDD formatting, and a full WebUI File Manager.

---

## Highlights

- **Launch and manage BIOS banks** — boot Banks 1–4, the stock **TSOP**, **XbDiag Lite**, or a compatible BIOS from a **FAT32 SD card**.
- **256 KB / 512 KB / 1 MB BIOS support** — dynamic bank layout and oversized-bank placement, with linked-slot information visible in Bank Management.
- **Auto Boot** — select a user bank and configure a **2–30 second** countdown; **B** or the physical **EJECT** button cancels it.
- **Four menu layouts** — **Classic 3D**, **Dynamic Grid**, **Floating Bubbles 3D**, and **Orbit 3D**, saved independently of the chosen theme.
- **Bank Details Card** — selected bank, capacity, state, autoboot, LED color, and slot allocation alongside Bank Management.
- **Web control panel** — BIOS and SD management, EEPROM tools, custom themes, system information, and the **File Manager** from a browser.
- **WebUI File Manager** — browse mounted HDD0/HDD1 and SD volumes; upload, download, rename, create folders, and delete files or folders.
- **FTP server** — transfer files using a standard FTP client.
- **EEPROM tools** — inspect, back up, restore, repair, and edit supported EEPROM fields.
- **HDD tools** — drive identification, LBA48 capacity, ATA security, and drive formatting with explicit **HDD0/HDD1** selection.
- **DLC / Update Signer** — scan installed content metadata and sign all entries or only invalid signatures.
- **Fan control** — SMC Auto or persistent Manual **20–100%** in **5%** UI steps.
- **Power controls** — dedicated **Shutdown** and **Reboot** actions with confirmation.
- **EOS Scripts** — use the expansion engine for programmable GPIO and peripheral control.
- **Themes and music** — built-in themes and custom themes stored on the **HDD or SD**, including backgrounds and music.
- **Live system information** — optional CPU, motherboard, and RAM information card, including the EOS 1.6-mode indicator.
- **Colored controller hints** — recognizable A/B/X/Y, Black/White, D-pad, and Start prompts in the interface.
- **Optional 20x4 LCD**, **XBOX-RGB handoff effects**, and **EOS HDMI HUD control** on compatible hardware.

---

## First boot and navigation

The console cold-boots into EOS. The main menu is:

1. **Launch Bank**
2. **Bank Management**
3. **Tools**
4. **Settings**
5. **Power**
6. **About**

Use the **D-pad** to navigate, **A** to select, and **B** to return. The available directions follow the active menu layout; the on-screen footer shows context-specific controls using colored controller glyphs.

The **Classic 3D** carousel remains the default. The other layouts change menu presentation and directional focus, not the underlying operations. There are no added screen transitions.

The optional **System Info Card** appears in the upper-left and shows live CPU temperature, motherboard temperature, and RAM information. It can be disabled under **Settings → EOS Settings** without stopping telemetry used by other components, including the LCD.

If Auto Boot is configured, EOS displays a countdown after startup. Press **B** or the console **EJECT** button to cancel and remain in the Loader.

---

## BIOS banks

### Launch Bank

The launch screen provides quick access to the current EOS bank layout. User BIOS banks support **256 KB**, **512 KB**, and **1 MB** images, with the placement and linked-slot rules handled by EOS.

The configured Auto Boot bank receives a distinct visual indication, without adding an extra launch step. Protected Loader and recovery areas are not ordinary writable user banks.

If installed, **XbDiag Lite** appears as a launchable diagnostic target. The onboard BIOS can be selected through **TSOP**.

### Bank Management

Bank Management retains its established actions for renaming, erasing, locking, assigning autoboot, and configuring LED color. The read-only **Bank Details Card** sits on the left, beneath the System Info Card when it is enabled.

The card follows the highlighted bank and shows:

- Bank name and number, capacity, and state.
- Whether the bank is the Auto Boot target or is locked.
- Configured bank LED color.
- A four-slot allocation view, including the ownership of linked **512 KB** and **1 MB** BIOS images.

The menu uses the region to the right of the card. The selected global layout still applies; no new Bank Management navigation flow was introduced.

### Auto Boot

In **Bank Management**, press **WHITE** on an occupied user BIOS in Banks 1–4 to choose the Auto Boot target. Only one user bank can be selected at a time.

Change the countdown under **Settings → Auto Boot**. SD Card, Recovery, XbDiag Lite, TSOP, empty banks, and shadow slots are not valid Auto Boot targets.

### Flashing BIOS images

Transfer a compatible BIOS image through FTP or the WebUI and select its target in Bank Management. EOS handles erasing and programming, with operation feedback identifying the result and the affected bank allocation.

Supported image sizes:

- **256 KB**
- **512 KB**
- **1 MB**

Larger images may occupy linked slots or be assigned to an available compatible allocation. EOS reports the actual destination. A programming-success message does **not** imply an additional read-back comparison unless verification is explicitly reported for that operation.

> **Important:** Never intentionally interrupt power while a BIOS or firmware write is in progress. Keep a known-good recovery route available.

### Booting from SD

Choose **SD Card** in Launch Bank and browse a compatible BIOS on a **FAT32** card. EOS stages the selected image in SDRAM and launches it without rewriting EOS flash.

The shared browser supplies consistent paths, selection, and scrolling. The SD BIOS staging path has its own contiguous-file requirement; a normal WebUI file upload does not guarantee that a subsequently selected SD BIOS meets that requirement.

---

## Web control panel

Open a browser to the **Xbox IP address** while EOS is running. The WebUI includes:

- **Bank management** — rename, erase, flash, and launch supported BIOS banks.
- **SD BIOS Manager** — browse and transfer compatible FAT32 SD BIOS files.
- **File Manager** — navigate mounted HDD0/HDD1 and SD filesystems and manage their contents.
- **System information** — revision, RAM, video encoder, network/identity information, and bank/slot usage.
- **EEPROM backup/restore** — download an EEPROM backup, validate a replacement, and restore it when deliberately requested.
- **XbDiag controls** — detect and clear the XbDiag bank when present.
- **Reset Settings** — restore Loader settings without intentionally erasing user BIOS banks.
- **Custom Theme editor** — create and edit supported custom themes from a browser.

### File Manager

Open **File Manager** in the WebUI. The browser presents available mounted volumes using a **HDD0 / HDD1 / SD** hierarchy. HDD1 is shown only when the running BIOS and hardware expose a usable secondary drive.

- **Browse** — open folders, use breadcrumbs or the parent-folder control, and refresh the current listing.
- **Sorting** — folders appear first, followed by files, in alphabetical order (A–Z). Large directories are paginated.
- **Create** — make folders in the selected mounted location.
- **Rename** — rename individual files or folders.
- **Upload** — send one file, multiple files, or a folder tree with nested directories.
- **Download** — retrieve files through the browser.
- **Delete** — delete an individual file or recursively delete a folder and its contents, **after one clear confirmation**.

File-management operations are available inside mounted, writable locations, including nested `Eos` directories. There is no special prohibition on editing a directory merely because it is named `Eos` or sits several levels below a volume. A request can still fail when the filesystem itself rejects it or storage is unavailable.

**Uploads and memory:** Transfers use bounded streaming buffers rather than loading entire files into Xbox RAM. The baseline target is a **stock 64 MB Xbox**; consoles equipped with **128 MB** can benefit from larger buffering where available. Files are sent sequentially, and uploads are finalized only after the transfer completes.

**Large files:** The WebUI warns when an upload may take a while and displays transfer progress. Keep EOS running, retain network connectivity, and avoid powering off or rebooting the console until the transfer is finished. Interrupted transfers and other filesystem errors are reported in the WebUI.

**Deleting:** A file deletion requires confirmation. Deleting a nonempty folder also requires confirmation and removes its contents recursively. This cannot be undone; inspect the displayed target before proceeding.

> **Network note:** EOS's WebUI is intended for use on a trusted local network. Do not expose its file-modification endpoints directly to the public internet.

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
- **DLC / Update Signer**
- **Clear Settings**

### EEPROM

EOS can read and decode the 256-byte Xbox EEPROM, verify its security block, create backups, restore validated images, and edit supported region and video fields.

Back up the EEPROM before modifying it. The EEPROM contains console-specific identity and security information.

### Firmware backup / restore

Firmware tools dump supported EOS flash regions and restore matching images. Read operation feedback carefully and do not interrupt a write. Retain a known-good backup and recovery method.

### HDD tools

Drive Info reports model, serial, capacity, and ATA security state. EOS reports full LBA48 device capacity and mounted FATX partition usage.

**Lock** binds a compatible drive to the current Xbox HDD key. **Unlock** removes ATA security where supported. Destructive or security-sensitive operations require confirmation.

### Fan Control

**Tools → Fan Control** provides:

- **Auto** — return control to the Xbox SMC thermal policy.
- **Manual** — persistent **20–100%** selection in **5%** UI increments.

The SMC has its own duty resolution, so a requested value may read back slightly differently. EOS reapplies the chosen fan policy when the Loader starts.

### Hard-drive setup / Format

The Format tool prepares an Xbox FATX partition layout on the **explicitly selected physical drive**:

- **HDD0 / Harddisk0** — primary drive.
- **HDD1 / Harddisk1** — secondary drive, when detected and supported by the active BIOS.

Use **Left/Right** to choose the drive. EOS displays the device, capacity, and proposed partition arrangement before entering the confirmation screen. You must confirm the selected physical target before formatting proceeds; the formatter rechecks the selected drive's geometry.

The layout includes the standard Xbox partitions and the available extended data area, using FATX sizing appropriate to the drive. Secondary-drive layout mode is handled separately from the primary.

> **Warning:** Formatting erases **all data on the selected physical drive**. Verify that HDD0 or HDD1 is correct, back up everything important, and do not use a drive containing data you need to keep.

### DLC / Update Signer

**Tools → DLC / Update Signer** scans installed title data for supported `ContentMeta.xbx` metadata used by DLC and title updates.

The tool offers:

- **Sign All** — sign all discovered, valid metadata entries.
- **Sign Invalid Only** — update only entries whose existing signature does not match.
- **Results** — counts for discovered, signed, skipped, and failed entries.

The signature uses the Xbox HDD key. The implementation works on content metadata rather than changing BIOS banks or launching titles. Back up content metadata before bulk signing.

### Cerbios tools

EOS includes a Cerbios configuration editor and CPU/GPU overclock calculator for supported Cerbios options.

### EOS Scripts

EOS Scripts provide access to the expansion engine and programmable EXP pins for custom hardware and peripheral projects.

---

## Settings

The Settings hub contains the console and Loader configuration pages.

### Audio

Configure supported Xbox audio/NVRAM options.

### Auto Boot

Set the Auto Boot delay from **2–30 seconds**. Choose the target BIOS in Bank Management.

### Date & Time

Set the Xbox clock manually. With an optional **X-RTC**, EOS can initialize the clock from the battery-backed RTC at boot and mirror changes back to it.

### LCD

Configure the optional 20x4 SMBus status display:

- **Disabled**
- **HD44780** through a PCF8574 I2C backpack
- **US2066 / SSD1311** native I2C OLED

Address and brightness options are available where supported. EOS reports whether the configured display responds.

The live LCD layout shows:

- `EOS <spinner> <current screen>`
- `NET <IP address>`
- `CPU <temp> | MB <temp>`
- `RAM <free>/<total>  <loader version>`

During BIOS launch, the LCD changes to a dedicated ** EOS ** display.

### Network

Configure DHCP/static addressing and FTP credentials. Numeric entry uses the EOS on-screen keyboard with validation before values are committed.

### Region / Video

Inspect and edit the supported EEPROM/NVRAM region and video fields exposed by EOS.

### System Info

Display decoded console and EEPROM information on-console.

### Theme

EOS supports built-in palettes and custom HDD/SD themes. Select **Settings → Theme → Menu Layout** to choose independently between:

1. **Classic 3D** — original perspective carousel and default presentation.
2. **Dynamic Grid** — text-based grid with row/column navigation.
3. **Floating Bubbles 3D** — spatially positioned bubbles with dynamic depth and foreground focus.
4. **Orbit 3D** — rotating 3D elliptical arrangement with depth-focused text selection.

Changing the theme does not automatically change the menu layout. Both choices are persisted in EOS flash. The Theme screen also presents palette information and, where applicable, background-music and track settings.

Custom themes can be discovered from:

```text
E:\Eos\Themes\<theme>\
SD:\Eos\Themes\<theme>\
```

A custom theme can supply its own palette, background image, and music. EOS persists the custom-theme identity and its HDD/SD source independently.

### EOS Settings

Loader-specific controls include:

- **HDMI HUD** — show or hide the onboard overlay on compatible EOS HDMI hardware. Persisted in flash and reapplied at Loader boot.
- **System Info Card** — show or hide the upper-left telemetry card. Monitoring continues for other consumers.
- **HD Fix (1.0/1.1)** — enable or disable the existing Conexant/XboxHD+ **PBUS Xcode pad-control correction** on supported consoles. The setting affects subsequent compatible BIOS launches and does not replace the gateware implementation.

HDMI HUD and System Info Card retain their enabled defaults when migrating old settings. The HD Fix defaults to its previous enabled behavior on eligible hardware; **Classic 3D** remains the default menu layout.

---

## Power

The main-menu **Power** page offers **Shutdown** and **Reboot**. Both require a second **A** press before EOS sends a command to the Xbox SMC.

Reboot uses a full power-cycle path and explicitly returns EOS to the Loader boot bank before the cycle begins.

---

## UI and themes

EOS 1.0.7 provides four genuinely different layouts while preserving established menu operations and avoiding screen transitions:

- **Classic 3D** maintains the original capsule carousel; distant entries fade before colliding with the title/logo or footer.
- **Dynamic Grid** organizes text-only entries into a compact grid, with a smoothly moving selection.
- **Floating Bubbles 3D** places entries in an organic spatial constellation; selection brings an individual bubble forward.
- **Orbit 3D** rotates entries on a tilted 3D orbit and emphasizes the foreground selection using depth and soft glow.

The shared UI uses refined typography, truncation and scaling for long labels, and consistent status feedback. Controller prompts include colored A/B/X/Y and Black/White glyphs. The optional System Info Card and the contextual Bank Details Card are arranged together at the left edge of Bank Management.

The built-in **Darkone83** presentation uses the EOS plasma/model background path. Other built-in and custom themes share the same presentation infrastructure while keeping their own palettes and assets.

---

## Safety

- **Formatting is destructive.** Confirm the selected physical HDD0/HDD1 device before proceeding.
- **Back up the EEPROM** before changing or restoring its contents.
- **Back up DLC metadata** before bulk signing or replacing content.
- **File Manager deletions are permanent.** Individual files and recursive folder deletions require confirmation; there is no undo.
- **Do not interrupt power** during BIOS/firmware writes or ongoing file transfers.
- EOS keeps the Loader/recovery areas outside ordinary bank-management operations.
- Keep the WebUI and FTP services on a trusted local network rather than exposing them to the internet.

---

## For developers

### Requirements

- Original Xbox with an EOS board.
- **RXDK / MSVC 2003** toolchain for the Loader XBE.
- **Python 3** and the current **LZX-capable EOS packing workflow** from the EOS firmware/tooling project. On Windows, the LZX packing path may use `makecab.exe`.

The codebase targets the RXDK / MSVC 2003 environment and retains compatibility with the original Xbox toolchain. It is not intended to compile unchanged under a modern desktop C++ compiler.

### Building

Build the Loader project to produce `default.xbe`. The Visual Studio project includes the WebUI file-management and DLC-signer modules.

### Packing a bootable EOS image

The **1.0.7** Loader uses the updated **LZX** packing method rather than the earlier raw **LZ4** block workflow. The packer is maintained with the EOS firmware/tooling, separately from this Loader README. Use the packer version that matches your boot template and firmware; **the old `eos_pack.py` LZ4 examples from previous releases are not applicable to this LZX image format**.

For a packer exposing the `eos_pack_v2.py` command used in the EOS development workflow, the packing invocation is:

```bash
python eos_pack_v2.py pack <template.bin> default.xbe eos.bin
```

Follow the corresponding tool's current verification and recovery procedure before programming hardware. Do not assume that a packer built for the old LZ4 payload understands an LZX-packed image.

The established template layout places the embedded XBE region at `0x100000` and the kernel region at `0x180000`. The packed Loader must still fit its assigned region. Actual board programming and firmware preparation belong to the EOS firmware project.

### Source layout

```text
main.cpp                    phase/frame loop, bank/tools UI and cards
eos_menu                    main-menu logic and logo/presentation
eos_ui                      menu layouts, footer glyphs and shared UI chrome
eos_gfx / eos_font          rendering, geometry and font atlas
eos_splash                  EOS logo/splash
eos_theme                   built-in palettes
eos_theme_custom            HDD/SD custom-theme discovery and loading
eos_image                   image -> swizzled Xbox texture loader
eos_model / eos_plasma      Darkone83 model/plasma background
eos_audio                   background-music engine
input                       controller input
eos_osk                     on-screen keyboard

dd_net / dd_ftp             network and FTP services
eos_http                    WebUI/HTTP routes and transfer handling
eos_webfiles                WebUI file-management filesystem operations
dd_mount / eos_file         mounted filesystem and shared file-browser I/O
eos_sdcard / eos_sddiskio   onboard SD + FatFs integration
ff / ffunicode / diskio     vendored FatFs

eos_bank                    bank selection / launch / TSOP / XbDiag control
eos_descriptor              dynamic bank geometry
eos_flash                   flash engine bridge
eos_firmware_io             firmware image backup/restore

eos_hdd / eos_format        HDD information, security and HDD0/HDD1 format
eos_dlc                     DLC / Update Signer
eos_fan                     SMC fan control
eos_cerbios                 Cerbios configuration tools

eos_eeprom                  EEPROM UI/operations
eos_eeprom_io               EEPROM transport
eos_ee_crypto               EEPROM SHA-1/HMAC/RC4/CRC implementation
eos_ee_data                 raw EEPROM decode/encode helpers

eos_config                  persistent EOS bank/settings storage
eos_settings                Settings hub, Theme and EOS Settings
eos_nvram                   Xbox OS-section settings
eos_clock / eos_rtc         console clock and optional X-RTC
eos_lcd                     optional SMBus status LCD
eos_xboxrgb                 optional XBOX-RGB launch effects
```

---

## Credits

EOS Loader © Team Resurgent / Darkone83.

FTP functionality is adapted from the DarkDash codebase.

The DLC / Update Signer draws on the PrometheOS signing algorithm by Team Resurgent and contributors. WebUI File Manager workflow was informed by XbDiag's browser-based file management.

VSC HDD unlock support is based on the PrometheOS implementation by Team Resurgent and its contributors. Western Digital and Seagate vendor-specific recovery flows were adapted for EOS while preserving the original PrometheOS behaviour.
