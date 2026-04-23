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
#include <stdbool.h>
#include "backend_x11.h"
#include "keybind.h"
#include "bspwm.h"
#include "ewmh.h"
#include "monitor.h"
#include "query.h"
#include "settings.h"
#include "subscribe.h"
#include "tree.h"
#include "window.h"
#include "pointer.h"
#include "rule.h"
#include "events.h"

/* randr_base is set in backend_x11.c */

typedef void (*event_handler_t)(void *);
static event_handler_t handlers[256] = {0};
static bool handlers_initialized = false;

static void init_handlers(void)
{
	if (handlers_initialized)
		return;

	handlers[XCB_MAP_REQUEST] = map_request;
	handlers[XCB_DESTROY_NOTIFY] = destroy_notify;
	handlers[XCB_UNMAP_NOTIFY] = unmap_notify;
	handlers[XCB_CLIENT_MESSAGE] = client_message;
	handlers[XCB_CONFIGURE_REQUEST] = configure_request;
	handlers[XCB_CONFIGURE_NOTIFY] = configure_notify;
	handlers[XCB_PROPERTY_NOTIFY] = property_notify;
	handlers[XCB_ENTER_NOTIFY] = enter_notify;
	handlers[XCB_MOTION_NOTIFY] = motion_notify;
	handlers[XCB_BUTTON_PRESS] = button_press;
	handlers[XCB_FOCUS_IN] = focus_in;
	handlers[XCB_KEY_PRESS] = key_press;
	handlers[XCB_MAPPING_NOTIFY] = mapping_notify;
	handlers[0] = process_error;

	handlers_initialized = true;
}

void handle_event(void *evt)
{
	if (!evt)
		return;
		
	init_handlers();
	
	uint8_t resp_type = ((xcb_generic_event_t *)evt)->response_type & 0x7f;
	
	if (handlers[resp_type]) {
		handlers[resp_type](evt);
	} else if (randr && resp_type == randr_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
		update_monitors();
	}
}

void map_request(void *evt)
{
	if (!evt)
		return;
		
	xcb_map_request_event_t *e = (xcb_map_request_event_t *) evt;
	schedule_window(e->window);
}

void configure_request(void *evt)
{
	if (!evt)
		return;
		
	xcb_configure_request_event_t *e = (xcb_configure_request_event_t *) evt;
	coordinates_t loc;
	bool is_managed = locate_window(e->window, &loc);
	client_t *c = (is_managed && loc.node) ? loc.node->client : NULL;
	uint16_t width, height;

	if (!is_managed) {
		uint16_t mask = 0;
		uint32_t values[7];
		unsigned short i = 0;

		#define ADD_VALUE(flag, val) \
			if ((e->value_mask & flag) && i < 7) { \
				mask |= flag; \
				values[i++] = val; \
			}

		ADD_VALUE(XCB_CONFIG_WINDOW_X, e->x)
		ADD_VALUE(XCB_CONFIG_WINDOW_Y, e->y)
		ADD_VALUE(XCB_CONFIG_WINDOW_WIDTH, e->width)
		ADD_VALUE(XCB_CONFIG_WINDOW_HEIGHT, e->height)
		ADD_VALUE(XCB_CONFIG_WINDOW_BORDER_WIDTH, e->border_width)
		ADD_VALUE(XCB_CONFIG_WINDOW_SIBLING, e->sibling)
		ADD_VALUE(XCB_CONFIG_WINDOW_STACK_MODE, e->stack_mode)
		#undef ADD_VALUE

		xcb_configure_window(dpy, e->window, mask, values);

	} else if (c && IS_FLOATING(c)) {
		width = c->floating_rectangle.width;
		height = c->floating_rectangle.height;

		if (e->value_mask & XCB_CONFIG_WINDOW_X)
			c->floating_rectangle.x = e->x;
		if (e->value_mask & XCB_CONFIG_WINDOW_Y)
			c->floating_rectangle.y = e->y;
		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
			width = e->width;
		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
			height = e->height;

		apply_size_hints(c, &width, &height);
		c->floating_rectangle.width = width;
		c->floating_rectangle.height = height;
		bspwm_rect_t r = c->floating_rectangle;

		if (c->border_width <= (unsigned)(INT16_MAX - r.x))
			r.x -= c->border_width;
		if (c->border_width <= (unsigned)(INT16_MAX - r.y))
			r.y -= c->border_width;

		window_move_resize(e->window, r.x, r.y, r.width, r.height);

		if (loc.monitor && loc.desktop) {
			put_status(SBSC_MASK_NODE_GEOMETRY, "node_geometry 0x%08X 0x%08X 0x%08X %ux%u+%i+%i\n",
			          loc.monitor->id, loc.desktop->id, e->window, r.width, r.height, r.x, r.y);

			monitor_t *m = monitor_from_client(c);
			if (m && m != loc.monitor) {
				transfer_node(loc.monitor, loc.desktop, loc.node, m, m->desk, 
				             m->desk ? m->desk->focus : NULL, false);
			}
		}
	} else if (c) {
		if (c->state == STATE_PSEUDO_TILED) {
			width = c->floating_rectangle.width;
			height = c->floating_rectangle.height;
			if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
				width = e->width;
			if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
				height = e->height;
			apply_size_hints(c, &width, &height);
			if (width != c->floating_rectangle.width || height != c->floating_rectangle.height) {
				c->floating_rectangle.width = width;
				c->floating_rectangle.height = height;
				if (loc.monitor && loc.desktop)
					arrange(loc.monitor, loc.desktop);
			}
		}

		unsigned int bw = c->border_width;
		bspwm_rect_t r = (IS_FULLSCREEN(c) && loc.monitor) ?
		                    loc.monitor->rectangle : c->tiled_rectangle;

		backend_send_configure_notify(e->window, r, bw);
	}
}

