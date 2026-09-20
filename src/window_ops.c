/* Backend-agnostic window operation wrappers.
 *
 * These thin functions are used by the core (tree.c, stack.c, etc.)
 * and delegate to the backend_* API. On X11, window.c provides the
 * full implementations; this file provides them for other backends.
 *
 * Only compiled when BACKEND != x11.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "bspwm.h"
#include "desktop.h"
#include "monitor.h"
#include "ewmh.h"
#include "query.h"
#include "rule.h"
#include "settings.h"
#include "geometry.h"
#include "pointer.h"
#include "stack.h"
#include "tree.h"
#include "subscribe.h"
#include "window.h"

/* ---- Globals that window.c normally defines ---- */
bool grabbing = false;
node_t *grabbed_node = NULL;
uint16_t num_lock = 0;
uint16_t caps_lock = 0;
uint16_t scroll_lock = 0;

/* ---- Thin wrapper functions ---- */

void window_move(bspwm_wid_t win, int16_t x, int16_t y)
{
	backend_window_move(win, x, y);
}

void window_resize(bspwm_wid_t win, uint16_t w, uint16_t h)
{
	backend_window_resize(win, w, h);
}

void window_move_resize(bspwm_wid_t win, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	backend_window_move_resize(win, x, y, w, h);
}

void window_show(bspwm_wid_t win)
{
	backend_window_show(win);
}

void window_hide(bspwm_wid_t win)
{
	backend_window_hide(win);
}

void window_set_visibility(bspwm_wid_t win, bool visible)
{
	if (visible)
		backend_window_show(win);
	else
		backend_window_hide(win);
}

void window_border_width(bspwm_wid_t win, uint32_t bw)
{
	backend_window_set_border_width(win, bw);
}

void window_draw_border(bspwm_wid_t win, uint32_t border_color_pxl)
{
	backend_window_set_border_color(win, border_color_pxl);
}

void window_above(bspwm_wid_t w1, bspwm_wid_t w2)
{
	backend_window_stack_above(w1, w2);
}

void window_below(bspwm_wid_t w1, bspwm_wid_t w2)
{
	backend_window_stack_below(w1, w2);
}

void window_lower(bspwm_wid_t win)
{
	backend_window_lower(win);
}

void window_center(monitor_t *m, client_t *c)
{
	if (!m || !c) return;
	c->floating_rectangle.x = m->rectangle.x +
		(m->rectangle.width - c->floating_rectangle.width) / 2;
	c->floating_rectangle.y = m->rectangle.y +
		(m->rectangle.height - c->floating_rectangle.height) / 2;
}

bool window_exists(bspwm_wid_t win)
{
	return backend_window_exists(win);
}

void query_pointer(bspwm_wid_t *win, bspwm_point_t *pt)
{
	backend_query_pointer(win, pt);
}

void center_pointer(bspwm_rect_t r)
{
	backend_warp_pointer(r);
}

void set_input_focus(node_t *n)
{
	if (!n || !n->client) {
		clear_input_focus();
	} else {
		if (n->client->icccm_props.input_hint) {
			backend_set_input_focus(n->id);
		} else if (n->client->icccm_props.take_focus) {
			backend_send_take_focus(n->id, &n->client->icccm_props);
		}
	}
}

void clear_input_focus(void)
{
	backend_clear_input_focus();
}

void update_input_focus(void)
{
	if (mon && mon->desk && mon->desk->focus) {
		set_input_focus(mon->desk->focus);
	}
}

/* ---- Motion recorder (no-ops on non-X11) ---- */

void update_motion_recorder(void) {}
void enable_motion_recorder(bspwm_wid_t win) { (void)win; }
void disable_motion_recorder(void) {}

/* ---- Pointer / grab stubs ---- */

void pointer_init(void) {}
void window_grab_buttons(bspwm_wid_t win) { (void)win; }
void grab_buttons(void) {}
void ungrab_buttons(void) {}

/* ---- EWMH wrappers (delegated to backend, mostly no-ops on non-X11) ---- */

