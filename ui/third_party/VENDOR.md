# Cast and AirPlay vendored dependencies

VENDOR.json records upstream URLs, archive hashes and pruned paths.
SHA256SUMS pins retained files, configuration, recipes and generated files.
Retained upstream source files are unmodified. The development tree also
retains pinned audit archives; public source packages omit these archives.

Public source packages include NOTICE.md and licenses/README.md at the
package root for licences and source inventories. Google descriptor.proto
retains its complete BSD-3-Clause notice in the source header. Cast schema
attribution is in cast/openscreen-LICENSE; VENDOR.json records its URL and hash.

For an offline build, see the UI README and supply the documented stock
firmware and FFmpeg header inputs. The UI tools/build-native-deps.py runner
uses the retained recipes with explicit source and output directories.
The production UI links mbedTLS, Monocypher, libplist, SpeexDSP and FreeType;
nanopb runtime sources and dr_libs headers compile directly into the UI.

libplist is LGPL-2.1-or-later, with MIT notices in src/jsmn.c and src/time64.c.
Its libcnary source is included. Complete UI source and dependency recipes
provide a rebuild and relink path; see NOTICE.md in the public package.

stb_image_write.h v1.16 is pinned at nothings/stb@1ee679ca2ef753a528db5ba6801e1067b40481b8.
The header is unchanged. Its final licence comment is extracted verbatim
into LICENSE and copied into the distributed licence notices.
The JPEG callback writer is compiled statically in cast_art.c. NO_STDIO
disables file output and HDR; unused PNG/BMP/TGA code is removed at -Os.
AirPlay uses quality 85 and a fixed 256 KiB output allocation, with PNG
fallback. The transparent 1x1 placeholder remains PNG.
