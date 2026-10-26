# Selected macOS runtime notices

These notice inputs describe the supplied python.org CPython 3.12.10
universal2 framework, inspected on arm64, and its installed packages.
RUNTIME_COMPONENTS.json is the component index used by the release checker.
Its notices and sources use paths relative to the installer root.

## Inventory and collection

The supplied versions.txt, otool.txt and libdirs.txt establish the following
runtime inventory. The CPython build-installer-3.12.10.py recipe is retained
under corresponding-source/macos-runtime/. Line numbers below refer to those
supplied inventory files and that recipe, not to a frozen app manifest.

| Component | Version | Collection and terms |
|---|---|---|
| CPython | 3.12.10 | Framework Python and selected standard library; PSF and historical terms, plus incorporated notices. versions.txt:1; otool.txt:10-18. |
| Tcl | 8.6.16 | Bundled libtcl8.6.dylib and runtime scripts; permissive terms and incorporated notices. otool.txt:32-40; recipe:267-293. |
| Tk | 8.6.16 | Bundled libtk8.6.dylib, scripts and assets; permissive terms, per-file notices and CC-BY-SA-3.0 Tango terms. otool.txt:86-112; versions.txt:5. |
| PyCryptodome | 3.23.0 | Crypto modules and native extensions; BSD/public-domain terms with native-source notices. versions.txt:9; supplied dist-info LICENSE.rst. |
| OpenSSL | 3.0.16 | Bundled libssl.3.dylib and libcrypto.3.dylib; Apache-2.0. versions.txt:2; recipe:249-254; otool.txt:160-166,222-229. |
| NCurses | 6.5 | libncurses.6.dylib and libpanel.6.dylib when curses modules are collected; MIT-style terms. Other framework ncurses libraries have the same terms. otool.txt:26-31,72-79,169-177; recipe:328-360. |
| SQLite | 3.49.1 | Static in _sqlite3.so if collected; public domain. versions.txt:3; otool.txt:412-416; recipe:362-380. |
| xz/liblzma | 5.2.3 | Static in _lzma.so; public-domain liblzma. otool.txt:292-296; recipe:317-324. This is separate from the native tools' xz 5.8.4. |
| libffi | Apple ABI 27.0.0 | External /usr/lib/libffi.dylib, loaded by _ctypes; no redistributed library. otool.txt:319-325. Upstream version unverified. |
| zlib | Runtime 1.2.12 | External /usr/lib/libz.1.dylib; no redistributed library. versions.txt:4; otool.txt:233-239. Some load commands report 1.2.11; that is not the probed runtime version. |
| bzip2 | Apple ABI 1.0.8 | External /usr/lib/libbz2.1.0.dylib, loaded by _bz2; no redistributed library. otool.txt:313-318. Upstream version unverified. |
| pyusb | 1.3.1 | Bundled Python USB package; existing BSD-3-Clause notice matches supplied package notice. |
| certifi | 2026.2.25 | Bundled CA data and Python package; existing MPL-2.0 terms and package notice match supplied notice. |
| platformdirs | 4.2.2 | Bundled platform directory package; existing MIT notice matches supplied notice. |
| PyInstaller | 6.16.0 | Bootloader and runtime support; existing GPL-2.0-or-later text with bootloader exception matches supplied notice. |
| hooks-contrib | 2025.9 | Build hooks and generated runtime hooks; GPL with hook exception. |
| altgraph / macholib | 0.17.4 / 1.16.3 | Build dependencies; MIT notices retained conservatively. |
| packaging | 25.0 | Build dependency, possibly collected runtime support; Apache-2.0 OR BSD-2-Clause notices retained. |

System frameworks under /System/Library and libraries under /usr/lib remain
external in build/macos_links.py. The libffi, zlib and bzip2 records point to
this explanation because those system binaries are not shipped. No notice
from a different platform or guessed upstream version is substituted.

## Notice provenance and source duties

PROVENANCE.txt identifies the inputs and SHA256 of each extracted notice.
CPython-LICENSE.txt and PyCryptodome-LICENSE.txt preserve the supplied Mac
files byte for byte. CPython-Incorporated-NOTICES.txt explicitly identifies
its supplemental shared CPython 3.12.10 terms. PyCryptodome-NOTICES.txt retains
the existing shared 3.23.0 native-source notices. Neither supplemental file
claims to be an inventory extracted from the selected Mac binary.

Tcl/Tk incorporated notices preserve labelled verbatim licence files and
legal/credit comment blocks from the matching source archives, including
runtime scripts, image credits, bundled packages and libtommath. This is a
conservative source inventory, including some files not collected. Tango
icons remain unmodified; their attribution is in Tk-Incorporated-NOTICES.txt
and their complete licence is licenses/CC-BY-SA-3.0-Tango.txt. No Windows-only
Tk data exclusions are assumed for this app.

OpenSSL, ncurses, SQLite and liblzma texts come from the exact supplied source
archives. No GPL/LGPL corresponding-source duty was identified for these
runtime libraries: liblzma is public domain, whereas other programs and
build files in the xz source archive have GPL/LGPL terms. The full xz 5.2.3
archive and the CPython Mac build recipe are nevertheless included to satisfy
the freeze checker's source requirement. No xz utility binary is added.
Native flash-tool source duties remain covered by the existing installer
notices and corresponding-source/macos-arm64; they are a separate inventory.

certifi's MPL-2.0 requires source availability for covered files. The existing
spec ships unmodified cacert.pem in source form. Matching unmodified package
source is available at https://pypi.org/project/certifi/2026.2.25/#files .
Recipients must retain this source-availability notice with the package;
modified covered files require their modified source. PyInstaller's compiled
bootloader combination and generated hook output have their respective
exceptions. This does not extend to separate distribution of those tools
outside the exceptions. Full terms govern; per-component duties are recorded
in RUNTIME_COMPONENTS.json.

## Evidence still required from the Mac

The supplied framework lib/python3.12/LICENSE.txt ends after 279 lines of
base/documentation terms. The selected installer License.rtf (the Mac build
recipe:1733-1753 generates it) was not supplied. Obtain and compare that full
licence, including incorporated terms, before freezing. The shared CPython
appendix here is supplemental evidence, not a substitute claim that the
selected framework's entire incorporated-software inventory was supplied.

Reconcile the actual PyInstaller Analysis and frozen-app file manifest with
this conservative inventory. In particular, verify any extra Tcl packages
listed by libdirs.txt (itcl4.3.2, sqlite3.47.2, tdbc1.1.10 and its drivers,
thread2.8.11), their scripts/assets, and any package newly collected such as
setuptools. Tcl source pkgs/ notices cover the supplied source versions, but
final collection and installed package content were not supplied. Retain
wheel/source hashes, the reviewed input lock and the final app manifest on
the Mac. This notice-input patch does not establish those binary hashes or
perform a macOS freeze, signature or clean-machine qualification.