void configure_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *) evt;

	if (e->window == root) {
		screen_width = e->width;
		screen_height = e->height;
	}
}

void destroy_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *) evt;
	unmanage_window(e->window);
}

void unmap_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *) evt;

	if (e->window == motion_recorder.id) {
		motion_recorder.sequence = e->sequence;
		return;
	}

	if (!window_exists(e->window))
		return;

	set_window_state(e->window, BSP_WM_STATE_WITHDRAWN);
	unmanage_window(e->window);
}

void property_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_property_notify_event_t *e = (xcb_property_notify_event_t *) evt;

	if (!ignore_ewmh_struts && e->atom == ewmh->_NET_WM_STRUT_PARTIAL && ewmh_handle_struts(e->window)) {
		for (monitor_t *m = mon_head; m; m = m->next) {
			for (desktop_t *d = m->desk_head; d; d = d->next) {
				arrange(m, d);
			}
		}
	}

	if (e->atom != XCB_ATOM_WM_HINTS && e->atom != XCB_ATOM_WM_NORMAL_HINTS)
		return;

	coordinates_t loc;
	if (!locate_window(e->window, &loc)) {
		for (pending_rule_t *pr = pending_rule_head; pr; pr = pr->next) {
			if (pr->win == e->window) {
				postpone_event(pr, evt);
				break;
			}
		}
		return;
	}

	if (!loc.node || !loc.node->client)
		return;

	if (e->atom == XCB_ATOM_WM_HINTS) {
		bool urgent = backend_get_urgency(e->window);
		set_urgent(loc.monitor, loc.desktop, loc.node, urgent);
	} else if (e->atom == XCB_ATOM_WM_NORMAL_HINTS) {
		client_t *c = loc.node->client;
		if (backend_get_size_hints(e->window, &c->size_hints)) {
			if (loc.monitor && loc.desktop)
				arrange(loc.monitor, loc.desktop);
		}
	}
}

