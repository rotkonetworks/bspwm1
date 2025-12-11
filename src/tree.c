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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include "bspwm.h"
#include "desktop.h"
#include "ewmh.h"
#include "history.h"
#include "monitor.h"
#include "query.h"
#include "geometry.h"
#include "subscribe.h"
#include "settings.h"
#include "pointer.h"
#include "stack.h"
#include "window.h"
#include "tree.h"
#include "animation.h"
#include "rule.h"

#define MAX_TREE_DEPTH 256
#define SAFE_ADD(a, b, max) ((b) > 0 && (a) > (max) - (b)) ? (max) : (a) + (b)
#define SAFE_SUB(a, b) ((a) < (b) ? 0 : (a) - (b))

/* Secure memset that won't be optimized away */
void secure_memzero(void *ptr, size_t len)
{
	volatile unsigned char *p = ptr;
	while (len--) {
		*p++ = 0;
	}
}

void arrange(monitor_t *m, desktop_t *d)
{
	if (!m || !d || !d->root) {
		return;
	}

	xcb_rectangle_t rect = m->rectangle;

	rect.x = SAFE_ADD(rect.x, SAFE_ADD(m->padding.left, d->padding.left, UINT16_MAX), UINT16_MAX);
	rect.y = SAFE_ADD(rect.y, SAFE_ADD(m->padding.top, d->padding.top, UINT16_MAX), UINT16_MAX);
	rect.width = SAFE_SUB(rect.width, SAFE_ADD(m->padding.left + d->padding.left,
	                                           d->padding.right + m->padding.right, UINT16_MAX));
	rect.height = SAFE_SUB(rect.height, SAFE_ADD(m->padding.top + d->padding.top,
	                                             d->padding.bottom + m->padding.bottom, UINT16_MAX));

	if (d->layout == LAYOUT_MONOCLE) {
		rect.x = SAFE_ADD(rect.x, monocle_padding.left, UINT16_MAX);
		rect.y = SAFE_ADD(rect.y, monocle_padding.top, UINT16_MAX);
		rect.width = SAFE_SUB(rect.width, monocle_padding.left + monocle_padding.right);
		rect.height = SAFE_SUB(rect.height, monocle_padding.top + monocle_padding.bottom);
	}

	if (!gapless_monocle || d->layout != LAYOUT_MONOCLE) {
		rect.x = SAFE_ADD(rect.x, d->window_gap, UINT16_MAX);
		rect.y = SAFE_ADD(rect.y, d->window_gap, UINT16_MAX);
		rect.width = SAFE_SUB(rect.width, d->window_gap);
		rect.height = SAFE_SUB(rect.height, d->window_gap);
	}

	apply_layout(m, d, d->root, rect, rect);
}

void apply_layout(monitor_t *m, desktop_t *d, node_t *n, xcb_rectangle_t rect, xcb_rectangle_t root_rect)
{
	if (!n || !m || !d) {
		return;
	}

	n->rectangle = rect;

	if (n->presel) {
		draw_presel_feedback(m, d, n);
	}

	if (is_leaf(n)) {
		if (!n->client) {
			return;
		}

		unsigned int bw;
		bool the_only_window = !m->prev && !m->next && d->root && d->root->client;

		if ((borderless_monocle && d->layout == LAYOUT_MONOCLE && IS_TILED(n->client)) ||
		    (borderless_singleton && the_only_window) ||
		    n->client->state == STATE_FULLSCREEN) {
			bw = 0;
		} else {
			bw = n->client->border_width;
		}

		xcb_rectangle_t r;
		xcb_rectangle_t cr = get_window_rectangle(n);
		client_state_t s = n->client->state;

		if (s == STATE_TILED || s == STATE_PSEUDO_TILED) {
			int wg = (gapless_monocle && d->layout == LAYOUT_MONOCLE ? 0 : d->window_gap);
			r = rect;

			if ((int)bw > (INT_MAX - wg) / 2) {
				bw = 0;
			}

			int bleed = wg + 2 * (int)bw;
			r.width = (bleed < (int)r.width ? r.width - bleed : 1);
			r.height = (bleed < (int)r.height ? r.height - bleed : 1);

			if (s == STATE_PSEUDO_TILED) {
				xcb_rectangle_t f = n->client->floating_rectangle;
				r.width = MIN(r.width, f.width);
				r.height = MIN(r.height, f.height);
				if (center_pseudo_tiled) {
					r.x = rect.x - bw + (rect.width - wg - r.width) / 2;
					r.y = rect.y - bw + (rect.height - wg - r.height) / 2;
				}
			}
			n->client->tiled_rectangle = r;
		} else if (s == STATE_FLOATING) {
			r = n->client->floating_rectangle;
		} else {
			r = m->rectangle;
			n->client->tiled_rectangle = r;
		}

		apply_size_hints(n->client, &r.width, &r.height);

		if (!rect_eq(r, cr)) {
			client_t *c = n->client;
			if (animation_enabled && c != NULL && c->state == STATE_TILED) {
				animate_window(n->id, r);
			} else {
				window_move_resize(n->id, r.x, r.y, r.width, r.height);
			}
			if (!grabbing) {
				put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
			   m->id, d->id, n->id, r.width, r.height, r.x, r.y);
			}
		}

		window_border_width(n->id, bw);
	} else {
		if (!n->first_child || !n->second_child) {
			return;
		}

		xcb_rectangle_t first_rect;
		xcb_rectangle_t second_rect;

		if (d->layout == LAYOUT_MONOCLE || n->first_child->vacant || n->second_child->vacant) {
			first_rect = second_rect = rect;
		} else {
			unsigned int fence;
			if (n->split_type == TYPE_VERTICAL) {
				fence = (unsigned int)(rect.width * n->split_ratio);
				uint16_t min_sum = SAFE_ADD(n->first_child->constraints.min_width,
				                           n->second_child->constraints.min_width, UINT16_MAX);

				if (min_sum <= rect.width) {
					if (fence < n->first_child->constraints.min_width) {
						fence = n->first_child->constraints.min_width;
						n->split_ratio = (double)fence / (double)rect.width;
					} else if (fence > (unsigned int)rect.width - n->second_child->constraints.min_width) {
						fence = (unsigned int)rect.width - n->second_child->constraints.min_width;
						n->split_ratio = (double)fence / (double)rect.width;
					}
				}

				first_rect = (xcb_rectangle_t){rect.x, rect.y, fence, rect.height};
				second_rect = (xcb_rectangle_t){rect.x + fence, rect.y, rect.width - fence, rect.height};
			} else {
				fence = (unsigned int)(rect.height * n->split_ratio);
				uint16_t min_sum = SAFE_ADD(n->first_child->constraints.min_height,
				                           n->second_child->constraints.min_height, UINT16_MAX);

				if (min_sum <= rect.height) {
					if (fence < n->first_child->constraints.min_height) {
						fence = n->first_child->constraints.min_height;
						n->split_ratio = (double)fence / (double)rect.height;
					} else if (fence > (unsigned int)rect.height - n->second_child->constraints.min_height) {
						fence = (unsigned int)rect.height - n->second_child->constraints.min_height;
						n->split_ratio = (double)fence / (double)rect.height;
					}
				}

				first_rect = (xcb_rectangle_t){rect.x, rect.y, rect.width, fence};
				second_rect = (xcb_rectangle_t){rect.x, rect.y + fence, rect.width, rect.height - fence};
			}
		}

		apply_layout(m, d, n->first_child, first_rect, root_rect);
		apply_layout(m, d, n->second_child, second_rect, root_rect);
	}
}

presel_t *make_presel(void)
{
	presel_t *p = calloc(1, sizeof(presel_t));
	if (!p) {
		return NULL;
	}
	p->split_dir = DIR_EAST;
	p->split_ratio = split_ratio;
	p->feedback = XCB_NONE;
	return p;
}

bool set_type(node_t *n, split_type_t typ)
{
	if (!n || n->split_type == typ) {
		return false;
	}

	n->split_type = typ;
	update_constraints(n);
	rebuild_constraints_towards_root(n);
	return true;
}

bool set_ratio(node_t *n, double rat)
{
	if (!n || n->split_ratio == rat || rat < 0.0 || rat > 1.0) {
		return false;
	}

	n->split_ratio = rat;
	return true;
}

void presel_dir(monitor_t *m, desktop_t *d, node_t *n, direction_t dir)
{
	if (!m || !d || !n) {
		return;
	}

	if (!n->presel) {
		n->presel = make_presel();
		if (!n->presel) {
			return;
		}
	}

	n->presel->split_dir = dir;
	put_status(SBSC_MASK_NODE_PRESEL, "node_presel 0x%08X 0x%08X 0x%08X dir %s\n",
	           m->id, d->id, n->id, SPLIT_DIR_STR(dir));
}

void presel_ratio(monitor_t *m, desktop_t *d, node_t *n, double ratio)
{
	if (!m || !d || !n || ratio < 0.0 || ratio > 1.0) {
		return;
	}

	if (!n->presel) {
		n->presel = make_presel();
		if (!n->presel) {
			return;
		}
	}

	n->presel->split_ratio = ratio;
	put_status(SBSC_MASK_NODE_PRESEL, "node_presel 0x%08X 0x%08X 0x%08X ratio %lf\n",
	           m->id, d->id, n->id, ratio);
}

void cancel_presel(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!n || !n->presel) {
		return;
	}

	if (n->presel->feedback != XCB_NONE) {
		xcb_destroy_window(dpy, n->presel->feedback);
	}

	free(n->presel);
	n->presel = NULL;

	if (m && d) {
		put_status(SBSC_MASK_NODE_PRESEL, "node_presel 0x%08X 0x%08X 0x%08X cancel\n",
		           m->id, d->id, n->id);
	}
}

static void cancel_presel_in_bounded(monitor_t *m, desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}
	cancel_presel(m, d, n);
	cancel_presel_in_bounded(m, d, n->first_child, depth + 1);
	cancel_presel_in_bounded(m, d, n->second_child, depth + 1);
}