void ewmh_update_active_window(void)
{
	bspwm_wid_t win = (mon && mon->desk && mon->desk->focus) ? mon->desk->focus->id : BSPWM_WID_NONE;
	backend_ewmh_update_active_window(win);
}

void ewmh_update_number_of_desktops(void) { backend_workspaces_update(); }
void ewmh_update_current_desktop(void) { backend_workspaces_update(); }
void ewmh_update_desktop_names(void) { backend_workspaces_update(); }
void ewmh_update_desktop_viewport(void) {}
void ewmh_set_wm_desktop(node_t *n, desktop_t *d) { (void)n; (void)d; }
void ewmh_update_wm_desktops(void) {}
void ewmh_update_client_list(bool stacking) { (void)stacking; }
void ewmh_update_client_lists(void) {}
void ewmh_wm_state_update(node_t *n) { (void)n; }
void ewmh_set_supporting(bspwm_wid_t win) { (void)win; }
bool ewmh_handle_struts(bspwm_wid_t win) { (void)win; return false; }

/* ---- Border color ---- */

uint32_t get_border_color(bool focused_node, bool focused_monitor)
{
	if (focused_monitor && focused_node)
		return backend_get_color_pixel(focused_border_color);
	else if (focused_node)
		return backend_get_color_pixel(active_border_color);
	else
		return backend_get_color_pixel(normal_border_color);
}

void draw_border(node_t *n, bool focused_node, bool focused_monitor)
{
	if (!n) return;
	uint32_t color = get_border_color(focused_node, focused_monitor);
	/* The core passes internal nodes (set_hidden, transfer_node, focus of
	 * a subtree); paint every leaf like the X11 backend does. */
	for (node_t *f = first_extrema(n); f != NULL; f = next_leaf(f, n)) {
		if (f->client) {
			window_draw_border(f->id, color);
		}
	}
}

void update_colors_in(node_t *n, desktop_t *d, monitor_t *m)
{
	if (!n) return;
	if (n->client) {
		bool focused_node = (d && d->focus == n);
		bool focused_monitor = (mon == m);
		draw_border(n, focused_node, focused_monitor);
	}
	update_colors_in(n->first_child, d, m);
	update_colors_in(n->second_child, d, m);
}

/* ---- Presel feedback stubs ---- */

void draw_presel_feedback(monitor_t *m, desktop_t *d, node_t *n) { (void)m; (void)d; (void)n; }
void refresh_presel_feedbacks(monitor_t *m, desktop_t *d, node_t *n) { (void)m; (void)d; (void)n; }
void show_presel_feedbacks(monitor_t *m, desktop_t *d, node_t *n) { (void)m; (void)d; (void)n; }
void hide_presel_feedbacks(monitor_t *m, desktop_t *d, node_t *n) { (void)m; (void)d; (void)n; }

/* ---- Window rectangle ---- */

bspwm_rect_t get_window_rectangle(node_t *n)
{
	if (!n || !n->client) return (bspwm_rect_t){0, 0, 0, 0};
	/* apply_layout compares this against the wanted rectangle and only
	 * moves the window when they differ, so it has to be the real current
	 * geometry, not the desired one (or state changes never move anything). */
	bspwm_rect_t r;
	if (backend_window_get_geometry(n->id, &r)) {
		return r;
	}
	if (IS_FLOATING(n->client))
		return n->client->floating_rectangle;
	return n->client->tiled_rectangle;
}

void initialize_floating_rectangle(node_t *n)
{
	if (!n || !n->client) return;
	bspwm_rect_t geo;
	if (backend_window_get_geometry(n->id, &geo)) {
		n->client->floating_rectangle = geo;
	}
}

/* ---- Size hints ---- */

