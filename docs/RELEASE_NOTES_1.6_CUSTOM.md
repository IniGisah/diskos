# diskOS 1.6-custom Release Notes

**Base Upstream Version:** diskOS v1.2.2 (`53a9cc5`)  
**Previous Version:** diskOS v1.5-custom  
**Release Date:** 2026-11-17  
**Target Device:** FiiO Snowsky Disc (Ingenic X2000 SoC, CS43131 Quad DAC)

diskOS **v1.6-custom** builds upon the comprehensive Bluetooth 5-codec suite and upstream v1.2.2 integration of v1.5-custom, delivering track artwork thumbnails across song lists, complete Bluetooth receiver device management with a live codec picker, unblocked USB DAC host audio output (Mode 4), robust modal touch isolation, and refined round-screen layouts.

---

## Highlights

### 1. Track Artwork Thumbnails Across Song Lists
* **List Artwork Rendering (`ui/library.c`, `ui/playlistview.c`, `ui/search.c`, `ui/folderbrowser.c`, `ui/upnext.c`):**
  - Displays 36×36 thumbnail tiles for tracks across Library Songs, Folder Browser, Playlist View, Search results, and Up Next queue.
  - Zero-copy SD path lookup (`artcache_thumb_path()`) retrieving 42px BMP thumbnails cached on the MicroSD card.
  - 128-entry direct-mapped thumbnail memory cache in `ui/library.c` ensuring fluid 60 FPS scrolling without storage bottlenecks.
  - Graceful placeholder fallback (`LV_SYMBOL_AUDIO`) when no embedded art or thumbnail is available.
  - Preserves track-number layout in single-album track drill-ins by excluding thumbnails in album views.
* **Browsing Customization & Layout Polish:**
  - Added user toggle under **Settings > Library > Browsing > Track Artwork** (`song_art`).
  - Corrected A–Z rim scroll letter tick index calculation for virtualized lists.

---

### 2. Bluetooth Receiver Device Management & Live Codec Switching
* **Smart Audio Source Filtering (`ui/modelock.c`):**
  - Inspects BlueZ cached device descriptors for UUID `0000110a` (A2DP Audio Source) vs `0000110b` (Audio Sink) and Bluetooth Class of Device.
  - Filters device lists to show only transmitting audio hosts (smartphones, tablets, PCs, TVs) while omitting wireless headphones and earbuds.
* **Paired Devices Sheet & Receiver Controls (`ui/ui.c`, `ui/modelock.c`, `ui/npmenus.c`):**
  - Dedicated "[ Paired Devices ]" button and dynamic status lines ("Ready to Connect", "Pairing Mode") when disconnected.
  - Interactive "Paired Devices" bottom sheet supporting one-tap switching, device disconnect, and device forget via `bluetoothctl`.
  - Context-aware action menu when tapping the top status badge or subtitle in Now Playing.
  - Now Playing Options Hub (`SCR_NPHUB`) adds quick receiver actions: Disconnect, Paired Devices, Pair New Device, and Source selection.
* **Live Receiver Codec Switching & Auto-Healing (`ui/bt.c`, `ui/modelock.c`, `payload/bluealsa`):**
  - Interactive codec picker dialog accessible from Now Playing and Mode Lock to change the receiver codec live without playback interruption.
  - Receiver codec persisted across reboots in `/usr/data/bt_rx_codec`.
  - Prevented crash on codec changes by withholding transmitter command `06b3` from `mq_player` while in receiver mode.
  - Background auto-healing watchdog in `bt_rx_worker()` for `bluetoothd` and `bluealsa`.
  - Added 14 localized strings across 10 languages with updated i18n lookup tables.

---

### 3. External USB DAC Host Audio Restored (Mode 4)
* **Unblocked Mode 4 USB DAC Host Switching (`ui/main.c`, `ui/npmenus.c`):**
  - Replaced premature `!ui_usb_dac_connected()` check in `ui_set_source_mode()` with `fw_has_usb_audio_out()`, allowing the DWC2 USB controller to switch into host mode and power VBUS before enumerating the external DAC.
  - Allowed Mode 4 in `ui_local_playback_allowed()`, MicroSD coldplug mounting, and playlist/audiobook playback start routines.
  - Fixed `ui_detect_source_mode()` to accurately report Mode 4, eliminating spurious "Mode didn't switch" toasts and checkmark rollbacks.
  - Suppressed false disconnect toasts during initial USB bus enumeration by latching `usb_dac_disconnected_latched` outside Mode 4.

---

### 4. Modal Touch Isolation & Circular Display Layout Polish
* **Modal Touch & Gesture Isolation (`ui/main.c`, `ui/ui_dialog.c`):**
  - Main input loop latches `modal_touch` via `screen_modal_open()` to swallow edge back-swipes, rim wheel scrolling, and track scrubbing while modal dialogs or bottom sheets are open.
  - Added `LV_OBJ_FLAG_CLICKABLE` to dialog panels in `ui_dialog.c` to prevent accidental click bleed-through to the scrim layer.
  - Corrected scrim tap-to-dismiss event wiring for system and device name modals.
* **Circular Screen Edge Polish (`ui/ui_dialog.c`, `ui/quicksettings.c`, `ui/settings.c`, `ui/albumwall.c`):**
  - Enforced minimum dialog width and padding in `ui_dialog` to prevent horizontal squishing on the 360×360 circular panel.
  - Font step-down to `HEADER_SM` before multi-line split and tightened header vertical descent to prevent text clipping against round screen bezels.
  - Inset quick settings header, date box, and battery summary into two readable lines away from bezel edges.
  - Enabled direct touch scrubbing on the Album Wall timeline progress bar.
  - Restricted fast-scroll activation to lists with content overflow $\ge 180\text{ px}$.

---

### 5. Documentation & Developer Tooling
* **README & Guide Overhaul:**
  - Redesigned `README.md` structure with visual branding, organized sections, and verified links.
  - Updated `docs/TOUR.md` and `docs/COMMAND_MAP.md` covering Bluetooth receiver device management workflows.
* **Deployment Automation (`tools/diskos-deploy.sh`):**
  - Added support for live-deploying and hot-reloading `payload/bluealsa` directly to the device over Wi-Fi.

---

## Upgrade Instructions

- **Fresh Install / Full Upgrade:** Use the diskOS installer with your stock FiiO firmware ZIP (V2.09, V2.28, V2.40, or V2.57) to flash over mask-ROM USB.
- **OTA / Direct Hot-Reload:** Developers can use `./tools/diskos-deploy.sh` to stage and test `mq_ui` and `bluealsa` without reflashing rootfs.