void cancel_presel_in(monitor_t *m, desktop_t *d, node_t *n)
{
	cancel_presel_in_bounded(m, d, n, 0);
}

node_t *find_public(desktop_t *d)
{
	if (!d || !d->root) {
		return NULL;
	}

	unsigned int b_manual_area = 0;
	unsigned int b_automatic_area = 0;
	node_t *b_manual = NULL;
	node_t *b_automatic = NULL;

	for (node_t *n = first_extrema(d->root); n; n = next_leaf(n, d->root)) {
		if (n->vacant) {
			continue;
		}
		unsigned int n_area = node_area(d, n);
		if (n_area > b_manual_area && (n->presel || !n->private)) {
			b_manual = n;
			b_manual_area = n_area;
		}
		if (n_area > b_automatic_area && !n->presel && !n->private && private_count(n->parent) == 0) {
			b_automatic = n;
			b_automatic_area = n_area;
		}
	}

	return b_automatic ? b_automatic : b_manual;
}

bool window_ignores_tile_limits(xcb_window_t win)
{
	if (!win) {
		return false;
	}

	xcb_icccm_get_wm_class_reply_t reply;
	bool result = false;

	if (xcb_icccm_get_wm_class_reply(dpy, xcb_icccm_get_wm_class(dpy, win), &reply, NULL) != 1) {
		return false;
	}

	if (!reply.class_name) {
		goto cleanup;
	}

	for (rule_t *r = rule_head; r != NULL; r = r->next) {
		if (streq(r->class_name, MATCH_ANY) || streq(reply.class_name, r->class_name)) {
			if (strstr(r->effect, "ignore_tile_limits=on") != NULL) {
				result = true;
				break;
			}
		}
	}

cleanup:
	xcb_icccm_get_wm_class_reply_wipe(&reply);
	return result;
}

static void count_tiled_nodes_iterative(node_t *root, int *count)
{
	if (!root || !count) {
		return;
	}

	node_t *stack[1024];
	int stack_top = 0;

	stack[stack_top++] = root;

	while (stack_top > 0) {
		if (stack_top >= 1024) {
			break;
		}

		node_t *n = stack[--stack_top];
		if (!n) {
			continue;
		}

		if (n->client && !IS_FLOATING(n->client)) {
			if (*count >= INT_MAX - 1) {
				break;
			}
			(*count)++;
		}

		if (n->second_child && stack_top < 1023) {
			stack[stack_top++] = n->second_child;
		}
		if (n->first_child && stack_top < 1023) {
			stack[stack_top++] = n->first_child;
		}
	}
}

int count_tiled_windows(desktop_t *d)
{
	if (!d || !d->root) {
		return 0;
	}

	int count = 0;
	count_tiled_nodes_iterative(d->root, &count);
	return count;
}

node_t *insert_node(monitor_t *m, desktop_t *d, node_t *n, node_t *f)
{
	if (!d || !n) {
		return NULL;
	}

	if (!d->tile_limit_enabled || !n->client || IS_FLOATING(n->client)) {
		goto insert_node;
	}

	if (window_ignores_tile_limits(n->id)) {
		goto insert_node;
	}

	int current_tiles = count_tiled_windows(d);
	if (current_tiles >= d->max_tiles_per_desktop) {
		// Force window to be floating when tile limit is reached
		// This keeps it managed by bspwm instead of becoming unmanaged
		if (n->client) {
			n->client->state = STATE_FLOATING;
		}
		goto insert_node;
	}

insert_node:

	bool d_was_not_occupied = !d->root;

	if (!f) {
		f = d->root;
	}

	if (!f) {
		d->root = n;
	} else if (IS_RECEPTACLE(f) && !f->presel) {
		node_t *p = f->parent;
		if (p) {
			if (is_first_child(f)) {
				p->first_child = n;
			} else {
				p->second_child = n;
			}
		} else {
			d->root = n;
		}
		n->parent = p;
		free(f);
		f = NULL;
	} else {
		node_t *c = make_node(XCB_NONE);
		if (!c) {
			return NULL;
		}

		node_t *p = f->parent;
		if (!f->presel && (f->private || private_count(f->parent) > 0)) {
			node_t *k = find_public(d);
			if (k) {
				f = k;
				p = f->parent;
			}
			if (!f->presel && (f->private || private_count(f->parent) > 0)) {
				xcb_rectangle_t rect = get_rectangle(m, d, f);
				presel_dir(m, d, f, (rect.width >= rect.height ? DIR_EAST : DIR_SOUTH));
			}
		}

		n->parent = c;
		if (!f->presel) {
			bool single_tiled = f->client && IS_TILED(f->client) && tiled_count(d->root, true) == 1;
			if (!p || automatic_scheme != SCHEME_SPIRAL || single_tiled) {
				if (p) {
					if (is_first_child(f)) {
						p->first_child = c;
					} else {
						p->second_child = c;
					}
				} else {
					d->root = c;
				}
				c->parent = p;
				f->parent = c;
				if (initial_polarity == FIRST_CHILD) {
					c->first_child = n;
					c->second_child = f;
				} else {
					c->first_child = f;
					c->second_child = n;
				}
				if (!p || automatic_scheme == SCHEME_LONGEST_SIDE || single_tiled) {
					c->split_type = (f->rectangle.width > f->rectangle.height) ? TYPE_VERTICAL : TYPE_HORIZONTAL;
				} else {
					node_t *q = p;
					while (q && (q->first_child->vacant || q->second_child->vacant)) {
						q = q->parent;
					}
					if (!q) {
						q = p;
					}
					c->split_type = (q->split_type == TYPE_HORIZONTAL) ? TYPE_VERTICAL : TYPE_HORIZONTAL;
				}
			} else {
				node_t *g = p->parent;
				c->parent = g;
				if (g) {
					if (is_first_child(p)) {
						g->first_child = c;
					} else {
						g->second_child = c;
					}
				} else {
					d->root = c;
				}
				c->split_type = p->split_type;
				c->split_ratio = p->split_ratio;
				p->parent = c;
				int rot;
				if (is_first_child(f)) {
					c->first_child = n;
					c->second_child = p;
					rot = 90;
				} else {
					c->first_child = p;
					c->second_child = n;
					rot = 270;
				}
				if (!n->vacant) {
					rotate_tree(p, rot);
				}
			}
		} else {
			if (p) {
				if (is_first_child(f)) {
					p->first_child = c;
				} else {
					p->second_child = c;
				}
			}
			c->split_ratio = f->presel->split_ratio;
			c->parent = p;
			f->parent = c;
			switch (f->presel->split_dir) {
				case DIR_WEST:
					c->split_type = TYPE_VERTICAL;
					c->first_child = n;
					c->second_child = f;
					break;
				case DIR_EAST:
					c->split_type = TYPE_VERTICAL;
					c->first_child = f;
					c->second_child = n;
					break;
				case DIR_NORTH:
					c->split_type = TYPE_HORIZONTAL;
					c->first_child = n;
					c->second_child = f;
					break;
				case DIR_SOUTH:
					c->split_type = TYPE_HORIZONTAL;
					c->first_child = f;
					c->second_child = n;
					break;
			}
			if (d->root == f) {
				d->root = c;
			}
			cancel_presel(m, d, f);
			set_marked(m, d, n, false);
		}
	}

	propagate_flags_upward(m, d, n);

	if (!d->focus && is_focusable(n)) {
		d->focus = n;
	}

	if (d_was_not_occupied) {
		put_status(SBSC_MASK_REPORT);
	}

	return f;
}

void insert_receptacle(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m || !d) {
		return;
	}

	node_t *r = make_node(XCB_NONE);
	if (!r) {
		return;
	}

	insert_node(m, d, r, n);
	put_status(SBSC_MASK_NODE_ADD, "node_add 0x%08X 0x%08X 0x%08X 0x%08X\n",
	           m->id, d->id, n ? n->id : 0, r->id);

	if (single_monocle && d->layout == LAYOUT_MONOCLE && tiled_count(d->root, true) > 1) {
		set_layout(m, d, d->user_layout, false);
	}
}

bool activate_node(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m || !d) {
		return false;
	}

	if (!n && d->root) {
		n = d->focus;
		if (!n) {
			n = history_last_node(d, NULL);
		}
		if (!n) {
			n = first_focusable_leaf(d->root);
		}
	}

	if (d == mon->desk || (n && !is_focusable(n))) {
		return false;
	}

	if (n && !find_by_id_in(d->root, n->id)) {
		return false;
	}

	if (n) {
		if (d->focus && n != d->focus) {
			neutralize_occluding_windows(m, d, n);
		}
		stack(d, n, true);
		if (d->focus != n) {
			for (node_t *f = first_extrema(d->focus); f; f = next_leaf(f, d->focus)) {
				if (f->client && !is_descendant(f, n)) {
					window_draw_border(f->id, get_border_color(false, (m == mon)));
				}
			}
		}
		draw_border(n, true, (m == mon));
	}

	d->focus = n;
	history_add(m, d, n, false);
	put_status(SBSC_MASK_REPORT);

	if (n) {
		put_status(SBSC_MASK_NODE_ACTIVATE, "node_activate 0x%08X 0x%08X 0x%08X\n",
		           m->id, d->id, n->id);
	}

	return true;
}

static void transfer_sticky_nodes_bounded(monitor_t *ms, desktop_t *ds, monitor_t *md, desktop_t *dd, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	if (n->sticky) {
		sticky_still = false;
		transfer_node(ms, ds, n, md, dd, dd->focus, false);
		sticky_still = true;
	} else {
		node_t *first_child = n->first_child;
		node_t *second_child = n->second_child;
		transfer_sticky_nodes_bounded(ms, ds, md, dd, first_child, depth + 1);
		transfer_sticky_nodes_bounded(ms, ds, md, dd, second_child, depth + 1);
	}
}

