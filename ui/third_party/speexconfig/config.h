/* SPDX-License-Identifier: GPL-3.0-or-later */
/* diskOS configuration for the pinned SpeexDSP 1.2.1 resampler only.
   Replaces the autoconf config.h; no upstream file is modified. Floating
   point, no SIMD (the Disc is a plain MIPS32 core). */
#ifndef DISKOS_SPEEX_CONFIG_H
#define DISKOS_SPEEX_CONFIG_H
#define FLOATING_POINT 1
#define EXPORT
#endif
