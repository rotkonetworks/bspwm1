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
#include <xcb/xcb_keysyms.h>
#include <stdlib.h>
#include <stdbool.h>
#include "backend_x11.h"
#include "bspwm.h"
#include "query.h"
#include "settings.h"
#include "stack.h"
#include "tree.h"
#include "monitor.h"
#include "subscribe.h"
#include "events.h"
#include "window.h"
#include "snap.h"
#include "magnet.h"
#include "pointer.h"

uint16_t num_lock;
uint16_t caps_lock;
uint16_t scroll_lock;

bool grabbing;
node_t *grabbed_node;

/* Snap preview window */
static bspwm_wid_t snap_preview_win = BSPWM_WID_NONE;
static snap_zone_t current_snap_zone = SNAP_NONE;
static monitor_t *snap_target_monitor = NULL;

void pointer_init(void)
{
	num_lock = modfield_from_keysym(XK_Num_Lock);
	caps_lock = modfield_from_keysym(XK_Caps_Lock);
	scroll_lock = modfield_from_keysym(XK_Scroll_Lock);
	if (caps_lock == XCB_NO_SYMBOL)
		caps_lock = XCB_MOD_MASK_LOCK;
	grabbing = false;
	grabbed_node = NULL;
}



void window_grab_button(bspwm_wid_t win, uint8_t button, uint16_t modifier)
{
#define GRAB(b, m) \
	xcb_grab_button(dpy, false, win, XCB_EVENT_MASK_BUTTON_PRESS, \
	                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, BSPWM_WID_NONE, BSPWM_WID_NONE, b, m)
	GRAB(button, modifier);
	if (num_lock != XCB_NO_SYMBOL && caps_lock != XCB_NO_SYMBOL && scroll_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | num_lock | caps_lock | scroll_lock);
	}
	if (num_lock != XCB_NO_SYMBOL && caps_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | num_lock | caps_lock);
	}
	if (caps_lock != XCB_NO_SYMBOL && scroll_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | caps_lock | scroll_lock);
	}
	if (num_lock != XCB_NO_SYMBOL && scroll_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | num_lock | scroll_lock);
	}
	if (num_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | num_lock);
	}
	if (caps_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | caps_lock);
	}
	if (scroll_lock != XCB_NO_SYMBOL) {
		GRAB(button, modifier | scroll_lock);
	}
#undef GRAB
}

void window_grab_buttons(bspwm_wid_t win)
{
	for (unsigned int i = 0; i < LENGTH(BUTTONS); i++) {
		if (click_to_focus == (int8_t)XCB_BUTTON_INDEX_ANY || click_to_focus == (int8_t)BUTTONS[i])
			window_grab_button(win, BUTTONS[i], BSPWM_WID_NONE);
		if (pointer_actions[i] != ACTION_NONE)
			window_grab_button(win, BUTTONS[i], pointer_modifier);
	}
}

void grab_buttons(void)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
				window_grab_buttons(n->id);
				if (n->presel)
					window_grab_buttons(n->presel->feedback);
			}
		}
	}
}

void ungrab_buttons(void)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
				xcb_ungrab_button(dpy, XCB_BUTTON_INDEX_ANY, n->id, XCB_MOD_MASK_ANY);
			}
		}
	}
}

int16_t modfield_from_keysym(uint32_t keysym)
{
	uint16_t modfield = 0;
	xcb_keycode_t *keycodes = NULL, *mod_keycodes = NULL;
	xcb_get_modifier_mapping_reply_t *reply = NULL;
	xcb_key_symbols_t *symbols = xcb_key_symbols_alloc(dpy);

	if (!symbols)
		return 0;

	keycodes = xcb_key_symbols_get_keycode(symbols, keysym);
	if (!keycodes)
		goto end;

	reply = xcb_get_modifier_mapping_reply(dpy, xcb_get_modifier_mapping(dpy), NULL);
	if (!reply || reply->keycodes_per_modifier < 1)
		goto end;

	mod_keycodes = xcb_get_modifier_mapping_keycodes(reply);
	if (!mod_keycodes)
		goto end;

	unsigned int num_mod = xcb_get_modifier_mapping_keycodes_length(reply) / reply->keycodes_per_modifier;
	for (unsigned int i = 0; i < num_mod && i < 8; i++) {
		for (unsigned int j = 0; j < reply->keycodes_per_modifier; j++) {
			xcb_keycode_t mk = mod_keycodes[i * reply->keycodes_per_modifier + j];
			if (mk == XCB_NO_SYMBOL)
				continue;
			for (xcb_keycode_t *k = keycodes; *k != XCB_NO_SYMBOL; k++) {
				if (*k == mk)
					modfield |= (1 << i);
			}
		}
	}

end:
	xcb_key_symbols_free(symbols);
	free(keycodes);
	free(reply);
	return modfield;
}

