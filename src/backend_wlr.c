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

/*
 * Wayland/wlroots backend implementation.
 *
 * bspwm becomes a full Wayland compositor via wlroots. The scene graph API
 * handles rendering and damage tracking. xdg-shell provides window management,
 * and wlr_cursor + wlr_seat handle input routing.
 *
 * Build with: make BACKEND=wlroots
 */

#define WLR_USE_UNSTABLE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "backend.h"
#include "bspwm.h"
#include "monitor.h"
#include "window.h"
#include "tree.h"
#include "query.h"
#include "keybind.h"

/* ------------------------------------------------------------------ */
/*  Compositor state                                                  */
/* ------------------------------------------------------------------ */

struct bspwm_wlr_toplevel {
	bspwm_wid_t id;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;   /* container tree (holds borders + surface) */
	struct wlr_scene_tree *surface_tree; /* the xdg surface itself, offset by border */
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;

	/* Border rects: top, bottom, left, right */
	struct wlr_scene_rect *border[4];
	uint32_t border_width;
	float border_color[4];

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;

	struct wl_list link; /* wlr_server.toplevels */
};

struct bspwm_wlr_output {
	bspwm_output_id_t id;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_list link; /* wlr_server.outputs */
};

struct bspwm_wlr_keyboard {
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	struct wl_list link; /* wlr_server.keyboards */
};

struct bspwm_wlr_xwayland_surface {
	bspwm_wid_t id;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *scene_tree;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener destroy;
	struct wl_listener request_configure;

	struct wl_list link; /* xwayland_surfaces list */
};

struct bspwm_wlr_layer_surface {
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_list link; /* layer_surfaces list */
};

enum bspwm_cursor_mode {
	BSPWM_CURSOR_PASSTHROUGH,
	BSPWM_CURSOR_MOVE,
	BSPWM_CURSOR_RESIZE,
};

static struct {
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
	struct wl_listener new_decoration;

	/* Foreign toplevel management (for bars) */
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;

	/* Idle inhibit (prevent screensaver during video) */
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wlr_idle_notifier_v1 *idle_notifier;

	/* XDG activation (urgency / focus requests) */
	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener xdg_activation_request;

	/* Layer shell */
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	/* XWayland */
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_new_surface;
	struct wl_list xwayland_surfaces;

	/* Scene trees for the 4 layer shell layers */
	struct wlr_scene_tree *layer_trees[4]; /* background, bottom, top, overlay */

	/* Session lock */
	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wlr_session_lock_v1 *active_lock;
	struct wl_listener new_lock;
	struct wl_listener lock_destroy;
	bool locked;

	/* Cursor grab state for drag/resize */
	enum bspwm_cursor_mode cursor_mode;
	struct bspwm_wlr_toplevel *grabbed_tl;
	double grab_x, grab_y;
	double grab_sx, grab_sy;
	int grab_width, grab_height;

	/* ID generation */
	uint32_t next_toplevel_id;
	uint32_t next_output_id;

	/* Wayland socket name */
	const char *socket;
} server;

/* ------------------------------------------------------------------ */
/*  Border helpers                                                    */
/* ------------------------------------------------------------------ */

static void color_u32_to_float(uint32_t pixel, float out[4])
{
	out[0] = ((pixel >> 16) & 0xFF) / 255.0f;
	out[1] = ((pixel >> 8) & 0xFF) / 255.0f;
	out[2] = (pixel & 0xFF) / 255.0f;
	out[3] = ((pixel >> 24) & 0xFF) / 255.0f;
}

static void toplevel_create_borders(struct bspwm_wlr_toplevel *tl)
{
	float color[4] = {0.5f, 0.5f, 0.5f, 1.0f}; /* default grey */
	for (int i = 0; i < 4; i++) {
		tl->border[i] = wlr_scene_rect_create(tl->scene_tree, 0, 0, color);
		/* Place borders below the surface so they don't steal input */
		wlr_scene_node_place_below(&tl->border[i]->node,
			&tl->surface_tree->node);
	}
	tl->border_width = 0;
	memcpy(tl->border_color, color, sizeof(color));
}

/* Reposition and resize the 4 border rects around the surface.
 * Call after border_width or surface size changes. */
static void toplevel_update_borders(struct bspwm_wlr_toplevel *tl)
{
	uint32_t bw = tl->border_width;
	if (bw == 0) {
		for (int i = 0; i < 4; i++) {
			wlr_scene_node_set_enabled(&tl->border[i]->node, false);
		}
		wlr_scene_node_set_position(&tl->surface_tree->node, 0, 0);
		return;
	}

	int w = tl->xdg_toplevel->base->geometry.width;
	int h = tl->xdg_toplevel->base->geometry.height;
	if (w <= 0 || h <= 0) return;

	int total_w = w + 2 * (int)bw;
	int total_h = h + 2 * (int)bw;

	/* Offset the surface by border width */
	wlr_scene_node_set_position(&tl->surface_tree->node, bw, bw);

	/* Top border: full width, border height */
	wlr_scene_rect_set_size(tl->border[0], total_w, bw);
	wlr_scene_node_set_position(&tl->border[0]->node, 0, 0);
	wlr_scene_node_set_enabled(&tl->border[0]->node, true);

	/* Bottom border */
	wlr_scene_rect_set_size(tl->border[1], total_w, bw);
	wlr_scene_node_set_position(&tl->border[1]->node, 0, bw + h);
	wlr_scene_node_set_enabled(&tl->border[1]->node, true);

	/* Left border */
	wlr_scene_rect_set_size(tl->border[2], bw, h);
	wlr_scene_node_set_position(&tl->border[2]->node, 0, bw);
	wlr_scene_node_set_enabled(&tl->border[2]->node, true);

	/* Right border */
	wlr_scene_rect_set_size(tl->border[3], bw, h);
	wlr_scene_node_set_position(&tl->border[3]->node, bw + w, bw);
	wlr_scene_node_set_enabled(&tl->border[3]->node, true);
}

