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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
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
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include "ext-workspace-v1-protocol.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
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
#include "settings.h"

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

	/* Size the WM core last requested via move_resize/resize. The border
	 * box is drawn to this size (not the client's committed geometry) so a
	 * client that quantizes or ignores the configure — foot rounds to whole
	 * character cells unless told it is tiled — still yields the same
	 * on-screen footprint as the X11 backend: the full tile rectangle. */
	int req_width;
	int req_height;
	/* Last tiled-edge bitfield sent to the client (enum wlr_edges), or
	 * UINT32_MAX before the first move_resize so the first state is always
	 * pushed. Telling tiled clients they are tiled stops the cell-rounding. */
	uint32_t tiled_edges;
	bool fullscreen_sent;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	/* foreign-toplevel requests (taskbar clicks); valid while foreign_handle is */
	struct wl_listener foreign_request_activate;
	struct wl_listener foreign_request_close;
	struct wlr_output *foreign_output;

	/* xdg-decoration object, if the client created one. The mode can only
	 * be sent once the surface is initialized (after its initial commit),
	 * so it is applied from xdg_toplevel_commit when it arrives early. */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener decoration_destroy;

	struct wl_list link; /* wlr_server.toplevels */
};

struct bspwm_wlr_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/* ext-workspace-v1: one group per monitor, one workspace per desktop. */
struct bspwm_wlr_ws_group {
	uint32_t mon_id;
	struct wlr_output *output;
	struct wlr_ext_workspace_group_handle_v1 *group;
	struct wl_list link;
};

struct bspwm_wlr_ws {
	uint32_t desk_id;
	struct wlr_ext_workspace_handle_v1 *ws;
	struct wlr_ext_workspace_group_handle_v1 *group;
	char name[SMALEN];
	uint32_t coord;
	bool active, urgent;
	struct wl_list link;
};

struct bspwm_wlr_output {
	bspwm_output_id_t id;
	struct wlr_output *wlr_output;
	/* Exclusive-zone padding last applied to the monitor, so the user's
	 * own top_padding etc. can be preserved by applying only the delta. */
	int strut_top, strut_right, strut_bottom, strut_left;
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
	/* The subsurface tree destroys itself with the wl_surface (X unmap,
	 * client exit); this watches it so the pointer never dangles. */
	struct wl_listener scene_destroy;
	/* Geometry the core last applied; re-asserted when the client asks
	 * for something else while managed. */
	int16_t x, y;
	uint16_t width, height;
	bool managed_geometry;

	struct wl_list link; /* xwayland_surfaces list */
};

struct bspwm_wlr_layer_surface {
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	/* Cached layer index (0..3). Tracked so that when a client calls
	 * set_layer() to move between layers we can reparent the scene
	 * subtree to the matching layer_trees[] entry on commit. */
	int current_layer;
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
	struct wlr_session *session;   /* NULL when nested (X11/headless) */
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
	struct wl_listener request_set_primary_selection;

	/* Protocols that bars, launchers, clipboard managers and idle tools
	 * require. All but xdg-output are optional to the tree itself. */
	struct wlr_output_power_manager_v1 *output_power_mgr;
	struct wl_listener output_power_set_mode;
	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wl_listener cursor_shape_request;
	struct wlr_gamma_control_manager_v1 *gamma_mgr;
	struct wl_listener gamma_set;
	struct wlr_output_manager_v1 *output_mgr;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;

	/* All managed windows (xdg and xwayland) live under this tree, which sits
	 * between the bottom and top layer-shell trees. */
	struct wlr_scene_tree *window_tree;
	/* Mapped layer surface that currently holds keyboard focus (wofi, rofi,
	 * a lock screen), or NULL. While set, window focus changes from the
	 * core are recorded but not applied to the seat. */
	struct bspwm_wlr_layer_surface *focused_layer;

	struct wlr_ext_workspace_manager_v1 *workspace_mgr;
	struct wl_listener workspace_commit;
	struct wl_list ws_groups;   /* bspwm_wlr_ws_group.link */
	struct wl_list workspaces;  /* bspwm_wlr_ws.link */
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
	struct wl_listener new_idle_inhibitor;
	int idle_inhibitors;

	/* XDG activation (urgency / focus requests) */
	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener xdg_activation_request;

	/* Layer shell */
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	/* XWayland */
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_new_surface;
	struct wl_listener xwayland_ready;
	struct wl_list xwayland_surfaces;

	/* Scene trees for the 4 layer shell layers */
	struct wlr_scene_tree *layer_trees[4]; /* background, bottom, top, overlay */

	/* Session lock */
	struct wlr_session_lock_manager_v1 *session_lock_mgr;
	struct wlr_session_lock_v1 *active_lock;
	struct wl_listener lock_new_surface;
	struct wl_listener lock_unlock;
	/* Lock surfaces and the blanking rect live here, above every layer. */
	struct wlr_scene_tree *lock_tree;
	struct wlr_scene_rect *lock_blank;
	/* DISPLAY as inherited at startup. It is overwritten with Xwayland's
	 * display for children; a restart re-execs with this environment, and
	 * a nested run's X11 backend needs the original back. */
	char *inherited_display;
	/* Drag and drop */
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy;
	struct wlr_scene_tree *drag_icon;
	int pointer_count;
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

	/* Size the border box from the size the WM core requested, so the
	 * footprint (surface + border) always equals the tile rectangle the
	 * layout computed — exactly like the X11 backend, where the server
	 * honours the WM configure regardless of what the client asks for.
	 * Fall back to the client's committed geometry only before the first
	 * move_resize (req_* still 0), e.g. a floating window sizing itself. */
	int w = tl->req_width  > 0 ? tl->req_width  : tl->xdg_toplevel->base->geometry.width;
	int h = tl->req_height > 0 ? tl->req_height : tl->xdg_toplevel->base->geometry.height;
	if (w <= 0 || h <= 0) return;

	int total_w = w + 2 * (int)bw;

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

/* Tell the client whether it is tiled. Tiled clients (foot, and other
 * terminals) must not round their surface down to whole character cells,
 * otherwise the committed surface is smaller than the tile and the unused
 * strip shows up as an enlarged gap on the right/bottom edges. This mirrors
 * the X11 backend, where the server enforces the WM-configured size. Only
 * re-sends when the edge set actually changes, to avoid configure spam. */
static void toplevel_apply_tiled(struct bspwm_wlr_toplevel *tl)
{
	uint32_t edges = 0;
	coordinates_t loc;
	if (locate_window(tl->id, &loc) && loc.node && loc.node->client) {
		client_state_t s = loc.node->client->state;
		if (s == STATE_TILED || s == STATE_PSEUDO_TILED || s == STATE_FULLSCREEN) {
			edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
			        WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
		}
	}
	if (edges != tl->tiled_edges) {
		tl->tiled_edges = edges;
		wlr_xdg_toplevel_set_tiled(tl->xdg_toplevel, edges);
	}

	/* The client must be told it is fullscreen, or browsers keep their
	 * chrome and video players their controls; and told when it no longer
	 * is, or it can never leave. */
	bool fs = loc.node && loc.node->client && loc.node->client->state == STATE_FULLSCREEN;
	if (fs != tl->fullscreen_sent) {
		tl->fullscreen_sent = fs;
		wlr_xdg_toplevel_set_fullscreen(tl->xdg_toplevel, fs);
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

static struct bspwm_wlr_toplevel *toplevel_from_xdg(struct wlr_xdg_toplevel *xdg)
{
	struct bspwm_wlr_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->xdg_toplevel == xdg) return tl;
	}
	return NULL;
}

static struct bspwm_wlr_toplevel *toplevel_from_id(bspwm_wid_t id)
{
	struct bspwm_wlr_toplevel *tl;
	wl_list_for_each(tl, &server.toplevels, link) {
		if (tl->id == id)
			return tl;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Output (monitor) handling                                         */
/* ------------------------------------------------------------------ */

/* Forward decl: output state/mode changes need to re-arrange layers so
 * that exclusive-zone padding tracks the new resolution. */
static void arrange_layers(struct bspwm_wlr_output *output);
static bool session_lock_input_ok(struct wlr_surface *surface);
static void xwayland_scene_destroy(struct wl_listener *listener, void *data);
static struct wl_list xwayland_surfaces_list;
static bool xwayland_surfaces_initialized;
static void close_layer_surfaces_on_output(struct wlr_output *wlr_output);
static void workspace_commit(struct wl_listener *listener, void *data);

static void output_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct bspwm_wlr_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(server.scene, output->wlr_output);
	if (!scene_output) return;
	wlr_scene_output_commit(scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

/* wlr-output-management: publish the current layout to clients (wlr-randr,
 * kanshi, wdisplays) and apply the configurations they send back. */
static void output_manager_update(void)
{
	if (!server.output_mgr) return;
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, out->wlr_output);
		struct wlr_output_layout_output *lo =
			wlr_output_layout_get(server.output_layout, out->wlr_output);
		head->state.enabled = lo != NULL && out->wlr_output->enabled;
		if (lo) {
			head->state.x = lo->x;
			head->state.y = lo->y;
		}
	}
	wlr_output_manager_v1_set_configuration(server.output_mgr, config);
}

static void output_manager_handle(struct wlr_output_configuration_v1 *config, bool test_only)
{
	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, head->state.enabled);
		if (head->state.enabled) {
			if (head->state.mode) {
				wlr_output_state_set_mode(&state, head->state.mode);
			} else {
				wlr_output_state_set_custom_mode(&state,
					head->state.custom_mode.width, head->state.custom_mode.height,
					head->state.custom_mode.refresh);
			}
			wlr_output_state_set_transform(&state, head->state.transform);
			wlr_output_state_set_scale(&state, head->state.scale);
			wlr_output_state_set_adaptive_sync_enabled(&state, head->state.adaptive_sync_enabled);
		}
		if (test_only) {
			ok = wlr_output_test_state(wlr_output, &state) && ok;
		} else if (wlr_output_commit_state(wlr_output, &state)) {
			if (head->state.enabled) {
				/* Adding an output already in the layout moves it. A
				 * re-enabled output needs its scene output linked again;
				 * the scene output itself survives layout removal. */
				struct wlr_output_layout_output *lo = wlr_output_layout_add(
					server.output_layout, wlr_output, head->state.x, head->state.y);
				struct wlr_scene_output *so = wlr_scene_get_scene_output(server.scene, wlr_output);
				if (lo && so) {
					wlr_scene_output_layout_add_output(server.scene_layout, lo, so);
				}
			} else {
				wlr_output_layout_remove(server.output_layout, wlr_output);
			}
		} else {
			ok = false;
		}
		wlr_output_state_finish(&state);
	}

	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);

	if (!test_only) {
		update_monitors();
		struct bspwm_wlr_output *out;
		wl_list_for_each(out, &server.outputs, link) {
			arrange_layers(out);
		}
		output_manager_update();
	}
}

