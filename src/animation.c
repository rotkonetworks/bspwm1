#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include "animation.h"
#include "bspwm.h"
#include "window.h"
#include "types.h"
#include "helpers.h"

#define MAX_ANIMATIONS 64
#define MIN_ANIMATION_DISTANCE 3
#define MAX_ANIMATION_DURATION 1000

// global settings are defined in settings.c
extern bool animation_enabled;
extern uint64_t animation_duration;

// animation list with bounds
static animation_t *animations = NULL;
static size_t animation_count = 0;

// time utilities with overflow protection
static uint64_t get_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    // prevent overflow: check if multiplication would exceed UINT64_MAX
	if ((uint64_t)ts.tv_sec > UINT64_MAX / 1000) {
        return UINT64_MAX;
    }

    uint64_t ms = (uint64_t)ts.tv_sec * 1000;
    uint64_t ns_ms = ts.tv_nsec / 1000000;

    // Prevent overflow in addition
    if (ms > UINT64_MAX - ns_ms) {
        return UINT64_MAX;
    }

    return ms + ns_ms;
}

// smooth easing functions for 60fps+ animations
static double ease_in_out_cubic(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    if (t < 0.5) {
        return 4.0 * t * t * t;
    } else {
        double f = 2.0 * t - 2.0;
        return 1.0 + f * f * f * 0.5;
    }
}

static double ease_out_cubic(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    double t1 = t - 1.0;
    return t1 * t1 * t1 + 1.0;
}

static double ease_in_out_quart(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    if (t < 0.5) {
        return 8.0 * t * t * t * t;
    } else {
        double f = t - 1.0;
        return 1.0 - 8.0 * f * f * f * f;
    }
}

static double ease_out_back(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    const double c1 = 1.70158;
    const double c3 = c1 + 1.0;

    double t1 = t - 1.0;
    if (t1 < -1.0) t1 = -1.0;

    return 1.0 + c3 * (t1 * t1 * t1) + c1 * (t1 * t1);
}

// optimized ease for window movements - feels most natural
static double ease_window_move(double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    // custom curve: starts slow, speeds up, then eases out
    return t * t * (3.0 - 2.0 * t);
}

void animation_init(void)
{
    animations = NULL;
    animation_count = 0;
}

void animation_cleanup(void)
{
    animation_t *current = animations;
    while (current != NULL) {
        animation_t *next = current->next;
        secure_memzero(current, sizeof(animation_t));
        free(current);
        current = next;
    }
    animations = NULL;
    animation_count = 0;
}

// safe rectangle distance calculation
static bool calculate_distance(const xcb_rectangle_t *from, const xcb_rectangle_t *to,
                              int *dx, int *dy, int *dw, int *dh)
{
    if (!from || !to || !dx || !dy || !dw || !dh) {
        return false;
    }

    *dx = abs((int)to->x - (int)from->x);
    *dy = abs((int)to->y - (int)from->y);
    *dw = abs((int)to->width - (int)from->width);
    *dh = abs((int)to->height - (int)from->height);

    return true;
}

animation_t *animate_window(xcb_window_t win, xcb_rectangle_t to)
{
    if (!animation_enabled || animation_duration == 0 || animation_count >= MAX_ANIMATIONS) {
        window_move_resize(win, to.x, to.y, to.width, to.height);
        return NULL;
    }

    // validate window
    if (win == XCB_WINDOW_NONE) {
        return NULL;
    }

    // get current geometry
    xcb_get_geometry_cookie_t cookie = xcb_get_geometry(dpy, win);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(dpy, cookie, NULL);
    if (!geo) {
        window_move_resize(win, to.x, to.y, to.width, to.height);
        return NULL;
    }

    xcb_rectangle_t from = {
        .x = geo->x,
        .y = geo->y,
        .width = geo->width,
        .height = geo->height
    };
    free(geo);

    // calculate movement distance safely
    int dx, dy, dw, dh;
    if (!calculate_distance(&from, &to, &dx, &dy, &dw, &dh)) {
        window_move_resize(win, to.x, to.y, to.width, to.height);
        return NULL;
    }

    // Skip tiny movements
    if (dx < MIN_ANIMATION_DISTANCE && dy < MIN_ANIMATION_DISTANCE &&
        dw < MIN_ANIMATION_DISTANCE && dh < MIN_ANIMATION_DISTANCE) {
        window_move_resize(win, to.x, to.y, to.width, to.height);
        return NULL;
    }

    // remove existing animation for this window
    animation_stop_window(win);

    // allocate with calloc for zero initialization
    animation_t *anim = calloc(1, sizeof(animation_t));
    if (!anim) {
        window_move_resize(win, to.x, to.y, to.width, to.height);
        return NULL;
    }

    // initialize animation with bounds checking
    anim->type = ANIM_WINDOW_MOVE_RESIZE;
    anim->window = win;
    anim->start_time = get_time_ms();
    anim->duration = animation_duration > MAX_ANIMATION_DURATION ?
                     MAX_ANIMATION_DURATION : animation_duration;
    anim->easing = EASE_WINDOW_MOVE;
    anim->data.move_resize.from = from;
    anim->data.move_resize.to = to;
    anim->on_complete = NULL;
    anim->user_data = NULL;

    // add to list with count check
    anim->next = animations;
    animations = anim;
    animation_count++;

    return anim;
}

