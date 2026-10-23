/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AUDIO_STOCK_H
#define DISKOS_AUDIO_STOCK_H
#include "audio_decode.h"
#include <strings.h>
#include <string.h>
/* Hints disappear only after an actual helper handshake in this process.
 * Native/stock byte-level preflight remains authoritative for each item. */
static inline const char *audio_stock_unavailable_label(const char *path){
    if(audio_decode_stock_available()) return NULL;
    const char *ext = path ? strrchr(path, '.') : NULL;
    if(!ext) return NULL;
    if(!strcasecmp(ext,".m4a") || !strcasecmp(ext,".aac") || !strcasecmp(ext,".alac")) return "AAC / ALAC can't be beamed";
    if(!strcasecmp(ext,".opus")) return "Opus can't be beamed";
    if(!strcasecmp(ext,".ogg") || !strcasecmp(ext,".oga")) return "Ogg audio can't be beamed";
    if(!strcasecmp(ext,".wma")) return "WMA can't be beamed";
    if(!strcasecmp(ext,".ape")) return "APE can't be beamed";
    if(!strcasecmp(ext,".dsf") || !strcasecmp(ext,".dff")) return "DSD can't be beamed";
    return NULL;
}
#endif
