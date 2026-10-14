# Windows notice inventory

Texts copied verbatim (no edits) for the Windows tool bundle built by
`../../build/build-windows-tools.sh`. Package versions are those recorded in
`vendor/windows-x86_64/MANIFEST.txt`.

| File | Component | Taken from |
|---|---|---|
| `lzo-COPYING.txt` | LZO 2.10 (GPL-2.0) | `corresponding-source/lzo2_2.10.orig.tar.gz`, member `lzo-2.10/COPYING` |
| `libusb-COPYING.txt` | libusb 1.0.27 (LGPL-2.1) | `corresponding-source/libusb-1.0_1.0.27.orig.tar.bz2`, member `libusb-1.0.27/COPYING` |
| `msys2-runtime-CYGWIN_LICENSE.txt` | msys-2.0.dll licence terms and linking exception | MSYS2 `/usr/share/doc/Cygwin/CYGWIN_LICENSE` (msys2-runtime 3.6.10-6); identical to `winsup/CYGWIN_LICENSE` at tag cygwin-3.6.10 in the pinned source package |
| `msys2-runtime-COPYING.LIB.txt` | LGPL-3.0 text named by CYGWIN_LICENSE | `winsup/COPYING.LIB` at tag cygwin-3.6.10 in the pinned source package |
| `msys2-runtime-COPYING.txt` | GPL-3.0 text named by CYGWIN_LICENSE | MSYS2 `/usr/share/doc/Cygwin/COPYING`; identical to `winsup/COPYING` at tag cygwin-3.6.10 |
| `msys2-runtime-COPYING.NEWLIB.txt` | Complete newlib default licence inventory | Pinned source asset, `COPYING.NEWLIB:1-1572`; verbatim after provenance header |
| `msys2-runtime-BSD-NOTICES.txt` | Per-file runtime, newlib and header notices, including MSYS CC0 terms | Pinned source asset, cygwin-3.6.10 with all 45 package patches applied; verbatim source ranges recorded in the file |
| `zlib-LICENSE.txt` | zlib 1.3.2 (static, squashfs tools) | MSYS2 `/usr/share/licenses/zlib/LICENSE` (zlib 1.3.2-1) |
| `msys-libgcc-RUNTIME.LIBRARY.EXCEPTION.txt` | GCC Runtime Library Exception 3.1 (static libgcc, squashfs tools) | MSYS2 `/usr/share/licenses/gcc-libs/RUNTIME.LIBRARY.EXCEPTION` (gcc-libs 15.3.0-1) |
| `ucrt64-libgcc-COPYING.RUNTIME.txt` | GCC Runtime Library Exception 3.1 (static libgcc, usbboot) | MSYS2 `/ucrt64/share/licenses/gcc/COPYING.RUNTIME` (mingw-w64-ucrt-x86_64-gcc 16.2.0-4) |
| `mingw-w64-crt-COPYING.txt`, `mingw-w64-crt-COPYING.MinGW-w64.txt`, `mingw-w64-crt-COPYING.MinGW-w64-runtime.txt` | mingw-w64 CRT (static, usbboot) | MSYS2 `/ucrt64/share/licenses/crt/` (mingw-w64-ucrt-x86_64-crt 14.0.0.r426.g4564ee4b5-1) |
| `winpthreads-COPYING.txt` | winpthreads (static, usbboot) | MSYS2 `/ucrt64/share/licenses/winpthreads/COPYING` (mingw-w64-ucrt-x86_64-winpthreads 14.0.0.r426.g4564ee4b5-1) |

GPL-2.0 (squashfs-tools, usbboot) and GPL-3.0 texts are also in `../GPL-2.0.txt`
and `../GPL-3.0.txt`. `mingw-w64-crt-COPYING.MinGW-w64-runtime.txt` contains
non-ASCII characters and is kept verbatim.

## Runtime reconciliation (cygwin-3.6.10 source package)

`winsup/CYGWIN_LICENSE:5-19` distinguishes LGPL DLL code from GPL utilities
and explicitly defers to per-file BSD notices. `winsup/cygwin/Makefile.am`
links libdll.a, libcygserver.a, newlib/libm.a, newlib/libc.a and libgcc into
new-cygwin1.dll; MSYS patch 0002 renames the DLL. The runtime is not just LGPL.

Binary notice requirements occur in `winsup/cygwin/ctype.cc:64-70`,
`flock.cc:25-31,69-75`, `glob.cc:8-14`, `libc/bsdlib.cc:5-11`, and many
other libc, math, regex and header files. `libc/base64.c:1-15` also has an
ISC permission notice. These are preserved in `msys2-runtime-BSD-NOTICES.txt`.
Newlib's `COPYING.NEWLIB:1-5` makes per-file notices override its defaults;
its complete 1572-line inventory must accompany the DLL, with relevant
per-file overrides. It includes target-specific terms: do not infer that
all architectures or all listed components are linked into this x64 DLL.

The two canonical notice files carry the complete upstream default
inventory and complete per-file source ranges. Identical notice blocks are
printed once with every source location. The inventory covers libdll.a
sources, cygserver sources, runtime headers, generic newlib libc/libm and
x86_64/shared_x86 machine inputs. Conditional archive members and headers
are retained conservatively; other CPU/OS implementations and tests are
excluded. This does not assert that every listed file is linked into the DLL.

Source asset: `msys2-runtime-3.6.10-6.src.tar.zst`, SHA256
`1d06b1482199ca8adaea770c4b596ec3adb2749345ef81a5d111cb792f0f1643`.
Extraction commands and patched-tree line ranges are in the notice headers.
Upstream non-ASCII spelling is preserved: COPYING.NEWLIB contains an accented
institution name; per-file notices contain copyright signs and accented names.
Authored headers and this inventory are ASCII.

The source asset contains no GCC source or built DLL link map. The DLL link
recipe uses -lgcc; GPL-3.0 and the GCC Runtime Library Exception texts are
supplied. The squashfs EXEs' compiler version does not identify the DLL's
compiler. Authenticate the source signature as described in
`../../build/windows-qualification.md`. Packaged copies of both canonical
notice files must match byte for byte; a source archive does not replace
binary attribution.