void apply_size_hints(client_t *c, uint16_t *width, uint16_t *height)
{
	if (!c || !width || !height) return;
	if (!SHOULD_HONOR_SIZE_HINTS(c->honor_size_hints, c->state)) return;

	bspwm_size_hints_t *sh = &c->size_hints;
	if (sh->flags & BSP_SIZE_HINT_P_MIN_SIZE) {
		if (*width < (uint16_t)sh->min_width) *width = sh->min_width;
		if (*height < (uint16_t)sh->min_height) *height = sh->min_height;
	}
	if (sh->flags & BSP_SIZE_HINT_P_MAX_SIZE) {
		if (sh->max_width > 0 && *width > (uint16_t)sh->max_width) *width = sh->max_width;
		if (sh->max_height > 0 && *height > (uint16_t)sh->max_height) *height = sh->max_height;
	}
}

/* ---- Window management (core logic) ---- */

void schedule_window(bspwm_wid_t win)
{
	coordinates_t loc;
	if (backend_is_override_redirect(win) || locate_window(win, &loc))
		return;

	for (pending_rule_t *pr = pending_rule_head; pr != NULL; pr = pr->next) {
		if (pr->win == win)
			return;
	}

	rule_consequence_t *csq = make_rule_consequence();
	apply_rules(win, csq);
	if (!schedule_rules(win, csq)) {
		manage_window(win, csq, -1);
		free(csq);
	}
}

static void free_consequence(rule_consequence_t *csq)
{
	free(csq->rect); csq->rect = NULL;
	free(csq->layer); csq->layer = NULL;
	free(csq->state); csq->state = NULL;
	free(csq->split_dir); csq->split_dir = NULL;
}

/* Mirrors src/window.c's manage_window minus the X11-only pieces (event
 * masks, button grabs, WM_STATE). Everything that decides where a window
 * goes and what state it starts in must behave the same on both backends. */