resize_handle_t get_handle(node_t *n, bspwm_point_t pos, pointer_action_t pac)
{
	if (!n)
		return HANDLE_BOTTOM_RIGHT;

	resize_handle_t rh = HANDLE_BOTTOM_RIGHT;
	bspwm_rect_t rect = get_rectangle(NULL, NULL, n);
	
	if (rect.width == 0 || rect.height == 0)
		return rh;

	if (pac == ACTION_RESIZE_SIDE) {
		float W = rect.width;
		float H = rect.height;
		float ratio = W / H;
		float x = pos.x - rect.x;
		float y = pos.y - rect.y;
		float diag_a = ratio * y;
		float diag_b = W - diag_a;
		
		if (x < diag_a) {
			if (x < diag_b)
				rh = HANDLE_LEFT;
			else
				rh = HANDLE_BOTTOM;
		} else {
			if (x < diag_b)
				rh = HANDLE_TOP;
			else
				rh = HANDLE_RIGHT;
		}
	} else if (pac == ACTION_RESIZE_CORNER) {
		int16_t mid_x = rect.x + rect.width / 2;
		int16_t mid_y = rect.y + rect.height / 2;
		
		if (pos.x > mid_x) {
			if (pos.y > mid_y)
				rh = HANDLE_BOTTOM_RIGHT;
			else
				rh = HANDLE_TOP_RIGHT;
		} else {
			if (pos.y > mid_y)
				rh = HANDLE_BOTTOM_LEFT;
			else
				rh = HANDLE_TOP_LEFT;
		}
	}
	return rh;
}

bool grab_pointer(pointer_action_t pac)
{
	bspwm_wid_t win = BSPWM_WID_NONE;
	bspwm_point_t pos;

	query_pointer(&win, &pos);

	coordinates_t loc;

	if (!locate_window(win, &loc)) {
		if (pac == ACTION_FOCUS) {
			monitor_t *m = monitor_from_point(pos);
			if (m && m != mon && (win == BSPWM_WID_NONE || win == m->root)) {
				focus_node(m, m->desk, m->desk->focus);
				return true;
			}
		}
		return false;
	}

	if (pac == ACTION_FOCUS) {
		/* `raise_floating_on_click` gates whether a plain click also raises a
		 * floating window, or only focuses it. `stack()` already skips
		 * floating nodes when `auto_raise` is clear, and `focus_node` stacks
		 * internally, so borrowing that flag is the least invasive way to
		 * honour the setting on both paths. */
		bool raise_ok = raise_floating_on_click ||
		                loc.node->client == NULL ||
		                !IS_FLOATING(loc.node->client);
		bool saved_auto_raise = auto_raise;
		if (!raise_ok) {
			auto_raise = false;
		}

		bool focused = false;
		if (loc.node != mon->desk->focus) {
			focus_node(loc.monitor, loc.desktop, loc.node);
			focused = true;
		} else if (raise_ok) {
			stack(loc.desktop, loc.node, true);
		}

		auto_raise = saved_auto_raise;
		return focused;
	}

	if (loc.node->client->state == STATE_FULLSCREEN)
		return true;

	xcb_grab_pointer_reply_t *reply = xcb_grab_pointer_reply(dpy, 
		xcb_grab_pointer(dpy, 0, root, 
		                 XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION,
		                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, 
		                 BSPWM_WID_NONE, BSPWM_WID_NONE, XCB_CURRENT_TIME), NULL);

	if (!reply || reply->status != XCB_GRAB_STATUS_SUCCESS) {
		free(reply);
		return true;
	}
	free(reply);

	/* Windows-like behavior: drag/resize raises the window to the top.
	 * Route through bspwm's own focus/stack machinery so the internal
	 * stacking list stays in sync with the X stack. */
	if (loc.node != mon->desk->focus) {
		focus_node(loc.monitor, loc.desktop, loc.node);
	} else {
		stack(loc.desktop, loc.node, true);
	}

	if (pac == ACTION_MOVE) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X move begin\n",
		          loc.monitor->id, loc.desktop->id, loc.node->id);
	} else if (pac == ACTION_RESIZE_CORNER) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X resize_corner begin\n",
		          loc.monitor->id, loc.desktop->id, loc.node->id);
	} else if (pac == ACTION_RESIZE_SIDE) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X resize_side begin\n",
		          loc.monitor->id, loc.desktop->id, loc.node->id);
	}

	track_pointer(loc, pac, pos);

	return true;
}