static void output_manager_apply(struct wl_listener *listener, void *data)
{
	(void)listener;
	output_manager_handle(data, false);
}

static void output_manager_test(struct wl_listener *listener, void *data)
{
	(void)listener;
	output_manager_handle(data, true);
}

static void output_request_state(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	/* Resolution or transform may have changed: the core's monitor
	 * rectangle must follow (nested window resize, backend mode change),
	 * then exclusive-zone padding is recomputed for the new area. */
	update_monitors();
	arrange_layers(output);
	output_manager_update();
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_output *output = wl_container_of(listener, output, destroy);
	(void)data;

	/* wlroots does not close layer surfaces with their output; left alone
	 * they keep a dangling output pointer and render at stale coordinates
	 * on whatever monitor moves into that space. */
	close_layer_surfaces_on_output(output->wlr_output);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);

	/* Notify bspwm core of output change — but not during shutdown. On
	 * teardown, cleanup() has already freed every monitor before
	 * backend_destroy() tears the outputs down; rebuilding the monitor tree
	 * here would recreate a monitor+desktop that nothing then frees (a leak)
	 * and would touch already-destroyed compositor state. */
	if (running) {
		update_monitors();
		output_manager_update();
	}
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

	/* Append: backend_query_outputs marks the first entry primary, and
	 * prepending made every hotplugged monitor steal that role. */
	wl_list_insert(server.outputs.prev, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server.output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, l_output, scene_output);

	/* Notify bspwm core of the new output — but not during initial startup.
	 * The outputs are created while wlr_backend_start() runs inside
	 * backend_init(), before setup()/init() have run. Building the monitor
	 * tree here would allocate a monitor+desktop that init() then drops on
	 * the floor (it nulls mon_head without freeing) before setup() rebuilds
	 * the tree — a per-output startup leak. setup() calls update_monitors()
	 * itself once the core is ready; only later hotplug events need this.
	 *
	 * output_manager_update() only publishes server.outputs to the wlr
	 * output-management protocol (it never touches mon_head), and it is the
	 * only startup path that seeds the manager's heads — skipping it here
	 * would leave wlr-randr and other output-management clients seeing no
	 * outputs until the first hotplug. So run it unconditionally. */
	if (running) {
		update_monitors();
	}
	output_manager_update();
}

/* ------------------------------------------------------------------ */
/*  XDG toplevel (window) handling                                    */
/* ------------------------------------------------------------------ */

/* Tell taskbars which output the window is on. waybar's wlr/taskbar only
 * lists toplevels that have entered the bar's own output. */
static void toplevel_update_foreign_output(struct bspwm_wlr_toplevel *tl)
{
	if (!tl->foreign_handle) return;
	int lx, ly;
	wlr_scene_node_coords(&tl->scene_tree->node, &lx, &ly);
	struct wlr_box geo = tl->xdg_toplevel->base->geometry;
	struct wlr_output *out = wlr_output_layout_output_at(server.output_layout,
		lx + geo.width / 2.0, ly + geo.height / 2.0);
	if (!out) {
		out = wlr_output_layout_output_at(server.output_layout, lx, ly);
	}
	if (out == tl->foreign_output) return;
	if (tl->foreign_output) {
		wlr_foreign_toplevel_handle_v1_output_leave(tl->foreign_handle, tl->foreign_output);
	}
	if (out) {
		wlr_foreign_toplevel_handle_v1_output_enter(tl->foreign_handle, out);
	}
	tl->foreign_output = out;
}

static void foreign_request_activate(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, foreign_request_activate);
	(void)data;
	coordinates_t loc;
	if (locate_window(tl->id, &loc)) {
		focus_node(loc.monitor, loc.desktop, loc.node);
	}
}

static void foreign_request_close(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, foreign_request_close);
	(void)data;
	wlr_xdg_toplevel_send_close(tl->xdg_toplevel);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, set_title);
	(void)data;
	if (tl->foreign_handle && tl->xdg_toplevel->title) {
		wlr_foreign_toplevel_handle_v1_set_title(tl->foreign_handle, tl->xdg_toplevel->title);
	}
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, set_app_id);
	(void)data;
	if (tl->foreign_handle && tl->xdg_toplevel->app_id) {
		wlr_foreign_toplevel_handle_v1_set_app_id(tl->foreign_handle, tl->xdg_toplevel->app_id);
	}
}

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
			tl->foreign_request_activate.notify = foreign_request_activate;
			wl_signal_add(&tl->foreign_handle->events.request_activate, &tl->foreign_request_activate);
			tl->foreign_request_close.notify = foreign_request_close;
			wl_signal_add(&tl->foreign_handle->events.request_close, &tl->foreign_request_close);
			tl->foreign_output = NULL;
			toplevel_update_foreign_output(tl);
		}
	}

	/* Notify bspwm core: new window mapped */
	schedule_window(tl->id);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, unmap);
	(void)data;

	if (server.grabbed_tl == tl) {
		server.grabbed_tl = NULL;
		server.cursor_mode = BSPWM_CURSOR_PASSTHROUGH;
	}

	if (tl->foreign_handle) {
		wl_list_remove(&tl->foreign_request_activate.link);
		wl_list_remove(&tl->foreign_request_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(tl->foreign_handle);
		tl->foreign_handle = NULL;
		tl->foreign_output = NULL;
	}

	unmanage_window(tl->id);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, commit);
	(void)data;

	if (tl->xdg_toplevel->base->initial_commit) {
		/* The compositor must answer the initial commit with a configure
		 * before the client can map. 0x0 lets the client pick its size;
		 * the tree layout resizes it once it is managed. */
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, 0, 0);
		if (tl->decoration) {
			wlr_xdg_toplevel_decoration_v1_set_mode(tl->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
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
	wl_list_remove(&tl->set_title.link);
	wl_list_remove(&tl->set_app_id.link);
	if (server.grabbed_tl == tl) {
		server.grabbed_tl = NULL;
		server.cursor_mode = BSPWM_CURSOR_PASSTHROUGH;
	}
	/* The xdg_surface outlives its toplevel role; a popup created against
	 * it afterwards must not find the freed scene tree. */
	tl->xdg_toplevel->base->data = NULL;
	if (tl->decoration) {
		wl_list_remove(&tl->decoration_destroy.link);
		tl->decoration = NULL;
	}
	wl_list_remove(&tl->link);
	/* Destroy the container scene tree (borders + surface). Without this the
	 * border rectangles keep rendering after the window is gone. */
	if (tl->scene_tree != NULL) {
		wlr_scene_node_destroy(&tl->scene_tree->node);
	}
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
	/* Deny maximize — bspwm uses its own state management. Before the
	 * initial commit a configure cannot be scheduled (wlroots asserts);
	 * the reply goes out with the initial configure instead. */
	if (!tl->xdg_toplevel->base->initialized) return;
	wlr_xdg_toplevel_set_maximized(tl->xdg_toplevel, false);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, request_fullscreen);
	(void)data;

	/* Fires for both set_fullscreen and unset_fullscreen; the direction is
	 * in requested.fullscreen. Let the core's state machine apply it. */
	bool want = tl->xdg_toplevel->requested.fullscreen;
	coordinates_t loc;
	if (locate_window(tl->id, &loc) && loc.monitor && loc.desktop && loc.node && loc.node->client) {
		client_state_t target = want ? STATE_FULLSCREEN : loc.node->client->last_state;
		if (!want && target == STATE_FULLSCREEN) target = STATE_TILED;
		set_state(loc.monitor, loc.desktop, loc.node, target);
		arrange(loc.monitor, loc.desktop);
	}
	/* xdg-shell requires a configure in reply even when nothing changes
	 * (e.g. the request arrived before the window is managed; the rule
	 * pass picks requested.fullscreen up at manage time). */
	if (tl->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(tl->xdg_toplevel->base);
	}
}