static void toplevel_set_border_color(struct bspwm_wlr_toplevel *tl, uint32_t pixel)
{
	float color[4];
	color_u32_to_float(pixel, color);
	memcpy(tl->border_color, color, sizeof(color));
	for (int i = 0; i < 4; i++) {
		wlr_scene_rect_set_color(tl->border[i], color);
	}
}

/* ------------------------------------------------------------------ */
/*  Presel feedback tracking                                          */
/* ------------------------------------------------------------------ */

struct bspwm_wlr_presel {
	bspwm_wid_t id;
	struct wlr_scene_rect *rect;
	struct wl_list link;
};
static struct wl_list presel_list = {0};
static bool presel_list_initialized = false;

static struct bspwm_wlr_presel *presel_from_id(bspwm_wid_t id)
{
	if (!presel_list_initialized) return NULL;
	struct bspwm_wlr_presel *p;
	wl_list_for_each(p, &presel_list, link) {
		if (p->id == id) return p;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Toplevel lookup by ID                                             */
/* ------------------------------------------------------------------ */

static struct bspwm_wlr_toplevel *toplevel_from_id(bspwm_wid_t id)
{
	struct bspwm_wlr_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->id == id)
			return tl;
	}
	return NULL;
}

static struct bspwm_wlr_output *output_from_id(bspwm_output_id_t id)
{
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (out->id == id)
			return out;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Output (monitor) handling                                         */
/* ------------------------------------------------------------------ */

static void output_frame(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(server.scene, output->wlr_output);
	if (scene_output) {
		wlr_scene_output_commit(scene_output, NULL);
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_output *output = wl_container_of(listener, output, destroy);
	(void)data;

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);

	/* Notify bspwm core of output change */
	update_monitors();
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_output *wlr_output = data;

	/* Configure output with preferred mode */
	wlr_output_init_render(wlr_output, server.allocator, server.renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct bspwm_wlr_output *output = calloc(1, sizeof(*output));
	if (!output) return;

	output->id = ++server.next_output_id;
	output->wlr_output = wlr_output;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server.outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server.output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

	/* Notify bspwm core */
	update_monitors();
}

/* ------------------------------------------------------------------ */
/*  XDG toplevel (window) handling                                    */
/* ------------------------------------------------------------------ */

static void xdg_toplevel_map(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, map);
	(void)data;

	/* Create foreign-toplevel handle for external tools */
	if (server.foreign_toplevel_mgr) {
		tl->foreign_handle = wlr_foreign_toplevel_handle_v1_create(
			server.foreign_toplevel_mgr);
		if (tl->foreign_handle) {
			if (tl->xdg_toplevel->title)
				wlr_foreign_toplevel_handle_v1_set_title(tl->foreign_handle,
					tl->xdg_toplevel->title);
			if (tl->xdg_toplevel->app_id)
				wlr_foreign_toplevel_handle_v1_set_app_id(tl->foreign_handle,
					tl->xdg_toplevel->app_id);
		}
	}

	/* Notify bspwm core: new window mapped */
	schedule_window(tl->id);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, unmap);
	(void)data;

	if (tl->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_destroy(tl->foreign_handle);
		tl->foreign_handle = NULL;
	}

	unmanage_window(tl->id);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, commit);
	(void)data;

	if (tl->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
	}

	/* Update borders when surface geometry changes */
	if (tl->border_width > 0) {
		toplevel_update_borders(tl);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, destroy);
	(void)data;

	wl_list_remove(&tl->map.link);
	wl_list_remove(&tl->unmap.link);
	wl_list_remove(&tl->commit.link);
	wl_list_remove(&tl->destroy.link);
	wl_list_remove(&tl->request_move.link);
	wl_list_remove(&tl->request_resize.link);
	wl_list_remove(&tl->request_maximize.link);
	wl_list_remove(&tl->request_fullscreen.link);
	wl_list_remove(&tl->link);
	free(tl);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	/* bspwm handles all window positioning — ignore client move requests */
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	/* bspwm handles all window sizing — ignore client resize requests */
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, request_maximize);
	(void)data;
	/* Deny maximize — bspwm uses its own state management */
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, request_fullscreen);
	(void)data;

	/* Let bspwm core handle fullscreen via its state machine */
	coordinates_t loc;
	if (locate_window(tl->id, &loc) && loc.monitor && loc.desktop && loc.node) {
		set_state(loc.monitor, loc.desktop, loc.node, STATE_FULLSCREEN);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct bspwm_wlr_toplevel *tl = calloc(1, sizeof(*tl));
	if (!tl) return;

	tl->id = ++server.next_toplevel_id;
	tl->xdg_toplevel = xdg_toplevel;

	/* Container tree holds borders + surface */
	tl->scene_tree = wlr_scene_tree_create(&server.scene->tree);
	tl->scene_tree->node.data = tl;

	/* Surface tree is a child, offset by border width */
	tl->surface_tree = wlr_scene_xdg_surface_create(tl->scene_tree, xdg_toplevel->base);
	xdg_toplevel->base->data = tl->surface_tree;

	toplevel_create_borders(tl);

	tl->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &tl->map);
	tl->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &tl->unmap);
	tl->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &tl->commit);
	tl->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &tl->destroy);

	tl->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &tl->request_move);
	tl->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &tl->request_resize);
	tl->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &tl->request_maximize);
	tl->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &tl->request_fullscreen);

	wl_list_insert(&server.toplevels, &tl->link);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_popup *popup = data;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent) return;

	struct wlr_scene_tree *parent_tree = parent->data;
	popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
}

/* ------------------------------------------------------------------ */
/*  Keyboard handling                                                 */
/* ------------------------------------------------------------------ */

