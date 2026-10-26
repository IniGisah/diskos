# macOS frozen dependencies

The release is a self-contained, architecture-specific .app and accompanying
source/notices in a tar.gz. Users need no Python, Tk, Homebrew, libusb or LZO.
Build on arm64 first. A separate x86_64 build is feasible with all native and
Python inputs matching that architecture; it needs independent qualification.

## Offline build inputs

Prepare a dedicated python.org framework CPython 3.12.10 installation with
Tk >= 8.6. The builder never downloads or installs anything. Required build
versions: PyInstaller 6.16.0 (GPL-2.0 with
bootloader exception), hooks-contrib 2025.9 (GPL-2.0 with hook exception),
altgraph 0.17.4 (MIT), macholib 1.16.3 (MIT), packaging 25.0 (BSD/Apache-2.0).
Use requirements.txt for pyusb 1.3.1 (BSD-3-Clause), PyCryptodome 3.23.0
(BSD/public domain), certifi 2026.2.25 (MPL-2.0), platformdirs 4.2.2 (MIT).
Provision these build dependencies offline and retain their exact wheel/source
hashes. The freeze recipe does not install packages.

Prepare native tools separately with vendor/setup-macos.sh. It already installs
LZO when missing and pins libusb 1.0.30, squashfs 4.7.5, LZO 2.10, lz4 1.10.0,
xz 5.8.4 and zstd 1.5.7_1. That source-setup script uses Homebrew; the freeze
script does not call it. Keep the existing macos-arm64 source archives, patch
and formula snapshots and the usbboot source/recipe with the release.

Create an externally retained JSON input lock with two maps, `repo` and
`python`: keys are relative paths, values are lowercase SHA256. `repo` covers
all files in diskos_installer, payload, flash, build (except build/dist),
licenses, corresponding-source, src, spl-src, ui, vendor/macos-<arch>, plus
LICENSE, NOTICE.md, README.md, SPL_SOURCE.md, requirements.txt, docs/INSTALL.md
and docs/MACOS_RECOVERY.txt. `python` covers every regular file below
sys.base_prefix, including installed packages/frameworks, except *.pyc and
__pycache__. Dereferenced internal symlinks are hashed by their relative name.
Review the inputs and retain the lock SHA256 independently. Supply that digest
as the third build argument; the build checks every recorded hash and requires
complete coverage. Do not generate and trust a lock in the same build step.
No macOS binary hashes are asserted before the actual inputs are available.

## Notices and corresponding source required before freezing

Add these verbatim files from the chosen macOS runtime to licenses/macos/:

- CPython-LICENSE.txt and CPython-Incorporated-NOTICES.txt (the entire selected
  macOS framework licence, including incorporated software).
- Tcl-license.terms.txt, Tcl-Incorporated-NOTICES.txt, Tk-license.terms.txt,
  Tk-Incorporated-NOTICES.txt, matching the actual framework versions and all
  collected scripts/assets. Keep Tango and other per-file credits and terms.
- PyCryptodome-LICENSE.txt and PyCryptodome-NOTICES.txt from the selected macOS
  distribution; keep the existing native-source/libtom/SipHash notices too.
- RUNTIME_COMPONENTS.json: map CPython, Tcl, Tk, PyCryptodome, OpenSSL, libffi,
  zlib, bzip2 and xz to objects with `version`, `license`, `notices` (list of
  repo-relative files) and `sources` (list of repo-relative files if applicable).
  Include the actual framework's incorporated notices for OpenSSL, libffi,
  zlib and bzip2; add notices/source records for any further collected library.
  Match the runtime liblzma source/version, not the native tools' xz version.
  Provide its corresponding source and applicable GPL/LGPL/permissive terms.

Do not substitute Windows runtime notices for the selected macOS distribution.
Existing native-tool GPL/LGPL sources are under corresponding-source/macos-arm64;
licenses/ contains their terms. Include the complete build recipe and usbboot
source for rebuilding/relinking with a modified libusb. The release stager ships
all notices, corresponding-source, UI/SPL/helper sources and their build
recipes together with the app. Input lock covers
them. Missing notices fail before freezing; content and component completeness
still require inspection against the actual macOS binary/script inventory.

## Build

    bash build/build-macos.sh /Library/Frameworks/Python.framework/Versions/3.12/bin/python3 /absolute/inputs.json <retained-sha256>

PyInstaller collects framework Python/Tk, native Mach-O files and runtime
packages. macos_links.py resolves every non-system load within the app,
rewrites it to @loader_path, and rejects external LC_RPATH, unresolved loads,
escaping symlinks and missing architecture slices. Only /usr/lib and
/System/Library are external exceptions. Ad-hoc signatures are mandatory and
verified; failure aborts. Doctor proves LZO packing/extraction and libusb loading.
The final MANIFEST records every binary hash, package file hash, symlink target
and pinned package version. macos_release.py preserves app modes/symlinks in a
stable-header archive; byte-identical independent signed freezes are not claimed.

Signing is ad-hoc, not Developer ID or notarization. First-run Gatekeeper
approval is required. Qualification must include a clean Mac without Homebrew
or external Python and a browser-downloaded quarantined archive.
