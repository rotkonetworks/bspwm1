/* Copyright (c) 2012, Bastien Dejean
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef BSPWM_MAGNET_H
#define BSPWM_MAGNET_H

#include <stdbool.h>
#include <stddef.h>

/* Magnetic edges: while a floating window is dragged, its edges stick to the
 * edges of the work area and of the other windows that come within
 * `magnet_threshold` pixels.
 *
 * Pure geometry with no backend: the pointer-drag path feeds it boxes and
 * applies the result, and the unit tests drive it directly. */

/* Edges of a box, as a bit mask. The values match resize_handle_t, so a
 * resize handle converts with a plain cast. */
enum {
	MAGNET_LEFT = 0b0001,
	MAGNET_TOP = 0b0010,
	MAGNET_RIGHT = 0b0100,
	MAGNET_BOTTOM = 0b1000,
	MAGNET_ALL = 0b1111,
};

/* A window's outer box (border included) as edge coordinates. `x2` and `y2`
 * are one past the last pixel, so two boxes touch when one's x2 equals the
 * other's x1. */
typedef struct {
	int x1, y1, x2, y2;
} magnet_box_t;

/* Nearest candidate found so far for each edge. Start with magnet_begin(),
 * feed every other window to magnet_consider(), then read the result. */
typedef struct {
	magnet_box_t free;
	unsigned int edges;
	int threshold;
	int offset[4];
	bool found[4];
} magnet_t;

/* `free` is where the pointer alone would put the window, `edges` the edges
 * being dragged (MAGNET_ALL for a move) and `area` the work area. */
void magnet_begin(magnet_t *mg, magnet_box_t free, unsigned int edges,
                  magnet_box_t area, int threshold);
void magnet_consider(magnet_t *mg, magnet_box_t other);
/* magnet_consider() for a window with `n` others stacked over it, given in
 * `above`: the stretches of its edges under them do not count, because a
 * window would stick to a line the user cannot see. */
void magnet_consider_visible(magnet_t *mg, magnet_box_t other,
                             const magnet_box_t *above, size_t n);
magnet_box_t magnet_result(const magnet_t *mg);

#endif