void client_message(void *evt)
{
	if (!evt)
		return;
		
	xcb_client_message_event_t *e = (xcb_client_message_event_t *) evt;

	if (e->type == ewmh->_NET_CURRENT_DESKTOP) {
		coordinates_t loc;
		if (ewmh_locate_desktop(e->data.data32[0], &loc)) {
			if (loc.monitor && loc.desktop)
				focus_node(loc.monitor, loc.desktop, loc.desktop->focus);
		}
		return;
	}

	coordinates_t loc;
	if (!locate_window(e->window, &loc)) {
		for (pending_rule_t *pr = pending_rule_head; pr; pr = pr->next) {
			if (pr->win == e->window) {
				postpone_event(pr, evt);
				break;
			}
		}
		return;
	}

	if (!loc.monitor || !loc.desktop || !loc.node)
		return;

	if (e->type == ewmh->_NET_WM_STATE) {
		handle_state(loc.monitor, loc.desktop, loc.node, e->data.data32[1], e->data.data32[0]);
		handle_state(loc.monitor, loc.desktop, loc.node, e->data.data32[2], e->data.data32[0]);
	} else if (e->type == ewmh->_NET_ACTIVE_WINDOW) {
		if ((ignore_ewmh_focus && e->data.data32[0] == XCB_EWMH_CLIENT_SOURCE_TYPE_NORMAL) ||
		    (mon && mon->desk && loc.node == mon->desk->focus)) {
			return;
		}
		focus_node(loc.monitor, loc.desktop, loc.node);
	} else if (e->type == ewmh->_NET_WM_DESKTOP) {
		coordinates_t dloc;
		if (ewmh_locate_desktop(e->data.data32[0], &dloc)) {
			if (dloc.monitor && dloc.desktop) {
				transfer_node(loc.monitor, loc.desktop, loc.node, dloc.monitor, 
				             dloc.desktop, dloc.desktop->focus, false);
			}
		}
	} else if (e->type == ewmh->_NET_CLOSE_WINDOW) {
		close_node(loc.node);
	}
}

void focus_in(void *evt)
{
	if (!evt)
		return;
		
	xcb_focus_in_event_t *e = (xcb_focus_in_event_t *) evt;

	if (e->mode == XCB_NOTIFY_MODE_GRAB || e->mode == XCB_NOTIFY_MODE_UNGRAB ||
	    e->detail == XCB_NOTIFY_DETAIL_POINTER || e->detail == XCB_NOTIFY_DETAIL_POINTER_ROOT ||
	    e->detail == XCB_NOTIFY_DETAIL_NONE) {
		return;
	}

	if (mon && mon->desk && mon->desk->focus) {
		if (e->event == mon->desk->focus->id)
			return;
		if (e->event == root) {
			/* Some clients expect the window manager to refocus the
			   focused window in this case. Temporarily disable
			   pointer_follows_focus to prevent unwanted warping */
			bool pff = pointer_follows_focus;
			pointer_follows_focus = false;
			focus_node(mon, mon->desk, mon->desk->focus);
			pointer_follows_focus = pff;
			return;
		}
	}

	coordinates_t loc;
	if (locate_window(e->event, &loc)) {
		update_input_focus();
	}
}

void button_press(void *evt)
{
	if (!evt)
		return;
		
	xcb_button_press_event_t *e = (xcb_button_press_event_t *) evt;
	bool replay = false;
	
	for (unsigned int i = 0; i < LENGTH(BUTTONS); i++) {
		if (e->detail != BUTTONS[i])
			continue;
			
		if ((click_to_focus == (int8_t) XCB_BUTTON_INDEX_ANY || 
		     click_to_focus == (int8_t) BUTTONS[i]) &&
		    cleaned_mask(e->state) == XCB_NONE) {
			bool pff = pointer_follows_focus;
			bool pfm = pointer_follows_monitor;
			pointer_follows_focus = false;
			pointer_follows_monitor = false;
			replay = !grab_pointer(ACTION_FOCUS) || !swallow_first_click;
			pointer_follows_focus = pff;
			pointer_follows_monitor = pfm;
		} else if (i < LENGTH(pointer_actions)) {
			grab_pointer(pointer_actions[i]);
		}
	}
	
	xcb_allow_events(dpy, replay ? XCB_ALLOW_REPLAY_POINTER : XCB_ALLOW_SYNC_POINTER, e->time);
	xcb_flush(dpy);
}