void transfer_sticky_nodes(monitor_t *ms, desktop_t *ds, monitor_t *md, desktop_t *dd, node_t *n)
{
	transfer_sticky_nodes_bounded(ms, ds, md, dd, n, 0);
}

bool focus_node(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m) {
		m = mon;
		if (!m) {
			m = history_last_monitor(NULL);
		}
		if (!m) {
			m = mon_head;
		}
	}

	if (!m) {
		return false;
	}

	if (!d) {
		d = m->desk;
		if (!d) {
			d = history_last_desktop(m, NULL);
		}
		if (!d) {
			d = m->desk_head;
		}
	}

	if (!d) {
		return false;
	}

	bool guess = !n;

	if (!n && d->root) {
		n = d->focus;
		if (!n) {
			n = history_last_node(d, NULL);
		}
		if (!n) {
			n = first_focusable_leaf(d->root);
		}
	}

	if (n && !is_focusable(n)) {
		return false;
	}

	if ((mon && mon->desk != d) || !n || !n->client) {
		clear_input_focus();
	}

	if (m->sticky_count > 0 && m->desk && d != m->desk) {
		if (guess && m->desk->focus && m->desk->focus->sticky) {
			n = m->desk->focus;
		}

		transfer_sticky_nodes(m, m->desk, m, d, m->desk->root);

		if (!n && d->focus) {
			n = d->focus;
		}
	}

	if (d->focus && n != d->focus) {
		neutralize_occluding_windows(m, d, n);
	}

	if (n && n->client && n->client->urgent) {
		set_urgent(m, d, n, false);
	}

	if (mon != m) {
		if (mon) {
			for (desktop_t *e = mon->desk_head; e; e = e->next) {
				draw_border(e->focus, true, false);
			}
		}
		for (desktop_t *e = m->desk_head; e; e = e->next) {
			if (e == d) {
				continue;
			}
			draw_border(e->focus, true, true);
		}
	}

	if (d->focus != n) {
		for (node_t *f = first_extrema(d->focus); f; f = next_leaf(f, d->focus)) {
			if (f->client && !is_descendant(f, n)) {
				window_draw_border(f->id, get_border_color(false, true));
			}
		}
	}

	draw_border(n, true, true);

	bool desk_changed = (m != mon || m->desk != d);
	bool has_input_focus = false;

	if (mon != m) {
		mon = m;

		if (pointer_follows_monitor) {
			center_pointer(m->rectangle);
		}

		put_status(SBSC_MASK_MONITOR_FOCUS, "monitor_focus 0x%08X\n", m->id);
	}

	if (m->desk != d) {
		show_desktop(d);
		set_input_focus(n);
		has_input_focus = true;
		hide_desktop(m->desk);
		m->desk = d;
	}

	if (desk_changed) {
		ewmh_update_current_desktop();
		put_status(SBSC_MASK_DESKTOP_FOCUS, "desktop_focus 0x%08X 0x%08X\n", m->id, d->id);
	}

	d->focus = n;
	if (!has_input_focus) {
		set_input_focus(n);
	}
	ewmh_update_active_window();
	history_add(m, d, n, true);

	put_status(SBSC_MASK_REPORT);

	if (!n) {
		if (focus_follows_pointer) {
			update_motion_recorder();
		}
		return true;
	}

	put_status(SBSC_MASK_NODE_FOCUS, "node_focus 0x%08X 0x%08X 0x%08X\n", m->id, d->id, n->id);

	stack(d, n, true);

	if (pointer_follows_focus) {
		xcb_rectangle_t rect = get_rectangle(m, d, n);
		if (rect.width > 0 && rect.height > 0) {
			center_pointer(rect);
		}
	} else if (focus_follows_pointer) {
		update_motion_recorder();
	}

	return true;
}

static void hide_node_bounded(desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH || (!hide_sticky && n->sticky)) {
		return;
	}

	if (!n->hidden) {
		if (n->presel && d->layout != LAYOUT_MONOCLE) {
			window_hide(n->presel->feedback);
		}
		if (n->client) {
			window_hide(n->id);
		}
	}
	if (n->client) {
		n->client->shown = false;
	}
	hide_node_bounded(d, n->first_child, depth + 1);
	hide_node_bounded(d, n->second_child, depth + 1);
}

void hide_node(desktop_t *d, node_t *n)
{
	hide_node_bounded(d, n, 0);
}

static void show_node_bounded(desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	if (!n->hidden) {
		if (n->client) {
			window_show(n->id);
		}
		if (n->presel && d->layout != LAYOUT_MONOCLE) {
			window_show(n->presel->feedback);
		}
	}
	if (n->client) {
		n->client->shown = true;
	}
	show_node_bounded(d, n->first_child, depth + 1);
	show_node_bounded(d, n->second_child, depth + 1);
}

void show_node(desktop_t *d, node_t *n)
{
	show_node_bounded(d, n, 0);
}

node_t *make_node(uint32_t id)
{
	if (id == XCB_NONE) {
		id = xcb_generate_id(dpy);
	}
	node_t *n = calloc(1, sizeof(node_t));
	if (!n) {
		return NULL;
	}
	n->id = id;
	n->parent = n->first_child = n->second_child = NULL;
	n->vacant = n->hidden = n->sticky = n->private = n->locked = n->marked = false;
	n->split_ratio = split_ratio;
	n->split_type = TYPE_VERTICAL;
	n->constraints = (constraints_t){MIN_WIDTH, MIN_HEIGHT};
	n->presel = NULL;
	n->client = NULL;
	return n;
}

client_t *make_client(void)
{
	client_t *c = calloc(1, sizeof(client_t));
	if (!c) {
		return NULL;
	}
	c->state = c->last_state = STATE_TILED;
	c->layer = c->last_layer = LAYER_NORMAL;
	strncpy(c->class_name, MISSING_VALUE, sizeof(c->class_name) - 1);
	c->class_name[sizeof(c->class_name) - 1] = '\0';
	strncpy(c->instance_name, MISSING_VALUE, sizeof(c->instance_name) - 1);
	c->instance_name[sizeof(c->instance_name) - 1] = '\0';
	c->border_width = border_width;
	c->urgent = false;
	c->shown = false;
	c->wm_flags = 0;
	c->icccm_props.input_hint = true;
	c->icccm_props.take_focus = false;
	c->icccm_props.delete_window = false;
	c->size_hints.flags = 0;
	c->honor_size_hints = honor_size_hints;
	return c;
}

/*
 * Pipelined client initialization - sends all 4 property requests at once.
 * Reduces 4 sequential X11 round-trips to 1 batch (~2000μs → ~500μs).
 */
void initialize_client(node_t *n)
{
	if (!n || !n->client) {
		return;
	}

	xcb_window_t win = n->id;
	client_t *c = n->client;

	/* Send all cookies first - no waiting yet */
	xcb_get_property_cookie_t protos_cookie = xcb_icccm_get_wm_protocols(dpy, win, ewmh->WM_PROTOCOLS);
	xcb_get_property_cookie_t state_cookie = xcb_ewmh_get_wm_state(ewmh, win);
	xcb_get_property_cookie_t hints_cookie = xcb_icccm_get_wm_hints(dpy, win);
	xcb_get_property_cookie_t size_cookie = xcb_icccm_get_wm_normal_hints(dpy, win);

	/* Now collect all replies */
	xcb_icccm_get_wm_protocols_reply_t protos;
	if (xcb_icccm_get_wm_protocols_reply(dpy, protos_cookie, &protos, NULL) == 1) {
		for (uint32_t i = 0; i < protos.atoms_len && i < 32; i++) {
			if (protos.atoms[i] == WM_TAKE_FOCUS) {
				c->icccm_props.take_focus = true;
			} else if (protos.atoms[i] == WM_DELETE_WINDOW) {
				c->icccm_props.delete_window = true;
			}
		}
		xcb_icccm_get_wm_protocols_reply_wipe(&protos);
	}

	xcb_ewmh_get_atoms_reply_t wm_state;
	if (xcb_ewmh_get_wm_state_reply(ewmh, state_cookie, &wm_state, NULL) == 1) {
		for (unsigned int i = 0; i < wm_state.atoms_len && i < MAX_WM_STATES; i++) {
#define HANDLE_WM_STATE(s) \
			if (wm_state.atoms[i] == ewmh->_NET_WM_STATE_##s) { \
				c->wm_flags |= WM_FLAG_##s; continue; \
			}
			HANDLE_WM_STATE(MODAL)
			HANDLE_WM_STATE(STICKY)
			HANDLE_WM_STATE(MAXIMIZED_VERT)
			HANDLE_WM_STATE(MAXIMIZED_HORZ)
			HANDLE_WM_STATE(SHADED)
			HANDLE_WM_STATE(SKIP_TASKBAR)
			HANDLE_WM_STATE(SKIP_PAGER)
			HANDLE_WM_STATE(HIDDEN)
			HANDLE_WM_STATE(FULLSCREEN)
			HANDLE_WM_STATE(ABOVE)
			HANDLE_WM_STATE(BELOW)
			HANDLE_WM_STATE(DEMANDS_ATTENTION)
#undef HANDLE_WM_STATE
		}
		xcb_ewmh_get_atoms_reply_wipe(&wm_state);
	}

	xcb_icccm_wm_hints_t hints;
	if (xcb_icccm_get_wm_hints_reply(dpy, hints_cookie, &hints, NULL) == 1 &&
	    (hints.flags & XCB_ICCCM_WM_HINT_INPUT)) {
		c->icccm_props.input_hint = hints.input;
	}

	xcb_icccm_get_wm_normal_hints_reply(dpy, size_cookie, &c->size_hints, NULL);
}

bool is_focusable(node_t *n)
{
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		if (f->client && !f->hidden) {
			return true;
		}
	}
	return false;
}

bool is_leaf(node_t *n)
{
	return n && !n->first_child && !n->second_child;
}

bool is_first_child(node_t *n)
{
	return n && n->parent && n->parent->first_child == n;
}

