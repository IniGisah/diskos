# diskOS 1.3-custom Release Notes

**Base Upstream Version:** diskOS v1.2.1 (`4cd4a60`)  
**Release Date:** 2026-10-15  
**Target Device:** FiiO Snowsky Disc (Ingenic X2000 SoC, CS43131 Quad DAC)

diskOS **v1.3-custom** is an integrated release incorporating all upstream 1.2.1 additions (Ogg Opus decoding, Windows installation toolchain, setup checklists, halo theme enhancements) alongside crucial hardware sleep fixes, battery monitoring, external USB DAC output, and UI performance optimizations.

---

## What's Changed from Upstream 1.2.1

### 1. Hardware Sleep & Wake Stabilization
* **Resolved Screen-Off False Wake & Touch Freeze (`ui/main.c`, `ui/power.c`):**
  - Upstream 1.2.1 could enter a false wake loop after timed auto-sleep where the screen would illuminate immediately upon timeout with touch input frozen until an idle screensaver triggered.
  - Implemented proper touch drain (`lv_indev_wait_release`) before sleep and clean input device enablement upon confirmed wake.
* **evdev Physical Power Key Injection:**
  - Synchronizes `mq_player`'s internal screen power state with diskOS via Linux evdev key injection (`KEY_POWER` / code 265 on `/dev/input/event*`), ensuring single physical power button clicks reliably wake the unit without touching process memory.
* **CST816T Deep Sleep & Panel Power Sequencing (`ui/power.c`):**
  - Replicates stock FiiO firmware hardware sleep sequencing: CST816T touch controller suspended via `0x2000ef03` (0mA current draw), backlight cut (`bl_power=4`), 120ms ST77916 timing delay enforced, and panel suspended via `0x2000ef01`.
  - On wake: panel resumed (`0x2000ef02`), 50ms stabilization delay, backlight restored, touch resumed.
* **Touch-to-Wake Setting:**
  - Added "Touch to Wake" toggle under **Settings > Display** (`touch_wake`, default: Off for maximum standby battery life). When disabled, hardware touch enters 0mA deep sleep.
* **Standby Polling Optimization:**
  - Reduced dark-screen background polling to 200ms intervals to maximize battery life while asleep.

---

### 2. Battery & Power Management
* **Battery ADC Fuel Gauge Tracking (`ui/power.c`):**
  - Continuous real-time readings from `/sys/class/power_supply/battery/voltage_now` and `/capacity`.
* **24-Hour Battery & Usage History Screen (`SCR_USAGE`):**
  - Interactive 24-hour circular dial tracking battery %, charging status, estimated remaining runtime based on rolling discharge rate, and active screen-on vs. playback segments.
  - Accessible via the **Battery** tile in Apps and via a clickable shortcut in Quick Settings.
* **Low-Battery Auto-Shutdown Guard:**
  - Automatically triggers safe system shutdown when battery drops to critical levels, preventing filesystem corruption and deep battery discharge.
* **Quick Settings Live Clock:**
  - Instantly populates the system clock and date upon opening the drawer, eliminating the `--:--` placeholder delay.

---

### 3. External USB DAC Host Mode (Mode 4)
* **Digital Audio Output via USB-C OTG (`ui/modes.c`, `ui/main.c`, `ui/ui.c`):**
  - Enables routing digital audio to external USB DACs / dongles on supported firmware (V2.40 and V2.57).
  - Configures gadget selector 5 (DWC2 host OTG with SGM41513 5V VBUS boost) and audio route `out_dev = 3` (`USB_HOST`).
* **Hotplug Netlink Uevent Tracking:**
  - Parses `a60a` TIP_INFO_EVENT notifications from `mq_player` (`0x0213` USB DAC disconnect, `0x0212` USB DAC connect) with procfs fallback (`/proc/asound/cards`).
* **Disconnect Queue-Burn Protection:**
  - On stock firmware, unplugging a USB DAC causes the player to continuously cycle through tracks in a silent loop.
  - diskOS guards playback while unplugged, displays `"USB DAC not connected"`, and cleanly pauses/recovers when replugged.

---

### 4. Mode Lockdown Architecture (`SCR_MODELOCK`)
* **Dedicated Mode Interfaces (`ui/modelock.c`):**
  - Full-screen status views for non-local playback modes:
    - **Bluetooth Receiving**: Displays connection status, active audio codec badge (`LDAC`, `AAC`, `SBC`), AVRCP transport controls (play/pause, skip), and synchronized volume.
    - **USB DAC**: Functions as a slave USB DAC for computers/smartphones with format indicator (PCM, sample rate, bit depth).
    - **USB Storage**: Manages SD card mass storage export to host PC with unmount safeguards.
* **Safe Edge-Swipe Navigation:**
  - Pull down from top edge opens Quick Settings.
  - Swipe right from left edge triggers an exit confirmation dialog to safely return to Local Playback.

---

### 5. Audio & Bluetooth Enhancements
* **Hierarchical Codec Negotiation (`ui/bt.c`):**
  - Automatic fallback hierarchy (`LDAC` -> `AAC` -> `SBC`) during initial Bluetooth route handshake, matching stock firmware behavior.
* **Active Codec Badge:**
  - Displays current negotiated Bluetooth codec on the Song Info screen.
* **Subprocess Hardening:**
  - Resilient BlueALSA and bluetoothd IPC error handling to prevent audio thread lockups.

---

### 6. UI Performance & Ergonomics
* **14-Slot Row Virtualization Engine (`ui/library.c`):**
  - Implements row recycling for music lists, rendering only active on-screen rows to eliminate scrolling stutter and high memory usage on large SD cards (10,000+ tracks).
* **Folder-Scoped Queues & Shuffle Play (`ui/folderbrowser.c`):**
  - Browse Files supports "Play All" and "Shuffle Play" strictly scoped to the active folder rather than forcing the global music queue.
* **Fast-Scroll A-Z Index:**
  - Quick alphabetical jump grid popup in Folder Browser and Album Wall.
* **Extended CJK Japanese Typography:**
  - Bundled supplementary font tables with 3,724 Japanese Kanji and Kana glyphs for full East Asian metadata rendering.

---

## Upstream 1.2.1 Features Preserved
All upstream 1.2.1 capabilities are intact:
* Native Ogg Opus (.opus, .oga, .ogg) tag parsing and playback.
* Halo theme now-playing border progress ring.
* Legacy tag encoding selector (Auto / Western / Cyrillic CP1251).
* Windows 10/11 x64 installer support with WinUSB driver setup.
* Ready checklist and pre-flash integrity verification.
