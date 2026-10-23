/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* shutdown_anim.h - the boot splash's closing act (PROTOTYPE, not wired into the product).
 *
 * The animation is a passenger of the SD-safe shutdown in power.c: it never starts, delays or ends any phase. The caller
 * feeds it the real phase (power_shutdown_phase()) and whether the panel is lit, every main-loop pass; it draws on
 * lv_layer_top and swallows taps. See SHUTDOWN_PROTO.md. */
#ifndef DISKOS_SHUTDOWN_ANIM_H
#define DISKOS_SHUTDOWN_ANIM_H
#include <stdint.h>
#include "power.h"

/* Begin the animation right after power_shutdown_request() returned 0. screen_lit = the panel is lit (backlight on or
 * dimmed). A dark panel gets NO animation and is never woken for one: returns 0 and creates nothing. */
int  shutdown_anim_start(int screen_lit);

/* Every main-loop pass while active, after power_tick(): the real phase (pw_phase_t) and the panel state. PW_DRAIN,
 * PW_SYNC and PW_HANDOFF move the light; PW_OFF or PW_PROTECTED (the shutdown failed) cancels and reopens the screen.
 * A panel that goes dark pauses the animation (no timer, no drawing) and a lit panel resumes it where real time is. */
void shutdown_anim_update(int phase, int screen_lit);

void shutdown_anim_cancel(void);   /* the shutdown failed: the disc opens again onto the current screen, then it deletes itself */
int  shutdown_anim_active(void);   /* 1 from start until the animation has deleted itself (cancel finished); 0 otherwise */

/* The progress the light is steering to (0..1000) for a phase and the ms spent in it. Pure: the single place where
 * power.c phases become an angle. -1 for PW_OFF / PW_PROTECTED (no shutdown running). */
int  shutdown_anim_target(int phase, uint32_t phase_ms);

/* host harness only */
int  shutdown_anim_host_progress(void);   /* the light's displayed progress, 0..1000 */
int  shutdown_anim_host_state(void);      /* 0 none, 1 run, 2 leave, 3 cancel, 4 done (black) */
int  shutdown_anim_host_objects(void);    /* number of LVGL objects the animation owns */
#endif
