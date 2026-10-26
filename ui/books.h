/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef BOOKS_H
#define BOOKS_H
#include "lvgl/lvgl.h"
#include "ipc.h"
/* Books (SCR_BOOKS): browse audiobooks (.m4b) with saved progress; tap to play + resume. */
void books_create(lv_obj_t *root);   /* build the screen once (from screens_init) */
void books_open(void);               /* (re)build the list from the DB + show SCR_BOOKS */
void chapters_create(lv_obj_t *root);/* build the chapter-navigation screen once */
void chapters_open(void);            /* request the current book's chapters + show SCR_CHAPTERS */
void chapters_update(const track_state_t *st); /* consume the shared snapshot while visible */
#endif