static void keyboard_key(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;

	wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);

	/* When locked, forward all keys to the lock surface only */
	if (server.locked) {
		wlr_seat_keyboard_notify_key(server.seat,
			event->time_msec, event->keycode, event->state);
		return;
	}

	/* Try keybinding interception on key press */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t modifiers = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

		/* Translate keycode to keysym via xkb */
		const xkb_keysym_t *syms;
		int nsyms = xkb_state_key_get_syms(
			kb->wlr_keyboard->xkb_state, event->keycode + 8, &syms);

		/* Map wlr modifiers to our KBMOD flags */
		uint32_t kbmod = 0;
		if (modifiers & WLR_MODIFIER_SHIFT) kbmod |= KBMOD_SHIFT;
		if (modifiers & WLR_MODIFIER_CAPS)  kbmod |= KBMOD_CAPS;
		if (modifiers & WLR_MODIFIER_CTRL)  kbmod |= KBMOD_CTRL;
		if (modifiers & WLR_MODIFIER_ALT)   kbmod |= KBMOD_ALT;
		if (modifiers & WLR_MODIFIER_MOD2)  kbmod |= KBMOD_MOD2;
		if (modifiers & WLR_MODIFIER_MOD3)  kbmod |= KBMOD_MOD3;
		if (modifiers & WLR_MODIFIER_LOGO)  kbmod |= KBMOD_SUPER;
		if (modifiers & WLR_MODIFIER_MOD5)  kbmod |= KBMOD_MOD5;

		for (int i = 0; i < nsyms; i++) {
			const char *cmd = keybind_match(kbmod, syms[i]);
			if (cmd) {
				keybind_exec(cmd);
				return; /* consumed — don't forward to client */
			}
		}
	}

	/* Not a binding — forward to client */
	wlr_seat_keyboard_notify_key(server.seat,
		event->time_msec, event->keycode, event->state);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_keyboard *kb = wl_container_of(listener, kb, modifiers);
	(void)data;
	wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server.seat,
		&kb->wlr_keyboard->modifiers);
}

static void keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_keyboard *kb = wl_container_of(listener, kb, destroy);
	(void)data;
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static void server_new_keyboard(struct wlr_input_device *device)
{
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct bspwm_wlr_keyboard *kb = calloc(1, sizeof(*kb));
	if (!kb) return;

	kb->wlr_keyboard = wlr_keyboard;

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_keyboard->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&device->events.destroy, &kb->destroy);

	wlr_seat_set_keyboard(server.seat, wlr_keyboard);
	wl_list_insert(&server.keyboards, &kb->link);
}

/* ------------------------------------------------------------------ */
/*  Pointer / cursor handling                                         */
/* ------------------------------------------------------------------ */

static void process_cursor_motion(uint32_t time)
{
	/* Handle interactive move/resize */
	if (server.cursor_mode == BSPWM_CURSOR_MOVE && server.grabbed_tl) {
		int new_x = (int)(server.cursor->x - server.grab_x);
		int new_y = (int)(server.cursor->y - server.grab_y);
		wlr_scene_node_set_position(&server.grabbed_tl->scene_tree->node, new_x, new_y);

		/* Update bspwm's floating rectangle */
		coordinates_t loc;
		if (locate_window(server.grabbed_tl->id, &loc) && loc.node && loc.node->client) {
			loc.node->client->floating_rectangle.x = new_x;
			loc.node->client->floating_rectangle.y = new_y;
		}
		return;
	}

	if (server.cursor_mode == BSPWM_CURSOR_RESIZE && server.grabbed_tl) {
		int dx = (int)(server.cursor->x - server.grab_x);
		int dy = (int)(server.cursor->y - server.grab_y);
		int new_w = server.grab_width + dx;
		int new_h = server.grab_height + dy;
		if (new_w < 32) new_w = 32;
		if (new_h < 32) new_h = 32;

		wlr_xdg_toplevel_set_size(server.grabbed_tl->xdg_toplevel, new_w, new_h);

		/* Update bspwm's floating rectangle */
		coordinates_t loc;
		if (locate_window(server.grabbed_tl->id, &loc) && loc.node && loc.node->client) {
			loc.node->client->floating_rectangle.width = new_w;
			loc.node->client->floating_rectangle.height = new_h;
		}
		return;
	}

	double sx, sy;
	struct wlr_seat *seat = server.seat;
	struct wlr_surface *surface = NULL;

	struct wlr_scene_node *node = wlr_scene_node_at(
		&server.scene->tree.node, server.cursor->x, server.cursor->y, &sx, &sy);

	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface) {
			surface = scene_surface->surface;
		}
	}

	if (!surface) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(seat);
		return;
	}

	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(event->time_msec);
}

/* Find the toplevel at cursor position */
static struct bspwm_wlr_toplevel *toplevel_at_cursor(double *sx, double *sy)
{
	double lx = server.cursor->x, ly = server.cursor->y;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server.scene->tree.node, lx, ly, sx, sy);

	while (node) {
		if (node->data) {
			/* Check if it's a toplevel (not a presel or layer surface) */
			struct bspwm_wlr_toplevel *tl = node->data;
			/* Verify it's actually in our toplevel list */
			struct bspwm_wlr_toplevel *check;
			wl_list_for_each(check, &server.toplevels, link) {
				if (check == tl) return tl;
			}
		}
		node = node->parent ? &node->parent->node : NULL;
	}
	return NULL;
}

static void begin_interactive_move(struct bspwm_wlr_toplevel *tl)
{
	server.cursor_mode = BSPWM_CURSOR_MOVE;
	server.grabbed_tl = tl;
	server.grab_x = server.cursor->x - tl->scene_tree->node.x;
	server.grab_y = server.cursor->y - tl->scene_tree->node.y;
}

static void begin_interactive_resize(struct bspwm_wlr_toplevel *tl)
{
	server.cursor_mode = BSPWM_CURSOR_RESIZE;
	server.grabbed_tl = tl;
	server.grab_x = server.cursor->x;
	server.grab_y = server.cursor->y;
	server.grab_sx = tl->scene_tree->node.x;
	server.grab_sy = tl->scene_tree->node.y;
	server.grab_width = tl->xdg_toplevel->base->geometry.width;
	server.grab_height = tl->xdg_toplevel->base->geometry.height;
}

