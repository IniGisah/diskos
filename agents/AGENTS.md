# diskOS - contributor working rules

Working instructions for **diskOS**, a custom UI for the FiiO Snowsky Disc digital audio
player (Ingenic X2000 SoC). Read these before changing the installer or device code.

## The one rule that matters most: verify, do not guess

This device's hardware, firmware, audio path, and command surface are **easy to misidentify**.
Before asserting how the device behaves:

- **Check `docs/HARDWARE.md`** (hardware capability map) for chip part numbers,
  sample rates, dev nodes, partition layout, I2C addresses.
- **Read the actual binary / source** for what code does instead of relying on memory.
- If a claim is not in a doc and you cannot verify it against the device or a binary, **say it
  is unverified** rather than stating it confidently.

Resolve assumptions by reading the source first.

## Hardware facts that are easy to get wrong

- Wireless chip is **BCM43438 / AP6212 (2.4 GHz only, BT over UART)**. There is **no 5 GHz**.
  (Old notes saying "BCM4345C5" are wrong - those `.hcd` files are leftovers for other models.)
- **Four CS43131 DACs**, fully balanced (L+/L-/R+/R-), driven by a kernel driver via **ioctl**
  on `/dev/cs43131{,b,c,d}` - **not** via ALSA controls.
- **No physical LED** on the unit. `/sys/class/leds` is empty; `RGB_*` config fields are
  vestigial. Plan no LED features.
- **No GPU / VPU / hardware JPEG or thermal sensors; no DVFS interface is exposed.**
  The CPU provides MSA SIMD, and the display controller provides scaling planes.
- The rootfs partition (`mtd2`) is a **read-only squashfs**, which is why the boot hook cannot
  be edited in place; persistent state lives in the writable `/usr/data` (`mtd7`).

See `docs/HARDWARE.md` for the full map and the reproducible probe commands.

## Repo layout

- `diskos_installer/` - the Python installer (GUI + CLI) that builds a diskOS image from the
  user's own stock firmware and flashes it over mask-ROM USB.
- `flash/` - the low-level flashing pieces: the mask-ROM writer source (`my_write6.c`), the
  stage-1 SPL, and helper scripts.
- `spl-src/` - GPL corresponding source for the stage-1 DRAM bring-up SPL (patch series over
  upstream U-Boot). See `SPL_SOURCE.md`.
- `src/usbboot/` - the Ingenic mask-ROM USB loader (third-party GPL; do not rewrite).
- `payload/` - the on-device first-boot hook and the diskOS UI binary (`mq_ui`).
- `docs/` - hardware map and other reference docs.
- `licenses/`, `NOTICE.md` - third-party attribution and license texts.

## Device-safety rules (read before touching the device)

- A flash **rewrites the main rootfs**. If it fails, follow [RECOVERY.txt](../RECOVERY.txt):
  after the wait described below, power off, hold Volume Down, and reconnect USB to enter
  mask-ROM mode before reflashing.
  Mask-ROM lives in on-chip ROM and runs before flashed code. The smaller recovery rootfs
  is not an A/B slot for the main image.
- The first-boot install is **fail-closed**: if the UI cannot be verified against the baked
  manifest (SHA-256), the stock UI runs instead. Never weaken that contract.
- **You cannot install from the device's own "System updates" menu** - it checks an ECDSA
  signature against FiiO's public key, which a custom image will not have. Initial
  installation uses mask-ROM USB; signed diskOS UI updates are a separate mechanism.
- If a mask-ROM run hangs, keep power and USB connected for **at least 25 minutes after the
  error**, with the computer awake, before attempting recovery. The device may keep writing
  after the host tool stops. Then follow [RECOVERY.txt](../RECOVERY.txt); do not resume at a
  guessed NAND block or bypass an installer refusal.

## Working habits

- **Honest limitations.** When something is blocked by hardware (no 5 GHz, no GPU, DAC ioctls
  not yet reverse-engineered) or a toolchain gap, say so plainly instead of implying it works.
- **Preserve image validation.** Keep independent squashfs extraction and hash checks in
  the image builder and boot hook.
- **Fail closed, log loudly.** Every error path in the installer has a code (`E1xx` preflight,
  `E2xx` build, `E3xx` flash, `F1xx` writer failures, `F2xx` base-gate refusals).
  If you add a failure mode, give it a code and document it in [docs/INSTALL.md](../docs/INSTALL.md#error-codes).
- **Cite file:line** when you summarize what changed, so a human can verify it.

## Contributing

- Keep the flashed image = stock FiiO rootfs + the minimal diskOS patch. We do **not**
  redistribute FiiO's rootfs; the installer builds it from the user's own firmware zip.
- GPL components (usbboot, squashfs-tools, the SPL) ship with corresponding source. If you
  touch how they are bundled, keep the source and `NOTICE.md` in sync.
- Command-tag meanings differ across firmware versions. Add explicit firmware mappings and
  retain refusals for unknown versions; a wrong command can misbehave.
