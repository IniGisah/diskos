<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/diskos-wordmark-light.svg">
  <img src="assets/diskos-wordmark-dark.svg" alt="diskOS" width="260">
</picture>

# diskOS Documentation

*Technical guides, hardware specifications, and interface reference for the FiiO Snowsky Disc*

[Main README](../README.md) • [Installation](INSTALL.md) • [Interface Tour](TOUR.md) • [Web UI Guide](WEBUI.md) • [Hardware Map](HARDWARE.md)

</div>

---

Welcome to the diskOS documentation directory. This index categorizes the guides, technical architecture documents, reverse-engineering catalogues, and reference manuals across the project.

## Document Directory

### Installation & Deployment

| Guide | Description | Target Audience |
|---|---|---|
| [**Installation Guide**](INSTALL.md) | Step-by-step setup for Linux, macOS, and Windows, udev permissions, flashing workflow, and error code catalogue (`E1xx`–`F1xx`). | All Users |
| [**Emergency Recovery**](../RECOVERY.txt) | Detailed mask-ROM recovery instructions, bootloader unbricking steps, and backup validation. | All Users |
| [**Privacy & Network Use**](PRIVACY.md) | Complete audit of device network behavior, telemetry-free design, offline guarantees, and Last.fm scrobbler privacy. | All Users |

### Features & User Interface

| Guide | Description | Target Audience |
|---|---|---|
| [**Interface Tour**](TOUR.md) | Exhaustive screen-by-screen breakdown covering Now Playing, Up Next, Library, Lyrics, Beam (Cast/AirPlay), Equalizer, Themes, and Quick Settings. | All Users |
| [**Web UI & Remote Control**](WEBUI.md) | Architecture of the embedded `diskos-webd` daemon, browser player SPA, REST/WebSocket API endpoints, and MicroSD file manager. | Users & Developers |
| [**Design System**](DESIGN_SYSTEM.md) | Round-screen visual design principles, 480×480 circular geometry constraints, touch affordances, typography, and LVGL 9 styling. | Designers & UI Devs |

### Hardware & Reverse Engineering

| Guide | Description | Target Audience |
|---|---|---|
| [**Hardware Capability Map**](HARDWARE.md) | Live-probed hardware specification: Ingenic X2000 SoC, 4× CS43131 DAC ioctls, AP6212 Wi-Fi/BT, MTD partition layout, and memory limits. | Developers & Hackers |
| [**IPC Command Map**](COMMAND_MAP.md) | Inter-process communication protocol for the `/player` POSIX message queue, command tags, payload structures, and response states. | Developers |
| [**Reverse Engineering Catalogue**](RE_CATALOGUE.md) | Deep catalogue of FiiO stock binaries (`mq_player`, `bluealsa`), proprietary shared libraries (`libaptx`, `libldacBT_enc`), and hardware driver ioctls. | Developers |

### Development & Tooling

| Guide | Description | Target Audience |
|---|---|---|
| [**UI Development Workflow**](DEV_WORKFLOW.md) | Local development environment setup, MIPS cross-compilation toolchains, QEMU emulator testing, and on-device SSH debugging. | Developers |
| [**UI Source Code**](../ui/README.md) | Structure of the `mq_ui` binary, LVGL screen managers, models, input event loops, and theme engines. | Developers |
| [**Developer Tools & Scripts**](../tools/README.md) | Utility scripts for automated testing, hot-reloading binaries over SSH, and firmware unpacking. | Developers |
| [**Native Flashing Tools**](../build/NATIVE_TOOLS.md) | Compilation instructions for native host tools: `usbboot`, `my_write5`, and LZO/XZ-enabled `squashfs-tools`. | Maintainers |

### Release Notes & Changelogs

| Document | Description | Status |
|---|---|---|
| [**Release Notes v1.6-custom**](RELEASE_NOTES_1.6_CUSTOM.md) | Track artwork thumbnails across song lists, BT receiver device management, live codec picker dialog, USB DAC host mode unblocking, and circular display polish. | **Current** |
| [**Release Notes v1.5-custom**](RELEASE_NOTES_1.5_CUSTOM.md) | Custom fork rebase onto upstream v1.2.2, Web UI daemon, Mode Lockdown screens, full BT codec suite, sleep/wake stability, and safe shutdown. | Historical |
| [**Release Notes v1.2.2**](RELEASE_NOTES_1.2.2.md) | Upstream v1.2.2 features: Beam to Cast/AirPlay, AirPlay Receiving, custom image screensavers, and network device renaming. | Upstream Baseline |
| [**Release Notes v1.4-custom**](RELEASE_NOTES_1.4_CUSTOM.md) | Custom fork v1.4 additions: Touch-to-Wake, battery usage history, and playback queue safeguards. | Historical |
| [**Release Notes v1.3-custom**](RELEASE_NOTES_1.3_CUSTOM.md) | Custom fork v1.3 additions: Hardware power button listener and startup splash screens. | Historical |
| [**Release Notes v1.2.1**](RELEASE_NOTES_1.2.1.md) | Upstream v1.2.1 initial public release notes. | Historical |
| [**Project Changelog**](../CHANGELOG.md) | Complete chronological release history and commit summaries. | Historical |

### Project Policies & Attribution

- [**Contributing Guidelines**](../CONTRIBUTING.md) — Patch discipline, safety invariants, and pull request requirements.
- [**Security Policy**](../SECURITY.md) — Vulnerability reporting channel and coordinated disclosure guidelines.
- [**License Overview**](../LICENSE) — Installer and scripts (MIT License).
- [**UI License**](../ui/COPYING) — `mq_ui` source code (GPL-3.0-or-later).
- [**Third-Party Notices**](../NOTICE.md) — Attribution and licensing for bundled third-party open-source components.
- [**SPL Source & Provenance**](../SPL_SOURCE.md) — U-Boot DRAM bring-up SPL source and GPL compliance.