bool is_second_child(node_t *n)
{
	return n && n->parent && n->parent->second_child == n;
}

static unsigned int clients_count_in_bounded(node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return 0;
	}
	return (n->client ? 1 : 0) +
	       clients_count_in_bounded(n->first_child, depth + 1) +
	       clients_count_in_bounded(n->second_child, depth + 1);
}

unsigned int clients_count_in(node_t *n)
{
	return clients_count_in_bounded(n, 0);
}

node_t *brother_tree(node_t *n)
{
	if (!n || !n->parent) {
		return NULL;
	}
	return is_first_child(n) ? n->parent->second_child : n->parent->first_child;
}

node_t *first_extrema(node_t *n)
{
	if (!n) return NULL;

	int depth = 0;
	while (n->first_child && depth < MAX_TREE_DEPTH) {
		n = n->first_child;
		depth++;
	}
	return depth > MAX_TREE_DEPTH ? NULL : n;
}

node_list_t *collect_leaves(node_t *root)
{
	if (!root) return NULL;

	node_list_t *list = malloc(sizeof(node_list_t));
	if (!list) return NULL;

	list->capacity = 64;
	list->count = 0;
	list->nodes = safe_malloc_array(list->capacity, sizeof(node_t*));
	if (!list->nodes) {
		free(list);
		return NULL;
	}

	node_t *stack[MAX_TREE_DEPTH];
	int stack_top = 0;
	stack[stack_top++] = root;

	while (stack_top > 0) {
		node_t *n = stack[--stack_top];
		if (!n) continue;

		if (is_leaf(n)) {
			if (list->count >= list->capacity) {
				size_t new_cap = list->capacity;
				if (!safe_double(&new_cap)) break;  /* overflow protection */
				node_t **new_nodes = safe_realloc_array(list->nodes, new_cap, sizeof(node_t*));
				if (!new_nodes) break;
				list->nodes = new_nodes;
				list->capacity = new_cap;
			}
			list->nodes[list->count++] = n;
		} else {
			if (n->second_child && stack_top < MAX_TREE_DEPTH - 1)
				stack[stack_top++] = n->second_child;
			if (n->first_child && stack_top < MAX_TREE_DEPTH - 1)
				stack[stack_top++] = n->first_child;
		}
	}
	return list;
}

void free_node_list(node_list_t *list)
{
	if (!list) return;
	free(list->nodes);
	free(list);
}

static node_t *second_extrema_bounded(node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return NULL;
	}
	if (!n->second_child) {
		return n;
	}
	return second_extrema_bounded(n->second_child, depth + 1);
}

node_t *second_extrema(node_t *n)
{
	return second_extrema_bounded(n, 0);
}

node_t *first_focusable_leaf(node_t *n)
{
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		if (f->client && !f->hidden) {
			return f;
		}
	}
	return NULL;
}

node_t *next_node(node_t *n)
{
	if (!n) {
		return NULL;
	}

	if (n->second_child) {
		return first_extrema(n->second_child);
	}

	node_t *p = n;
	while (is_second_child(p)) {
		p = p->parent;
		if (!p) {
			return NULL;
		}
	}

	if (is_first_child(p)) {
		return p->parent;
	}

	return NULL;
}

node_t *prev_node(node_t *n)
{
	if (!n) {
		return NULL;
	}

	if (n->first_child) {
		return second_extrema(n->first_child);
	}

	node_t *p = n;
	while (is_first_child(p)) {
		p = p->parent;
		if (!p) {
			return NULL;
		}
	}

	if (is_second_child(p)) {
		return p->parent;
	}

	return NULL;
}

node_t *next_leaf(node_t *n, node_t *r)
{
	if (!n) {
		return NULL;
	}

	node_t *p = n;
	while (is_second_child(p) && p != r) {
		p = p->parent;
		if (!p) {
			return NULL;
		}
	}

	if (p == r || !p->parent) {
		return NULL;
	}

	return first_extrema(p->parent->second_child);
}

node_t *prev_leaf(node_t *n, node_t *r)
{
	if (!n) {
		return NULL;
	}

	node_t *p = n;
	while (is_first_child(p) && p != r) {
		p = p->parent;
		if (!p) {
			return NULL;
		}
	}

	if (p == r || !p->parent) {
		return NULL;
	}

	return second_extrema(p->parent->first_child);
}

node_t *next_tiled_leaf(node_t *n, node_t *r)
{
	node_t *next = next_leaf(n, r);
	if (!next || (next->client && !next->vacant)) {
		return next;
	}
	return next_tiled_leaf(next, r);
}

node_t *prev_tiled_leaf(node_t *n, node_t *r)
{
	node_t *prev = prev_leaf(n, r);
	if (!prev || (prev->client && !prev->vacant)) {
		return prev;
	}
	return prev_tiled_leaf(prev, r);
}

bool is_adjacent(node_t *a, node_t *b, direction_t dir)
{
	if (!a || !b) {
		return false;
	}

	switch (dir) {
		case DIR_EAST:
			return (a->rectangle.x + a->rectangle.width) == b->rectangle.x;
		case DIR_SOUTH:
			return (a->rectangle.y + a->rectangle.height) == b->rectangle.y;
		case DIR_WEST:
			return (b->rectangle.x + b->rectangle.width) == a->rectangle.x;
		case DIR_NORTH:
			return (b->rectangle.y + b->rectangle.height) == a->rectangle.y;
	}
	return false;
}

node_t *find_fence(node_t *n, direction_t dir)
{
	if (!n) {
		return NULL;
	}

	node_t *p = n->parent;

	while (p) {
		if ((dir == DIR_NORTH && p->split_type == TYPE_HORIZONTAL && p->rectangle.y < n->rectangle.y) ||
		    (dir == DIR_WEST && p->split_type == TYPE_VERTICAL && p->rectangle.x < n->rectangle.x) ||
		    (dir == DIR_SOUTH && p->split_type == TYPE_HORIZONTAL &&
		     (p->rectangle.y + p->rectangle.height) > (n->rectangle.y + n->rectangle.height)) ||
		    (dir == DIR_EAST && p->split_type == TYPE_VERTICAL &&
		     (p->rectangle.x + p->rectangle.width) > (n->rectangle.x + n->rectangle.width))) {
			return p;
		}
		p = p->parent;
	}

	return NULL;
}

bool is_child(node_t *a, node_t *b)
{
	return a && b && a->parent && a->parent == b;
}

bool is_descendant(node_t *a, node_t *b)
{
	if (!a || !b) {
		return false;
	}
	while (a != b && a) {
		a = a->parent;
	}
	return a == b;
}

bool find_by_id(uint32_t id, coordinates_t *loc)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			node_t *n = find_by_id_in(d->root, id);
			if (n) {
				if (loc) {
					loc->monitor = m;
					loc->desktop = d;
					loc->node = n;
				}
				return true;
			}
		}
	}
	return false;
}

static node_t *find_by_id_in_bounded(node_t *r, uint32_t id, int depth)
{
	if (!r || depth > MAX_TREE_DEPTH) {
		return NULL;
	}
	if (r->id == id) {
		return r;
	}
	node_t *f = find_by_id_in_bounded(r->first_child, id, depth + 1);
	if (f) {
		return f;
	}
	return find_by_id_in_bounded(r->second_child, id, depth + 1);
}

node_t *find_by_id_in(node_t *r, uint32_t id)
{
	return find_by_id_in_bounded(r, id, 0);
}

void find_any_node(coordinates_t *ref, coordinates_t *dst, node_select_t *sel)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			if (find_any_node_in(m, d, d->root, ref, dst, sel)) {
				return;
			}
		}
	}
}

static bool find_any_node_in_bounded(monitor_t *m, desktop_t *d, node_t *n,
                                    coordinates_t *ref, coordinates_t *dst,
                                    node_select_t *sel, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return false;
	}

	coordinates_t loc = {m, d, n};
	if (node_matches(&loc, ref, sel)) {
		*dst = loc;
		return true;
	}

	if (find_any_node_in_bounded(m, d, n->first_child, ref, dst, sel, depth + 1)) {
		return true;
	}

	return find_any_node_in_bounded(m, d, n->second_child, ref, dst, sel, depth + 1);
}

bool find_any_node_in(monitor_t *m, desktop_t *d, node_t *n,
                     coordinates_t *ref, coordinates_t *dst, node_select_t *sel)
{
	return find_any_node_in_bounded(m, d, n, ref, dst, sel, 0);
}

void find_first_ancestor(coordinates_t *ref, coordinates_t *dst, node_select_t *sel)
{
	if (!ref || !ref->node) {
		return;
	}

	coordinates_t loc = {ref->monitor, ref->desktop, ref->node};
	while ((loc.node = loc.node->parent)) {
		if (node_matches(&loc, ref, sel)) {
			*dst = loc;
			return;
		}
	}
}

void find_nearest_neighbor(coordinates_t *ref, coordinates_t *dst, direction_t dir, node_select_t *sel)
{
	if (!ref || !ref->monitor || !ref->desktop || !ref->node) {
		return;
	}

	xcb_rectangle_t rect = get_rectangle(ref->monitor, ref->desktop, ref->node);
	uint32_t md = UINT32_MAX, mr = UINT32_MAX;

	for (monitor_t *m = mon_head; m; m = m->next) {
		desktop_t *d = m->desk;
		if (!d) {
			continue;
		}

		for (node_t *f = first_extrema(d->root); f; f = next_leaf(f, d->root)) {
			coordinates_t loc = {m, d, f};
			xcb_rectangle_t r = get_rectangle(m, d, f);

			if (f == ref->node || !f->client || f->hidden ||
			    is_descendant(f, ref->node) || !node_matches(&loc, ref, sel) ||
			    !on_dir_side(rect, r, dir)) {
				continue;
			}

			uint32_t fd = boundary_distance(rect, r, dir);
			uint32_t fr = history_rank(f);

			if (fd < md || (fd == md && fr < mr)) {
				md = fd;
				mr = fr;
				*dst = loc;
			}
		}
	}
}

