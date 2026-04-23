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

void ewmh_update_number_of_desktops(void) {}
void ewmh_update_current_desktop(void) {}
void ewmh_update_desktop_names(void) {}
void ewmh_update_desktop_viewport(void) {}
void ewmh_set_wm_desktop(node_t *n, desktop_t *d) { (void)n; (void)d; }
void ewmh_update_wm_desktops(void) {}
void ewmh_update_client_list(bool stacking) { (void)stacking; }
void ewmh_wm_state_update(node_t *n) { (void)n; }
void ewmh_set_supporting(bspwm_wid_t win) { (void)win; }
bool ewmh_handle_struts(bspwm_wid_t win) { (void)win; return false; }

/* ---- Border color ---- */

uint32_t get_border_color(bool focused_node, bool focused_monitor)
{
	if (focused_monitor && focused_node)
		return backend_get_color_pixel(focused_border_color);
	else if (focused_monitor)
		return backend_get_color_pixel(active_border_color);
	else
		return backend_get_color_pixel(normal_border_color);
}

void draw_border(node_t *n, bool focused_node, bool focused_monitor)
{
	if (!n || !n->client) return;
	uint32_t color = get_border_color(focused_node, focused_monitor);
	window_draw_border(n->id, color);
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

bool manage_window(bspwm_wid_t win, rule_consequence_t *csq, int fd)
{
	if (!csq) return false;

	parse_rule_consequence(fd, csq);

	if (!csq->manage) {
		free(csq->rect);
		free(csq->layer);
		free(csq->state);
		free(csq->split_dir);
		return false;
	}

	monitor_t *m = mon;
	desktop_t *d = m ? m->desk : NULL;
	node_t *f = d ? d->focus : NULL;

	if (csq->monitor_desc[0] != '\0') {
		coordinates_t mloc;
		if (monitor_from_desc(csq->monitor_desc, &(coordinates_t){m, d, f}, &mloc) == SELECTOR_OK) {
			m = mloc.monitor;
			d = m->desk;
			f = d ? d->focus : NULL;
		}
	}
	if (csq->desktop_desc[0] != '\0') {
		coordinates_t dloc;
		if (desktop_from_desc(csq->desktop_desc, &(coordinates_t){m, d, f}, &dloc) == SELECTOR_OK) {
			m = dloc.monitor;
			d = dloc.desktop;
			f = d ? d->focus : NULL;
		}
	}

	if (!m || !d) {
		free(csq->rect);
		free(csq->layer);
		free(csq->state);
		free(csq->split_dir);
		return false;
	}

	node_t *n = make_node(win);
	client_t *c = make_client();
	n->client = c;

	/* Copy rule consequence data */
	snprintf(c->class_name, sizeof(c->class_name), "%s", csq->class_name);
	snprintf(c->instance_name, sizeof(c->instance_name), "%s", csq->instance_name);
	snprintf(c->name, sizeof(c->name), "%s", csq->name);

	if (csq->state)
		c->state = c->last_state = *csq->state;
	if (csq->layer)
		c->layer = c->last_layer = *csq->layer;

	initialize_client(n);
	initialize_floating_rectangle(n);

	if (csq->rect) {
		c->floating_rectangle = *csq->rect;
	}
	if (csq->center) {
		window_center(m, c);
	}

	/* Insert into tree */
	insert_node(m, d, n, f);

	if (csq->hidden) set_hidden(m, d, n, true);
	if (csq->sticky) set_sticky(m, d, n, true);

	c->border_width = csq->border ? d->border_width : 0;

	/* Apply */
	arrange(m, d);
	stack(d, n, d->focus == n);

	backend_window_set_border_width(win, c->border_width);
	uint32_t bcolor = get_border_color(d->focus == n, mon == m);
	backend_window_set_border_color(win, bcolor);

	backend_window_show(win);
	c->shown = true;

	if (csq->focus && d == m->desk) {
		focus_node(m, d, n);
	}

	ewmh_update_client_list(false);
	ewmh_update_client_list(true);

	put_status(SBSC_MASK_NODE_ADD, "node_add 0x%08X 0x%08X 0x%08X 0x%08X\n",
	           m->id, d->id, f ? f->id : 0, win);

	free(csq->rect);
	free(csq->layer);
	free(csq->state);
	free(csq->split_dir);

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
	}
}

void adopt_orphans(void)
{
	backend_enumerate_windows(schedule_window);
}

bool move_client(coordinates_t *loc, int dx, int dy)
{
	if (!loc || !loc->node || !loc->node->client) return false;
	client_t *c = loc->node->client;
	c->floating_rectangle.x += dx;
	c->floating_rectangle.y += dy;
	bspwm_rect_t r = c->floating_rectangle;
	window_move_resize(loc->node->id, r.x, r.y, r.width, r.height);
	return true;
}

bool resize_client(coordinates_t *loc, resize_handle_t rh, int dx, int dy, bool relative)
{
	(void)rh; (void)relative;
	if (!loc || !loc->node || !loc->node->client) return false;
	client_t *c = loc->node->client;
	c->floating_rectangle.width += dx;
	c->floating_rectangle.height += dy;
	bspwm_rect_t r = c->floating_rectangle;
	window_move_resize(loc->node->id, r.x, r.y, r.width, r.height);
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

/* ---- Snap stubs ---- */
snap_zone_t get_snap_zone(bspwm_point_t pos, monitor_t *m) { (void)pos; (void)m; return SNAP_NONE; }
void apply_snap_zone(coordinates_t *loc, monitor_t *target_monitor, snap_zone_t zone) { (void)loc; (void)target_monitor; (void)zone; }
void show_snap_preview(monitor_t *m, snap_zone_t zone) { (void)m; (void)zone; }
void hide_snap_preview(void) {}
void destroy_snap_preview(void) {}
