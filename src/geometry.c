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
/* geometry.c */
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include "types.h"
#include "settings.h"
#include "geometry.h"

/* Bottom-right corner, computed in int: x + width - 1 can exceed INT16_MAX. */
typedef struct {
	int x, y;
} rect_max_t;

static inline rect_max_t rect_max(bspwm_rect_t r)
{
	return (rect_max_t){r.x + r.width - 1, r.y + r.height - 1};
}

bool is_inside(bspwm_point_t p, bspwm_rect_t r)
{
	return p.x >= r.x && p.x < r.x + r.width &&
	       p.y >= r.y && p.y < r.y + r.height;
}

/* Returns true if a contains b */
bool contains(bspwm_rect_t a, bspwm_rect_t b)
{
	return a.x <= b.x && a.x + a.width >= b.x + b.width &&
	       a.y <= b.y && a.y + a.height >= b.y + b.height;
}

unsigned int area(bspwm_rect_t r)
{
	return (unsigned int)r.width * r.height;
}

uint32_t boundary_distance(bspwm_rect_t r1, bspwm_rect_t r2, direction_t dir)
{
	rect_max_t r1_max = rect_max(r1);
	rect_max_t r2_max = rect_max(r2);

	switch (dir) {
		case DIR_NORTH:
			return r2_max.y > r1.y ? r2_max.y - r1.y : r1.y - r2_max.y;
		case DIR_WEST:
			return r2_max.x > r1.x ? r2_max.x - r1.x : r1.x - r2_max.x;
		case DIR_SOUTH:
			return r2.y < r1_max.y ? r1_max.y - r2.y : r2.y - r1_max.y;
		case DIR_EAST:
			return r2.x < r1_max.x ? r1_max.x - r2.x : r2.x - r1_max.x;
		default:
			return UINT32_MAX;
	}
}

bool on_dir_side(bspwm_rect_t r1, bspwm_rect_t r2, direction_t dir)
{
	rect_max_t r1_max = rect_max(r1);
	rect_max_t r2_max = rect_max(r2);

	/* Check if r2 is on the correct side */
	switch (directional_focus_tightness) {
		case TIGHTNESS_LOW:
			switch (dir) {
				case DIR_NORTH:
					if (r2.y > r1_max.y) return false;
					break;
				case DIR_WEST:
					if (r2.x > r1_max.x) return false;
					break;
				case DIR_SOUTH:
					if (r2_max.y < r1.y) return false;
					break;
				case DIR_EAST:
					if (r2_max.x < r1.x) return false;
					break;
				default:
					return false;
			}
			break;
		case TIGHTNESS_HIGH:
			switch (dir) {
				case DIR_NORTH:
					if (r2.y >= r1.y) return false;
					break;
				case DIR_WEST:
					if (r2.x >= r1.x) return false;
					break;
				case DIR_SOUTH:
					if (r2_max.y <= r1_max.y) return false;
					break;
				case DIR_EAST:
					if (r2_max.x <= r1_max.x) return false;
					break;
				default:
					return false;
			}
			break;
		default:
			return false;
	}

	/* Check overlap */
	switch (dir) {
		case DIR_NORTH:
		case DIR_SOUTH:
			return r2_max.x >= r1.x && r2.x <= r1_max.x;
		case DIR_WEST:
		case DIR_EAST:
			return r2_max.y >= r1.y && r2.y <= r1_max.y;
		default:
			return false;
	}
}

/* SIMD-style rect comparison - single 64-bit compare instead of 4 branches */
bool rect_eq(bspwm_rect_t a, bspwm_rect_t b)
{
	/* bspwm_rect_t is exactly 8 bytes - compare as uint64_t */
	uint64_t va, vb;
	memcpy(&va, &a, sizeof(va));
	memcpy(&vb, &b, sizeof(vb));
	return va == vb;
}

int rect_cmp(bspwm_rect_t r1, bspwm_rect_t r2)
{
	if (r1.y >= r2.y + r2.height)
		return 1;
	if (r2.y >= r1.y + r1.height)
		return -1;
	if (r1.x >= r2.x + r2.width)
		return 1;
	if (r2.x >= r1.x + r1.width)
		return -1;

	unsigned int a1 = area(r1);
	unsigned int a2 = area(r2);
	return (a2 < a1) - (a1 < a2);
}