unsigned int node_area(desktop_t *d, node_t *n)
{
	if (!n) {
		return 0;
	}
	return area(get_rectangle(NULL, d, n));
}

int tiled_count(node_t *n, bool include_receptacles)
{
	if (!n) {
		return 0;
	}

	int cnt = 0;
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		if (!f->hidden && ((include_receptacles && !f->client) ||
		                   (f->client && IS_TILED(f->client)))) {
			cnt++;
		}
	}
	return cnt;
}

void find_by_area(area_peak_t ap, coordinates_t *ref, coordinates_t *dst, node_select_t *sel)
{
	unsigned int p_area = (ap == AREA_BIGGEST) ? 0 : UINT_MAX;

	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			for (node_t *f = first_extrema(d->root); f; f = next_leaf(f, d->root)) {
				coordinates_t loc = {m, d, f};
				if (f->vacant || !node_matches(&loc, ref, sel)) {
					continue;
				}

				unsigned int f_area = node_area(d, f);
				if ((ap == AREA_BIGGEST && f_area > p_area) ||
				    (ap == AREA_SMALLEST && f_area < p_area)) {
					*dst = loc;
					p_area = f_area;
				}
			}
		}
	}
}

void rotate_tree(node_t *n, int deg)
{
	rotate_tree_rec(n, deg);
	rebuild_constraints_from_leaves(n);
	rebuild_constraints_towards_root(n);
}

static void rotate_tree_rec_bounded(node_t *n, int deg, int depth)
{
	if (!n || is_leaf(n) || deg == 0 || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *tmp;

	if ((deg == 90 && n->split_type == TYPE_HORIZONTAL) ||
	    (deg == 270 && n->split_type == TYPE_VERTICAL) ||
	    deg == 180) {
		tmp = n->first_child;
		n->first_child = n->second_child;
		n->second_child = tmp;
		n->split_ratio = 1.0 - n->split_ratio;
	}

	if (deg != 180) {
		if (n->split_type == TYPE_HORIZONTAL) {
			n->split_type = TYPE_VERTICAL;
		} else if (n->split_type == TYPE_VERTICAL) {
			n->split_type = TYPE_HORIZONTAL;
		}
	}

	if (depth < MAX_TREE_DEPTH) rotate_tree_rec_bounded(n->first_child, deg, depth + 1);
	rotate_tree_rec_bounded(n->second_child, deg, depth + 1);
}

void rotate_tree_rec(node_t *n, int deg)
{
	rotate_tree_rec_bounded(n, deg, 0);
}

static void flip_tree_bounded(node_t *n, flip_t flp, int depth)
{
	if (!n || is_leaf(n) || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *tmp;

	if ((flp == FLIP_HORIZONTAL && n->split_type == TYPE_HORIZONTAL) ||
	    (flp == FLIP_VERTICAL && n->split_type == TYPE_VERTICAL)) {
		tmp = n->first_child;
		n->first_child = n->second_child;
		n->second_child = tmp;
		n->split_ratio = 1.0 - n->split_ratio;
	}

	flip_tree_bounded(n->first_child, flp, depth + 1);
	flip_tree_bounded(n->second_child, flp, depth + 1);
}

void flip_tree(node_t *n, flip_t flp)
{
	flip_tree_bounded(n, flp, 0);
}

static void equalize_tree_bounded(node_t *n, int depth)
{
	if (!n || n->vacant || depth > MAX_TREE_DEPTH) {
		return;
	}

	n->split_ratio = split_ratio;
	equalize_tree_bounded(n->first_child, depth + 1);
	equalize_tree_bounded(n->second_child, depth + 1);
}

void equalize_tree(node_t *n)
{
	equalize_tree_bounded(n, 0);
}

static int balance_tree_bounded(node_t *n, int depth)
{
	if (!n || n->vacant || depth > MAX_TREE_DEPTH) {
		return 0;
	}

	if (is_leaf(n)) {
		return 1;
	}

	int b1 = balance_tree_bounded(n->first_child, depth + 1);
	int b2 = balance_tree_bounded(n->second_child, depth + 1);
	int b = b1 + b2;

	if (b1 > 0 && b2 > 0 && b > 0) {
		n->split_ratio = (double)b1 / (double)b;
	}

	return b;
}

int balance_tree(node_t *n)
{
	return balance_tree_bounded(n, 0);
}

static void adjust_ratios_bounded(node_t *n, xcb_rectangle_t rect, int depth)
{
	if (!n || n->vacant || depth > MAX_TREE_DEPTH) {
		return;
	}

	double ratio;

	if (n->split_type == TYPE_VERTICAL) {
		double position = (double)n->rectangle.x + n->split_ratio * (double)n->rectangle.width;
		if (rect.width > 0) {
			ratio = (position - (double)rect.x) / (double)rect.width;
		} else {
			ratio = 0.5;
		}
	} else {
		double position = (double)n->rectangle.y + n->split_ratio * (double)n->rectangle.height;
		if (rect.height > 0) {
			ratio = (position - (double)rect.y) / (double)rect.height;
		} else {
			ratio = 0.5;
		}
	}

	ratio = MAX(0.0, MIN(1.0, ratio));
	n->split_ratio = ratio;

	if (!n->first_child || n->first_child->vacant) {
		adjust_ratios_bounded(n->second_child, rect, depth + 1);
		return;
	}
	if (!n->second_child || n->second_child->vacant) {
		adjust_ratios_bounded(n->first_child, rect, depth + 1);
		return;
	}

	xcb_rectangle_t first_rect, second_rect;
	unsigned int fence;

	if (n->split_type == TYPE_VERTICAL) {
		fence = (unsigned int)(rect.width * n->split_ratio);
		first_rect = (xcb_rectangle_t){rect.x, rect.y, fence, rect.height};
		second_rect = (xcb_rectangle_t){rect.x + fence, rect.y, rect.width - fence, rect.height};
	} else {
		fence = (unsigned int)(rect.height * n->split_ratio);
		first_rect = (xcb_rectangle_t){rect.x, rect.y, rect.width, fence};
		second_rect = (xcb_rectangle_t){rect.x, rect.y + fence, rect.width, rect.height - fence};
	}

	adjust_ratios_bounded(n->first_child, first_rect, depth + 1);
	adjust_ratios_bounded(n->second_child, second_rect, depth + 1);
}

void adjust_ratios(node_t *n, xcb_rectangle_t rect)
{
	adjust_ratios_bounded(n, rect, 0);
}

void unlink_node(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!d || !n) {
		return;
	}

	node_t *p = n->parent;

	if (!p) {
		d->root = NULL;
		d->focus = NULL;
		put_status(SBSC_MASK_REPORT);
	} else {
		if (d->focus == p || is_descendant(d->focus, n)) {
			d->focus = NULL;
		}

		history_remove(d, p, false);
		cancel_presel(m, d, p);

		if (p->sticky && m) {
			m->sticky_count--;
		}

		node_t *b = brother_tree(n);
		node_t *g = p->parent;

		if (b) {
			b->parent = g;
		}

		if (g) {
			if (is_first_child(p)) {
				g->first_child = b;
			} else {
				g->second_child = b;
			}
		} else {
			d->root = b;
		}

		if (!n->vacant && removal_adjustment && b) {
			if (automatic_scheme == SCHEME_SPIRAL) {
				if (is_first_child(n)) {
					rotate_tree(b, 270);
				} else {
					rotate_tree(b, 90);
				}
			} else if (automatic_scheme == SCHEME_LONGEST_SIDE || !g) {
				if (p->rectangle.width > p->rectangle.height) {
					b->split_type = TYPE_VERTICAL;
				} else {
					b->split_type = TYPE_HORIZONTAL;
				}
			} else if (automatic_scheme == SCHEME_ALTERNATE && g) {
				b->split_type = (g->split_type == TYPE_HORIZONTAL) ? TYPE_VERTICAL : TYPE_HORIZONTAL;
			}
		}

		free(p);
		n->parent = NULL;

		if (b) {
			propagate_flags_upward(m, d, b);
		}
	}
}

static void close_node_bounded(node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	if (n->client) {
		if (n->client->icccm_props.delete_window) {
			send_client_message(n->id, ewmh->WM_PROTOCOLS, WM_DELETE_WINDOW);
		} else {
			xcb_kill_client(dpy, n->id);
		}
	} else {
		close_node_bounded(n->first_child, depth + 1);
		close_node_bounded(n->second_child, depth + 1);
	}
}

void close_node(node_t *n)
{
	close_node_bounded(n, 0);
}

void kill_node(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m || !d || !n) {
		return;
	}

	if (IS_RECEPTACLE(n)) {
		put_status(SBSC_MASK_NODE_REMOVE, "node_remove 0x%08X 0x%08X 0x%08X\n",
		           m->id, d->id, n->id);
		remove_node(m, d, n);
	} else {
		for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
			if (f->client) {
				xcb_kill_client(dpy, f->id);
			}
		}
	}
}

void remove_node(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m || !d || !n) {
		return;
	}

	unlink_node(m, d, n);
	history_remove(d, n, true);
	remove_stack_node(n);
	cancel_presel_in(m, d, n);

	if (m->sticky_count > 0 && d == m->desk) {
		m->sticky_count = SAFE_SUB(m->sticky_count, sticky_count(n));
	}

	clients_count = SAFE_SUB(clients_count, clients_count_in(n));

	if (is_descendant(grabbed_node, n)) {
		grabbed_node = NULL;
	}

	free_node(n);

	if (single_monocle && d->layout != LAYOUT_MONOCLE && tiled_count(d->root, true) <= 1) {
		set_layout(m, d, LAYOUT_MONOCLE, false);
	}

	ewmh_update_client_list(false);
	ewmh_update_client_list(true);

	if (mon && !d->focus) {
		if (d == mon->desk) {
			focus_node(m, d, NULL);
		} else {
			activate_node(m, d, NULL);
		}
	}
}