bool manage_window(bspwm_wid_t win, rule_consequence_t *csq, int fd)
{
	if (!csq) return false;

	parse_rule_consequence(fd, csq);

	if (!csq->manage) {
		free_consequence(csq);
		backend_window_show(win);
		return false;
	}

	monitor_t *m = mon;
	desktop_t *d = m ? m->desk : NULL;
	node_t *f = d ? d->focus : NULL;

	if (csq->node_desc[0] != '\0') {
		coordinates_t ref = {m, d, f};
		coordinates_t trg = {NULL, NULL, NULL};
		if (node_from_desc(csq->node_desc, &ref, &trg) == SELECTOR_OK) {
			m = trg.monitor;
			d = trg.desktop;
			f = trg.node;
		}
	} else if (csq->desktop_desc[0] != '\0') {
		coordinates_t ref = {m, d, NULL};
		coordinates_t trg = {NULL, NULL, NULL};
		if (desktop_from_desc(csq->desktop_desc, &ref, &trg) == SELECTOR_OK) {
			m = trg.monitor;
			d = trg.desktop;
			f = trg.desktop->focus;
		}
	} else if (csq->monitor_desc[0] != '\0') {
		coordinates_t ref = {m, NULL, NULL};
		coordinates_t trg = {NULL, NULL, NULL};
		if (monitor_from_desc(csq->monitor_desc, &ref, &trg) == SELECTOR_OK) {
			m = trg.monitor;
			d = trg.monitor->desk;
			f = trg.monitor->desk ? trg.monitor->desk->focus : NULL;
		}
	}

	if (csq->sticky && mon && mon->desk) {
		m = mon;
		d = mon->desk;
		f = mon->desk->focus;
	}

	if (!m || !d) {
		free_consequence(csq);
		return false;
	}

	if (csq->split_dir != NULL && f != NULL) {
		presel_dir(m, d, f, *csq->split_dir);
	}
	if (csq->split_ratio != 0 && f != NULL) {
		presel_ratio(m, d, f, csq->split_ratio);
	}

	node_t *n = make_node(win);
	if (n == NULL) {
		free_consequence(csq);
		return false;
	}
	client_t *c = make_client();
	if (c == NULL) {
		free_node(n);
		free_consequence(csq);
		return false;
	}
	c->border_width = csq->border ? d->border_width : 0;
	n->client = c;
	initialize_client(n);

	if (csq->rect != NULL) {
		c->floating_rectangle = *csq->rect;
	} else {
		initialize_floating_rectangle(n);
		/* A client that comes up at 0,0 has no position of its own;
		 * every transient/dialog otherwise lands in the top-left corner. */
		if (c->floating_rectangle.x == 0 && c->floating_rectangle.y == 0) {
			csq->center = true;
		}
	}

	monitor_t *mm = monitor_from_client(c);
	if (mm == NULL) mm = m;
	embrace_client(mm, c);
	adapt_geometry(&mm->rectangle, &m->rectangle, n);

	if (csq->center) {
		window_center(m, c);
	}

	snprintf(c->class_name, sizeof(c->class_name), "%s", csq->class_name);
	snprintf(c->instance_name, sizeof(c->instance_name), "%s", csq->instance_name);
	snprintf(c->name, sizeof(c->name), "%s", csq->name);

	/* A window that starts floating, fullscreen or hidden must not take a
	 * tile: the split is decided at insert time. */
	if ((csq->state != NULL && (*(csq->state) == STATE_FLOATING || *(csq->state) == STATE_FULLSCREEN)) || csq->hidden) {
		n->vacant = true;
	}

	f = insert_node(m, d, n, f);
	clients_count++;
	if (single_monocle && d->layout == LAYOUT_MONOCLE && tiled_count(d->root, true) > 1) {
		set_layout(m, d, d->user_layout, false);
	}

	n->vacant = false;

	put_status(SBSC_MASK_NODE_ADD, "node_add 0x%08X 0x%08X 0x%08X 0x%08X\n",
	           m->id, d->id, f ? f->id : 0, win);

	if (f != NULL && f->client != NULL && csq->state != NULL && *(csq->state) == STATE_FLOATING) {
		c->layer = f->client->layer;
	}
	if (csq->layer != NULL) {
		c->layer = *(csq->layer);
	}
	if (csq->state != NULL) {
		set_state(m, d, n, *(csq->state));
	}
	enforce_layer_invariant(m, d, n);

	if (csq->honor_size_hints != HONOR_SIZE_HINTS_DEFAULT) {
		c->honor_size_hints = csq->honor_size_hints;
	}

	set_hidden(m, d, n, csq->hidden);
	set_sticky(m, d, n, csq->sticky);
	set_private(m, d, n, csq->private);
	set_locked(m, d, n, csq->locked);
	set_marked(m, d, n, csq->marked);

	arrange(m, d);

	backend_window_set_border_width(win, c->border_width);

	/* Visible only if its desktop is the one shown on its monitor. */
	if (d == m->desk) {
		show_node(d, n);
	} else {
		hide_node(d, n);
	}

	ewmh_update_client_lists();
	ewmh_set_wm_desktop(n, d);

	if (!csq->hidden && csq->focus) {
		if ((mon != NULL && d == mon->desk) || csq->follow) {
			focus_node(m, d, n);
		} else {
			activate_node(m, d, n);
		}
	} else {
		stack(d, n, false);
		draw_border(n, false, (m == mon));
	}

	free_consequence(csq);
	return true;
}

void set_window_state(bspwm_wid_t win, bspwm_wm_state_t state)
{
	backend_set_window_state(win, state);
}

void unmanage_window(bspwm_wid_t win)
{
	coordinates_t loc;
	if (locate_window(win, &loc)) {
		put_status(SBSC_MASK_NODE_REMOVE, "node_remove 0x%08X 0x%08X 0x%08X\n",
		           loc.monitor->id, loc.desktop->id, win);
		remove_node(loc.monitor, loc.desktop, loc.node);
		arrange(loc.monitor, loc.desktop);
	} else {
		/* Closed while its external rule was still running: drop the
		 * pending rule, or manage_window later inserts a dead id. */
		for (pending_rule_t *pr = pending_rule_head; pr != NULL; pr = pr->next) {
			if (pr->win == win) {
				remove_pending_rule(pr);
				return;
			}
		}
	}
}