static void cursor_button(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server.cursor_mode != BSPWM_CURSOR_PASSTHROUGH) {
			server.cursor_mode = BSPWM_CURSOR_PASSTHROUGH;
			server.grabbed_tl = NULL;
		}
		wlr_seat_pointer_notify_button(server.seat,
			event->time_msec, event->button, event->state);
		return;
	}

	/* Check for modifier+click to initiate move/resize */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
	bool super_held = (modifiers & WLR_MODIFIER_LOGO);

	if (super_held && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct bspwm_wlr_toplevel *tl = toplevel_at_cursor(&sx, &sy);
		if (tl) {
			/* Focus the window */
			backend_set_input_focus(tl->id);

			/* Check which button for move vs resize */
			if (event->button == BTN_LEFT) {
				/* Check if the window is floating in bspwm */
				coordinates_t loc;
				if (locate_window(tl->id, &loc) && loc.node && loc.node->client &&
				    IS_FLOATING(loc.node->client)) {
					begin_interactive_move(tl);
				}
			} else if (event->button == BTN_RIGHT) {
				coordinates_t loc;
				if (locate_window(tl->id, &loc) && loc.node && loc.node->client &&
				    IS_FLOATING(loc.node->client)) {
					begin_interactive_resize(tl);
				}
			}
			return;
		}
	}

	/* Normal click — focus + pass through */
	double sx, sy;
	struct bspwm_wlr_toplevel *tl = toplevel_at_cursor(&sx, &sy);
	if (tl) {
		backend_set_input_focus(tl->id);

		/* Notify bspwm core of focus change */
		coordinates_t loc;
		if (locate_window(tl->id, &loc) && loc.monitor && loc.desktop) {
			focus_node(loc.monitor, loc.desktop, loc.node);
		}
	}

	wlr_seat_pointer_notify_button(server.seat,
		event->time_msec, event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server.seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	wlr_seat_pointer_notify_frame(server.seat);
}

static void server_new_pointer(struct wlr_input_device *device)
{
	wlr_cursor_attach_input_device(server.cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server.keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server.seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused = server.seat->pointer_state.focused_client;
	if (focused == event->seat_client) {
		wlr_cursor_set_surface(server.cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server.seat, event->source, event->serial);
}

/* ------------------------------------------------------------------ */
/*  Decoration handling (force server-side)                           */
/* ------------------------------------------------------------------ */

static void new_decoration(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	wlr_xdg_toplevel_decoration_v1_set_mode(deco,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ------------------------------------------------------------------ */
/*  Layer shell handling                                              */
/* ------------------------------------------------------------------ */

/* Track all layer surfaces for arrange_layers iteration */
static struct wl_list layer_surfaces;  /* bspwm_wlr_layer_surface.link */
static bool layer_surfaces_initialized = false;

static void arrange_layers(struct bspwm_wlr_output *output)
{
	if (!layer_surfaces_initialized) return;

	struct wlr_box full_area = {0};
	wlr_output_effective_resolution(output->wlr_output,
		&full_area.width, &full_area.height);

	struct wlr_box usable_area = full_area;

	/* Configure all layer surfaces on this output, in layer order */
	for (int layer = 0; layer < 4; layer++) {
		struct bspwm_wlr_layer_surface *ls;
		wl_list_for_each(ls, &layer_surfaces, link) {
			if (ls->layer_surface->output != output->wlr_output)
				continue;
			if ((int)ls->layer_surface->current.layer != layer)
				continue;
			wlr_scene_layer_surface_v1_configure(ls->scene, &full_area, &usable_area);
		}
	}

	/* Update bspwm monitor padding from exclusive zones */
	monitor_t *m = get_monitor_by_output_id(output->id);
	if (m) {
		m->padding.top = usable_area.y;
		m->padding.left = usable_area.x;
		m->padding.right = full_area.width - (usable_area.x + usable_area.width);
		m->padding.bottom = full_area.height - (usable_area.y + usable_area.height);
	}
}

static void layer_surface_commit(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, commit);
	(void)data;

	if (!ls->layer_surface->initialized) return;

	/* Find the output this layer is on */
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (out->wlr_output == ls->layer_surface->output) {
			arrange_layers(out);
			break;
		}
	}
}

static void layer_surface_map(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
}

static void layer_surface_unmap(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
}

static void layer_surface_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, destroy);
	(void)data;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	free(ls);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_layer_surface_v1 *layer_surface = data;

	if (!layer_surfaces_initialized) {
		wl_list_init(&layer_surfaces);
		layer_surfaces_initialized = true;
	}

	/* Assign output if not set */
	if (!layer_surface->output) {
		struct bspwm_wlr_output *out;
		wl_list_for_each(out, &server.outputs, link) {
			layer_surface->output = out->wlr_output;
			break;
		}
		if (!layer_surface->output) return;
	}

	/* Pick the right scene tree based on layer */
	int layer_idx = layer_surface->pending.layer;
	if (layer_idx < 0 || layer_idx > 3) layer_idx = 0;

	struct bspwm_wlr_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) return;

	ls->layer_surface = layer_surface;
	ls->scene = wlr_scene_layer_surface_v1_create(
		server.layer_trees[layer_idx], layer_surface);

	ls->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &ls->unmap);
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);

	wl_list_insert(&layer_surfaces, &ls->link);

	/* Initial configure */
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (out->wlr_output == layer_surface->output) {
			arrange_layers(out);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/*  XWayland surface handling                                         */
/* ------------------------------------------------------------------ */

static struct wl_list xwayland_surfaces_list;
static bool xwayland_surfaces_initialized = false;

static struct bspwm_wlr_xwayland_surface *xsurface_from_id(bspwm_wid_t id)
{
	if (!xwayland_surfaces_initialized) return NULL;
	struct bspwm_wlr_xwayland_surface *xs;
	wl_list_for_each(xs, &xwayland_surfaces_list, link) {
		if (xs->id == id) return xs;
	}
	return NULL;
}

static void xwayland_surface_map(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, map);
	(void)data;

	if (xs->xsurface->override_redirect) {
		/* Override-redirect: position directly, don't manage */
		wlr_scene_node_set_position(&xs->scene_tree->node,
			xs->xsurface->x, xs->xsurface->y);
		return;
	}

	schedule_window(xs->id);
}