static void free_node_bounded(node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *first_child = n->first_child;
	node_t *second_child = n->second_child;

	n->first_child = NULL;
	n->second_child = NULL;
	n->parent = NULL;

	if (n->client) {
		secure_memzero(n->client, sizeof(client_t));
		free(n->client);
		n->client = NULL;
	}

	if (n->presel) {
		secure_memzero(n->presel, sizeof(presel_t));
		free(n->presel);
		n->presel = NULL;
	}

	secure_memzero(n, sizeof(node_t));
	free(n);

	free_node_bounded(first_child, depth + 1);
	free_node_bounded(second_child, depth + 1);
}

void free_node(node_t *n)
{
	free_node_bounded(n, 0);
}

bool swap_nodes(monitor_t *m1, desktop_t *d1, node_t *n1, monitor_t *m2, desktop_t *d2, node_t *n2, bool follow)
{
	if (!n1 || !n2 || n1 == n2 || !m1 || !m2 || !d1 || !d2) {
		return false;
	}

	if (is_descendant(n1, n2) || is_descendant(n2, n1)) {
		return false;
	}

	if (d1 != d2) {
		unsigned int n1_sticky = sticky_count(n1);
		unsigned int n2_sticky = sticky_count(n2);

		if ((m1->sticky_count > 0 && n1_sticky > 0) ||
		    (m2->sticky_count > 0 && n2_sticky > 0)) {
			return false;
		}
	}

	if (!find_by_id_in(d1->root, n1->id) || !find_by_id_in(d2->root, n2->id)) {
		return false;
	}

	put_status(SBSC_MASK_NODE_SWAP, "node_swap 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
	           m1->id, d1->id, n1->id, m2->id, d2->id, n2->id);

	node_t *pn1 = n1->parent;
	node_t *pn2 = n2->parent;

	if (pn1 == n2 || pn2 == n1) {
		return false;
	}

	bool n1_first_child = is_first_child(n1);
	bool n2_first_child = is_first_child(n2);
	bool n1_held_focus = is_descendant(d1->focus, n1);
	bool n2_held_focus = is_descendant(d2->focus, n2);

	node_t *last_d1_focus = d1->focus;
	node_t *last_d2_focus = d2->focus;
	uint32_t last_d1_focus_id = last_d1_focus ? last_d1_focus->id : 0;
	uint32_t last_d2_focus_id = last_d2_focus ? last_d2_focus->id : 0;

	if (n1->presel && n1->presel->feedback != XCB_NONE) {
		xcb_destroy_window(dpy, n1->presel->feedback);
		n1->presel->feedback = XCB_NONE;
	}
	if (n2->presel && n2->presel->feedback != XCB_NONE) {
		xcb_destroy_window(dpy, n2->presel->feedback);
		n2->presel->feedback = XCB_NONE;
	}

	if (pn1) {
		if (n1_first_child) {
			pn1->first_child = n2;
		} else {
			pn1->second_child = n2;
		}
	}

	if (pn2) {
		if (n2_first_child) {
			pn2->first_child = n1;
		} else {
			pn2->second_child = n1;
		}
	}

	n1->parent = pn2;
	n2->parent = pn1;

	propagate_flags_upward(m2, d2, n1);
	propagate_flags_upward(m1, d1, n2);

	if (d1 != d2) {
		if (d1->root == n1) {
			d1->root = n2;
		}
		if (d2->root == n2) {
			d2->root = n1;
		}

		if (n1_held_focus) {
			if (n2_held_focus && last_d2_focus_id != 0) {
				coordinates_t loc;
				if (find_by_id(last_d2_focus_id, &loc) && loc.node == last_d2_focus) {
					d1->focus = last_d2_focus;
				} else {
					d1->focus = n2;
				}
			} else {
				d1->focus = n2;
			}
		}

		if (n2_held_focus) {
			if (n1_held_focus && last_d1_focus_id != 0) {
				coordinates_t loc;
				if (find_by_id(last_d1_focus_id, &loc) && loc.node == last_d1_focus) {
					d2->focus = last_d1_focus;
				} else {
					d2->focus = n1;
				}
			} else {
				d2->focus = n1;
			}
		}

		if (m1 != m2) {
			adapt_geometry(&m2->rectangle, &m1->rectangle, n2);
			adapt_geometry(&m1->rectangle, &m2->rectangle, n1);
		}

		ewmh_set_wm_desktop(n1, d2);
		ewmh_set_wm_desktop(n2, d1);

		history_remove(d1, n1, true);
		history_remove(d2, n2, true);

		bool d1_was_focused = (d1 == mon->desk);
		bool d2_was_focused = (d2 == mon->desk);

		if (m1->desk != d1 && m2->desk == d2) {
			show_node(d2, n1);
			if (!follow || !d2_was_focused || !n2_held_focus) {
				hide_node(d2, n2);
			}
		} else if (m1->desk == d1 && m2->desk != d2) {
			if (!follow || !d1_was_focused || !n1_held_focus) {
				hide_node(d1, n1);
			}
			show_node(d1, n2);
		}

		if (single_monocle) {
			int d1_tiled = tiled_count(d1->root, true);
			int d2_tiled = tiled_count(d2->root, true);

			layout_t l1 = (d1_tiled <= 1) ? LAYOUT_MONOCLE : d1->user_layout;
			layout_t l2 = (d2_tiled <= 1) ? LAYOUT_MONOCLE : d2->user_layout;

			set_layout(m1, d1, l1, false);
			set_layout(m2, d2, l2, false);
		}

		if (n1_held_focus) {
			if (d1_was_focused) {
				if (follow && last_d1_focus_id != 0) {
					coordinates_t loc;
					if (find_by_id(last_d1_focus_id, &loc) && loc.desktop == d2) {
						focus_node(m2, d2, loc.node);
					} else {
						focus_node(m2, d2, d2->focus);
					}
				} else {
					focus_node(m1, d1, d1->focus);
				}
			} else {
				activate_node(m1, d1, d1->focus);
			}
		} else {
			draw_border(n2, is_descendant(n2, d1->focus), (m1 == mon));
		}

		if (n2_held_focus) {
			if (d2_was_focused) {
				if (follow && last_d2_focus_id != 0) {
					coordinates_t loc;
					if (find_by_id(last_d2_focus_id, &loc) && loc.desktop == d1) {
						focus_node(m1, d1, loc.node);
					} else {
						focus_node(m1, d1, d1->focus);
					}
				} else {
					focus_node(m2, d2, d2->focus);
				}
			} else {
				activate_node(m2, d2, d2->focus);
			}
		} else {
			draw_border(n1, is_descendant(n1, d2->focus), (m2 == mon));
		}
	} else {
		if (!n1_held_focus) {
			draw_border(n1, is_descendant(n1, d2->focus), (m2 == mon));
		}
		if (!n2_held_focus) {
			draw_border(n2, is_descendant(n2, d1->focus), (m1 == mon));
		}
	}

	arrange(m1, d1);

	if (d1 != d2) {
		arrange(m2, d2);
	} else {
		if (pointer_follows_focus && (n1_held_focus || n2_held_focus)) {
			if (d1->focus) {
				xcb_rectangle_t rect = get_rectangle(m1, d1, d1->focus);
				if (rect.width > 0 && rect.height > 0 &&
				    rect.x < m1->rectangle.width &&
				    rect.y < m1->rectangle.height) {
					center_pointer(rect);
				}
			}
		}
	}

	return true;
}

bool transfer_node(monitor_t *ms, desktop_t *ds, node_t *ns, monitor_t *md, desktop_t *dd, node_t *nd, bool follow)
{
	if (!ns || ns == nd || is_child(ns, nd) || is_descendant(nd, ns)) {
		return false;
	}

	if (!ms || !ds || !md || !dd) {
		return false;
	}

	unsigned int sc = (ms->sticky_count > 0 && ds == ms->desk) ? sticky_count(ns) : 0;
	if (sticky_still && sc > 0 && dd != md->desk) {
		return false;
	}

	put_status(SBSC_MASK_NODE_TRANSFER, "node_transfer 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
	           ms->id, ds->id, ns->id, md->id, dd->id, nd ? nd->id : 0);

	bool held_focus = is_descendant(ds->focus, ns);
	bool focus_was_child = is_child(ns, ds->focus);
	node_t *last_ds_focus = focus_was_child ? NULL : ds->focus;
	uint32_t last_focus_id = focus_was_child ? 0 : (ds->focus ? ds->focus->id : 0);
	bool ds_was_focused = (ds == mon->desk);

	if (held_focus && ds_was_focused) {
		clear_input_focus();
	}

	unlink_node(ms, ds, ns);

	if (last_ds_focus && last_focus_id != 0) {
		coordinates_t loc;
		if (!find_by_id(last_focus_id, &loc) || loc.node != last_ds_focus) {
			last_ds_focus = NULL;
		}
	}

	insert_node(md, dd, ns, nd);

	if (md != ms) {
		if (!ns->client || monitor_from_client(ns->client) != md) {
			adapt_geometry(&ms->rectangle, &md->rectangle, ns);
		}
		ms->sticky_count = SAFE_SUB(ms->sticky_count, sc);
		md->sticky_count = SAFE_ADD(md->sticky_count, sc, UINT_MAX);
	}

	if (ds != dd) {
		ewmh_set_wm_desktop(ns, dd);
		if (sticky_still) {
			if (ds == ms->desk && dd != md->desk) {
				hide_node(ds, ns);
			} else if (ds != ms->desk && dd == md->desk) {
				show_node(dd, ns);
			}
		}
	}

	history_remove(ds, ns, true);
	stack(dd, ns, false);

	if (ds == dd) {
		if (held_focus) {
			if (ds_was_focused) {
				focus_node(ms, ds, last_ds_focus);
			} else {
				activate_node(ms, ds, last_ds_focus);
			}
		} else {
			draw_border(ns, is_descendant(ns, ds->focus), (ms == mon));
		}
	} else {
		if (single_monocle) {
			if (ds->layout != LAYOUT_MONOCLE && tiled_count(ds->root, true) <= 1) {
				set_layout(ms, ds, LAYOUT_MONOCLE, false);
			}
			if (dd->layout == LAYOUT_MONOCLE && tiled_count(dd->root, true) > 1) {
				set_layout(md, dd, dd->user_layout, false);
			}
		}
		if (held_focus) {
			if (follow) {
				if (ds_was_focused) {
					focus_node(md, dd, last_ds_focus);
				}
				activate_node(ms, ds, ds->focus);
			} else {
				if (ds_was_focused) {
					focus_node(ms, ds, ds->focus);
				} else {
					activate_node(ms, ds, ds->focus);
				}
			}
		}
		if (!held_focus || !follow || !ds_was_focused) {
			if (dd->focus == ns) {
				if (dd == mon->desk) {
					focus_node(md, dd, held_focus ? last_ds_focus : ns);
				} else {
					activate_node(md, dd, held_focus ? last_ds_focus : ns);
				}
			} else {
				draw_border(ns, is_descendant(ns, dd->focus), (md == mon));
			}
		}
	}

	arrange(ms, ds);

	if (ds != dd) {
		arrange(md, dd);
	}

	return true;
}