/* ---- Magnetic edges (inter-window snapping, X11) ---------------------- */

/* The MAGNET_* bits are shared with resize_handle_t so a handle casts straight
 * to an edge mask; cast both sides because -Wenum-compare flags a bare compare
 * between the anonymous magnet enum and resize_handle_t. */
static_assert((int) MAGNET_LEFT == (int) HANDLE_LEFT && (int) MAGNET_TOP == (int) HANDLE_TOP &&
              (int) MAGNET_RIGHT == (int) HANDLE_RIGHT && (int) MAGNET_BOTTOM == (int) HANDLE_BOTTOM,
              "magnet edges must match resize handles");

/* At most this many windows of a desktop take part in a magnet pass. */
#define MAGNET_MAX_WINDOWS 64

/* Per-drag snapshot of the X stacking order (bottom first) plus an id->level
 * index sorted by window id. The magnet needs each window's stacking level to
 * tell which edges are hidden behind windows above; building the index once
 * per drag turns the per-window lookup from a linear scan of the whole child
 * list into a binary search. Other windows do not restack mid-drag (we hold
 * the pointer grab), so the snapshot stays valid for the drag's lifetime. */
typedef struct { xcb_window_t id; int level; } magnet_level_t;
static xcb_query_tree_reply_t *magnet_stack = NULL;
static magnet_level_t *magnet_levels = NULL;
static int magnet_levels_len = 0;

static int magnet_level_cmp(const void *a, const void *b)
{
	xcb_window_t ia = ((const magnet_level_t *) a)->id;
	xcb_window_t ib = ((const magnet_level_t *) b)->id;
	return (ia > ib) - (ia < ib);
}

static void magnet_stack_acquire(void)
{
	magnet_stack = xcb_query_tree_reply(dpy, xcb_query_tree(dpy, root), NULL);
	magnet_levels = NULL;
	magnet_levels_len = 0;
	if (magnet_stack == NULL)
		return;
	xcb_window_t *wins = xcb_query_tree_children(magnet_stack);
	int len = xcb_query_tree_children_length(magnet_stack);
	if (len <= 0)
		return;
	magnet_levels = malloc((size_t) len * sizeof(*magnet_levels));
	if (magnet_levels == NULL)
		return;
	for (int i = 0; i < len; i++)
		magnet_levels[i] = (magnet_level_t) {wins[i], i};
	qsort(magnet_levels, (size_t) len, sizeof(*magnet_levels), magnet_level_cmp);
	magnet_levels_len = len;
}

static void magnet_stack_release(void)
{
	free(magnet_levels);
	magnet_levels = NULL;
	magnet_levels_len = 0;
	free(magnet_stack);
	magnet_stack = NULL;
}

/* Events that change what the stacking snapshot should say: a window mapping,
 * unmapping, going away, or restacking while we drag. They arrive on root
 * (bspwm selects SubstructureNotify there) and are handled between motions, so
 * the snapshot is refreshed when one does. */
static bool magnet_stack_stale_after(uint8_t resp_type)
{
	switch (resp_type) {
	case XCB_MAP_NOTIFY:
	case XCB_UNMAP_NOTIFY:
	case XCB_DESTROY_NOTIFY:
	case XCB_CONFIGURE_NOTIFY:
	case XCB_REPARENT_NOTIFY:
		return true;
	default:
		return false;
	}
}

