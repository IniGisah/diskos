/* SPDX-License-Identifier: GPL-3.0-or-later */
/* diskOS configuration for the pinned libplist 2.6.0 C core (binary plist
   client). Replaces the autoconf config.h; no upstream file is modified.
   Target libc: musl (and glibc for host smoke tests). No iconv, no libxml. */
#ifndef DISKOS_PLIST_CONFIG_H
#define DISKOS_PLIST_CONFIG_H
#define PACKAGE_VERSION "2.6.0"
#define HAVE_STRNDUP 1
#define HAVE_MEMMEM 1
#define HAVE_STRPTIME 1
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define HAVE_TM_TM_GMTOFF 1
#define HAVE_TM_TM_ZONE 1
#define HAVE_ATTRIBUTE_CONSTRUCTOR 1
#endif