void adopt_orphans(void)
{
	backend_enumerate_windows(schedule_window);
}

bool move_client(coordinates_t *loc, int dx, int dy)
{
	node_t *n = loc->node;
	if (n == NULL || n->client == NULL) {
		return false;
	}

	monitor_t *pm = NULL;

	if (IS_TILED(n->client)) {
		/* Tiled windows only move by pointer drag, where the drop target
		 * decides: swap with the tiled window under the pointer, or move
		 * to the monitor under it. */
		if (!grabbing) {
			return false;
		}
		bspwm_wid_t pwin = BSPWM_WID_NONE;
		backend_query_pointer(&pwin, NULL);
		if (pwin == n->id) {
			return false;
		}
		coordinates_t dst;
		bool is_managed = (pwin != BSPWM_WID_NONE && locate_window(pwin, &dst));
		if (is_managed && dst.monitor == loc->monitor && IS_TILED(dst.node->client)) {
			swap_nodes(loc->monitor, loc->desktop, n, loc->monitor, loc->desktop, dst.node, false);
			return true;
		} else if (is_managed && dst.monitor == loc->monitor) {
			return false;
		} else {
			bspwm_point_t pt = {0, 0};
			backend_query_pointer(NULL, &pt);
			pm = monitor_from_point(pt);
		}
	} else {
		client_t *c = n->client;
		bspwm_rect_t rect = c->floating_rectangle;
		int16_t x = rect.x + dx;
		int16_t y = rect.y + dy;
		window_move_resize(n->id, x, y, rect.width, rect.height);
		c->floating_rectangle.x = x;
		c->floating_rectangle.y = y;
		if (!grabbing) {
			put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
			           loc->monitor->id, loc->desktop->id, loc->node->id, rect.width, rect.height, x, y);
		}
		pm = monitor_from_client(c);
	}

	if (pm == NULL || pm == loc->monitor) {
		return true;
	}

	transfer_node(loc->monitor, loc->desktop, n, pm, pm->desk, pm->desk->focus, true);
	loc->monitor = pm;
	loc->desktop = pm->desk;
	return true;
}

bool resize_client(coordinates_t *loc, resize_handle_t rh, int dx, int dy, bool relative)
{
	node_t *n = loc->node;
	if (n == NULL || n->client == NULL || n->client->state == STATE_FULLSCREEN) {
		return false;
	}
	node_t *horizontal_fence = NULL, *vertical_fence = NULL;
	bspwm_rect_t rect = get_rectangle(NULL, NULL, n);
	uint16_t width = rect.width, height = rect.height;
	int16_t x = rect.x, y = rect.y;
	if (n->client->state == STATE_TILED) {
		/* Resizing a tiled window means moving the split it borders. */
		if (rh & HANDLE_LEFT) {
			vertical_fence = find_fence(n, DIR_WEST);
		} else if (rh & HANDLE_RIGHT) {
			vertical_fence = find_fence(n, DIR_EAST);
		}
		if (rh & HANDLE_TOP) {
			horizontal_fence = find_fence(n, DIR_NORTH);
		} else if (rh & HANDLE_BOTTOM) {
			horizontal_fence = find_fence(n, DIR_SOUTH);
		}
		if (vertical_fence == NULL && horizontal_fence == NULL) {
			return false;
		}
		if (vertical_fence != NULL) {
			double sr;
			if (relative) {
				sr = vertical_fence->split_ratio + (double) dx / (double) vertical_fence->rectangle.width;
			} else {
				sr = (double) (dx - vertical_fence->rectangle.x) / (double) vertical_fence->rectangle.width;
			}
			sr = MAX(0, sr);
			sr = MIN(1, sr);
			vertical_fence->split_ratio = sr;
			adjust_ratios(vertical_fence, vertical_fence->rectangle);
		}
		if (horizontal_fence != NULL) {
			double sr;
			if (relative) {
				sr = horizontal_fence->split_ratio + (double) dy / (double) horizontal_fence->rectangle.height;
			} else {
				sr = (double) (dy - horizontal_fence->rectangle.y) / (double) horizontal_fence->rectangle.height;
			}
			sr = MAX(0, sr);
			sr = MIN(1, sr);
			horizontal_fence->split_ratio = sr;
			adjust_ratios(horizontal_fence, horizontal_fence->rectangle);
		}
		arrange(loc->monitor, loc->desktop);
	} else {
		int w = width, h = height;
		if (relative) {
			w += dx * (rh & HANDLE_LEFT ? -1 : (rh & HANDLE_RIGHT ? 1 : 0));
			h += dy * (rh & HANDLE_TOP ? -1 : (rh & HANDLE_BOTTOM ? 1 : 0));
		} else {
			if (rh & HANDLE_LEFT) {
				w = x + width - dx;
			} else if (rh & HANDLE_RIGHT) {
				w = dx - x;
			}
			if (rh & HANDLE_TOP) {
				h = y + height - dy;
			} else if (rh & HANDLE_BOTTOM) {
				h = dy - y;
			}
		}
		width = MIN(MAX(1, w), UINT16_MAX);
		height = MIN(MAX(1, h), UINT16_MAX);
		apply_size_hints(n->client, &width, &height);
		if (rh & HANDLE_LEFT) {
			x += rect.width - width;
		}
		if (rh & HANDLE_TOP) {
			y += rect.height - height;
		}
		n->client->floating_rectangle = (bspwm_rect_t) {x, y, width, height};
		if (n->client->state == STATE_FLOATING) {
			window_move_resize(n->id, x, y, width, height);
			if (!grabbing) {
				put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
				           loc->monitor->id, loc->desktop->id, loc->node->id, width, height, x, y);
			}
		} else {
			arrange(loc->monitor, loc->desktop);
		}
	}
	return true;
}

