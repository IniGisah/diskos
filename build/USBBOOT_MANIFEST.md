# Linux usbboot provenance

`vendor/linux-x86_64/usbboot.manifest` is an ASCII JSON object, schema 1.
Do not create a placeholder manifest. `build-usbboot-pinned.sh` writes the
actual candidate's manifest into its output directory after all checks pass.
Copy the binary and manifest into `vendor/linux-x86_64/` together.

| Field | Required value / format |
|---|---|
| `schema` | Integer `1` |
| `target` | `linux-x86_64` |
| `inputs` | Object mapping every path in `usbboot_manifest.py:INPUTS` to its lowercase SHA256; includes application, both shell recipes, validator, complete glibc source package and libusb source archives/sidecars |
| `image_id` | `sha256:` followed by the full 64-character lowercase image digest |
| `source_date_epoch` | Nonnegative JSON integer, fixed for both reproduction runs |
| `glibc_version` | `2.39-0ubuntu8.8` |
| `glibc_packages` | Object with `libc6`, `libc6-dev`, `libc-bin`, `libc-dev-bin`, each exactly `2.39-0ubuntu8.8` |
| `compiler` | Object with `command` (`gcc-13`) and `version` (actual `-dumpfullversion`, e.g. `13.x.y`) |
| `flags` | Object with `libusb_cflags`, `configure`, `link`: arrays matching the recipe constants in the validator |
| `output` | Object with `file` (`usbboot`) and `sha256` (actual stripped executable digest) |

Unknown or duplicate fields, missing inputs, different flags, changed hashes
and dynamic ELF outputs are refused. This recipe supports only Linux x86-64.
The manifest records provenance; it does not authenticate its inputs.
Staging checks input hashes against the git archive export, and the output hash
against the copied staged executable. Retain the build logs, link map, package
records, authenticated snapshot indexes, downloaded package hashes, image archive
and its hash with the internal build evidence.
