# diskOS 1.4-custom Release Notes

**Base Upstream Version:** diskOS v1.2.1 (`4cd4a60`)  
**Previous Version:** diskOS v1.3-custom  
**Release Date:** 2026-10-20  
**Target Device:** FiiO Snowsky Disc (Ingenic X2000 SoC, CS43131 Quad DAC)

diskOS **v1.4-custom** is a major feature release building on the v1.3-custom hardware foundation. It introduces a visual shutdown animation mirroring the boot splash, hardware long-press power button integration, an embedded Wi-Fi Web UI remote control server with single-page browser app, comprehensive navigation and drawer fixes, and player readiness safeguards.

---

## What's New in v1.4-custom

### 1. Graceful Shutdown Animation & SD-Safe Power-Off
* **Reverse Boot Splash on `lv_layer_top` (`ui/splash.c`):**
  - Smooth 3-phase shutdown sequence:
    1. **Contract (0–550 ms):** The hub closes inward from the screen edge over Home (`ok`: 1024 $\to$ 0), masking the active UI beneath the dark vinyl disc while the hub warms in the active theme accent and the Archivo `diskOS` wordmark emerges.
    2. **Storage Sync & Light Sweep (550–1000+ ms):** Dual rainbow spectra sweep along the 8 concentric vinyl grooves while storage writers drain and the filesystem flushes buffers with `/bin/sync`.
    3. **Dissolve & Rail Cut (450 ms outro):** Once storage sync is verified, the spectra and wordmark dissolve, the hub glow cools to solid black, and the panel backlight rail is cleanly cut (`bl_power=4`, `brightness=0`).
* **Hardware Power Button Long-Press Listener (`ui/main.c`, `ui/power.c`):**
  - Detects hardware power key holds (~3–5 seconds) from both `/dev/input/event0` (`x2000_key` codes 259/265/116) and stock `aa03` (`POWER_KEY_EVENT_REPORT`) IPC frames.
  - Automatically begins safe SD card quiescing and launches the shutdown animation while coordinating with `mq_player`'s low-level poweroff routine.
  - Suppresses redundant toast messages and avoids re-sending sleep commands when shutdown was initiated by the player itself (`power_shutdown_request_from_player`).
* **Elapsed Time & Sync Hardening (`ui/power.c`):**
  - Corrected `pw_elapsed()` calculation to prevent false-positive `"Couldn't shut down - card busy"` alert timeouts during flash synchronization.
  - Sanitized child file descriptors (`fd >= 3`) before `/bin/sync` execution to prevent hanging on device locks.
* **On-Device Shut Down Action (`ui/settings.c`):**
  - Added dedicated **Shut down** action under **Settings > System > Updates & Maintenance** with a confirmation modal for intentional on-device power-off.

---

### 2. Embedded Web UI Server & Remote Control SPA
* **Embedded POSIX HTTP/WebSocket Server (`web/server/diskos-webd.c`):**
  - Multi-threaded C daemon running on port 8080 with path jailing (`/tmp/sdcard`), token-based authentication (`/usr/data/diskos.conf`), and `/player` IPC communication.
  - Bundled directly into rootfs at `/opt/diskos/` with first-boot deployment via `S97diskos_install`.
* **Responsive Single-Page Web App (`web/`):**
  - Modern browser SPA featuring rotating vinyl record artwork, live metadata display, volume control, timeline scrubber, queue view with queue-jumping, and quick settings adjustments.
  - Built-in MicroSD card file manager with multi-format audio playback, folder navigation, folder creation, and drag-and-drop audio uploads supporting both lossless (.flac, .wav, .dsf, .dff, .ape, .alac) and compressed formats (.mp3, .m4a, .opus, .ogg).
* **On-Device Management Screen (`SCR_WEBUI`):**
  - Added Web Server tile under Apps (`ui/apps.c`, `ui/webui.c`) providing real-time server status indicators (RUNNING, STOPPED, SD READY, NOT INSTALLED), Wi-Fi connection URL, and on-device PIN setup.
  - Background 500ms sync timer writing authoritative playback state and track metadata to `/tmp/diskos_player.json`.
* **Architecture & API Documentation (`docs/WEBUI.md`):**
  - Complete guide covering network architecture, REST and WebSocket endpoints, security model, and zero-reflash MicroSD deployment.

---

### 3. Startup Splash & Player Readiness Enhancements
* **Looping Boot Splash Light Orbit (`ui/splash.c`):**
  - Enhanced startup splash animation: the light orbit smoothly loops around the disc while waiting for `mq_player` initialization, with a 12-second backstop before revealing Home.
* **Player Readiness Guards (`ui/main.c`):**
  - Enforced `ipc_player_ready()` checks before transport commands and queue jumps, displaying `"Player is starting up - try again"` to prevent dropped commands or wedged playback state during boot.
* **In-Process SQLite `WORK_MODE` Reset (`ui/main.c`):**
  - Replaced slow forked `sh` / `sqlite3` CLI subprocesses with direct in-process SQLite queries against `sysconfig.db`, shaving several hundred milliseconds off boot time.

---

### 4. Navigation & Touch Ergonomics
* **Quick Settings History Isolation (`ui/screenmgr.c`):**
  - Isolated `SCR_QUICK` from the navigation back-stack so closing the drawer returns to the screen it was opened from rather than forcing a jump to Home.
* **Interactive Drawer Drag Restoration (`ui/main.c`, `ui/screenmgr.c`):**
  - Restored fluid vertical drag gesture tracking for opening and closing the Quick Settings panel.
* **Universal Fast-Scroll & Auto-Scrollbars (`ui/main.c`):**
  - Added scroller getters and auto-scrollbars across all scrollable lists (Queue, Audiobooks, Chapters, Settings, Wi-Fi, Bluetooth, Lyrics).
  - Implemented touch-theft guards preventing fast-scroll from stealing taps on non-overflowing lists, hidden search results, or the Library A-Z grid.

---

## Core v1.3-custom Features Preserved

All enhancements from **v1.3-custom** are fully maintained:
* **Hardware Sleep & Wake Stability**: evdev `KEY_POWER` injection, CST816T deep sleep sequencing (0mA), and "Touch to Wake" toggle.
* **Battery Tracking**: Real-time ADC fuel gauge readings, 24-hour circular usage dial (`SCR_USAGE`), and critical battery shutdown guard.
* **External USB DAC Output (Mode 4)**: USB-C OTG digital audio out with netlink hotplug handling and disconnect loop prevention.
* **Mode Lockdown Hubs (`SCR_MODELOCK`)**: Full-screen interactive dashboards for Bluetooth Receiver (with AVRCP transport controls and codec badge), USB DAC slave, and USB Storage.
* **Performance**: 14-slot row virtualization for large libraries, folder-scoped queues, and extended CJK Japanese typography.