/* Stacking level of a window (bottom first), or -1 if not in the snapshot. */
static int magnet_stack_index(bspwm_wid_t win)
{
	int lo = 0, hi = magnet_levels_len - 1;
	xcb_window_t key = (xcb_window_t) win;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (magnet_levels[mid].id == key)
			return magnet_levels[mid].level;
		if (magnet_levels[mid].id < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* Outer box of a node's window: its rectangle plus the border on both sides. */
static magnet_box_t magnet_box_of(node_t *n)
{
	bspwm_rect_t r = get_rectangle(NULL, NULL, n);
	int b = 2 * (int) n->client->border_width;
	return (magnet_box_t) {r.x, r.y, r.x + r.width + b, r.y + r.height + b};
}

/* Work area of a desktop, computed the way arrange() does. */
static magnet_box_t magnet_area_of(monitor_t *m, desktop_t *d)
{
	bspwm_rect_t r = m->rectangle;
	padding_t p = m->padding;
	if (d != NULL) {
		p.top += d->padding.top;
		p.right += d->padding.right;
		p.bottom += d->padding.bottom;
		p.left += d->padding.left;
	}
	return (magnet_box_t) {r.x + p.left, r.y + p.top,
	                       r.x + r.width - p.right, r.y + r.height - p.bottom};
}

/* Where the dragged window goes when the pointer alone would put it at `free`:
 * a magnet pass against the work area and the other visible windows. A move is
 * checked against the monitor the window is about to land on, so it sticks to
 * the new monitor in the same step. */
static magnet_box_t magnet_snap_node(coordinates_t *loc, magnet_box_t free, unsigned int edges)
{
	monitor_t *m = loc->monitor;
	desktop_t *d = loc->desktop;
	if (edges == MAGNET_ALL) {
		/* Compute the centre in int and clamp to the point type's range:
		 * monitor coordinates are int16_t, so a centre past that is off the
		 * rightmost/bottommost monitor and clamps onto it instead of
		 * wrapping negative and selecting the wrong one. */
		int cx = (free.x1 + free.x2) / 2;
		int cy = (free.y1 + free.y2) / 2;
		if (cx < INT16_MIN) cx = INT16_MIN; else if (cx > INT16_MAX) cx = INT16_MAX;
		if (cy < INT16_MIN) cy = INT16_MIN; else if (cy > INT16_MAX) cy = INT16_MAX;
		bspwm_point_t center = {(int16_t) cx, (int16_t) cy};
		monitor_t *target = monitor_from_point(center);
		if (target != NULL && target != m) {
			m = target;
			d = target->desk;
		}
	}

	magnet_t mg;
	magnet_begin(&mg, free, edges, magnet_area_of(m, d), magnet_threshold);
	if (d != NULL) {
		/* Every other window on screen, with its place in the stack. */
		magnet_box_t boxes[MAGNET_MAX_WINDOWS];
		int levels[MAGNET_MAX_WINDOWS];
		size_t count = 0;
		for (node_t *f = first_extrema(d->root); f != NULL && count < MAGNET_MAX_WINDOWS;
		     f = next_leaf(f, d->root)) {
			if (f == loc->node || f->client == NULL || f->hidden || !f->client->shown)
				continue;
			boxes[count] = magnet_box_of(f);
			levels[count] = magnet_stack_index(f->id);
			count++;
		}
		for (size_t i = 0; i < count; i++) {
			magnet_box_t above[MAGNET_MAX_WINDOWS];
			size_t na = 0;
			for (size_t j = 0; j < count; j++)
				if (levels[j] > levels[i])
					above[na++] = boxes[j];
			magnet_consider_visible(&mg, boxes[i], above, na);
		}
	}
	return magnet_result(&mg);
}

/* One resize step with magnetic edges. `free` holds where the dragged edges
 * would be without the magnet and is advanced by this motion first. */
static void magnet_resize(coordinates_t *loc, resize_handle_t rh, magnet_box_t *free,
                          int root_x, int root_y, int dx, int dy, bool absolute)
{
	int b = 2 * (int) loc->node->client->border_width;
	if (absolute) {
		/* resize_client puts the outer left/top edge at the pointer, and
		 * x + width (y + height) for the right (bottom) edge, which is the
		 * outer edge minus both borders. */
		if (rh & HANDLE_LEFT)
			free->x1 = root_x;
		if (rh & HANDLE_RIGHT)
			free->x2 = root_x + b;
		if (rh & HANDLE_TOP)
			free->y1 = root_y;
		if (rh & HANDLE_BOTTOM)
			free->y2 = root_y + b;
	} else {
		if (rh & HANDLE_LEFT)
			free->x1 += dx;
		if (rh & HANDLE_RIGHT)
			free->x2 += dx;
		if (rh & HANDLE_TOP)
			free->y1 += dy;
		if (rh & HANDLE_BOTTOM)
			free->y2 += dy;
	}

	magnet_box_t want = magnet_snap_node(loc, *free, (unsigned int) rh);

	if (absolute) {
		int ax = (rh & HANDLE_LEFT) ? want.x1 : want.x2 - b;
		int ay = (rh & HANDLE_TOP) ? want.y1 : want.y2 - b;
		resize_client(loc, rh, ax, ay, false);
		return;
	}

	magnet_box_t cur = magnet_box_of(loc->node);
	int ddx = 0, ddy = 0;
	if (rh & HANDLE_LEFT)
		ddx = want.x1 - cur.x1;
	else if (rh & HANDLE_RIGHT)
		ddx = want.x2 - cur.x2;
	if (rh & HANDLE_TOP)
		ddy = want.y1 - cur.y1;
	else if (rh & HANDLE_BOTTOM)
		ddy = want.y2 - cur.y2;
	resize_client(loc, rh, ddx, ddy, true);
}

void track_pointer(coordinates_t loc, pointer_action_t pac, bspwm_point_t pos)
{
	node_t *n = loc.node;
	if (!n || !n->client)
		return;

	resize_handle_t rh = get_handle(loc.node, pos, pac);

	/* Magnetic edges: the box the pointer alone would give the window, which
	 * the magnet then adjusts on every motion. Off (and free) unless the
	 * window floats and magnet_threshold is set. */
	bool magnet_on = magnet_threshold > 0 && IS_FLOATING(n->client);
	magnet_box_t magnet_free = magnet_on ? magnet_box_of(n) : (magnet_box_t) {0};
	if (magnet_on)
		magnet_stack_acquire();

	uint16_t last_motion_x = pos.x, last_motion_y = pos.y;
	xcb_timestamp_t last_motion_time = 0;
	snap_zone_t final_snap_zone = SNAP_NONE;

	xcb_generic_event_t *evt = NULL;

	grabbing = true;
	grabbed_node = n;
	snap_target_monitor = NULL;

	do {
		free(evt);
		evt = xcb_wait_for_event(dpy);
		if (!evt) {
			/* NULL means the X connection errored (not "no event" — this call
			 * blocks). Continuing would spin the drag loop at 100% CPU, so bail. */
			if (xcb_connection_has_error(dpy)) {
				grabbing = false;
			}
			continue;
		}

		uint8_t resp_type = XCB_EVENT_RESPONSE_TYPE(evt);
		if (resp_type == XCB_MOTION_NOTIFY) {
			/* Coalesce: drain everything xcb has already queued and keep only
			 * the newest motion. Under load the queue backs up; acting on the
			 * latest position (not the oldest) tracks the cursor and does one
			 * move/resize per frame instead of one per stale sample. */
			xcb_generic_event_t *qev;
			while ((qev = xcb_poll_for_queued_event(dpy)) != NULL) {
				uint8_t qt = XCB_EVENT_RESPONSE_TYPE(qev);
				if (qt == XCB_MOTION_NOTIFY) {
					free(evt);
					evt = qev;
				} else if (qt == XCB_BUTTON_RELEASE) {
					grabbing = false;
					free(qev);
					break;
				} else {
					handle_event(qev);
					if (magnet_on && magnet_stack_stale_after(qt)) {
						magnet_stack_release();
						magnet_stack_acquire();
					}
					free(qev);
					if (grabbed_node && !locate_window(grabbed_node->id, &loc)) {
						grabbed_node = NULL;
					}
				}
			}
			if (!grabbing || !grabbed_node) {
				continue;
			}

			xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t*) evt;
			uint32_t dtime = e->time - last_motion_time;
			if (dtime < pointer_motion_interval)
				continue;

			last_motion_time = e->time;
			int16_t dx = e->root_x - last_motion_x;
			int16_t dy = e->root_y - last_motion_y;

			if (pac == ACTION_MOVE) {
				if (magnet_on) {
					/* Advance the pointer-only box, then let the magnet
					 * pull it to nearby edges and move by that delta. */
					magnet_free.x1 += dx; magnet_free.x2 += dx;
					magnet_free.y1 += dy; magnet_free.y2 += dy;
					magnet_box_t want = magnet_snap_node(&loc, magnet_free, MAGNET_ALL);
					magnet_box_t cur = magnet_box_of(n);
					move_client(&loc, want.x1 - cur.x1, want.y1 - cur.y1);
				} else {
					move_client(&loc, dx, dy);
				}

				/* Check for edge snap zones while dragging */
				if (edge_snap_enabled) {
					bspwm_point_t cur_pos = {e->root_x, e->root_y};
					monitor_t *m = monitor_from_point(cur_pos);
					/* Only process snap if pointer is on a valid monitor */
					if (m != NULL) {
						snap_zone_t zone = get_snap_zone(cur_pos, m);
						final_snap_zone = zone;
						show_snap_preview(m, zone);
					} else {
						/* Pointer between monitors - hide preview but keep last zone */
						hide_snap_preview();
					}
				}
			} else if (n && n->client) {
				client_t *c = n->client;
				bool absolute = SHOULD_HONOR_SIZE_HINTS(c->honor_size_hints, c->state);
				if (magnet_on) {
					magnet_resize(&loc, rh, &magnet_free, e->root_x, e->root_y, dx, dy, absolute);
				} else if (absolute) {
					resize_client(&loc, rh, e->root_x, e->root_y, false);
				} else {
					resize_client(&loc, rh, dx, dy, true);
				}
			}
			last_motion_x = e->root_x;
			last_motion_y = e->root_y;
			xcb_flush(dpy);
		} else if (resp_type == XCB_BUTTON_RELEASE) {
			grabbing = false;
		} else {
			handle_event(evt);
			if (magnet_on && magnet_stack_stale_after(resp_type)) {
				magnet_stack_release();
				magnet_stack_acquire();
			}
			/* handle_event may have moved the grabbed node to another
			 * desktop/monitor (e.g. via _NET_WM_DESKTOP), which would
			 * make loc.desktop / loc.monitor stale. Refresh loc so
			 * subsequent move/resize/snap logic clamps against the
			 * correct monitor. If the node is gone, exit the loop. */
			if (grabbed_node && !locate_window(grabbed_node->id, &loc)) {
				grabbed_node = NULL;
			}
		}
	} while (grabbing && grabbed_node);

	if (magnet_on)
		magnet_stack_release();

	/* Hide snap preview and apply snap if released in a zone */
	destroy_snap_preview();
	if (pac == ACTION_MOVE && final_snap_zone != SNAP_NONE && grabbed_node) {
		/* The monitor under the pointer was cached during motion and may
		 * have been freed by a RANDR-driven remove_monitor() in
		 * handle_event(). Re-resolve from the live pointer position. */
		bspwm_point_t pt;
		query_pointer(NULL, &pt);
		monitor_t *sm = monitor_from_point(pt);
		if (sm != NULL) {
			apply_snap_zone(&loc, sm, final_snap_zone);
		}
	}
	snap_target_monitor = NULL;
	
	free(evt);
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);

	if (!grabbed_node) {
		grabbing = false;
		return;
	}

	/* Re-locate node to get fresh coordinates - the monitor/desktop
	 * pointers in loc may be stale if events were processed during drag */
	if (!locate_window(n->id, &loc)) {
		return;
	}

	if (pac == ACTION_MOVE) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X move end\n",
		          loc.monitor->id, loc.desktop->id, n->id);
	} else if (pac == ACTION_RESIZE_CORNER) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X resize_corner end\n",
		          loc.monitor->id, loc.desktop->id, n->id);
	} else if (pac == ACTION_RESIZE_SIDE) {
		put_status(SBSC_MASK_POINTER_ACTION, "pointer_action 0x%08X 0x%08X 0x%08X resize_side end\n",
		          loc.monitor->id, loc.desktop->id, n->id);
	}

	bspwm_rect_t r = get_rectangle(NULL, NULL, n);
	put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
	          loc.monitor->id, loc.desktop->id, loc.node->id, r.width, r.height, r.x, r.y);

	if ((pac == ACTION_MOVE && IS_TILED(n->client)) ||
	    ((pac == ACTION_RESIZE_CORNER || pac == ACTION_RESIZE_SIDE) &&
	     n->client->state == STATE_TILED)) {
		for (node_t *f = first_extrema(loc.desktop->root); f; f = next_leaf(f, loc.desktop->root)) {
			if (f == n || !f->client || !IS_TILED(f->client))
				continue;
			bspwm_rect_t r = f->client->tiled_rectangle;
			put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
			          loc.monitor->id, loc.desktop->id, f->id, r.width, r.height, r.x, r.y);
		}
	}
}