bool backend_window_requests_fullscreen(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	return tl && tl->xdg_toplevel->requested.fullscreen;
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct bspwm_wlr_toplevel *tl = calloc(1, sizeof(*tl));
	if (!tl) return;

	tl->id = ++server.next_toplevel_id;
	tl->xdg_toplevel = xdg_toplevel;
	tl->tiled_edges = UINT32_MAX; /* force the first set_tiled */

	/* Container tree holds borders + surface */
	tl->scene_tree = wlr_scene_tree_create(server.window_tree);
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
	tl->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &tl->set_title);
	tl->set_app_id.notify = xdg_toplevel_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &tl->set_app_id);

	wl_list_insert(&server.toplevels, &tl->link);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_popup *p = wl_container_of(listener, p, commit);
	(void)data;

	/* Same contract as toplevels: no configure after the initial commit
	 * means the popup (menu, tooltip, combo box) never maps. */
	if (p->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(p->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_popup *p = wl_container_of(listener, p, destroy);
	(void)data;
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_popup *popup = data;

	struct wlr_scene_tree *parent_tree = NULL;
	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (parent) {
		parent_tree = parent->data;
	} else {
		/* waybar menus/tooltips and launcher popups have a layer surface
		 * as parent; they never mapped because this returned early. */
		struct wlr_layer_surface_v1 *lparent =
			wlr_layer_surface_v1_try_from_wlr_surface(popup->parent);
		if (lparent && lparent->data) {
			struct bspwm_wlr_layer_surface *ls = lparent->data;
			parent_tree = ls->scene->tree;
		}
	}
	if (!parent_tree) return;
	popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);

	/* Let the positioner's flip/slide rules keep the popup on screen:
	 * give it the output box in the parent surface's coordinate space. */
	{
		int lx, ly;
		wlr_scene_node_coords(&parent_tree->node, &lx, &ly);
		struct wlr_output *out = wlr_output_layout_output_at(server.output_layout, lx, ly);
		if (out) {
			struct wlr_box obox;
			wlr_output_layout_get_box(server.output_layout, out, &obox);
			struct wlr_box box = { .x = obox.x - lx, .y = obox.y - ly,
			                       .width = obox.width, .height = obox.height };
			wlr_xdg_popup_unconstrain_from_box(popup, &box);
		}
	}

	struct bspwm_wlr_popup *p = calloc(1, sizeof(*p));
	if (!p) return;
	p->xdg_popup = popup;
	p->commit.notify = xdg_popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}

/* ------------------------------------------------------------------ */
/*  Keyboard handling                                                 */
/* ------------------------------------------------------------------ */

/* Without activity notifications swayidle's timers run from creation
 * regardless of input: the screen locks and outputs power off while the
 * user is typing. Inhibitors (mpv, browsers playing video) hold idle off. */
static void idle_activity(void)
{
	if (server.idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
	}
}

struct bspwm_wlr_idle_inhibitor {
	struct wl_listener destroy;
};

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_idle_inhibitor *ih = wl_container_of(listener, ih, destroy);
	(void)data;
	wl_list_remove(&ih->destroy.link);
	free(ih);
	if (server.idle_inhibitors > 0) server.idle_inhibitors--;
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, server.idle_inhibitors > 0);
}

static void new_idle_inhibitor(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_idle_inhibitor_v1 *inhibitor = data;
	struct bspwm_wlr_idle_inhibitor *ih = calloc(1, sizeof(*ih));
	if (!ih) return;
	ih->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &ih->destroy);
	server.idle_inhibitors++;
	wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, true);
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
	idle_activity();
	struct bspwm_wlr_keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;

	wlr_seat_set_keyboard(server.seat, kb->wlr_keyboard);

	/* VT switch: Ctrl+Alt+F1..F12. The keymap emits XF86Switch_VT_n at the
	 * Ctrl+Alt level; act on it before anything else so the user can always
	 * leave the compositor, even from a lock screen. wlr_session is NULL when
	 * nested (X11/headless backend), where the host handles VT switching. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && server.session != NULL) {
		const xkb_keysym_t *vtsyms = NULL;
		int nvt = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state,
			event->keycode + 8, &vtsyms);
		for (int i = 0; i < nvt; i++) {
			if (vtsyms[i] >= XKB_KEY_XF86Switch_VT_1 &&
			    vtsyms[i] <= XKB_KEY_XF86Switch_VT_12) {
				wlr_session_change_vt(server.session,
					vtsyms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				return;
			}
		}
	}

	/* When locked, forward all keys to the lock surface only */
	if (server.locked) {
		if (session_lock_input_ok(server.seat->keyboard_state.focused_surface)) {
			wlr_seat_keyboard_notify_key(server.seat,
				event->time_msec, event->keycode, event->state);
		}
		return;
	}

	/* Try keybinding interception on key press */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t modifiers = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
		xkb_keycode_t keycode = event->keycode + 8;

		/* Match against the *unshifted* keysyms for this key, i.e. shift
		 * level 0 of the active layout. `xkb_state_key_get_syms` applies the
		 * current shift level, so Shift+q yields `Q` and Shift+1 yields
		 * `exclam`, neither of which can ever match what
		 * `keybind_parse_combo` registered — it resolves names
		 * case-insensitively and stores the lowercase form (`q`, `1`). The
		 * X11 backend already matches on column 0 in `key_press`; this keeps
		 * the two backends in agreement. */
		const xkb_keysym_t *syms = NULL;
		int nsyms = 0;
		struct xkb_keymap *keymap = kb->wlr_keyboard->keymap;
		if (keymap != NULL) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(
				kb->wlr_keyboard->xkb_state, keycode);
			if (layout != XKB_LAYOUT_INVALID) {
				nsyms = xkb_keymap_key_get_syms_by_level(
					keymap, keycode, layout, 0, &syms);
			}
		}
		if (nsyms == 0) {
			/* No keymap, or the key has no level-0 symbol. */
			nsyms = xkb_state_key_get_syms(
				kb->wlr_keyboard->xkb_state, keycode, &syms);
		}

		/* Map wlr modifiers to our KBMOD flags. Caps and Mod2 (Num Lock) are
		 * locking modifiers: they are latched independently of what the user
		 * is pressing, so folding them in would break every binding whenever
		 * either lock happens to be on. */
		uint32_t kbmod = 0;
		if (modifiers & WLR_MODIFIER_SHIFT) kbmod |= KBMOD_SHIFT;
		if (modifiers & WLR_MODIFIER_CTRL)  kbmod |= KBMOD_CTRL;
		if (modifiers & WLR_MODIFIER_ALT)   kbmod |= KBMOD_ALT;
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

