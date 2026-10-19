/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_WEBUI_H
#define DISKOS_WEBUI_H
#include "lvgl/lvgl.h"

void webui_create(lv_obj_t *root);
void webui_open(void);
void webui_refresh(void);

#endif