static void xwayland_surface_unmap(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, unmap);
	(void)data;

	if (!xs->xsurface->override_redirect) {
		unmanage_window(xs->id);
	}
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, associate);
	(void)data;

	/* Surface is now valid — create scene tree */
	if (!xs->scene_tree && xs->xsurface->surface) {
		xs->scene_tree = wlr_scene_subsurface_tree_create(
			&server.scene->tree, xs->xsurface->surface);
		if (xs->scene_tree) {
			xs->scene_tree->node.data = xs;
		}
	}

	xs->map.notify = xwayland_surface_map;
	wl_signal_add(&xs->xsurface->surface->events.map, &xs->map);
	xs->unmap.notify = xwayland_surface_unmap;
	wl_signal_add(&xs->xsurface->surface->events.unmap, &xs->unmap);
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, dissociate);
	(void)data;

	wl_list_remove(&xs->map.link);
	wl_list_remove(&xs->unmap.link);
}

static void xwayland_surface_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, destroy);
	(void)data;

	wl_list_remove(&xs->associate.link);
	wl_list_remove(&xs->dissociate.link);
	wl_list_remove(&xs->destroy.link);
	wl_list_remove(&xs->request_configure.link);
	wl_list_remove(&xs->link);
	free(xs);
}

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	wlr_xwayland_surface_configure(xs->xsurface, ev->x, ev->y, ev->width, ev->height);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xwayland_surface *xsurface = data;

	if (!xwayland_surfaces_initialized) {
		wl_list_init(&xwayland_surfaces_list);
		xwayland_surfaces_initialized = true;
	}

	struct bspwm_wlr_xwayland_surface *xs = calloc(1, sizeof(*xs));
	if (!xs) return;

	xs->id = ++server.next_toplevel_id;
	xs->xsurface = xsurface;
	xs->scene_tree = NULL; /* created in associate handler when surface is valid */

	xs->associate.notify = xwayland_surface_associate;
	wl_signal_add(&xsurface->events.associate, &xs->associate);
	xs->dissociate.notify = xwayland_surface_dissociate;
	wl_signal_add(&xsurface->events.dissociate, &xs->dissociate);
	xs->destroy.notify = xwayland_surface_destroy;
	wl_signal_add(&xsurface->events.destroy, &xs->destroy);
	xs->request_configure.notify = xwayland_surface_request_configure;
	wl_signal_add(&xsurface->events.request_configure, &xs->request_configure);

	wl_list_insert(&xwayland_surfaces_list, &xs->link);
}

/* ------------------------------------------------------------------ */
/*  XDG activation (urgency)                                         */
/* ------------------------------------------------------------------ */

static void xdg_activation_request(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_activation_v1_request_activate_event *event = data;

	if (!event->surface) return;

	/* Find the toplevel for this surface */
	struct bspwm_wlr_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->xdg_toplevel->base->surface == event->surface) {
			/* Set urgency via bspwm core */
			coordinates_t loc;
			if (locate_window(tl->id, &loc) && loc.monitor && loc.desktop && loc.node) {
				set_urgent(loc.monitor, loc.desktop, loc.node, true);
			}
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Session lock                                                      */
/* ------------------------------------------------------------------ */

static void session_lock_destroy(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	server.active_lock = NULL;
	server.locked = false;
}

static void session_new_lock(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_session_lock_v1 *lock = data;

	if (server.active_lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	server.active_lock = lock;
	server.locked = true;

	server.lock_destroy.notify = session_lock_destroy;
	wl_signal_add(&lock->events.destroy, &server.lock_destroy);

	wlr_session_lock_v1_send_locked(lock);
}

/* ================================================================== */
/*  backend_* interface implementation                                */
/* ================================================================== */

int backend_init(int *default_screen)
{
	*default_screen = 0;
	wlr_log_init(WLR_INFO, NULL);

	server.wl_display = wl_display_create();
	if (!server.wl_display) return -1;

	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), NULL);
	if (!server.backend) {
		wl_display_destroy(server.wl_display);
		return -1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return -1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return -1;
	}

	/* Core Wayland protocols */
	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* Output layout */
	server.output_layout = wlr_output_layout_create(server.wl_display);
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Scene graph */
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Layer shell scene trees (ordered bottom to top in scene graph) */
	server.layer_trees[0] = wlr_scene_tree_create(&server.scene->tree); /* background */
	server.layer_trees[1] = wlr_scene_tree_create(&server.scene->tree); /* bottom */

	/* XDG shell (toplevels render between bottom and top layers) */
	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	/* Remaining layer shell trees (above toplevels) */
	server.layer_trees[2] = wlr_scene_tree_create(&server.scene->tree); /* top */
	server.layer_trees[3] = wlr_scene_tree_create(&server.scene->tree); /* overlay */

	/* Foreign toplevel management (for bars like waybar) */
	server.foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server.wl_display);

	/* Idle inhibit + notifier */
	server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.wl_display);
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

	/* XDG activation (urgency / focus stealing) */
	server.xdg_activation = wlr_xdg_activation_v1_create(server.wl_display);
	server.xdg_activation_request.notify = xdg_activation_request;
	wl_signal_add(&server.xdg_activation->events.request_activate,
		&server.xdg_activation_request);

	/* Session lock */
	server.session_lock_mgr = wlr_session_lock_manager_v1_create(server.wl_display);
	server.new_lock.notify = session_new_lock;
	wl_signal_add(&server.session_lock_mgr->events.new_lock, &server.new_lock);
	server.locked = false;

	/* Screencopy (for grim, etc.) */
	wlr_screencopy_manager_v1_create(server.wl_display);

	/* Fractional scale + viewporter (HiDPI) */
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_viewporter_create(server.wl_display);

	/* Layer shell protocol */
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	/* XWayland — skip in headless mode (no GPU for Xwayland rendering) */
	const char *wlr_backends = getenv("WLR_BACKENDS");
	bool headless = wlr_backends && strstr(wlr_backends, "headless");
	server.xwayland = headless ? NULL : wlr_xwayland_create(server.wl_display, server.compositor, false);
	if (server.xwayland) {
		server.xwayland_new_surface.notify = server_new_xwayland_surface;
		wl_signal_add(&server.xwayland->events.new_surface, &server.xwayland_new_surface);
		wl_list_init(&server.xwayland_surfaces);
	}

	/* Cursor */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/* Seat */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

	/* XDG decoration — force server-side borders */
	server.decoration_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_decoration.notify = new_decoration;
	wl_signal_add(&server.decoration_mgr->events.new_toplevel_decoration, &server.new_decoration);

	/* ID counters start above 0 (BSPWM_WID_NONE) */
	server.next_toplevel_id = 0x1000;
	server.next_output_id = 0;

	/* Wayland socket */
	server.socket = wl_display_add_socket_auto(server.wl_display);
	if (!server.socket) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return -1;
	}
	setenv("WAYLAND_DISPLAY", server.socket, true);
	if (server.xwayland) {
		setenv("DISPLAY", server.xwayland->display_name, true);
		wlr_xwayland_set_seat(server.xwayland, server.seat);
	}

	/* Start the backend */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return -1;
	}

	server.wl_event_loop = wl_display_get_event_loop(server.wl_display);
	return 0;
}