static void update_seat_capabilities(void)
{
	uint32_t caps = 0;
	if (!wl_list_empty(&server.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (server.pointer_count > 0) caps |= WL_SEAT_CAPABILITY_POINTER;
	wlr_seat_set_capabilities(server.seat, caps);
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
	update_seat_capabilities();
}

struct bspwm_wlr_pointer {
	struct wl_listener destroy;
};

static void pointer_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_pointer *p = wl_container_of(listener, p, destroy);
	(void)data;
	wl_list_remove(&p->destroy.link);
	free(p);
	if (server.pointer_count > 0) server.pointer_count--;
	update_seat_capabilities();
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

static struct bspwm_wlr_toplevel *toplevel_at_cursor(double *sx, double *sy);
static bspwm_wid_t window_id_from_node(struct wlr_scene_node *node);
static struct bspwm_wlr_xwayland_surface *xsurface_from_id(bspwm_wid_t id);
static bspwm_wid_t window_at_cursor(double *sx, double *sy);
static bspwm_wid_t last_hover_id = BSPWM_WID_NONE;

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
		server.grabbed_tl->req_width = new_w;
		server.grabbed_tl->req_height = new_h;
		toplevel_update_borders(server.grabbed_tl);

		/* Update bspwm's floating rectangle */
		coordinates_t loc;
		if (locate_window(server.grabbed_tl->id, &loc) && loc.node && loc.node->client) {
			loc.node->client->floating_rectangle.width = new_w;
			loc.node->client->floating_rectangle.height = new_h;
		}
		return;
	}

	if (server.drag_icon) {
		wlr_scene_node_set_position(&server.drag_icon->node,
			(int)server.cursor->x, (int)server.cursor->y);
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

	if (!surface || !session_lock_input_ok(surface)) {
		wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(seat);
		if (server.locked) return;
	} else {
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	}
	if (server.locked) return;

	/* Focus-follows-pointer, with the X11 backend's semantics: refocus when
	 * the window under the cursor is not the globally focused one (so the
	 * focused monitor follows the pointer too), and focus the monitor under
	 * the pointer when it is over empty space. Pointer-follows-focus must
	 * not fire back during a pointer-driven change. */
	if (focus_follows_pointer) {
		double tsx, tsy;
		bspwm_wid_t id = window_at_cursor(&tsx, &tsy);
		if (id != BSPWM_WID_NONE && id != last_hover_id) {
			last_hover_id = id;
			coordinates_t loc;
			if (locate_window(id, &loc) && loc.monitor && loc.desktop &&
			    loc.desktop == loc.monitor->desk && mon && mon->desk &&
			    loc.node != mon->desk->focus) {
				bool pff = pointer_follows_focus, pfm = pointer_follows_monitor;
				pointer_follows_focus = false;
				pointer_follows_monitor = false;
				focus_node(loc.monitor, loc.desktop, loc.node);
				pointer_follows_focus = pff;
				pointer_follows_monitor = pfm;
			}
		} else if (id == BSPWM_WID_NONE) {
			last_hover_id = BSPWM_WID_NONE;
			bspwm_point_t pt = { (int16_t)server.cursor->x, (int16_t)server.cursor->y };
			monitor_t *m = monitor_from_point(pt);
			if (m && m != mon) {
				bool pff = pointer_follows_focus, pfm = pointer_follows_monitor;
				pointer_follows_focus = false;
				pointer_follows_monitor = false;
				focus_node(m, m->desk, m->desk ? m->desk->focus : NULL);
				pointer_follows_focus = pff;
				pointer_follows_monitor = pfm;
			}
		}
	}
}

static void cursor_motion(struct wl_listener *listener, void *data)
{
	idle_activity();
	(void)listener;
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	idle_activity();
	(void)listener;
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(event->time_msec);
}

/* Resolve a scene node's data pointer to a managed window id, for xdg and
 * xwayland windows alike, without trusting the pointer type. */
static bspwm_wid_t window_id_from_node(struct wlr_scene_node *node)
{
	while (node) {
		if (node->data) {
			struct bspwm_wlr_toplevel *check;
			wl_list_for_each(check, &server.toplevels, link) {
				if ((void *)check == node->data) return check->id;
			}
			if (xwayland_surfaces_initialized) {
				struct bspwm_wlr_xwayland_surface *xs;
				wl_list_for_each(xs, &xwayland_surfaces_list, link) {
					if ((void *)xs == node->data) return xs->id;
				}
			}
		}
		node = node->parent ? &node->parent->node : NULL;
	}
	return BSPWM_WID_NONE;
}

static bspwm_wid_t window_at_cursor(double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server.scene->tree.node, server.cursor->x, server.cursor->y, sx, sy);
	return window_id_from_node(node);
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
	idle_activity();
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

	if (server.locked) {
		/* Only the lock surface may see clicks; it already has pointer
		 * focus if the cursor is over it. */
		if (session_lock_input_ok(server.seat->pointer_state.focused_surface)) {
			wlr_seat_pointer_notify_button(server.seat,
				event->time_msec, event->button, event->state);
		}
		return;
	}

	if (super_held && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy;
		struct bspwm_wlr_toplevel *tl = toplevel_at_cursor(&sx, &sy);
		if (tl) {
			/* Focus through the core so bspc, borders and history agree
			 * with where the keyboard went. */
			coordinates_t floc;
			if (locate_window(tl->id, &floc) && floc.monitor && floc.desktop) {
				bool pff = pointer_follows_focus, pfm = pointer_follows_monitor;
				pointer_follows_focus = false;
				pointer_follows_monitor = false;
				focus_node(floc.monitor, floc.desktop, floc.node);
				pointer_follows_focus = pff;
				pointer_follows_monitor = pfm;
			}

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

	/* Normal click — focus + pass through. Re-evaluate what is under the
	 * cursor first: after a keyboard desktop switch the seat's pointer
	 * focus may still be a now-hidden surface. */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		process_cursor_motion(event->time_msec);
		double sx, sy;
		bspwm_wid_t id = window_at_cursor(&sx, &sy);
		coordinates_t loc;
		if (id != BSPWM_WID_NONE && locate_window(id, &loc) && loc.monitor && loc.desktop &&
		    !(mon && mon->desk && loc.node == mon->desk->focus)) {
			bool pff = pointer_follows_focus, pfm = pointer_follows_monitor;
			pointer_follows_focus = false;
			pointer_follows_monitor = false;
			focus_node(loc.monitor, loc.desktop, loc.node);
			pointer_follows_focus = pff;
			pointer_follows_monitor = pfm;
		}
	}

	wlr_seat_pointer_notify_button(server.seat,
		event->time_msec, event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data)
{
	idle_activity();
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
	struct bspwm_wlr_pointer *p = calloc(1, sizeof(*p));
	if (p) {
		p->destroy.notify = pointer_destroy;
		wl_signal_add(&device->events.destroy, &p->destroy);
		server.pointer_count++;
	}
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

	update_seat_capabilities();
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

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server.seat, event->source, event->serial);
}

/* wlr-output-power-management: swayidle/wlopm turning outputs off and on. */
static void output_power_set_mode(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
	wlr_output_commit_state(event->output, &state);
	wlr_output_state_finish(&state);
}

/* cursor-shape-v1: clients name a cursor instead of uploading one. Only the
 * client under the pointer may change it. */
static void cursor_shape_request(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->seat_client != server.seat->pointer_state.focused_client) return;
	wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr,
		wlr_cursor_shape_v1_name(event->shape));
}

/* wlr-gamma-control: wlsunset/gammastep. A NULL control resets the ramp. */
static void gamma_set(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_gamma_control_v1_apply(event->control, &state)) {
		wlr_output_state_finish(&state);
		return;
	}
	if (!wlr_output_commit_state(event->output, &state) && event->control) {
		wlr_gamma_control_v1_send_failed_and_destroy(event->control);
	}
	wlr_output_state_finish(&state);
}

/* ------------------------------------------------------------------ */
/*  Decoration handling (force server-side)                           */
/* ------------------------------------------------------------------ */

static void decoration_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_toplevel *tl = wl_container_of(listener, tl, decoration_destroy);
	(void)data;
	wl_list_remove(&tl->decoration_destroy.link);
	tl->decoration = NULL;
}

