#ifndef BSPWM_ANIMATION_H
#define BSPWM_ANIMATION_H

#include <xcb/xcb.h>
#include <stdbool.h>
#include <stdint.h>
#include "types.h"

typedef enum {
    EASE_LINEAR,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_IN_OUT_QUART,
    EASE_OUT_BACK,
    EASE_WINDOW_MOVE
} easing_function_t;

typedef enum {
    ANIM_WINDOW_MOVE,
    ANIM_WINDOW_RESIZE,
    ANIM_WINDOW_MOVE_RESIZE,
    ANIM_FOCUS_INDICATOR
} animation_type_t;

typedef struct animation_t animation_t;
struct animation_t {
    animation_type_t type;
    xcb_window_t window;
    
    uint64_t start_time;
    uint64_t duration;
    easing_function_t easing;
    
    union {
        struct {
            xcb_rectangle_t from;
            xcb_rectangle_t to;
        } move_resize;
    } data;
    
    void (*on_complete)(animation_t *anim);
    void *user_data;
    
    animation_t *next;
};

// global animation settings
extern bool animation_enabled;
extern uint64_t animation_duration;

// core functions
void animation_init(void);
void animation_cleanup(void);
void animation_tick(void);

// window animations
animation_t *animate_window(xcb_window_t win, xcb_rectangle_t to);
animation_t *animate_window_center(xcb_window_t win, xcb_rectangle_t to);
void animation_stop_window(xcb_window_t win);

// settings
void animation_set_enabled(bool enabled);
void animation_set_duration(uint64_t ms);

#endif
