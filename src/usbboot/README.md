# usbboot (Ingenic X2000 mask-ROM USB loader)

This is the host-side loader that talks to the Ingenic X2000's on-chip mask-ROM USB boot
mode (`a108:eaef`). diskOS's installer uses it to upload the stage-1 SPL and the on-device
NAND writer during a flash.

## Provenance and license

- **License:** GPL-2.0-or-later.
- **Copyright:** (c) 2021 Aidan MacDonald, (c) 2015 Amaury Pouly, and contributors.
- Part of the Ingenic X2000 community mask-ROM tooling lineage (the same tool family is used
  to recover other X2000 boards). The copy here is included as GPL corresponding source; see
  the repository's [`NOTICE.md`](../../NOTICE.md).

## Local modifications for diskOS

- Bulk transfers use chunks up to 4 MiB with a 600,000 ms overall deadline. Vendor
  controls default to 1,000 ms per attempt; `USBBOOT_TIMEOUT` overrides that timeout.
- The Linux x86_64 package uses a fully static build for portability - see [`build/build-usbboot-pinned.sh`](../../build/build-usbboot-pinned.sh).

## Building

```
make                     # dynamic build (needs libusb-1.0 dev headers)
```

For the Linux x86_64 release binary, use
[`build/build-usbboot-pinned.sh`](../../build/build-usbboot-pinned.sh).
It takes `--image-id FILE --out DIR --epoch N`, builds offline in a frozen
rootless Podman image, and uses the shipped libusb 1.0.27 source archive with
a SHA-256 check. It produces `usbboot` and `usbboot.manifest`; release staging
requires both in `vendor/linux-x86_64/`. Prepare the build image with
[`build/prepare-usbboot-env.sh`](../../build/prepare-usbboot-env.sh).

## Note

This tool drives raw mask-ROM USB and can erase/write device storage. It is intended to be
invoked by the diskOS installer, which adds device checks and the fail-closed flashing flow.
Running it directly is for developers who understand the Ingenic boot protocol.