static void new_decoration(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;

	struct bspwm_wlr_toplevel *tl = NULL, *it;
	wl_list_for_each(it, &server.toplevels, link) {
		if (it->xdg_toplevel == deco->toplevel) {
			tl = it;
			break;
		}
	}
	if (!tl) return;

	tl->decoration = deco;
	tl->decoration_destroy.notify = decoration_destroy;
	wl_signal_add(&deco->events.destroy, &tl->decoration_destroy);

	/* Sending the mode schedules a configure, which wlroots refuses (assert)
	 * on a surface that has not done its initial commit yet. Clients such as
	 * foot and alacritty create the decoration before that commit; for them
	 * the mode goes out from xdg_toplevel_commit instead. */
	if (deco->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

/* ------------------------------------------------------------------ */
/*  Layer shell handling                                              */
/* ------------------------------------------------------------------ */

/* Track all layer surfaces for arrange_layers iteration */
static struct wl_list layer_surfaces;  /* bspwm_wlr_layer_surface.link */
static bool layer_surfaces_initialized = false;

static void close_layer_surfaces_on_output(struct wlr_output *wlr_output)
{
	if (!layer_surfaces_initialized) return;
	struct bspwm_wlr_layer_surface *ls, *tmp;
	wl_list_for_each_safe(ls, tmp, &layer_surfaces, link) {
		if (ls->layer_surface->output == wlr_output) {
			wlr_layer_surface_v1_destroy(ls->layer_surface);
		}
	}
}

static void arrange_layers(struct bspwm_wlr_output *output)
{
	if (!layer_surfaces_initialized) return;

	/* Layer trees hang off the scene root, so surfaces must be placed in
	 * layout coordinates: an output-local (0,0) box put every bar on the
	 * monitor at the layout origin. */
	struct wlr_box full_area = {0};
	wlr_output_layout_get_box(server.output_layout, output->wlr_output, &full_area);
	if (wlr_box_empty(&full_area)) return;

	struct wlr_box usable_area = full_area;

	/* Configure all layer surfaces on this output, in layer order */
	for (int layer = 0; layer < 4; layer++) {
		struct bspwm_wlr_layer_surface *ls;
		wl_list_for_each(ls, &layer_surfaces, link) {
			if (ls->layer_surface->output != output->wlr_output)
				continue;
			if ((int)ls->layer_surface->current.layer != layer)
				continue;
			/* A surface that has not done its initial commit cannot be
			 * configured yet (wlroots asserts). layer_surface_commit
			 * re-arranges once it has. */
			if (!ls->layer_surface->initialized)
				continue;
			wlr_scene_layer_surface_v1_configure(ls->scene, &full_area, &usable_area);
		}
	}

	/* Update bspwm monitor padding from exclusive zones: the Wayland
	 * equivalent of _NET_WM_STRUT_PARTIAL. m->padding is also where the
	 * user's top_padding etc. live, so apply only the change in struts
	 * rather than overwriting it. */
	monitor_t *m = get_monitor_by_output_id(output->id);
	if (m) {
		int new_top    = usable_area.y - full_area.y;
		int new_left   = usable_area.x - full_area.x;
		int new_right  = (full_area.x + full_area.width)  - (usable_area.x + usable_area.width);
		int new_bottom = (full_area.y + full_area.height) - (usable_area.y + usable_area.height);
		bool changed = (output->strut_top    != new_top    ||
		                output->strut_left   != new_left   ||
		                output->strut_right  != new_right  ||
		                output->strut_bottom != new_bottom);
		if (changed) {
			m->padding.top    += new_top    - output->strut_top;
			m->padding.left   += new_left   - output->strut_left;
			m->padding.right  += new_right  - output->strut_right;
			m->padding.bottom += new_bottom - output->strut_bottom;
			output->strut_top = new_top;
			output->strut_left = new_left;
			output->strut_right = new_right;
			output->strut_bottom = new_bottom;
			/* Re-tile every desktop on this monitor so windows reflow
			 * around the freshly-claimed exclusive zone. */
			for (desktop_t *d = m->desk_head; d != NULL; d = d->next) {
				arrange(m, d);
			}
		}
	}
}

static void layer_surface_focus(struct bspwm_wlr_layer_surface *ls);

static void layer_surface_commit(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, commit);
	(void)data;

	if (!ls->layer_surface->initialized) return;

	if (ls->layer_surface->surface->mapped) {
		bool wants = ls->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
		if (wants && server.focused_layer != ls) {
			layer_surface_focus(ls);
		} else if (!wants && server.focused_layer == ls) {
			server.focused_layer = NULL;
			update_input_focus();
		}
	}

	/* A client can call zwlr_layer_surface_v1::set_layer() to move
	 * between layers after the initial map. The scene helper was
	 * parented to layer_trees[current_layer] at create time, so we
	 * must reparent the subtree on change — otherwise an overlay
	 * demoted to background would keep rendering on top. */
	int new_layer = (int)ls->layer_surface->current.layer;
	if (new_layer < 0 || new_layer > 3) new_layer = ls->current_layer;
	if (new_layer != ls->current_layer) {
		wlr_scene_node_reparent(&ls->scene->tree->node,
			server.layer_trees[new_layer]);
		ls->current_layer = new_layer;
	}

	/* Find the output this layer is on */
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (out->wlr_output == ls->layer_surface->output) {
			arrange_layers(out);
			break;
		}
	}
}

/* Give the seat's keyboard to a layer surface that asked for it (wofi,
 * rofi, swaylock's lock surface). Without this the launcher draws but every
 * key still goes to the focused window, so nothing can be typed or picked. */
static void layer_surface_focus(struct bspwm_wlr_layer_surface *ls)
{
	struct wlr_surface *surface = ls->layer_surface->surface;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	server.focused_layer = ls;
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server.seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server.seat, surface, NULL, 0, NULL);
	}
}

static void layer_surface_map(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, map);
	(void)data;
	if (ls->layer_surface->current.keyboard_interactive !=
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		layer_surface_focus(ls);
	}
}

static void layer_surface_unmap(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, unmap);
	(void)data;
	if (server.focused_layer == ls) {
		server.focused_layer = NULL;
		/* Hand the keyboard back to whatever the core considers focused. */
		update_input_focus();
	}
}

static void layer_surface_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_layer_surface *ls = wl_container_of(listener, ls, destroy);
	(void)data;
	if (server.focused_layer == ls) {
		server.focused_layer = NULL;
		update_input_focus();
	}

	/* Remember which output this lived on; we need to re-arrange after
	 * it's gone so the freed exclusive zone is reclaimed by tiled
	 * windows. Without this, killing waybar leaves m->padding stuck. */
	struct wlr_output *wlr_out = ls->layer_surface->output;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->link);
	free(ls);

	/* arrange_layers iterates all *remaining* live layer surfaces for
	 * the output and recomputes usable_area from full_area — so removing
	 * `ls` above and re-arranging here releases its cached padding. */
	if (wlr_out) {
		struct bspwm_wlr_output *out;
		wl_list_for_each(out, &server.outputs, link) {
			if (out->wlr_output == wlr_out) {
				arrange_layers(out);
				break;
			}
		}
	}
}

static void server_new_layer_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_layer_surface_v1 *layer_surface = data;

	if (!layer_surfaces_initialized) {
		wl_list_init(&layer_surfaces);
		layer_surfaces_initialized = true;
	}

	/* Assign output if the client didn't specify one. Prefer the focused
	 * monitor, then the primary monitor; only fall back to the first
	 * output if bspwm core has no monitors yet. This keeps multi-monitor
	 * bars (waybar without an explicit output:) from always landing on
	 * the head of server.outputs. */
	if (!layer_surface->output) {
		bspwm_output_id_t want = 0;
		if (mon)          want = mon->output_id;
		else if (pri_mon) want = pri_mon->output_id;

		struct bspwm_wlr_output *out;
		if (want != 0) {
			wl_list_for_each(out, &server.outputs, link) {
				if (out->id == want) {
					layer_surface->output = out->wlr_output;
					break;
				}
			}
		}
		if (!layer_surface->output) {
			wl_list_for_each(out, &server.outputs, link) {
				layer_surface->output = out->wlr_output;
				break;
			}
		}
		if (!layer_surface->output) {
			/* No output to put it on: close it so the client gets
			 * `closed` instead of a protocol error on its first buffer. */
			wlr_layer_surface_v1_destroy(layer_surface);
			return;
		}
	}

	/* Pick the right scene tree based on layer */
	int layer_idx = layer_surface->pending.layer;
	if (layer_idx < 0 || layer_idx > 3) layer_idx = 0;

	struct bspwm_wlr_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) return;

	ls->layer_surface = layer_surface;
	layer_surface->data = ls;
	ls->current_layer = layer_idx;
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
		xs->managed_geometry = false;
		unmanage_window(xs->id);
	}
	if (server.seat->pointer_state.focused_surface == xs->xsurface->surface) {
		wlr_seat_pointer_clear_focus(server.seat);
	}
}

static void xwayland_surface_associate(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, associate);
	(void)data;

	/* Surface is now valid — create scene tree */
	if (!xs->scene_tree && xs->xsurface->surface) {
		xs->scene_tree = wlr_scene_subsurface_tree_create(
			server.window_tree, xs->xsurface->surface);
		if (xs->scene_tree) {
			xs->scene_tree->node.data = xs;
			xs->scene_destroy.notify = xwayland_scene_destroy;
			wl_signal_add(&xs->scene_tree->node.events.destroy, &xs->scene_destroy);
		}
	}

	xs->map.notify = xwayland_surface_map;
	wl_signal_add(&xs->xsurface->surface->events.map, &xs->map);
	xs->unmap.notify = xwayland_surface_unmap;
	wl_signal_add(&xs->xsurface->surface->events.unmap, &xs->unmap);
}

