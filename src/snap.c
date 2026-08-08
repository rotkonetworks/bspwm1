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

#include "bspwm.h"
#include "settings.h"
#include "tree.h"
#include "snap.h"

/*
 * Windows-like edge snap zones
 * Detect which snap zone the pointer is in based on screen edges
 */
snap_zone_t get_snap_zone(bspwm_point_t pos, monitor_t *m)
{
	if (!edge_snap_enabled || !m)
		return SNAP_NONE;

	bspwm_rect_t rect = m->rectangle;
	int threshold = edge_snap_threshold;

	bool at_left = pos.x <= rect.x + threshold;
	bool at_right = pos.x >= rect.x + rect.width - threshold;
	bool at_top = pos.y <= rect.y + threshold;
	bool at_bottom = pos.y >= rect.y + rect.height - threshold;

	/* Corners first (higher priority) */
	if (at_left && at_top)
		return SNAP_TOP_LEFT;
	if (at_right && at_top)
		return SNAP_TOP_RIGHT;
	if (at_left && at_bottom)
		return SNAP_BOTTOM_LEFT;
	if (at_right && at_bottom)
		return SNAP_BOTTOM_RIGHT;

	/* Top edge = maximize */
	if (at_top)
		return SNAP_MAXIMIZE;

	/* Side edges = half screen */
	if (at_left)
		return SNAP_LEFT;
	if (at_right)
		return SNAP_RIGHT;

	return SNAP_NONE;
}

/*
 * Apply snap zone to a window - resize and position it
 * Uses target_monitor to support cross-monitor snapping
 */
void apply_snap_zone(coordinates_t *loc, monitor_t *target_monitor, snap_zone_t zone)
{
	if (!loc || !loc->node || !loc->node->client || zone == SNAP_NONE || !target_monitor)
		return;

	node_t *n = loc->node;
	monitor_t *m = target_monitor;
	desktop_t *d = m->desk;

	/* Safety check - ensure target desktop exists */
	if (!d)
		return;

	/* Cross-monitor snap: move the node to the target monitor's desktop
	 * before any geometry math, otherwise set_state/arrange operate on the
	 * source desktop and the node's bspwm-internal location diverges from
	 * what the user dropped. */
	if (loc->monitor != m || loc->desktop != d) {
		transfer_node(loc->monitor, loc->desktop, n, m, d, d->focus, false);
		loc->monitor = m;
		loc->desktop = d;
	}

	bspwm_rect_t rect = m->rectangle;

	/* Account for padding with underflow protection */
	int pad_h = m->padding.left + m->padding.right;
	int pad_v = m->padding.top + m->padding.bottom;
	rect.x += m->padding.left;
	rect.y += m->padding.top;
	rect.width = (pad_h < rect.width) ? rect.width - pad_h : 1;
	rect.height = (pad_v < rect.height) ? rect.height - pad_v : 1;

	bspwm_rect_t target = {0, 0, 0, 0};

	switch (zone) {
		case SNAP_LEFT:
			target.x = rect.x;
			target.y = rect.y;
			target.width = rect.width / 2;
			target.height = rect.height;
			break;
		case SNAP_RIGHT:
			target.x = rect.x + rect.width / 2;
			target.y = rect.y;
			target.width = rect.width / 2;
			target.height = rect.height;
			break;
		case SNAP_TOP:
			target.x = rect.x;
			target.y = rect.y;
			target.width = rect.width;
			target.height = rect.height / 2;
			break;
		case SNAP_BOTTOM:
			target.x = rect.x;
			target.y = rect.y + rect.height / 2;
			target.width = rect.width;
			target.height = rect.height / 2;
			break;
		case SNAP_TOP_LEFT:
			target.x = rect.x;
			target.y = rect.y;
			target.width = rect.width / 2;
			target.height = rect.height / 2;
			break;
		case SNAP_TOP_RIGHT:
			target.x = rect.x + rect.width / 2;
			target.y = rect.y;
			target.width = rect.width / 2;
			target.height = rect.height / 2;
			break;
		case SNAP_BOTTOM_LEFT:
			target.x = rect.x;
			target.y = rect.y + rect.height / 2;
			target.width = rect.width / 2;
			target.height = rect.height / 2;
			break;
		case SNAP_BOTTOM_RIGHT:
			target.x = rect.x + rect.width / 2;
			target.y = rect.y + rect.height / 2;
			target.width = rect.width / 2;
			target.height = rect.height / 2;
			break;
		case SNAP_MAXIMIZE:
			/* Set to fullscreen state instead of just resizing */
			set_state(m, d, n, STATE_FULLSCREEN);
			arrange(m, d);
			return;
		default:
			return;
	}

	/* Ensure window is floating for snap */
	if (n->client->state != STATE_FLOATING) {
		set_state(m, d, n, STATE_FLOATING);
	}

	/* Apply the target rectangle */
	n->client->floating_rectangle = target;
	arrange(m, d);
}
