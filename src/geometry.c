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

#ifdef __SSE2__
#include <emmintrin.h>
#define HAVE_SSE2 1
#endif

#ifdef __SSE4_1__
#include <smmintrin.h>
#define HAVE_SSE41 1
#endif

/*
 * Batch is_inside check - test point against 2 rectangles simultaneously
 * Returns bitmask: bit 0 = inside rect[0], bit 1 = inside rect[1]
 */
#ifdef HAVE_SSE2
int is_inside_batch2(xcb_point_t p, const xcb_rectangle_t rects[2])
{
	/* Load point as {px, py, px, py} */
	__m128i point = _mm_set_epi32(p.y, p.x, p.y, p.x);

	/* Load rect mins as {r0.x, r0.y, r1.x, r1.y} */
	__m128i mins = _mm_set_epi32(rects[1].y, rects[1].x, rects[0].y, rects[0].x);

	/* Load rect maxs as {r0.x+w, r0.y+h, r1.x+w, r1.y+h} */
	__m128i maxs = _mm_set_epi32(
		rects[1].y + rects[1].height,
		rects[1].x + rects[1].width,
		rects[0].y + rects[0].height,
		rects[0].x + rects[0].width
	);

	/* point >= mins AND point < maxs */
	__m128i ge_min = _mm_cmpgt_epi32(point, _mm_sub_epi32(mins, _mm_set1_epi32(1)));
	__m128i lt_max = _mm_cmplt_epi32(point, maxs);
	__m128i inside = _mm_and_si128(ge_min, lt_max);

	/* Extract results - need both x and y conditions true */
	int mask = _mm_movemask_ps(_mm_castsi128_ps(inside));
	int r0 = (mask & 0x3) == 0x3 ? 1 : 0;  /* bits 0,1 both set */
	int r1 = (mask & 0xC) == 0xC ? 2 : 0;  /* bits 2,3 both set */
	return r0 | r1;
}
#endif

static inline bool valid_rect(xcb_rectangle_t r)
{
	return r.width > 0 && r.height > 0 &&
	       r.x <= INT16_MAX - r.width &&
	       r.y <= INT16_MAX - r.height;
}

static inline xcb_point_t rect_max(xcb_rectangle_t r)
{
	return (xcb_point_t){r.x + r.width - 1, r.y + r.height - 1};
}

bool is_inside(xcb_point_t p, xcb_rectangle_t r)
{
	if (!valid_rect(r))
		return false;
	return p.x >= r.x && p.x < r.x + r.width &&
	       p.y >= r.y && p.y < r.y + r.height;
}

bool contains(xcb_rectangle_t a, xcb_rectangle_t b)
{
	if (!valid_rect(a) || !valid_rect(b))
		return false;
	xcb_point_t b_max = rect_max(b);
	return a.x <= b.x && a.y <= b.y &&
	       a.x + a.width >= b_max.x + 1 &&
	       a.y + a.height >= b_max.y + 1;
}

unsigned int area(xcb_rectangle_t r)
{
	if (!valid_rect(r))
		return 0;
	if (r.width > UINT_MAX / r.height)
		return UINT_MAX;
	return r.width * r.height;
}

uint32_t boundary_distance(xcb_rectangle_t r1, xcb_rectangle_t r2, direction_t dir)
{
	if (!valid_rect(r1) || !valid_rect(r2))
		return UINT32_MAX;

	xcb_point_t r1_max = rect_max(r1);
	xcb_point_t r2_max = rect_max(r2);

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

bool on_dir_side(xcb_rectangle_t r1, xcb_rectangle_t r2, direction_t dir)
{
	if (!valid_rect(r1) || !valid_rect(r2))
		return false;

	xcb_point_t r1_max = rect_max(r1);
	xcb_point_t r2_max = rect_max(r2);

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
bool rect_eq(xcb_rectangle_t a, xcb_rectangle_t b)
{
	/* xcb_rectangle_t is exactly 8 bytes - compare as uint64_t */
	uint64_t va, vb;
	memcpy(&va, &a, sizeof(va));
	memcpy(&vb, &b, sizeof(vb));
	return va == vb;
}

int rect_cmp(xcb_rectangle_t r1, xcb_rectangle_t r2)
{
	if (!valid_rect(r1) || !valid_rect(r2))
		return 0;

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