static void xwayland_scene_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, scene_destroy);
	(void)data;
	wl_list_remove(&xs->scene_destroy.link);
	xs->scene_tree = NULL;
}

static void xwayland_surface_dissociate(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, dissociate);
	(void)data;

	wl_list_remove(&xs->map.link);
	wl_list_remove(&xs->unmap.link);
	/* The wl_surface goes away with the X unmap and a fresh one arrives on
	 * remap. Its subsurface tree tears itself down with it (already gone
	 * by the time this fires on client exit), and xwayland_scene_destroy
	 * clears the pointer, so nothing to free here. */
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
	if (xs->scene_tree) {
		wlr_scene_node_destroy(&xs->scene_tree->node);
		xs->scene_tree = NULL;
	}
	free(xs);
}

static void xwayland_surface_request_configure(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_xwayland_surface *xs = wl_container_of(listener, xs, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;

	/* A managed window's geometry is the core's decision; re-assert it.
	 * Unmanaged and override-redirect windows get what they ask for. */
	if (xs->managed_geometry) {
		wlr_xwayland_surface_configure(xs->xsurface, xs->x, xs->y, xs->width, xs->height);
		return;
	}
	wlr_xwayland_surface_configure(xs->xsurface, ev->x, ev->y, ev->width, ev->height);
}

static void xwayland_ready(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	/* Xwayland may be restarted on a different display; keep DISPLAY
	 * current for anything spawned from here on. */
	if (server.xwayland && server.xwayland->display_name[0] != '\0') {
		setenv("DISPLAY", server.xwayland->display_name, true);
	}
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

struct bspwm_wlr_lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *tree;
	struct wl_listener map;
	struct wl_listener destroy;
};

static void lock_surface_focus(struct wlr_surface *surface)
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server.seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server.seat, surface, NULL, 0, NULL);
	}
}

static void lock_surface_map(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_lock_surface *ls = wl_container_of(listener, ls, map);
	(void)data;
	lock_surface_focus(ls->lock_surface->surface);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data)
{
	struct bspwm_wlr_lock_surface *ls = wl_container_of(listener, ls, destroy);
	(void)data;
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	if (ls->tree) wlr_scene_node_destroy(&ls->tree->node);
	free(ls);
}

/* ext-session-lock: the locker creates one surface per output. It has to
 * be configured to the output's size, placed over it, and given the
 * keyboard, or the lock never covers anything (see session_lock_input_ok
 * for the pointer side). */
static void session_lock_new_surface(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct wlr_box box;
	wlr_output_layout_get_box(server.output_layout, lock_surface->output, &box);
	if (wlr_box_empty(&box)) {
		wlr_output_effective_resolution(lock_surface->output, &box.width, &box.height);
	}
	wlr_session_lock_surface_v1_configure(lock_surface, box.width, box.height);

	struct bspwm_wlr_lock_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) return;
	ls->lock_surface = lock_surface;
	ls->tree = wlr_scene_tree_create(server.lock_tree);
	if (ls->tree) {
		wlr_scene_subsurface_tree_create(ls->tree, lock_surface->surface);
		wlr_scene_node_set_position(&ls->tree->node, box.x, box.y);
	}
	ls->map.notify = lock_surface_map;
	wl_signal_add(&lock_surface->surface->events.map, &ls->map);
	ls->destroy.notify = lock_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &ls->destroy);
	if (lock_surface->surface->mapped) {
		lock_surface_focus(lock_surface->surface);
	}
}

static void session_lock_blank(bool on)
{
	if (on && !server.lock_blank) {
		struct wlr_box all;
		wlr_output_layout_get_box(server.output_layout, NULL, &all);
		float black[4] = {0, 0, 0, 1};
		server.lock_blank = wlr_scene_rect_create(server.lock_tree,
			all.width > 0 ? all.width : 1, all.height > 0 ? all.height : 1, black);
		if (server.lock_blank) {
			wlr_scene_node_set_position(&server.lock_blank->node, all.x, all.y);
			wlr_scene_node_lower_to_bottom(&server.lock_blank->node);
		}
	} else if (!on && server.lock_blank) {
		wlr_scene_node_destroy(&server.lock_blank->node);
		server.lock_blank = NULL;
	}
}

static void session_lock_unlock(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	server.locked = false;
	session_lock_blank(false);
	/* Hand the keyboard back to the core's focused window. */
	update_input_focus();
}

static void session_lock_destroy(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	wl_list_remove(&server.lock_destroy.link);
	wl_list_remove(&server.lock_new_surface.link);
	wl_list_remove(&server.lock_unlock.link);
	server.active_lock = NULL;
	/* A locker that dies without unlocking must not expose the session:
	 * server.locked stays as it was and the blanking rect keeps covering
	 * the outputs until another locker takes over. */
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
	session_lock_blank(true);

	server.lock_new_surface.notify = session_lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &server.lock_new_surface);
	server.lock_unlock.notify = session_lock_unlock;
	wl_signal_add(&lock->events.unlock, &server.lock_unlock);
	server.lock_destroy.notify = session_lock_destroy;
	wl_signal_add(&lock->events.destroy, &server.lock_destroy);

	wlr_session_lock_v1_send_locked(lock);
}

/* While locked, only lock surfaces may receive pointer input. */
static bool session_lock_input_ok(struct wlr_surface *surface)
{
	if (!server.locked) return true;
	return surface && wlr_session_lock_surface_v1_try_from_wlr_surface(surface) != NULL;
}

/* ------------------------------------------------------------------ */
/*  Drag and drop                                                     */
/* ------------------------------------------------------------------ */

static void drag_destroy(struct wl_listener *listener, void *data)
{
	(void)listener; (void)data;
	wl_list_remove(&server.drag_destroy.link);
	server.drag_icon = NULL;
}

static void seat_request_start_drag(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server.seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(server.seat, event->drag, event->serial);
	} else {
		wlr_data_source_destroy(event->drag->source);
	}
}

