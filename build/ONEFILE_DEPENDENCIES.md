# Dependency inventory - generated from a self-built onefile artifact

> **Scope:** this is a Linux PyInstaller onefile inventory. Linux/macOS source
> release tarballs run with your Python and do not bundle that runtime. The
> Windows package bundles Python and has its own inventory in
> [`WINDOWS_FROZEN_DEPENDENCIES.md`](WINDOWS_FROZEN_DEPENDENCIES.md). See
> [`NOTICE.md`](../NOTICE.md) and
> [`licenses/THIRD_PARTY_BUNDLED.md`](../licenses/THIRD_PARTY_BUNDLED.md).

The Linux onefile inventory contains ~93 distinct native `.so` entries from
the CPython, Tkinter, pycryptodome and pyusb dependency chains. The libraries
below are grouped by licence obligation. Collection varies with the build
host; inventory the extracted shared objects before redistributing a build.

## Copyleft - obligations to satisfy (priority)
| Library | Bundled as | License | Obligation |
|---|---|---|---|
| **libudev** (systemd) | `libudev.so.1` | **LGPL-2.1** | ship license text + section 6 relink materials (or drop it) |
| **FreeType** | `libfreetype.so.6` | **FTL or GPLv2 (dual)** | ship FTL text (permissive path) + attribution |
| **libcap** | `libcap.so.2` | BSD-3 / GPLv2 (dual) | permissive path: BSD text |

> The Tkinter X/font dependency chain brings in `libudev`. A CLI build needs a
> modified spec that excludes `tkinter`; the current recipe includes the GUI.
> Check the resulting dependency inventory before omitting notices or libraries.

## Permissive - attribution/notice only (ship license texts)
| Library | Bundled as | License |
|---|---|---|
| OpenSSL 3 | `libssl.so.3`, `libcrypto.so.3` | Apache-2.0 |
| Tcl / Tk 8.6 | `libtcl8.6.so`, `libtk8.6.so` | TCL/Tk (BSD-style) |
| libpng | `libpng16.so.16` | PNG Reference License |
| zlib | `libz.so.1` | zlib |
| xz / liblzma | `liblzma.so.5` | 0BSD / public-domain |
| bzip2 | `libbz2.so.1.0` | bzip2 (BSD-style) |
| libffi | `libffi.so.8` | MIT |
| fontconfig | `libfontconfig.so.1` | MIT-style |
| expat | `libexpat.so.1` | MIT |
| brotli | `libbrotlicommon/dec` | MIT |
| X11 client libs | `libX11/Xext/Xrender/Xft/Xau/Xdmcp/Xss` | MIT (X11) |
| BLT | `libBLT*` | BSD-style |

## Already in NOTICE.md (direct deps)
CPython (PSF), pyusb (BSD-3), pycryptodome (BSD-2 + public-domain), PyInstaller bootloader
(GPL-2.0 + bootloader exception). The pycryptodome `_raw_*`/`_ghash_*` etc. modules are part of it.

## If you build and redistribute the onefile yourself

A onefile from `build/build.sh` embeds its collected runtime. Redistributing
it requires satisfying those licences, including LGPL libudev relink materials
and FreeType's FTL attribution and text if using that licence option. Map each
extracted shared object back to its distribution package to establish the
complete notice and source obligations.