/*
 * Show a preview overlay for the snap zone
 */
void show_snap_preview(monitor_t *m, snap_zone_t zone)
{
	if (!m || zone == SNAP_NONE) {
		hide_snap_preview();
		return;
	}

	/* Check if zone AND monitor are the same - only skip redraw if both match */
	if (zone == current_snap_zone && m == snap_target_monitor && snap_preview_win != BSPWM_WID_NONE)
		return;

	current_snap_zone = zone;
	snap_target_monitor = m;

	bspwm_rect_t rect = m->rectangle;
	int pad_h = m->padding.left + m->padding.right;
	int pad_v = m->padding.top + m->padding.bottom;
	rect.x += m->padding.left;
	rect.y += m->padding.top;
	rect.width = (pad_h < rect.width) ? rect.width - pad_h : 1;
	rect.height = (pad_v < rect.height) ? rect.height - pad_v : 1;

	bspwm_rect_t preview = {0, 0, 0, 0};

	switch (zone) {
		case SNAP_LEFT:
			preview.x = rect.x;
			preview.y = rect.y;
			preview.width = rect.width / 2;
			preview.height = rect.height;
			break;
		case SNAP_RIGHT:
			preview.x = rect.x + rect.width / 2;
			preview.y = rect.y;
			preview.width = rect.width / 2;
			preview.height = rect.height;
			break;
		case SNAP_TOP_LEFT:
			preview.x = rect.x;
			preview.y = rect.y;
			preview.width = rect.width / 2;
			preview.height = rect.height / 2;
			break;
		case SNAP_TOP_RIGHT:
			preview.x = rect.x + rect.width / 2;
			preview.y = rect.y;
			preview.width = rect.width / 2;
			preview.height = rect.height / 2;
			break;
		case SNAP_BOTTOM_LEFT:
			preview.x = rect.x;
			preview.y = rect.y + rect.height / 2;
			preview.width = rect.width / 2;
			preview.height = rect.height / 2;
			break;
		case SNAP_BOTTOM_RIGHT:
			preview.x = rect.x + rect.width / 2;
			preview.y = rect.y + rect.height / 2;
			preview.width = rect.width / 2;
			preview.height = rect.height / 2;
			break;
		case SNAP_MAXIMIZE:
			preview = rect;
			break;
		default:
			hide_snap_preview();
			return;
	}

	/* Create or update preview window */
	if (snap_preview_win == BSPWM_WID_NONE) {
		snap_preview_win = xcb_generate_id(dpy);
		uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
		uint32_t values[] = {0x40E6007A, 0xE6007A, 1};  /* Semi-transparent pink */
		xcb_create_window(dpy, XCB_COPY_FROM_PARENT, snap_preview_win, root,
		                  preview.x, preview.y, preview.width, preview.height,
		                  2, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		                  XCB_COPY_FROM_PARENT, mask, values);
	}

	/* Position and show */
	uint32_t cfg_values[] = {preview.x, preview.y, preview.width, preview.height};
	xcb_configure_window(dpy, snap_preview_win,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
	                     cfg_values);
	xcb_map_window(dpy, snap_preview_win);
	xcb_flush(dpy);
}

/*
 * Hide the snap preview overlay (keeps window for reuse)
 */
void hide_snap_preview(void)
{
	if (snap_preview_win != BSPWM_WID_NONE) {
		xcb_unmap_window(dpy, snap_preview_win);
		xcb_flush(dpy);
	}
	current_snap_zone = SNAP_NONE;
}

/*
 * Destroy the snap preview window completely (call when drag ends)
 */
void destroy_snap_preview(void)
{
	if (snap_preview_win != BSPWM_WID_NONE) {
		xcb_destroy_window(dpy, snap_preview_win);
		snap_preview_win = BSPWM_WID_NONE;
		xcb_flush(dpy);
	}
	current_snap_zone = SNAP_NONE;
	snap_target_monitor = NULL;
}