static void seat_start_drag(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_drag *drag = data;
	if (drag->icon) {
		server.drag_icon = wlr_scene_drag_icon_create(&server.scene->tree, drag->icon);
		if (server.drag_icon) {
			wlr_scene_node_set_position(&server.drag_icon->node,
				(int)server.cursor->x, (int)server.cursor->y);
		}
	}
	server.drag_destroy.notify = drag_destroy;
	wl_signal_add(&drag->events.destroy, &server.drag_destroy);
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
		wl_display_get_event_loop(server.wl_display), &server.session);
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
	server.window_tree = wlr_scene_tree_create(&server.scene->tree);    /* windows */

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
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);      /* session lock */

	/* Foreign toplevel management (for bars like waybar) */
	server.foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server.wl_display);

	/* Idle inhibit + notifier */
	server.idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server.wl_display);
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
	server.new_idle_inhibitor.notify = new_idle_inhibitor;
	wl_signal_add(&server.idle_inhibit_mgr->events.new_inhibitor, &server.new_idle_inhibitor);

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
	server.request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);
	server.request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag, &server.request_start_drag);
	server.start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.start_drag);

	/* Selections: primary (middle-click paste) and data-control (clipboard
	 * managers such as nocb, wl-paste --watch, cliphist). */
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_data_control_manager_v1_create(server.wl_display);
	wlr_ext_data_control_manager_v1_create(server.wl_display, 1);

	/* Output description for bars and launchers (waybar refuses to start
	 * without xdg-output), plus presentation timing and 1x1 buffers. */
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
	wlr_presentation_create(server.wl_display, server.backend, 2);
	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);

	server.output_power_mgr = wlr_output_power_manager_v1_create(server.wl_display);
	server.output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&server.output_power_mgr->events.set_mode, &server.output_power_set_mode);

	server.cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
	server.cursor_shape_request.notify = cursor_shape_request;
	wl_signal_add(&server.cursor_shape_mgr->events.request_set_shape, &server.cursor_shape_request);

	server.gamma_mgr = wlr_gamma_control_manager_v1_create(server.wl_display);
	server.gamma_set.notify = gamma_set;
	wl_signal_add(&server.gamma_mgr->events.set_gamma, &server.gamma_set);

	wl_list_init(&server.ws_groups);
	wl_list_init(&server.workspaces);
	server.workspace_mgr = wlr_ext_workspace_manager_v1_create(server.wl_display, 1);
	server.workspace_commit.notify = workspace_commit;
	wl_signal_add(&server.workspace_mgr->events.commit, &server.workspace_commit);

	server.output_mgr = wlr_output_manager_v1_create(server.wl_display);
	server.output_mgr_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_mgr->events.apply, &server.output_mgr_apply);
	server.output_mgr_test.notify = output_manager_test;
	wl_signal_add(&server.output_mgr->events.test, &server.output_mgr_test);

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
	{
		const char *d = getenv("DISPLAY");
		free(server.inherited_display);
		server.inherited_display = d ? strdup(d) : NULL;
	}
	if (server.xwayland) {
		/* A non-lazy Xwayland is started from an idle callback, so its
		 * display name is still empty here. Run the idle sources now: the
		 * sockets get bound and the name filled in, and X clients started
		 * by the config file queue on the socket until Xwayland is up. */
		wl_event_loop_dispatch_idle(wl_display_get_event_loop(server.wl_display));
		if (server.xwayland->display_name[0] != '\0') {
			setenv("DISPLAY", server.xwayland->display_name, true);
		} else {
			unsetenv("DISPLAY");
		}
		server.xwayland_ready.notify = xwayland_ready;
		wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
		wlr_xwayland_set_seat(server.xwayland, server.seat);
	} else {
		/* No Xwayland: an inherited DISPLAY would point X clients at some
		 * other server, or at nothing. */
		unsetenv("DISPLAY");
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
		wl_list_remove(&server.request_set_primary_selection.link);
		wl_list_remove(&server.new_output.link);
		wl_list_remove(&server.new_xdg_toplevel.link);
		wl_list_remove(&server.new_xdg_popup.link);
		wl_list_remove(&server.new_decoration.link);
		wl_list_remove(&server.new_layer_surface.link);
		wl_list_remove(&server.xdg_activation_request.link);
		wl_list_remove(&server.new_lock.link);
		wl_list_remove(&server.output_power_set_mode.link);
		wl_list_remove(&server.cursor_shape_request.link);
		wl_list_remove(&server.gamma_set.link);
		wl_list_remove(&server.output_mgr_apply.link);
		wl_list_remove(&server.output_mgr_test.link);
		wl_list_remove(&server.workspace_commit.link);
		wl_list_remove(&server.new_idle_inhibitor.link);
		wl_list_remove(&server.request_start_drag.link);
		wl_list_remove(&server.start_drag.link);
		if (server.active_lock) {
			wl_list_remove(&server.lock_destroy.link);
			wl_list_remove(&server.lock_new_surface.link);
			wl_list_remove(&server.lock_unlock.link);
			server.active_lock = NULL;
		}
		{
			struct bspwm_wlr_ws *w, *wt;
			wl_list_for_each_safe(w, wt, &server.workspaces, link) { wl_list_remove(&w->link); free(w); }
			struct bspwm_wlr_ws_group *g, *gt;
			wl_list_for_each_safe(g, gt, &server.ws_groups, link) { wl_list_remove(&g->link); free(g); }
		}
		if (server.xwayland) {
			wl_list_remove(&server.xwayland_new_surface.link);
			wl_list_remove(&server.xwayland_ready.link);
		}

		if (server.inherited_display) {
			setenv("DISPLAY", server.inherited_display, true);
		} else {
			unsetenv("DISPLAY");
		}
		unsetenv("WAYLAND_DISPLAY");

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
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs && xs->scene_tree) {
		wlr_scene_node_set_enabled(&xs->scene_tree->node, true);
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
		/* The seat may still point at this surface; the next click would
		 * otherwise go to an invisible window on another desktop. */
		if (server.seat->pointer_state.focused_surface == tl->xdg_toplevel->base->surface) {
			wlr_seat_pointer_clear_focus(server.seat);
		}
		return;
	}
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs && xs->scene_tree) {
		wlr_scene_node_set_enabled(&xs->scene_tree->node, false);
		if (server.seat->pointer_state.focused_surface == xs->xsurface->surface) {
			wlr_seat_pointer_clear_focus(server.seat);
		}
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
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_node_set_position(&p->rect->node, x, y);
	}
}

void backend_window_resize(bspwm_wid_t win, uint16_t w, uint16_t h)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		tl->req_width = w;
		tl->req_height = h;
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, w, h);
		toplevel_apply_tiled(tl);
		toplevel_update_borders(tl);
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_rect_set_size(p->rect, w, h);
	}
}

void backend_window_move_resize(bspwm_wid_t win, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl) {
		if (tl->scene_tree) {
			/* Position first so the output lookup below sees the new place. */
			wlr_scene_node_set_position(&tl->scene_tree->node, x, y);
		}
		toplevel_update_foreign_output(tl);
		tl->req_width = w;
		tl->req_height = h;
		wlr_xdg_toplevel_set_size(tl->xdg_toplevel, w, h);
		toplevel_apply_tiled(tl);
		toplevel_update_borders(tl);
		return;
	}
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs) {
		xs->x = x; xs->y = y; xs->width = w; xs->height = h;
		xs->managed_geometry = true;
		wlr_xwayland_surface_configure(xs->xsurface, x, y, w, h);
		if (xs->scene_tree) {
			wlr_scene_node_set_position(&xs->scene_tree->node, x, y);
		}
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_node_set_position(&p->rect->node, x, y);
		wlr_scene_rect_set_size(p->rect, w, h);
	}
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
	if (tl) {
		rect->x = tl->scene_tree->node.x;
		rect->y = tl->scene_tree->node.y;
		rect->width = tl->xdg_toplevel->base->geometry.width;
		rect->height = tl->xdg_toplevel->base->geometry.height;
		return true;
	}
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs) {
		rect->x = xs->scene_tree ? xs->scene_tree->node.x : xs->xsurface->x;
		rect->y = xs->scene_tree ? xs->scene_tree->node.y : xs->xsurface->y;
		rect->width = xs->xsurface->width;
		rect->height = xs->xsurface->height;
		return true;
	}
	return false;
}

void backend_window_listen_enter(bspwm_wid_t win, bool enable)
{
	(void)win; (void)enable;
	/* Wayland compositor receives all pointer events — no-op */
}

/* ------------------------------------------------------------------ */
/*  Stacking                                                          */
/* ------------------------------------------------------------------ */

static struct wlr_scene_node *scene_node_from_id(bspwm_wid_t id)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(id);
	if (tl && tl->scene_tree) return &tl->scene_tree->node;
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(id);
	if (xs && xs->scene_tree) return &xs->scene_tree->node;
	struct bspwm_wlr_presel *p = presel_from_id(id);
	if (p && p->rect) return &p->rect->node;
	return NULL;
}

