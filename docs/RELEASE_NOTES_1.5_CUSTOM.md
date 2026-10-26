# diskOS 1.5-custom Release Notes

**Base Upstream Version:** diskOS v1.2.2 (`53a9cc5`)  
**Previous Version:** diskOS v1.4-custom  
**Release Date:** 2026-10-26  
**Target Device:** FiiO Snowsky Disc (Ingenic X2000 SoC, CS43131 Quad DAC)

diskOS **v1.5-custom** is an integrated custom release merging upstream diskOS v1.2.2 with all custom diskOS optimizations, hardware refinements, and custom features. It brings native Google Cast and Apple AirPlay streaming (Beam), Wi-Fi AirPlay receiving, custom screensaver artwork from MicroSD, editable device and Bluetooth names, and a reorganized 8-group settings hierarchy, while preserving our graceful reverse splash shutdown animation, embedded Web UI remote control server, hardware sleep synchronization, external USB DAC mode, 24-hour battery fuel gauge dial, and library list virtualization.

---

## Highlights

### 1. Upstream v1.2.2 Feature Integration
* **Beam to Cast and AirPlay (`ui/cast_*.c`, `ui/airplay_*.c`, `ui/playback.c`):**
  - Stream music directly from the Disc's MicroSD card to Google Cast devices (Nest Audio, Chromecast, smart speakers, TVs) and Apple AirPlay speakers / Apple TVs over Wi-Fi.
  - Interactive device picker in Now Playing, Options hub, and Quick Settings (`SCR_CAST_PICK`).
  - Remote transport control, volume synchronization, track timeline position updates, and cover art delivery.
  - Transparent on-the-fly audio format transcoding for high-resolution formats not natively supported by receivers.
* **AirPlay Receiving (`ui/receiver.c`, `ui/receiver_progress.c`):**
  - Turn the Disc into a wireless AirPlay speaker (V2.40 / V2.57) discoverable as `SNOWSKY DISC` on iOS, iPadOS, and macOS.
  - Displays real-time track title, artist, and full album art transmitted from the sender.
* **Device & Bluetooth Names (`ui/device_names.c`, `ui/names.c`, `ui/btalias.c`):**
  - Customize the network name (used by Beam and AirPlay) and Bluetooth broadcast name under **Settings > Device > Name**.
  - Assign local nicknames to paired Bluetooth speakers and headphones that display across Now Playing and Bluetooth menus.
* **Custom Image Screensavers (`ui/custom_saver_*.c`, `ui/custom_picker_ui.c`):**
  - Choose any custom full-screen artwork stored in `CustomCover/` on the SD card (JPEG, PNG, WebP up to 1024x1024) under **Settings > Display > Screensaver**.
  - Interactive cover browser with real-time thumbnail preview.
* **Modern Non-Blocking Audio Reconnect Architecture (`ui/main.c`, `ui/ipc.c`):**
  - Upstream's asynchronous `ui_reapply_audio(unsigned gen)` and `audio_restore_tick()` state machine handles background player reconnects smoothly without blocking UI timers or wedging playback.

---

### 2. Harmonized Settings Placement
Settings have been seamlessly unified into upstream's clean 8-group structure (`Playing music`, `Bedtime`, `Sound`, `Output limits`, `Screen`, `Browsing`, `Maintenance`, `Device`) while keeping custom features intuitively accessible:

* **Web Server (`ui/settings.c`):** Placed under **Settings > Network > Web Server** (in addition to the dedicated tile in **Apps**).
* **Battery Usage History (`ui/settings.c`):** Placed under **Settings > Battery & Power > Usage History** (alongside Quick Settings and Apps access).
* **Touch to Wake (`ui/settings.c`):** Placed under **Settings > Display > Touch to Wake** to toggle CST816T deep sleep vs. tap-to-wake.
* **Shut down (`ui/settings.c`):** Placed under **Settings > Device > Shut down** with safety confirmation modal to trigger graceful SD quiescing and shutdown animation.

---

### 3. Preserved diskOS Custom Capabilities
* **Graceful Shutdown Animation & Safe Power-Off (`ui/splash.c`, `ui/power.c`):**
  - Reverse boot splash animation on `lv_layer_top` contracts the vinyl disc inward, illuminates the Archivo `diskOS` wordmark, sweeps orbiting spectra along the 8 vinyl grooves during storage sync, and fades cleanly to black before cutting display power rails (`bl_power=4`, `brightness=0`).
  - Hardware power button long-press listener (3–5 seconds) on `/dev/input/event0` (keys 259/265/116) and `aa03` IPC frames with automated storage drain.
* **Embedded Web UI Server & Remote Control SPA (`web/server/diskos-webd.c`, `web/`):**
  - Multi-threaded C HTTP/WebSocket server running on port 8080 with path jailing (`/tmp/sdcard`), token authentication, and direct `/player` IPC.
  - Single-page web app with interactive spinning vinyl, playback scrubbing, queue manipulation, volume control, and MicroSD card file manager with drag-and-drop audio uploads.
  - On-device management UI under **Apps > Web Server** and **Settings > Network > Web Server**.
* **Hardware Sleep & Wake Synchronization (`ui/main.c`):**
  - Linux evdev physical power key injection (`KEY_POWER` / code 265) synchronizing internal player power state for instant, single-press hardware wake (<100ms latency).
  - Deep idle polling (500ms) when screen is dark to conserve battery.
* **Mode Lockdown Dashboards (`SCR_MODELOCK`, `ui/modelock.c`):**
  - Full-screen kiosk-style interactive interfaces for Bluetooth Receiver (with AVRCP transport controls and synchronized volume), USB DAC slave mode (with stream format badge), and USB Storage mass storage export.
  - Edge-swipe navigation: top-edge swipe opens Quick Settings; left-edge swipe opens an exit confirmation prompt returning safely to Local Playback.
* **External USB DAC Host Mode (Mode 4) (`ui/main.c`, `ui/modes.c`):**
  - Digital audio output to external USB DACs over USB-C OTG (V2.40/V2.57) with netlink uevent hotplug detection and unplug auto-pause protection.
* **24-Hour Battery Usage History (`ui/usage.c`, `SCR_USAGE`):**
  - Circular dial tracking battery percentage, charging state, remaining runtime, and screen-on vs. audio playback active drain.
* **Virtualized Library Lists (`ui/library.c`):**
  - 14-slot dynamic row virtualization eliminating scrolling latency on large music libraries, integrated with upstream's exact target-y position restore when navigating Back.

---

## Upgrade Instructions

- **Fresh Install / Full Upgrade:** Use the diskOS installer with your stock FiiO firmware ZIP (V2.09, V2.28, V2.40, or V2.57) to flash over mask-ROM USB.
- **AirPlay Receiving Requirements:** AirPlay receiving requires stock firmware V2.40 or V2.57.