void backend_destroy(void)
{
	if (server.wl_display) {
		wl_display_destroy_clients(server.wl_display);

		/* Remove listeners before destroying objects */
		wl_list_remove(&server.cursor_motion.link);
		wl_list_remove(&server.cursor_motion_absolute.link);
		wl_list_remove(&server.cursor_button.link);
		wl_list_remove(&server.cursor_axis.link);
		wl_list_remove(&server.cursor_frame.link);
		wl_list_remove(&server.new_input.link);
		wl_list_remove(&server.request_cursor.link);
		wl_list_remove(&server.request_set_selection.link);
		wl_list_remove(&server.new_output.link);
		wl_list_remove(&server.new_xdg_toplevel.link);
		wl_list_remove(&server.new_xdg_popup.link);
		wl_list_remove(&server.new_decoration.link);
		wl_list_remove(&server.new_layer_surface.link);
		wl_list_remove(&server.xdg_activation_request.link);
		wl_list_remove(&server.new_lock.link);
		if (server.xwayland) {
			wl_list_remove(&server.xwayland_new_surface.link);
		}

		wlr_scene_node_destroy(&server.scene->tree.node);
		wlr_xcursor_manager_destroy(server.cursor_mgr);
		wlr_cursor_destroy(server.cursor);
		wlr_allocator_destroy(server.allocator);
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		server.wl_display = NULL;
	}
}

int backend_get_fd(void)
{
	return wl_event_loop_get_fd(server.wl_event_loop);
}

void backend_flush(void)
{
	wl_display_flush_clients(server.wl_display);
}

bool backend_dispatch_events(void)
{
	wl_event_loop_dispatch(server.wl_event_loop, 0);
	return true;
}

bool backend_check_connection(void)
{
	return server.wl_display != NULL;
}

/* ------------------------------------------------------------------ */
/*  Screen / root                                                     */
/* ------------------------------------------------------------------ */

void backend_get_screen_size(int *width, int *height)
{
	/* Sum output extents */
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, NULL, &box);
	*width = box.width;
	*height = box.height;
}

bspwm_wid_t backend_get_root(void)
{
	/* No root window concept in Wayland — return sentinel */
	return BSPWM_WID_NONE + 1;
}

/* ------------------------------------------------------------------ */
/*  Window management                                                 */
/* ------------------------------------------------------------------ */

bspwm_wid_t backend_create_internal_window(const char *kind, bspwm_rect_t rect, bool input_only)
{
	(void)kind; (void)rect; (void)input_only;
	/* Internal helper windows (meta, motion_recorder) are X11 concepts.
	 * On Wayland the compositor handles these internally.
	 * Return unique IDs for compatibility. */
	return ++server.next_toplevel_id;
}

void backend_destroy_window(bspwm_wid_t win)
{
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p) {
		wlr_scene_node_destroy(&p->rect->node);
		wl_list_remove(&p->link);
		free(p);
	}
}

void backend_window_show(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_node_set_enabled(&p->rect->node, true);
	}
}

void backend_window_hide(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_node_set_enabled(&p->rect->node, false);
	}
}

void backend_window_move(bspwm_wid_t win, int16_t x, int16_t y)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}
}

void backend_window_resize(bspwm_wid_t win, uint16_t w, uint16_t h)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, w, h);
	}
}

void backend_window_move_resize(bspwm_wid_t win, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return;
	if (tl->scene_tree) {
		wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
	}
	wlr_xdg_toplevel_set_size(tl->xdg_toplevel, w, h);
}

void backend_window_set_border_width(bspwm_wid_t win, uint32_t bw)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return;
	tl->border_width = bw;
	toplevel_update_borders(tl);
}

void backend_window_set_border_color(bspwm_wid_t win, uint32_t color)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return;
	toplevel_set_border_color(tl, color);
}

bool backend_window_exists(bspwm_wid_t win)
{
	return toplevel_from_id(win) != NULL || xsurface_from_id(win) != NULL;
}

bool backend_window_get_geometry(bspwm_wid_t win, bspwm_rect_t *rect)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return false;
	rect->x = tl->scene_tree->node.x;
	rect->y = tl->scene_tree->node.y;
	rect->width = tl->xdg_toplevel->base->geometry.width;
	rect->height = tl->xdg_toplevel->base->geometry.height;
	return true;
}

void backend_window_listen_enter(bspwm_wid_t win, bool enable)
{
	(void)win; (void)enable;
	/* Wayland compositor receives all pointer events — no-op */
}

/* ------------------------------------------------------------------ */
/*  Stacking                                                          */
/* ------------------------------------------------------------------ */

void backend_window_stack_above(bspwm_wid_t w1, bspwm_wid_t w2)
{
	struct bspwm_wlr_toplevel *tl1 = toplevel_from_id(w1);
	struct bspwm_wlr_toplevel *tl2 = toplevel_from_id(w2);
	if (tl1 && tl2 && tl1->scene_tree && tl2->scene_tree) {
		wlr_scene_node_place_above(&tl1->scene_tree->node, &tl2->scene_tree->node);
	}
}

