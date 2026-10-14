# Third-party libraries bundled in a self-built onefile

> **Scope:** this applies to a Linux self-contained PyInstaller onefile built
> with `build/build.sh`. Linux/macOS source release tarballs use your system
> Python; the Windows package bundles Python and has a separate inventory in
> [`../build/WINDOWS_FROZEN_DEPENDENCIES.md`](../build/WINDOWS_FROZEN_DEPENDENCIES.md).
> See [`../NOTICE.md`](../NOTICE.md) for the release-specific obligations.
>
> The Linux onefile inventory contains CPython plus **~90 native shared libraries**
> from the CPython, Tkinter, pycryptodome and pyusb dependency chains. The table
> lists the principal permissive families with verbatim texts in
> [`bundled/`](bundled/); it is not a complete manifest. Collection varies with
> the build host. Map each extracted shared object to its distribution package
> before redistributing your build. The GUI chain includes **libudev (LGPL-2.1)**
> and **FreeType (FTL/GPL dual)**; their obligations apply alongside the table.

| Bundled library (`.so`) | License | Text |
|---|---|---|
| `libcrypto.so.3` (OpenSSL 3) | Apache-2.0 | [`bundled/libcrypto.copyright.txt`](bundled/libcrypto.copyright.txt) |
| `libbrotlicommon.so.1`, `libbrotlidec.so.1` (Brotli) | MIT | [`bundled/libbrotli.copyright.txt`](bundled/libbrotli.copyright.txt) |
| `libbsd.so.0` | BSD-2/3-Clause, ISC, etc. | [`bundled/libbsd.copyright.txt`](bundled/libbsd.copyright.txt) |
| `libbz2.so.1.0` (bzip2) | bzip2 (BSD-style) | [`bundled/libbz2.copyright.txt`](bundled/libbz2.copyright.txt) |
| `libcap.so.2` | BSD-3-Clause OR GPL-2.0-only (used under BSD) | [`bundled/libcap.copyright.txt`](bundled/libcap.copyright.txt) |
| `libX11.so.6` | MIT/X11 | [`bundled/libX11.copyright.txt`](bundled/libX11.copyright.txt) |
| `libXau.so.6` | MIT/X11 | [`bundled/libXau.copyright.txt`](bundled/libXau.copyright.txt) |
| `libXdmcp.so.6` | MIT/X11 | [`bundled/libXdmcp.copyright.txt`](bundled/libXdmcp.copyright.txt) |
| `libXext.so.6` | MIT/X11 | [`bundled/libXext.copyright.txt`](bundled/libXext.copyright.txt) |
| `libXft.so.2` | MIT/X11 | [`bundled/libXft.copyright.txt`](bundled/libXft.copyright.txt) |
| `libXrender.so.1` | MIT/X11 | [`bundled/libXrender.copyright.txt`](bundled/libXrender.copyright.txt) |
| `libXss.so.1` | MIT/X11 | [`bundled/libXss.copyright.txt`](bundled/libXss.copyright.txt) |
| `libBLT.2.5.so.8.6` (BLT, a Tk widget library) | BSD-style | [`bundled/libBLT.copyright.txt`](bundled/libBLT.copyright.txt) |

CPython itself is PSF-licensed (see [`PSF-Python.txt`](PSF-Python.txt)); the Python packages `pyusb`
(BSD) and `pycryptodome` (BSD / public-domain) are covered in [`README.md`](README.md).

If you build and redistribute a onefile, you are responsible for the licences
of everything it embeds. Linux/macOS source releases run the installer with
your Python (`./install.sh` then `./diskos-installer`); they do not redistribute
that Python runtime or its transitive libraries.