bool find_closest_node(coordinates_t *ref, coordinates_t *dst, cycle_dir_t dir, node_select_t *sel)
{
	if (!ref || !ref->monitor || !ref->desktop) {
		return false;
	}

	monitor_t *m = ref->monitor;
	desktop_t *d = ref->desktop;
	node_t *n = ref->node;
	n = (dir == CYCLE_PREV ? prev_node(n) : next_node(n));

#define HANDLE_BOUNDARIES(m, d, n)  \
	while (!n) { \
		d = (dir == CYCLE_PREV ? d->prev : d->next); \
		if (!d) { \
			m = (dir == CYCLE_PREV ? m->prev : m->next); \
			if (!m) { \
				m = (dir == CYCLE_PREV ? mon_tail : mon_head); \
			} \
			d = (dir == CYCLE_PREV ? m->desk_tail : m->desk_head); \
		} \
		n = (dir == CYCLE_PREV ? second_extrema(d->root) : first_extrema(d->root)); \
		if (!ref->node && d == ref->desktop) { \
			break; \
		} \
	}

	HANDLE_BOUNDARIES(m, d, n);

	while (n != ref->node) {
		coordinates_t loc = {m, d, n};
		if (node_matches(&loc, ref, sel)) {
			*dst = loc;
			return true;
		}
		n = (dir == CYCLE_PREV ? prev_node(n) : next_node(n));
		HANDLE_BOUNDARIES(m, d, n);
		if (!ref->node && d == ref->desktop) {
			break;
		}
	}
#undef HANDLE_BOUNDARIES
	return false;
}

void circulate_leaves(monitor_t *m, desktop_t *d, node_t *n, circulate_dir_t dir)
{
	if (!m || !d || !n || tiled_count(n, false) < 2) {
		return;
	}

	if (!d->focus || !d->focus->parent) {
		return;
	}

	node_t *p = d->focus->parent;
	bool focus_first_child = is_first_child(d->focus);

	if (dir == CIRCULATE_FORWARD) {
		node_t *e = second_extrema(n);
		while (e && (!e->client || !IS_TILED(e->client))) {
			e = prev_leaf(e, n);
		}
		for (node_t *s = e, *f = prev_tiled_leaf(s, n); f;
		     s = prev_tiled_leaf(f, n), f = prev_tiled_leaf(s, n)) {
			swap_nodes(m, d, f, m, d, s, false);
		}
	} else {
		node_t *e = first_extrema(n);
		while (e && (!e->client || !IS_TILED(e->client))) {
			e = next_leaf(e, n);
		}
		for (node_t *f = e, *s = next_tiled_leaf(f, n); s;
		     f = next_tiled_leaf(s, n), s = next_tiled_leaf(f, n)) {
			swap_nodes(m, d, f, m, d, s, false);
		}
	}

	if (p) {
		node_t *f = focus_first_child ? p->first_child : p->second_child;
		if (is_leaf(f)) {
			if (d == mon->desk) {
				focus_node(m, d, f);
			} else {
				activate_node(m, d, f);
			}
		}
	}
}

void set_vacant(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->vacant == value) {
		return;
	}

	propagate_vacant_downward(m, d, n, value);
	propagate_vacant_upward(m, d, n);
}

void set_vacant_local(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->vacant == value) {
		return;
	}

	n->vacant = value;

	if (value) {
		cancel_presel(m, d, n);
	}
}

static void propagate_vacant_downward_bounded(monitor_t *m, desktop_t *d, node_t *n, bool value, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	set_vacant_local(m, d, n, value);
	propagate_vacant_downward_bounded(m, d, n->first_child, value, depth + 1);
	propagate_vacant_downward_bounded(m, d, n->second_child, value, depth + 1);
}

void propagate_vacant_downward(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	propagate_vacant_downward_bounded(m, d, n, value, 0);
}

static void propagate_vacant_upward_bounded(monitor_t *m, desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *p = n->parent;

	if (p && p->first_child && p->second_child) {
		set_vacant_local(m, d, p, (p->first_child->vacant && p->second_child->vacant));
	}

	propagate_vacant_upward_bounded(m, d, p, depth + 1);
}

void propagate_vacant_upward(monitor_t *m, desktop_t *d, node_t *n)
{
	propagate_vacant_upward_bounded(m, d, n, 0);
}

bool set_layer(monitor_t *m, desktop_t *d, node_t *n, stack_layer_t l)
{
	if (!n || !n->client || n->client->layer == l) {
		return false;
	}

	n->client->last_layer = n->client->layer;
	n->client->layer = l;

	if (l == LAYER_ABOVE) {
		n->client->wm_flags |= WM_FLAG_ABOVE;
		n->client->wm_flags &= ~WM_FLAG_BELOW;
	} else if (l == LAYER_BELOW) {
		n->client->wm_flags |= WM_FLAG_BELOW;
		n->client->wm_flags &= ~WM_FLAG_ABOVE;
	} else {
		n->client->wm_flags &= ~(WM_FLAG_ABOVE | WM_FLAG_BELOW);
	}

	ewmh_wm_state_update(n);

	if (m && d) {
		put_status(SBSC_MASK_NODE_LAYER, "node_layer 0x%08X 0x%08X 0x%08X %s\n",
		           m->id, d->id, n->id, LAYER_STR(l));
	}

	if (d && d->focus == n) {
		neutralize_occluding_windows(m, d, n);
	}

	if (d) {
		stack(d, n, (d->focus == n));
	}

	return true;
}

bool set_state(monitor_t *m, desktop_t *d, node_t *n, client_state_t s)
{
	if (!n || !n->client || n->client->state == s) {
		return false;
	}

	client_t *c = n->client;
	bool was_tiled = IS_TILED(c);

	c->last_state = c->state;
	c->state = s;

	switch (c->last_state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			break;
		case STATE_FLOATING:
			set_floating(m, d, n, false);
			break;
		case STATE_FULLSCREEN:
			set_fullscreen(m, d, n, false);
			break;
	}

	if (m && d) {
		put_status(SBSC_MASK_NODE_STATE, "node_state 0x%08X 0x%08X 0x%08X %s off\n",
		           m->id, d->id, n->id, STATE_STR(c->last_state));
	}

	switch (c->state) {
		case STATE_TILED:
		case STATE_PSEUDO_TILED:
			break;
		case STATE_FLOATING:
			set_floating(m, d, n, true);
			break;
		case STATE_FULLSCREEN:
			set_fullscreen(m, d, n, true);
			break;
	}

	if (m && d) {
		put_status(SBSC_MASK_NODE_STATE, "node_state 0x%08X 0x%08X 0x%08X %s on\n",
		           m->id, d->id, n->id, STATE_STR(c->state));
	}

	if (m && n == m->desk->focus) {
		put_status(SBSC_MASK_REPORT);
	}

	if (single_monocle && was_tiled != IS_TILED(c) && d) {
		if (was_tiled && d->layout != LAYOUT_MONOCLE && tiled_count(d->root, true) <= 1) {
			set_layout(m, d, LAYOUT_MONOCLE, false);
		} else if (!was_tiled && d->layout == LAYOUT_MONOCLE && tiled_count(d->root, true) > 1) {
			set_layout(m, d, d->user_layout, false);
		}
	}

	return true;
}

void set_floating(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n) {
		return;
	}

	cancel_presel(m, d, n);
	if (!n->hidden) {
		set_vacant(m, d, n, value);
	}

	if (!value && d && d->focus == n) {
		neutralize_occluding_windows(m, d, n);
	}

	if (d) {
		stack(d, n, (d->focus == n));
	}
}

void set_fullscreen(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || !n->client) {
		return;
	}

	client_t *c = n->client;

	cancel_presel(m, d, n);
	if (!n->hidden) {
		set_vacant(m, d, n, value);
	}

	if (value) {
		c->wm_flags |= WM_FLAG_FULLSCREEN;
	} else {
		c->wm_flags &= ~WM_FLAG_FULLSCREEN;
		if (d && d->focus == n) {
			neutralize_occluding_windows(m, d, n);
		}
	}

	ewmh_wm_state_update(n);
	if (d) {
		stack(d, n, (d->focus == n));
	}
}

void neutralize_occluding_windows(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!m || !d || !n) {
		return;
	}

	bool changed = false;
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		for (node_t *a = first_extrema(d->root); a; a = next_leaf(a, d->root)) {
			if (a != f && a->client && f->client &&
			    IS_FULLSCREEN(a->client) && stack_cmp(f->client, a->client) < 0) {
				set_state(m, d, a, a->client->last_state);
				changed = true;
			}
		}
	}
	if (changed) {
		arrange(m, d);
	}
}