animation_t *animate_window_center(xcb_window_t win, xcb_rectangle_t to)
{
    animation_t *anim = animate_window(win, to);
    if (anim != NULL) {
        anim->easing = EASE_IN_OUT_QUART;
        // slightly longer for centering animation - feels more natural
        uint64_t extra_duration = 50;
        if (anim->duration <= MAX_ANIMATION_DURATION - extra_duration) {
            anim->duration += extra_duration;
        }
    }
    return anim;
}

void animation_stop_window(xcb_window_t win)
{
    if (win == XCB_WINDOW_NONE) {
        return;
    }

    animation_t **current = &animations;
    while (*current != NULL) {
        if ((*current)->window == win) {
            animation_t *to_remove = *current;
            *current = to_remove->next;
            animation_count = animation_count > 0 ? animation_count - 1 : 0;
            secure_memzero(to_remove, sizeof(animation_t));
            free(to_remove);
        } else {
            current = &(*current)->next;
        }
    }
}

// smooth sub-pixel interpolation with proper rounding
static int16_t interpolate_int16(int16_t from, int16_t to, double progress)
{
    if (progress <= 0.0) return from;
    if (progress >= 1.0) return to;

    double diff = (double)to - (double)from;
    double result = (double)from + (diff * progress);

    // Round to nearest integer for smoother motion
    result = result >= 0.0 ? result + 0.5 : result - 0.5;

    if (result < INT16_MIN) return INT16_MIN;
    if (result > INT16_MAX) return INT16_MAX;

    return (int16_t)result;
}

static uint16_t interpolate_uint16(uint16_t from, uint16_t to, double progress)
{
    if (progress <= 0.0) return from;
    if (progress >= 1.0) return to;

    double diff = (double)to - (double)from;
    double result = (double)from + (diff * progress) + 0.5; // round to nearest

    if (result < 0) return 0;
    if (result > UINT16_MAX) return UINT16_MAX;

    return (uint16_t)result;
}

void animation_tick(void)
{
    if (!animation_enabled || animations == NULL) {
        return;
    }

    uint64_t current_time = get_time_ms();
    if (current_time == 0) {  // clock error
        return;
    }

    animation_t **current = &animations;
    size_t processed = 0;

    while (*current != NULL && processed < MAX_ANIMATIONS) {
        animation_t *anim = *current;
        processed++;

        // check for time overflow
        uint64_t elapsed;
        if (current_time < anim->start_time) {
            // clock went backwards, complete animation
            elapsed = anim->duration;
        } else {
            elapsed = current_time - anim->start_time;
        }

        if (elapsed >= anim->duration) {
            // complete animation
            xcb_rectangle_t *to = &anim->data.move_resize.to;
            window_move_resize(anim->window, to->x, to->y, to->width, to->height);

            // remove from list
            *current = anim->next;
            animation_count = animation_count > 0 ? animation_count - 1 : 0;

            // call completion callback before freeing
            if (anim->on_complete != NULL) {
                anim->on_complete(anim);
            }

            secure_memzero(anim, sizeof(animation_t));
            free(anim);
        } else {
            // update animation
            double progress = (double)elapsed / (double)anim->duration;
            if (progress > 1.0) progress = 1.0;
            if (progress < 0.0) progress = 0.0;

            double eased;
            switch (anim->easing) {
                case EASE_IN_OUT_CUBIC:
                    eased = ease_in_out_cubic(progress);
                    break;
                case EASE_IN_OUT_QUART:
                    eased = ease_in_out_quart(progress);
                    break;
                case EASE_OUT_BACK:
                    eased = ease_out_back(progress);
                    break;
                case EASE_OUT_CUBIC:
                    eased = ease_out_cubic(progress);
                    break;
                case EASE_WINDOW_MOVE:
                    eased = ease_window_move(progress);
                    break;
                case EASE_LINEAR:
                default:
                    eased = progress;
                    break;
            }

            xcb_rectangle_t *from = &anim->data.move_resize.from;
            xcb_rectangle_t *to = &anim->data.move_resize.to;

            // safe interpolation
            int16_t x = interpolate_int16(from->x, to->x, eased);
            int16_t y = interpolate_int16(from->y, to->y, eased);
            uint16_t w = interpolate_uint16(from->width, to->width, eased);
            uint16_t h = interpolate_uint16(from->height, to->height, eased);

            window_move_resize(anim->window, x, y, w, h);

            current = &(*current)->next;
        }
    }

    xcb_flush(dpy);
}

void animation_set_enabled(bool enabled)
{
    if (!enabled && animation_enabled) {
        // complete all animations immediately
        animation_t *anim = animations;
        size_t count = 0;

        while (anim != NULL && count < MAX_ANIMATIONS) {
            xcb_rectangle_t *to = &anim->data.move_resize.to;
            window_move_resize(anim->window, to->x, to->y, to->width, to->height);
            anim = anim->next;
            count++;
        }

        animation_cleanup();
    }
    animation_enabled = enabled;
}

void animation_set_duration(uint64_t ms)
{
    if (ms > MAX_ANIMATION_DURATION) {
        animation_duration = MAX_ANIMATION_DURATION;
    } else {
        animation_duration = ms;
    }
}
