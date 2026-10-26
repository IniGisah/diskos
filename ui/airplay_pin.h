/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_PIN_H
#define DISKOS_AIRPLAY_PIN_H
#include <stdint.h>
int airplay_pin_active(void);
void airplay_pin_open(uint32_t attempt, const char *receiver, void (*done)(uint32_t, const char *));
void airplay_pin_close(void);
/* Pure edit model: leading zeroes retained, no conversion to an integer. */
int airplay_pin_edit(char digits[9], unsigned key); /* 0..9 digit, 10 delete */
#endif