void enter_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *) evt;
	xcb_window_t win = e->event;

	if (e->mode != XCB_NOTIFY_MODE_NORMAL || e->detail == XCB_NOTIFY_DETAIL_INFERIOR)
		return;

	if (motion_recorder.enabled && motion_recorder.sequence == e->sequence)
		return;

	if (win == (mon ? mon->root : 0))
		return;
		
	if (mon && mon->desk && mon->desk->focus) {
		if (win == mon->desk->focus->id)
			return;
		if (mon->desk->focus->presel && win == mon->desk->focus->presel->feedback)
			return;
	}

	update_motion_recorder();
}

void motion_notify(void *evt)
{
	if (!evt)
		return;
		
	xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *) evt;

	static uint16_t last_motion_x = 0, last_motion_y = 0;
	static xcb_timestamp_t last_motion_time = 0;

	int64_t dtime = e->time - last_motion_time;

	if (dtime > 1000) {
		last_motion_time = e->time;
		last_motion_x = e->event_x;
		last_motion_y = e->event_y;
		return;
	}
	
	int mdist = 0;
	if (e->event_x > last_motion_x)
		mdist += e->event_x - last_motion_x;
	else
		mdist += last_motion_x - e->event_x;
		
	if (e->event_y > last_motion_y)
		mdist += e->event_y - last_motion_y;
	else
		mdist += last_motion_y - e->event_y;
		
	if (mdist < 10)
		return;

	disable_motion_recorder();

	xcb_window_t win = XCB_NONE;
	query_pointer(&win, NULL);
	coordinates_t loc;
	bool pff = pointer_follows_focus;
	bool pfm = pointer_follows_monitor;
	pointer_follows_focus = false;
	pointer_follows_monitor = false;
	auto_raise = false;

	if (locate_window(win, &loc)) {
		if (loc.monitor && loc.desktop && loc.node &&
		    loc.monitor->desk == loc.desktop && 
		    mon && mon->desk && loc.node != mon->desk->focus) {
			focus_node(loc.monitor, loc.desktop, loc.node);
		}
	} else {
		bspwm_point_t pt = {e->root_x, e->root_y};
		monitor_t *m = monitor_from_point(pt);
		if (m && m != mon && m->desk) {
			focus_node(m, m->desk, m->desk->focus);
		}
	}

	pointer_follows_focus = pff;
	pointer_follows_monitor = pfm;
	auto_raise = true;
}