void backend_window_stack_below(bspwm_wid_t w1, bspwm_wid_t w2)
{
	struct bspwm_wlr_toplevel *tl1 = toplevel_from_id(w1);
	struct bspwm_wlr_toplevel *tl2 = toplevel_from_id(w2);
	if (tl1 && tl2 && tl1->scene_tree && tl2->scene_tree) {
		wlr_scene_node_place_below(&tl1->scene_tree->node, &tl2->scene_tree->node);
	}
}

void backend_window_raise(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_raise_to_top(&tl->scene_tree->node);
	}
}

void backend_window_lower(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_lower_to_bottom(&tl->scene_tree->node);
	}
}

/* ------------------------------------------------------------------ */
/*  Focus                                                             */
/* ------------------------------------------------------------------ */

void backend_set_input_focus(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return;

	struct wlr_surface *surface = tl->xdg_toplevel->base->surface;
	struct wlr_seat *seat = server.seat;

	/* Deactivate previous */
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	if (prev == surface) return;

	if (prev) {
		struct wlr_xdg_toplevel *prev_tl =
			wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (prev_tl)
			wlr_xdg_toplevel_set_activated(prev_tl, false);
	}

	/* Activate new */
	wlr_xdg_toplevel_set_activated(tl->xdg_toplevel, true);
	if (tl->foreign_handle)
		wlr_foreign_toplevel_handle_v1_set_activated(tl->foreign_handle, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);
	}
}

void backend_clear_input_focus(void)
{
	struct wlr_surface *prev = server.seat->keyboard_state.focused_surface;
	if (prev) {
		struct wlr_xdg_toplevel *prev_tl =
			wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (prev_tl)
			wlr_xdg_toplevel_set_activated(prev_tl, false);
	}
	wlr_seat_keyboard_clear_focus(server.seat);
}

/* ------------------------------------------------------------------ */
/*  Window properties                                                 */
/* ------------------------------------------------------------------ */

bool backend_get_window_class(bspwm_wid_t win, char *class_name, char *instance_name, size_t len)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		const char *app_id = tl->xdg_toplevel->app_id;
		if (app_id) {
			snprintf(class_name, len, "%s", app_id);
			snprintf(instance_name, len, "%s", app_id);
		}
		return app_id != NULL;
	}
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs) {
		if (xs->xsurface->class)
			snprintf(class_name, len, "%s", xs->xsurface->class);
		if (xs->xsurface->instance)
			snprintf(instance_name, len, "%s", xs->xsurface->instance);
		return xs->xsurface->class != NULL;
	}
	return false;
}

bool backend_get_window_name(bspwm_wid_t win, char *name, size_t len)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return false;
	const char *title = tl->xdg_toplevel->title;
	if (title) {
		snprintf(name, len, "%s", title);
	}
	return title != NULL;
}

bool backend_get_icccm_props(bspwm_wid_t win, bspwm_icccm_props_t *props)
{
	(void)win;
	/* Wayland has no ICCCM. Clients always accept focus and close gracefully. */
	props->take_focus = false;
	props->input_hint = true;
	props->delete_window = true;
	return true;
}

bool backend_get_size_hints(bspwm_wid_t win, bspwm_size_hints_t *hints)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) return false;

	memset(hints, 0, sizeof(*hints));

	struct wlr_xdg_toplevel *xt = tl->xdg_toplevel;
	if (xt->current.min_width > 0 || xt->current.min_height > 0) {
		hints->flags |= BSP_SIZE_HINT_P_MIN_SIZE;
		hints->min_width = xt->current.min_width;
		hints->min_height = xt->current.min_height;
	}
	if (xt->current.max_width > 0 || xt->current.max_height > 0) {
		hints->flags |= BSP_SIZE_HINT_P_MAX_SIZE;
		hints->max_width = xt->current.max_width;
		hints->max_height = xt->current.max_height;
	}

	return true;
}

bool backend_get_transient_for(bspwm_wid_t win, bspwm_wid_t *transient_for)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl || !tl->xdg_toplevel->parent) {
		*transient_for = BSPWM_WID_NONE;
		return false;
	}
	/* Find the parent toplevel's ID */
	struct bspwm_wlr_toplevel *ptl;
	wl_list_for_each(ptl, &server.toplevels, link) {
		if (ptl->xdg_toplevel == tl->xdg_toplevel->parent) {
			*transient_for = ptl->id;
			return true;
		}
	}
	*transient_for = BSPWM_WID_NONE;
	return false;
}

bool backend_is_override_redirect(bspwm_wid_t win)
{
	(void)win;
	return false; /* No override_redirect in Wayland */
}

bool backend_get_urgency(bspwm_wid_t win)
{
	(void)win;
	return false; /* TODO: track activation tokens */
}

void backend_set_window_state(bspwm_wid_t win, bspwm_wm_state_t state)
{
	(void)win; (void)state;
	/* No WM_STATE in Wayland */
}

bool backend_get_window_type(bspwm_wid_t win, bspwm_window_type_t *type)
{
	(void)win;
	*type = BSP_WINDOW_TYPE_NORMAL;
	return true; /* All xdg-shell toplevels are "normal" */
}

/* ------------------------------------------------------------------ */
/*  EWMH (no-ops on Wayland — these are X11 concepts)                */
/* ------------------------------------------------------------------ */

void backend_ewmh_init(void) {}
void backend_ewmh_update_active_window(bspwm_wid_t win) { (void)win; }
void backend_ewmh_update_number_of_desktops(uint32_t count) { (void)count; }
void backend_ewmh_update_current_desktop(uint32_t index) { (void)index; }
void backend_ewmh_update_desktop_names(const char *names, size_t len) { (void)names; (void)len; }
void backend_ewmh_update_desktop_viewport(void) {}
void backend_ewmh_set_wm_desktop(bspwm_wid_t win, uint32_t desktop) { (void)win; (void)desktop; }
void backend_ewmh_update_client_list(bspwm_wid_t *list, uint32_t count, bool stacking) { (void)list; (void)count; (void)stacking; }
void backend_ewmh_wm_state_update(bspwm_wid_t win, uint16_t wm_flags) { (void)win; (void)wm_flags; }
void backend_ewmh_set_supporting(bspwm_wid_t win) { (void)win; }
bool backend_ewmh_handle_struts(bspwm_wid_t win) { (void)win; return false; }
void backend_ewmh_get_struts(bspwm_wid_t win, int *top, int *right, int *bottom, int *left) { (void)win; *top = *right = *bottom = *left = 0; }