void backend_window_stack_above(bspwm_wid_t w1, bspwm_wid_t w2)
{
	struct wlr_scene_node *n1 = scene_node_from_id(w1);
	struct wlr_scene_node *n2 = scene_node_from_id(w2);
	if (n1 && n2 && n1->parent == n2->parent) {
		wlr_scene_node_place_above(n1, n2);
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
	struct wlr_scene_node *n = scene_node_from_id(win);
	if (n) wlr_scene_node_raise_to_top(n);
}

void backend_window_lower(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (tl && tl->scene_tree) {
		wlr_scene_node_lower_to_bottom(&tl->scene_tree->node);
		return;
	}
	struct bspwm_wlr_presel *p = presel_from_id(win);
	if (p && p->rect) {
		wlr_scene_node_lower_to_bottom(&p->rect->node);
	}
}

/* ------------------------------------------------------------------ */
/*  Focus                                                             */
/* ------------------------------------------------------------------ */

static void deactivate_surface(struct wlr_surface *prev)
{
	if (!prev) return;
	struct wlr_xdg_toplevel *prev_tl = wlr_xdg_toplevel_try_from_wlr_surface(prev);
	if (prev_tl) {
		wlr_xdg_toplevel_set_activated(prev_tl, false);
		struct bspwm_wlr_toplevel *ptl = toplevel_from_xdg(prev_tl);
		if (ptl && ptl->foreign_handle)
			wlr_foreign_toplevel_handle_v1_set_activated(ptl->foreign_handle, false);
		return;
	}
	struct wlr_xwayland_surface *pxs = wlr_xwayland_surface_try_from_wlr_surface(prev);
	if (pxs) {
		wlr_xwayland_surface_activate(pxs, false);
	}
}

void backend_set_input_focus(bspwm_wid_t win)
{
	struct bspwm_wlr_toplevel *tl = toplevel_from_id(win);
	if (!tl) {
		struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
		if (!xs || !xs->xsurface->surface || server.focused_layer || server.locked) return;
		last_hover_id = BSPWM_WID_NONE;
		struct wlr_surface *xsurf = xs->xsurface->surface;
		struct wlr_surface *xprev = server.seat->keyboard_state.focused_surface;
		if (xprev == xsurf) return;
		deactivate_surface(xprev);
		wlr_xwayland_surface_activate(xs->xsurface, true);
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server.seat);
		if (kb) {
			wlr_seat_keyboard_notify_enter(server.seat, xsurf,
				kb->keycodes, kb->num_keycodes, &kb->modifiers);
		} else {
			wlr_seat_keyboard_notify_enter(server.seat, xsurf, NULL, 0, NULL);
		}
		return;
	}

	/* A launcher or lock surface holds the keyboard; the core's choice is
	 * re-applied from update_input_focus() when it goes away. */
	if (server.focused_layer || server.locked) return;
	last_hover_id = BSPWM_WID_NONE;

	struct wlr_surface *surface = tl->xdg_toplevel->base->surface;
	struct wlr_seat *seat = server.seat;

	/* Deactivate previous */
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;
	if (prev == surface) return;

	deactivate_surface(prev);

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
	if (server.focused_layer || server.locked) return;
	last_hover_id = BSPWM_WID_NONE;
	struct wlr_surface *prev = server.seat->keyboard_state.focused_surface;
	deactivate_surface(prev);
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

bool backend_get_window_role(bspwm_wid_t win, char *role, size_t len)
{
	/* No WM_WINDOW_ROLE equivalent on Wayland; wlroots clients don't set it. */
	(void) win;
	if (role != NULL && len > 0) {
		role[0] = '\0';
	}
	return false;
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

/* ---- ext-workspace-v1: desktops as workspaces for waybar & co. ---- */

static struct wlr_output *wlr_output_from_output_id(bspwm_output_id_t id)
{
	struct bspwm_wlr_output *out;
	wl_list_for_each(out, &server.outputs, link) {
		if (out->id == id) return out->wlr_output;
	}
	return NULL;
}

static bool desktop_exists(uint32_t id, monitor_t **mp, desktop_t **dp)
{
	for (monitor_t *m = mon_head; m; m = m->next) {
		for (desktop_t *d = m->desk_head; d; d = d->next) {
			if (d->id == id) {
				if (mp) *mp = m;
				if (dp) *dp = d;
				return true;
			}
		}
	}
	return false;
}

void backend_workspaces_update(void)
{
	if (!server.workspace_mgr) return;

	/* Drop groups and workspaces whose monitor or desktop is gone. */
	struct bspwm_wlr_ws_group *g, *gtmp;
	wl_list_for_each_safe(g, gtmp, &server.ws_groups, link) {
		bool alive = false;
		for (monitor_t *m = mon_head; m; m = m->next) {
			if (m->id == g->mon_id) { alive = true; break; }
		}
		if (!alive) {
			wl_list_remove(&g->link);
			wlr_ext_workspace_group_handle_v1_destroy(g->group);
			free(g);
		}
	}
	struct bspwm_wlr_ws *w, *wtmp;
	wl_list_for_each_safe(w, wtmp, &server.workspaces, link) {
		if (!desktop_exists(w->desk_id, NULL, NULL)) {
			wl_list_remove(&w->link);
			wlr_ext_workspace_handle_v1_destroy(w->ws);
			free(w);
		}
	}

	for (monitor_t *m = mon_head; m; m = m->next) {
		struct bspwm_wlr_ws_group *grp = NULL;
		wl_list_for_each(g, &server.ws_groups, link) {
			if (g->mon_id == m->id) { grp = g; break; }
		}
		struct wlr_output *out = wlr_output_from_output_id(m->output_id);
		if (!grp) {
			grp = calloc(1, sizeof(*grp));
			if (!grp) continue;
			grp->mon_id = m->id;
			grp->group = wlr_ext_workspace_group_handle_v1_create(server.workspace_mgr, 0);
			if (!grp->group) { free(grp); continue; }
			wl_list_insert(&server.ws_groups, &grp->link);
		}
		if (grp->output != out) {
			if (grp->output) wlr_ext_workspace_group_handle_v1_output_leave(grp->group, grp->output);
			if (out) wlr_ext_workspace_group_handle_v1_output_enter(grp->group, out);
			grp->output = out;
		}

		uint32_t idx = 0;
		for (desktop_t *d = m->desk_head; d; d = d->next, idx++) {
			struct bspwm_wlr_ws *ws = NULL;
			wl_list_for_each(w, &server.workspaces, link) {
				if (w->desk_id == d->id) { ws = w; break; }
			}
			if (!ws) {
				ws = calloc(1, sizeof(*ws));
				if (!ws) continue;
				char id[16];
				snprintf(id, sizeof(id), "%u", d->id);
				ws->desk_id = d->id;
				ws->ws = wlr_ext_workspace_handle_v1_create(server.workspace_mgr, id,
					EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
				if (!ws->ws) { free(ws); continue; }
				ws->ws->data = ws;
				ws->coord = UINT32_MAX;
				wl_list_insert(&server.workspaces, &ws->link);
			}
			if (ws->group != grp->group) {
				wlr_ext_workspace_handle_v1_set_group(ws->ws, grp->group);
				ws->group = grp->group;
			}
			if (strncmp(ws->name, d->name, sizeof(ws->name)) != 0) {
				snprintf(ws->name, sizeof(ws->name), "%s", d->name);
				wlr_ext_workspace_handle_v1_set_name(ws->ws, d->name);
			}
			if (ws->coord != idx) {
				ws->coord = idx;
				wlr_ext_workspace_handle_v1_set_coordinates(ws->ws, &idx, 1);
			}
			bool active = (d == m->desk);
			bool urgent = d->urgent_count > 0;
			if (ws->active != active) {
				ws->active = active;
				wlr_ext_workspace_handle_v1_set_active(ws->ws, active);
			}
			if (ws->urgent != urgent) {
				ws->urgent = urgent;
				wlr_ext_workspace_handle_v1_set_urgent(ws->ws, urgent);
			}
		}
	}
}

static void workspace_commit(struct wl_listener *listener, void *data)
{
	(void)listener;
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !req->activate.workspace) {
			continue;
		}
		struct bspwm_wlr_ws *ws = req->activate.workspace->data;
		monitor_t *m; desktop_t *d;
		if (ws && desktop_exists(ws->desk_id, &m, &d)) {
			focus_node(m, d, NULL);
		}
	}
	backend_workspaces_update();
}
void backend_ewmh_update_desktop_names(const char *names, size_t len) { (void)names; (void)len; }
void backend_ewmh_update_desktop_viewport(void) {}
void backend_ewmh_set_wm_desktop(bspwm_wid_t win, uint32_t desktop) { (void)win; (void)desktop; }
void backend_ewmh_update_client_list(bspwm_wid_t *list, uint32_t count, bool stacking) { (void)list; (void)count; (void)stacking; }
void backend_ewmh_wm_state_update(bspwm_wid_t win, uint16_t wm_flags) { (void)win; (void)wm_flags; }
void backend_ewmh_set_supporting(bspwm_wid_t win) { (void)win; }
bool backend_ewmh_handle_struts(bspwm_wid_t win)
{
	/* Wayland panels (waybar, etc.) reserve space via wlr-layer-shell's
	 * exclusive zone, NOT EWMH _NET_WM_STRUT_PARTIAL. Layer-shell zones
	 * are applied to monitor->padding in arrange_layers() above, which
	 * also re-tiles affected desktops. So there is nothing to do here:
	 * no Wayland-native client will ever set _NET_WM_STRUT.
	 *
	 * Returning false signals "no strut applied" — matching the X11
	 * backend's contract when the window has no strut property. */
	(void)win;
	return false;
}
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
		/* Effective size accounts for transform and scale; the raw mode
		 * size is wrong for a rotated or scaled output. */
		int ew = 0, eh = 0;
		wlr_output_effective_resolution(out->wlr_output, &ew, &eh);
		outputs[count].rect.width = ew;
		outputs[count].rect.height = eh;

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
		*win = window_id_from_node(node);
	}
}

void backend_warp_pointer(bspwm_rect_t rect)
{
	wlr_cursor_warp(server.cursor, NULL,
		rect.x + rect.width / 2.0, rect.y + rect.height / 2.0);
	/* Deliver leave/enter for the surfaces under the old and new spot. */
	process_cursor_motion(0);
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
	p->rect = wlr_scene_rect_create(server.window_tree, 1, 1, fcolor);
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
		return;
	}
	struct bspwm_wlr_xwayland_surface *xs = xsurface_from_id(win);
	if (xs) {
		wlr_xwayland_surface_close(xs->xsurface);
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
		return ((uint32_t)0xFF << 24) | ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
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