void update_colors(void)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			update_colors_in(d->root, d, m);
		}
	}
}

bool is_presel_window(bspwm_wid_t win)
{
	(void)win;
	return false;
}

void initialize_presel_feedback(node_t *n)
{
	if (!n || !n->presel) return;
	if (n->presel->feedback == BSPWM_WID_NONE) {
		n->presel->feedback = backend_create_presel_feedback(
			backend_get_color_pixel(presel_feedback_color));
	}
}

/* ---- Event handling stub for non-X11 ---- */

void handle_event(void *evt) { (void)evt; }

/* Keybind grabs — no-op on Wayland (handled in compositor keyboard handler) */
void backend_grab_keys(void) { }
void backend_ungrab_keys(void) { }

/* ---- Geometry cache stubs ---- */

bool get_cached_geometry(bspwm_wid_t win, bspwm_rect_t *geometry)
{
	(void)win; (void)geometry;
	return false;
}

void cache_geometry(bspwm_wid_t win, bspwm_rect_t geometry)
{
	(void)win; (void)geometry;
}

void invalidate_geometry_cache(bspwm_wid_t win)
{
	(void)win;
}

/* ---- Pointer tracking stubs ---- */

bool grab_pointer(pointer_action_t pac) { (void)pac; return false; }
void track_pointer(coordinates_t loc, pointer_action_t pac, bspwm_point_t pos) { (void)loc; (void)pac; (void)pos; }
int16_t modfield_from_keysym(uint32_t keysym) { (void)keysym; return 0; }
resize_handle_t get_handle(node_t *n, bspwm_point_t pos, pointer_action_t pac) { (void)n; (void)pos; (void)pac; return HANDLE_RIGHT; }
void window_grab_button(bspwm_wid_t win, uint8_t button, uint16_t modifier) { (void)win; (void)button; (void)modifier; }

/* ---- Snap stubs ----
 * get_snap_zone / apply_snap_zone are backend-agnostic and live in snap.c;
 * only the X11 drag-preview overlay is stubbed here. */
void show_snap_preview(monitor_t *m, snap_zone_t zone) { (void)m; (void)zone; }
void hide_snap_preview(void) {}
void destroy_snap_preview(void) {}
