/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ARTMGR_H
#define ARTMGR_H

#include <stdint.h>
#include "lvgl/lvgl.h"

typedef struct {
    uint64_t total_bytes;       /* combined storage bytes */
    uint64_t store_bytes;       /* artcache-v2.sqlite size */
    int      store_pics;        /* distinct pictures in SQLite */
    int      store_sources;     /* tracks mapped in SQLite */
    uint64_t netart_bytes;      /* online art disk bytes */
    int      netart_files;      /* online art file count */
    uint64_t legacy_bytes;      /* legacy artcache bytes */
    int      legacy_entries;    /* legacy artcache entries */
    int      total_songs;       /* total songs in music library */
    int      cached_songs;      /* tracks with cached artwork */
} art_cache_info_t;

typedef struct {
    int running;                /* 1 if on-demand generation in progress */
    int done;                   /* tracks examined so far */
    int total;                  /* total tracks to examine */
    int generated;              /* new covers decoded & cached */
    int skipped;                /* already cached, skipped */
    int failed;                 /* undecodable or missing */
    int cancelled;              /* cancelled by user */
} art_gen_status_t;

void artmgr_get_info(art_cache_info_t *out);
int  artmgr_clear_cache(int online_only);
int  artmgr_gen_start(void);
void artmgr_gen_stop(void);
int  artmgr_gen_active(void);
void artmgr_gen_status(art_gen_status_t *out);

void artmgr_open(void);
void artmgr_screen_create(lv_obj_t *root);
void artmgr_refresh(void);
void artmgr_confirm_clear_dialog(void);

#endif
