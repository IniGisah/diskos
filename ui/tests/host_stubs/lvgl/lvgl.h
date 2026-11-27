/* Minimal host-test stub for LVGL types when compiling pure UI helpers off-target. */
#ifndef LVGL_HOST_STUB_H
#define LVGL_HOST_STUB_H

#include <stdint.h>
#include <stdbool.h>

typedef void lv_obj_t;
typedef void lv_timer_t;
typedef void lv_event_t;
typedef void lv_font_t;
typedef struct { uint8_t blue, green, red; } lv_color_t;
typedef int32_t lv_coord_t;

#endif
