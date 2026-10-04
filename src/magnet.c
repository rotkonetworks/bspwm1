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

#include <stdlib.h>
#include "magnet.h"

/* Slots of magnet_t.offset and magnet_t.found. */
enum {
	SLOT_LEFT,
	SLOT_RIGHT,
	SLOT_TOP,
	SLOT_BOTTOM,
};

/* Keep `candidate` for the edge in `slot` when it is the closest one so far
 * within the threshold. An offset of 0 counts too: an edge already in place
 * holds there instead of drifting to a farther candidate. */
static void offer(magnet_t *mg, int slot, int edge, int candidate)
{
	int offset = candidate - edge;
	if (abs(offset) > mg->threshold)
		return;
	if (mg->found[slot] && abs(offset) >= abs(mg->offset[slot]))
		return;
	mg->offset[slot] = offset;
	mg->found[slot] = true;
}

/* Whether the spans [a1, a2) and [b1, b2) overlap or lie within `gap` of each
 * other. A window only attracts along one axis when it is next to the dragged
 * one on the other axis; otherwise an edge would catch on the coordinates of
 * windows across the screen. */
static bool spans_near(int a1, int a2, int b1, int b2, int gap)
{
	return b1 <= a2 + gap && a1 <= b2 + gap;
}

void magnet_begin(magnet_t *mg, magnet_box_t free, unsigned int edges,
                  magnet_box_t area, int threshold)
{
	*mg = (magnet_t) {.free = free, .edges = edges, .threshold = threshold};
	offer(mg, SLOT_LEFT, free.x1, area.x1);
	offer(mg, SLOT_RIGHT, free.x2, area.x2);
	offer(mg, SLOT_TOP, free.y1, area.y1);
	offer(mg, SLOT_BOTTOM, free.y2, area.y2);
}

/* Offer to `slot` the edge line at `pos`, drawn over [s1, s2) along it, less
 * the stretches the boxes in `above` cover. `px` is the pixel row or column of
 * the line: `pos` itself for a left or top edge and `pos - 1` for a right or
 * bottom one, whose coordinate is one past the box. */
static void offer_line(magnet_t *mg, int slot, int edge, bool vertical, int pos, int px,
                       int s1, int s2, const magnet_box_t *above, size_t n)
{
	for (; n > 0; above++, n--) {
		int across1 = vertical ? above->x1 : above->y1;
		int across2 = vertical ? above->x2 : above->y2;
		int along1 = vertical ? above->y1 : above->x1;
		int along2 = vertical ? above->y2 : above->x2;
		if (px < across1 || px >= across2 || along2 <= s1 || along1 >= s2)
			continue;
		/* This box hides [along1, along2): offer what is left on each side. */
		if (s1 < along1)
			offer_line(mg, slot, edge, vertical, pos, px, s1, along1, above + 1, n - 1);
		if (along2 < s2)
			offer_line(mg, slot, edge, vertical, pos, px, along2, s2, above + 1, n - 1);
		return;
	}
	magnet_box_t f = mg->free;
	bool near = vertical ? spans_near(f.y1, f.y2, s1, s2, mg->threshold)
	                     : spans_near(f.x1, f.x2, s1, s2, mg->threshold);
	if (near)
		offer(mg, slot, edge, pos);
}

void magnet_consider_visible(magnet_t *mg, magnet_box_t o, const magnet_box_t *above, size_t n)
{
	magnet_box_t f = mg->free;
	/* The same edge in line goes first, so it wins a tie against the
	 * opposite edges touching. */
	offer_line(mg, SLOT_LEFT, f.x1, true, o.x1, o.x1, o.y1, o.y2, above, n);
	offer_line(mg, SLOT_RIGHT, f.x2, true, o.x2, o.x2 - 1, o.y1, o.y2, above, n);
	offer_line(mg, SLOT_TOP, f.y1, false, o.y1, o.y1, o.x1, o.x2, above, n);
	offer_line(mg, SLOT_BOTTOM, f.y2, false, o.y2, o.y2 - 1, o.x1, o.x2, above, n);
	offer_line(mg, SLOT_LEFT, f.x1, true, o.x2, o.x2 - 1, o.y1, o.y2, above, n);
	offer_line(mg, SLOT_RIGHT, f.x2, true, o.x1, o.x1, o.y1, o.y2, above, n);
	offer_line(mg, SLOT_TOP, f.y1, false, o.y2, o.y2 - 1, o.x1, o.x2, above, n);
	offer_line(mg, SLOT_BOTTOM, f.y2, false, o.y1, o.y1, o.x1, o.x2, above, n);
}

void magnet_consider(magnet_t *mg, magnet_box_t o)
{
	magnet_consider_visible(mg, o, NULL, 0);
}

/* For a move, the closer of the two edges of an axis wins; the first one on a
 * tie. */
static int pick(const magnet_t *mg, int lo, int hi)
{
	if (mg->found[lo] && mg->found[hi])
		return abs(mg->offset[hi]) < abs(mg->offset[lo]) ? mg->offset[hi] : mg->offset[lo];
	if (mg->found[lo])
		return mg->offset[lo];
	if (mg->found[hi])
		return mg->offset[hi];
	return 0;
}

magnet_box_t magnet_result(const magnet_t *mg)
{
	magnet_box_t b = mg->free;
	if (mg->threshold <= 0)
		return b;

	if ((mg->edges & MAGNET_ALL) == MAGNET_ALL) {
		int dx = pick(mg, SLOT_LEFT, SLOT_RIGHT);
		int dy = pick(mg, SLOT_TOP, SLOT_BOTTOM);
		return (magnet_box_t) {b.x1 + dx, b.y1 + dy, b.x2 + dx, b.y2 + dy};
	}

	if ((mg->edges & MAGNET_LEFT) && mg->found[SLOT_LEFT])
		b.x1 += mg->offset[SLOT_LEFT];
	if ((mg->edges & MAGNET_RIGHT) && mg->found[SLOT_RIGHT])
		b.x2 += mg->offset[SLOT_RIGHT];
	if ((mg->edges & MAGNET_TOP) && mg->found[SLOT_TOP])
		b.y1 += mg->offset[SLOT_TOP];
	if ((mg->edges & MAGNET_BOTTOM) && mg->found[SLOT_BOTTOM])
		b.y2 += mg->offset[SLOT_BOTTOM];
	return b;
}
