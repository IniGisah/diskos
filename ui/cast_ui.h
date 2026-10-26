/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* cast_ui.h - the Beam session as the UI shows it (Cast slice 17b). The receiver's name and its paused flag live in main.c, which owns the
 * session; cast_ui_status() builds the one status line (owner decision 86) that Now Playing and the picker both show. */
#ifndef CAST_UI_H
#define CAST_UI_H
#include <stddef.h>
#include "remote_discovery.h"
int ui_airplay_pick_request(const rd_receiver *);
int ui_airplay_pin_wanted(void);
unsigned ui_airplay_attempt(void);
int ui_airplay_pin_submit(unsigned attempt, const char *);
void ui_airplay_clear_error(void);
int ui_airplay_rate(void);
int ui_airplay_bits(void);
int ui_airplay_volume(void);
int ui_beam_volume(void);
int ui_beam_refuse(const char *reason);
const char *ui_beam_error(void);
void ui_beam_clear_error(void);
int ui_airplay_volume_query(void);
int ui_airplay_error(void);
int ui_airplay_active(void);
int ui_airplay_return(void);
int ui_cast_return(void);
int ui_airplay_can_return(void);
int ui_airplay_return_single(void);
int ui_airplay_forget(void);
int ui_airplay_can_forget(void);
#define BEAM_GLYPH "\xEF\xA3\x90"   /* U+F8D0, the Beam glyph in the icon fonts (tools/gen_beam_glyph.py): the Now Playing button and the Options row */
void ui_cast_set_device(const char *name);   /* the picker's tap: the receiver being beamed to; main.c forgets it once no session and no request exists */
const char *ui_cast_device(void);            /* that name, "" when none */
int  ui_cast_paused(void);                   /* 1 while the receiver reports PAUSED */
int  cast_ui_status(char *out, size_t cap);  /* 1 + the line while beaming: "Beaming to <device>" or "Paused - <device>"; else 0 and out is empty */
#endif
