# CPython macOS runtime source input

xz-5.2.3.tar.gz is the exact source archive selected by the supplied
CPython 3.12.10 Mac build recipe, retained here as required by the macOS
freeze checker. The linked liblzma component is public domain; the source
archive also contains GPL/LGPL utility and build files. Their presence does
not mean those utility binaries are distributed in the app.

build-installer-3.12.10.py is the supplied upstream recipe. It records the
runtime dependencies and their source URLs/checksums. It is retained as
build evidence; this patch does not execute it.

See licenses/macos/RUNTIME_COMPONENTS.json for the component index and
licenses/macos/README.md for the limits of the supplied Mac inventory.