static void rebuild_constraints_from_leaves_bounded(node_t *n, int depth)
{
	if (!n || is_leaf(n) || depth > MAX_TREE_DEPTH) {
		return;
	}
	rebuild_constraints_from_leaves_bounded(n->first_child, depth + 1);
	rebuild_constraints_from_leaves_bounded(n->second_child, depth + 1);
	update_constraints(n);
}

void rebuild_constraints_from_leaves(node_t *n)
{
	rebuild_constraints_from_leaves_bounded(n, 0);
}

static void rebuild_constraints_towards_root_bounded(node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *p = n->parent;

	if (p) {
		update_constraints(p);
	}

	rebuild_constraints_towards_root_bounded(p, depth + 1);
}

void rebuild_constraints_towards_root(node_t *n)
{
	rebuild_constraints_towards_root_bounded(n, 0);
}

void update_constraints(node_t *n)
{
	if (!n || is_leaf(n) || !n->first_child || !n->second_child) {
		return;
	}

	if (n->split_type == TYPE_VERTICAL) {
		uint16_t min_w1 = n->first_child->constraints.min_width;
		uint16_t min_w2 = n->second_child->constraints.min_width;

		n->constraints.min_width = SAFE_ADD(min_w1, min_w2, UINT16_MAX);
		n->constraints.min_height = MAX(n->first_child->constraints.min_height,
		                               n->second_child->constraints.min_height);
	} else {
		n->constraints.min_width = MAX(n->first_child->constraints.min_width,
		                              n->second_child->constraints.min_width);

		uint16_t min_h1 = n->first_child->constraints.min_height;
		uint16_t min_h2 = n->second_child->constraints.min_height;

		n->constraints.min_height = SAFE_ADD(min_h1, min_h2, UINT16_MAX);
	}
}

static void propagate_flags_upward_bounded(monitor_t *m, desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *p = n->parent;

	if (p && p->first_child && p->second_child) {
		set_vacant_local(m, d, p, (p->first_child->vacant && p->second_child->vacant));
		set_hidden_local(m, d, p, (p->first_child->hidden && p->second_child->hidden));
		update_constraints(p);
	}

	propagate_flags_upward_bounded(m, d, p, depth + 1);
}

void propagate_flags_upward(monitor_t *m, desktop_t *d, node_t *n)
{
	propagate_flags_upward_bounded(m, d, n, 0);
}

void set_hidden(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->hidden == value) {
		return;
	}

	bool held_focus = d && is_descendant(d->focus, n);

	propagate_hidden_downward(m, d, n, value);
	propagate_hidden_upward(m, d, n);

	if (m && d) {
		put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X hidden %s\n",
		           m->id, d->id, n->id, ON_OFF_STR(value));
	}

	if (d && (held_focus || !d->focus)) {
		if (d->focus) {
			d->focus = NULL;
			draw_border(n, false, (mon == m));
		}
		if (d == mon->desk) {
			focus_node(m, d, d->focus);
		} else {
			activate_node(m, d, d->focus);
		}
	}

	if (single_monocle && d) {
		if (value && d->layout != LAYOUT_MONOCLE && tiled_count(d->root, true) <= 1) {
			set_layout(m, d, LAYOUT_MONOCLE, false);
		} else if (!value && d->layout == LAYOUT_MONOCLE && tiled_count(d->root, true) > 1) {
			set_layout(m, d, d->user_layout, false);
		}
	}
}

void set_hidden_local(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->hidden == value) {
		return;
	}

	n->hidden = value;

	if (n->client) {
		if (n->client->shown) {
			window_set_visibility(n->id, !value);
		}

		if (IS_TILED(n->client)) {
			set_vacant(m, d, n, value);
		}

		if (value) {
			n->client->wm_flags |= WM_FLAG_HIDDEN;
		} else {
			n->client->wm_flags &= ~WM_FLAG_HIDDEN;
		}

		ewmh_wm_state_update(n);
	}
}

static void propagate_hidden_downward_bounded(monitor_t *m, desktop_t *d, node_t *n, bool value, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	set_hidden_local(m, d, n, value);
	propagate_hidden_downward_bounded(m, d, n->first_child, value, depth + 1);
	propagate_hidden_downward_bounded(m, d, n->second_child, value, depth + 1);
}

void propagate_hidden_downward(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	propagate_hidden_downward_bounded(m, d, n, value, 0);
}

static void propagate_hidden_upward_bounded(monitor_t *m, desktop_t *d, node_t *n, int depth)
{
	if (!n || depth > MAX_TREE_DEPTH) {
		return;
	}

	node_t *p = n->parent;

	if (p && p->first_child && p->second_child) {
		set_hidden_local(m, d, p, p->first_child->hidden && p->second_child->hidden);
	}

	propagate_hidden_upward_bounded(m, d, p, depth + 1);
}

void propagate_hidden_upward(monitor_t *m, desktop_t *d, node_t *n)
{
	propagate_hidden_upward_bounded(m, d, n, 0);
}

void set_sticky(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->sticky == value || !m || !d) {
		return;
	}

	if (d != m->desk) {
		transfer_node(m, d, n, m, m->desk, m->desk->focus, false);
	}

	n->sticky = value;

	if (value) {
		m->sticky_count++;
	} else {
		m->sticky_count = SAFE_SUB(m->sticky_count, 1);
	}

	if (n->client) {
		if (value) {
			n->client->wm_flags |= WM_FLAG_STICKY;
		} else {
			n->client->wm_flags &= ~WM_FLAG_STICKY;
		}
		ewmh_wm_state_update(n);
	}

	put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X sticky %s\n",
	           m->id, d->id, n->id, ON_OFF_STR(value));

	if (n == m->desk->focus) {
		put_status(SBSC_MASK_REPORT);
	}
}

void set_private(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->private == value) {
		return;
	}

	n->private = value;

	if (m && d) {
		put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X private %s\n",
		           m->id, d->id, n->id, ON_OFF_STR(value));
	}

	if (m && n == m->desk->focus) {
		put_status(SBSC_MASK_REPORT);
	}
}

void set_locked(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->locked == value) {
		return;
	}

	n->locked = value;

	if (m && d) {
		put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X locked %s\n",
		           m->id, d->id, n->id, ON_OFF_STR(value));
	}

	if (m && n == m->desk->focus) {
		put_status(SBSC_MASK_REPORT);
	}
}

void set_marked(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || n->marked == value) {
		return;
	}

	n->marked = value;

	if (m && d) {
		put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X marked %s\n",
		           m->id, d->id, n->id, ON_OFF_STR(value));
	}

	if (m && n == m->desk->focus) {
		put_status(SBSC_MASK_REPORT);
	}
}

void set_urgent(monitor_t *m, desktop_t *d, node_t *n, bool value)
{
	if (!n || !n->client) {
		return;
	}

	if (value && mon && mon->desk && mon->desk->focus == n) {
		return;
	}

	n->client->urgent = value;

	if (value) {
		n->client->wm_flags |= WM_FLAG_DEMANDS_ATTENTION;
	} else {
		n->client->wm_flags &= ~WM_FLAG_DEMANDS_ATTENTION;
	}

	ewmh_wm_state_update(n);

	if (m && d) {
		put_status(SBSC_MASK_NODE_FLAG, "node_flag 0x%08X 0x%08X 0x%08X urgent %s\n",
		           m->id, d->id, n->id, ON_OFF_STR(value));
	}

	put_status(SBSC_MASK_REPORT);
}

xcb_rectangle_t get_rectangle(monitor_t *m, desktop_t *d, node_t *n)
{
	if (!n) {
		return m ? m->rectangle : (xcb_rectangle_t){0, 0, 0, 0};
	}

	client_t *c = n->client;
	if (c) {
		if (IS_FLOATING(c)) {
			return c->floating_rectangle;
		} else {
			return c->tiled_rectangle;
		}
	} else {
		int wg = (!d ? 0 : (gapless_monocle && d->layout == LAYOUT_MONOCLE ? 0 : d->window_gap));
		xcb_rectangle_t rect = n->rectangle;
		rect.width = SAFE_SUB(rect.width, wg);
		rect.height = SAFE_SUB(rect.height, wg);
		return rect;
	}
}

void listen_enter_notify(node_t *n, bool enable)
{
	uint32_t mask = CLIENT_EVENT_MASK | (enable ? XCB_EVENT_MASK_ENTER_WINDOW : 0);
	for (node_t *f = first_extrema(n); f; f = next_leaf(f, n)) {
		if (!f->client) {
			continue;
		}
		xcb_change_window_attributes(dpy, f->id, XCB_CW_EVENT_MASK, &mask);
		if (f->presel) {
			xcb_change_window_attributes(dpy, f->presel->feedback, XCB_CW_EVENT_MASK, &mask);
		}
	}
}

static void regenerate_ids_in_bounded(node_t *n, int depth)
{
	if (!n || n->client || depth > MAX_TREE_DEPTH) {
		return;
	}
	n->id = xcb_generate_id(dpy);
	regenerate_ids_in_bounded(n->first_child, depth + 1);
	regenerate_ids_in_bounded(n->second_child, depth + 1);
}

void regenerate_ids_in(node_t *n)
{
	regenerate_ids_in_bounded(n, 0);
}

#define DEF_FLAG_COUNT(flag) \
	static unsigned int flag##_count_bounded(node_t *n, int depth) \
	{ \
		if (!n || depth > MAX_TREE_DEPTH) { \
			return 0; \
		} \
		return ((n->flag ? 1 : 0) + \
		        flag##_count_bounded(n->first_child, depth + 1) + \
		        flag##_count_bounded(n->second_child, depth + 1)); \
	} \
	unsigned int flag##_count(node_t *n) \
	{ \
		return flag##_count_bounded(n, 0); \
	}
	DEF_FLAG_COUNT(sticky)
	DEF_FLAG_COUNT(private)
	DEF_FLAG_COUNT(locked)
#undef DEF_FLAG_COUNT