/* ------------------------------------------------------------------ */
/*  Monitor / output discovery                                        */
/* ------------------------------------------------------------------ */

int backend_query_outputs(bspwm_output_info_t *outputs, int max)
{
	int count = 0;
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (count >= max) break;

		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(server.output_layout, out->wlr_output);
		if (!lo) continue;

		outputs[count].id = out->id;
		outputs[count].primary = (count == 0);
		snprintf(outputs[count].name, sizeof(outputs[count].name),
			"%s", out->wlr_output->name);

		outputs[count].rect.x = lo->x;
		outputs[count].rect.y = lo->y;
		outputs[count].rect.width = out->wlr_output->width;
		outputs[count].rect.height = out->wlr_output->height;

		count++;
	}
	return count;
}

void backend_listen_output_changes(void)
{
	/* Already handled via new_output listener */
}

/* ------------------------------------------------------------------ */
/*  Input                                                             */
/* ------------------------------------------------------------------ */

void backend_pointer_init(void) {}
void backend_grab_buttons_on_window(bspwm_wid_t win) { (void)win; }
void backend_grab_buttons(void) {}
void backend_ungrab_buttons(void) {}
bool backend_grab_pointer(void) { return true; }
void backend_ungrab_pointer(void) {}

void backend_query_pointer(bspwm_wid_t *win, bspwm_point_t *pos)
{
	if (pos) {
		pos->x = (int16_t)server.cursor->x;
		pos->y = (int16_t)server.cursor->y;
	}
	if (win) {
		/* Find toplevel under cursor */
		double sx, sy;
		struct wlr_scene_node *node = wlr_scene_node_at(
			&server.scene->tree.node, server.cursor->x, server.cursor->y, &sx, &sy);
		*win = BSPWM_WID_NONE;
		while (node) {
			if (node->data) {
				struct bspwm_wlr_toplevel *tl = node->data;
				*win = tl->id;
				break;
			}
			node = &node->parent->node;
		}
	}
}

void backend_warp_pointer(bspwm_rect_t rect)
{
	wlr_cursor_warp(server.cursor, NULL,
		rect.x + rect.width / 2.0, rect.y + rect.height / 2.0);
}

void backend_enable_motion_recorder(bspwm_wid_t win) { (void)win; }
void backend_disable_motion_recorder(void) {}
void backend_allow_events(bool replay, uint32_t timestamp) { (void)replay; (void)timestamp; }

uint16_t backend_get_lock_fields(void) { return 0; }

/* ------------------------------------------------------------------ */
/*  Presel feedback                                                   */
/* ------------------------------------------------------------------ */

bspwm_wid_t backend_create_presel_feedback(uint32_t color)
{
	if (!presel_list_initialized) {
		wl_list_init(&presel_list);
		presel_list_initialized = true;
	}

	float fcolor[4];
	color_u32_to_float(color, fcolor);

	struct bspwm_wlr_presel *p = calloc(1, sizeof(*p));
	if (!p) return ++server.next_toplevel_id;

	p->id = ++server.next_toplevel_id;
	p->rect = wlr_scene_rect_create(&server.scene->tree, 1, 1, fcolor);
	wlr_scene_node_set_enabled(&p->rect->node, false);

	wl_list_insert(&presel_list, &p->link);
	return p->id;
}

/* ------------------------------------------------------------------ */
/*  Client message / close                                            */
/* ------------------------------------------------------------------ */

void backend_close_window(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
	}
}

void backend_request_close(bspwm_wid_t win)
{
	/* On Wayland, graceful and forceful close are the same */
	backend_close_window(win);
}

void backend_send_take_focus(bspwm_wid_t win, bspwm_icccm_props_t *props)
{
	(void)props;
	backend_set_input_focus(win);
}

void backend_send_configure_notify(bspwm_wid_t win, bspwm_rect_t rect, uint32_t border_width)
{
	(void)border_width;
	/* On Wayland, the compositor tells the client its size via configure */
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, rect.width, rect.height);
	}
}

/* ------------------------------------------------------------------ */
/*  Color                                                             */
/* ------------------------------------------------------------------ */

uint32_t backend_get_color_pixel(const char *color)
{
	unsigned int red, green, blue;
	if (sscanf(color + 1, "%02x%02x%02x", &red, &green, &blue) == 3) {
		return (0xFF << 24) | (red << 16 | green << 8 | blue);
	}
	return 0xFF000000;
}

/* ------------------------------------------------------------------ */
/*  Enumerate windows                                                 */
/* ------------------------------------------------------------------ */

void backend_enumerate_windows(backend_window_visitor_t visitor)
{
	struct bspwm_wlr_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->xdg_toplevel->base->surface->mapped) {
			visitor(tl->id);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Display name                                                      */
/* ------------------------------------------------------------------ */

bool backend_parse_display(char **host, int *display_num, int *screen_num)
{
	/* Wayland doesn't use X11 display strings.
	 * Return the Wayland socket name for state path generation. */
	*host = server.socket ? strdup(server.socket) : strdup("wayland");
	*display_num = 0;
	*screen_num = 0;
	return true;
}

bool backend_register_root_events(void)
{
	/* No root window events in Wayland — always succeeds */
	return true;
}

/* ------------------------------------------------------------------ */
/*  Atom helpers                                                      */
/* ------------------------------------------------------------------ */

void backend_set_atom(bspwm_wid_t win, const char *atom_name, uint32_t value)
{
	(void)win; (void)atom_name; (void)value;
	/* No atoms in Wayland */
}