void handle_state(monitor_t *m, desktop_t *d, node_t *n, uint32_t state, unsigned int action)
{
	if (!m || !d || !n || !n->client)
		return;

	if (state == ewmh->_NET_WM_STATE_FULLSCREEN) {
		if (action == XCB_EWMH_WM_STATE_ADD && 
		    (ignore_ewmh_fullscreen & STATE_TRANSITION_ENTER) == 0) {
			set_state(m, d, n, STATE_FULLSCREEN);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE && 
		           (ignore_ewmh_fullscreen & STATE_TRANSITION_EXIT) == 0) {
			if (n->client->state == STATE_FULLSCREEN) {
				set_state(m, d, n, n->client->last_state);
			}
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			client_state_t next_state = IS_FULLSCREEN(n->client) ? 
			                           n->client->last_state : STATE_FULLSCREEN;
			if ((next_state == STATE_FULLSCREEN && 
			     (ignore_ewmh_fullscreen & STATE_TRANSITION_ENTER) == 0) ||
			    (next_state != STATE_FULLSCREEN && 
			     (ignore_ewmh_fullscreen & STATE_TRANSITION_EXIT) == 0)) {
				set_state(m, d, n, next_state);
			}
		}
		arrange(m, d);
	} else if (state == ewmh->_NET_WM_STATE_BELOW) {
		if (action == XCB_EWMH_WM_STATE_ADD) {
			set_layer(m, d, n, LAYER_BELOW);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) {
			if (n->client->layer == LAYER_BELOW) {
				set_layer(m, d, n, n->client->last_layer);
			}
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			set_layer(m, d, n, n->client->layer == LAYER_BELOW ? 
			         n->client->last_layer : LAYER_BELOW);
		}
	} else if (state == ewmh->_NET_WM_STATE_ABOVE) {
		if (action == XCB_EWMH_WM_STATE_ADD) {
			set_layer(m, d, n, LAYER_ABOVE);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) {
			if (n->client->layer == LAYER_ABOVE) {
				set_layer(m, d, n, n->client->last_layer);
			}
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			set_layer(m, d, n, n->client->layer == LAYER_ABOVE ? 
			         n->client->last_layer : LAYER_ABOVE);
		}
	} else if (state == ewmh->_NET_WM_STATE_HIDDEN) {
		if (action == XCB_EWMH_WM_STATE_ADD) {
			set_hidden(m, d, n, true);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) {
			set_hidden(m, d, n, false);
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			set_hidden(m, d, n, !n->hidden);
		}
	} else if (state == ewmh->_NET_WM_STATE_STICKY) {
		if (action == XCB_EWMH_WM_STATE_ADD) {
			set_sticky(m, d, n, true);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) {
			set_sticky(m, d, n, false);
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			set_sticky(m, d, n, !n->sticky);
		}
	} else if (state == ewmh->_NET_WM_STATE_DEMANDS_ATTENTION) {
		if (action == XCB_EWMH_WM_STATE_ADD) {
			set_urgent(m, d, n, true);
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) {
			set_urgent(m, d, n, false);
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) {
			set_urgent(m, d, n, !n->client->urgent);
		}
#define HANDLE_WM_STATE(s)  \
	} else if (state == ewmh->_NET_WM_STATE_##s) { \
		if (action == XCB_EWMH_WM_STATE_ADD) { \
			n->client->wm_flags |= WM_FLAG_##s; \
		} else if (action == XCB_EWMH_WM_STATE_REMOVE) { \
			n->client->wm_flags &= ~WM_FLAG_##s; \
		} else if (action == XCB_EWMH_WM_STATE_TOGGLE) { \
			n->client->wm_flags ^= WM_FLAG_##s; \
		} \
		ewmh_wm_state_update(n);
	HANDLE_WM_STATE(MODAL)
	HANDLE_WM_STATE(MAXIMIZED_VERT)
	HANDLE_WM_STATE(MAXIMIZED_HORZ)
	HANDLE_WM_STATE(SHADED)
	HANDLE_WM_STATE(SKIP_TASKBAR)
	HANDLE_WM_STATE(SKIP_PAGER)
	}
#undef HANDLE_WM_STATE
}

void mapping_notify(void *evt)
{
	if (!evt || mapping_events_count == 0)
		return;

	xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *) evt;

	if (e->request == XCB_MAPPING_POINTER)
		return;

	if (mapping_events_count > 0)
		mapping_events_count--;

	ungrab_buttons();
	grab_buttons();
	backend_grab_keys();
}

void key_press(void *evt)
{
	if (!evt)
		return;

	xcb_key_press_event_t *e = (xcb_key_press_event_t *)evt;

	xcb_key_symbols_t *syms = xcb_key_symbols_alloc(dpy);
	if (!syms)
		return;

	xcb_keysym_t keysym = xcb_key_symbols_get_keysym(syms, e->detail, 0);
	xcb_key_symbols_free(syms);

	/* Strip lock-key modifiers to match our KBMOD_* flags */
	uint32_t modifiers = e->state & (BSP_MOD_MASK_SHIFT | BSP_MOD_MASK_CONTROL |
	                                  BSP_MOD_MASK_1 | BSP_MOD_MASK_2 |
	                                  BSP_MOD_MASK_3 | BSP_MOD_MASK_4 | BSP_MOD_MASK_5);

	const char *cmd = keybind_match(modifiers, keysym);
	if (cmd) {
		keybind_exec(cmd);
	}
}

void process_error(void *evt)
{
	if (!evt)
		return;
		
	xcb_request_error_t *e = (xcb_request_error_t *) evt;
	
	if (e->error_code == ERROR_CODE_BAD_WINDOW)
		return;
		
	warn("Failed request: %s, %s: 0x%08X.\n", 
	     xcb_event_get_request_label(e->major_opcode), 
	     xcb_event_get_error_label(e->error_code), 
	     e->bad_value);
}
