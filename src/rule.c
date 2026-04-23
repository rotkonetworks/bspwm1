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
#include <sys/types.h>
#include <sys/epoll.h>
#include <string.h>
#include <unistd.h>
#include "bspwm.h"
#ifdef BACKEND_X11
#include "backend_x11.h"
#endif
#include "ewmh.h"
#include "window.h"
#include "query.h"
#include "parse.h"
#include "settings.h"
#include "rule.h"

rule_t *make_rule(void)
{
	rule_t *r = calloc(1, sizeof(rule_t));
	if (r == NULL) {
		return NULL;
	}
	r->class_name[0] = r->instance_name[0] = r->name[0] = r->effect[0] = '\0';
	r->next = r->prev = NULL;
	r->one_shot = false;
	return r;
}

void add_rule(rule_t *r)
 {
	if (rule_head == NULL) {
		rule_head = rule_tail = r;
	} else {
		rule_tail->next = r;
		r->prev = rule_tail;
		rule_tail = r;
	}
}

void remove_rule(rule_t *r)
{
	if (r == NULL) {
		return;
	}
	rule_t *prev = r->prev;
	rule_t *next = r->next;
	if (prev != NULL) {
		prev->next = next;
	}
	if (next != NULL) {
		next->prev = prev;
	}
	if (r == rule_head) {
		rule_head = next;
	}
	if (r == rule_tail) {
		rule_tail = prev;
	}
	free(r);
}

void remove_rule_by_cause(char *cause)
{
    if (cause == NULL || strlen(cause) >= MAXLEN * 3) {
        return;
    }

    rule_t *r = rule_head;
    struct tokenize_state state;
    char *class_name = tokenize_with_escape(&state, cause, COL_TOK[0]);
    char *instance_name = tokenize_with_escape(&state, NULL, COL_TOK[0]);
    char *name = tokenize_with_escape(&state, NULL, COL_TOK[0]);

    if (class_name && strnlen(class_name, MAXLEN) >= MAXLEN) {
        free(class_name);
        class_name = NULL;
    }
    if (instance_name && strnlen(instance_name, MAXLEN) >= MAXLEN) {
        free(instance_name);
        instance_name = NULL;
    }
    if (name && strnlen(name, MAXLEN) >= MAXLEN) {
        free(name);
        name = NULL;
    }

    if (!class_name || !instance_name || !name) {
        free(class_name);
        free(instance_name);
        free(name);
        return;
    }

    while (r != NULL) {
	    rule_t *next = r->next;
	    if ((class_name != NULL && (streq(class_name, MATCH_ANY) || streq(r->class_name, class_name))) &&
			    (instance_name == NULL || streq(instance_name, MATCH_ANY) || streq(r->instance_name, instance_name)) &&
			    (name == NULL || streq(name, MATCH_ANY) || streq(r->name, name))) {
		    remove_rule(r);
	    }
	    r = next;
    }
    free(class_name);
    free(instance_name);
    free(name);
}


bool remove_rule_by_index(int idx)
{
	for (rule_t *r = rule_head; r != NULL; r = r->next, idx--) {
		if (idx == 0) {
			remove_rule(r);
			return true;
		}
	}
	return false;
}

rule_consequence_t *make_rule_consequence(void)
{
	rule_consequence_t *rc = calloc(1, sizeof(rule_consequence_t));
	if (rc == NULL) {
		return NULL;
	}
	rc->manage = rc->focus = rc->border = true;
	rc->layer = NULL;
	rc->state = NULL;
	rc->rect = NULL;
	rc->honor_size_hints = HONOR_SIZE_HINTS_DEFAULT;
	return rc;
}

pending_rule_t *make_pending_rule(int fd, bspwm_wid_t win, rule_consequence_t *csq)
{
	pending_rule_t *pr = calloc(1, sizeof(pending_rule_t));
	if (pr == NULL) {
		return NULL;
	}
	pr->prev = pr->next = NULL;
	pr->event_head = pr->event_tail = NULL;
	pr->fd = fd;
	pr->win = win;
	pr->csq = csq;
	return pr;
}

void add_pending_rule(pending_rule_t *pr)
{
	if (pr == NULL) {
		return;
	}
	if (pending_rule_head == NULL) {
		pending_rule_head = pending_rule_tail = pr;
	} else {
		pending_rule_tail->next = pr;
		pr->prev = pending_rule_tail;
		pending_rule_tail = pr;
	}
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = pr->fd };
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pr->fd, &ev);
}

void remove_pending_rule(pending_rule_t *pr)
{
	if (pr == NULL) {
		return;
	}
	pending_rule_t *a = pr->prev;
	pending_rule_t *b = pr->next;
	if (a != NULL) {
		a->next = b;
	}
	if (b != NULL) {
		b->prev = a;
	}
	if (pr == pending_rule_head) {
		pending_rule_head = b;
	}
	if (pr == pending_rule_tail) {
		pending_rule_tail = a;
	}
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, pr->fd, NULL);
	close(pr->fd);
	free(pr->csq);
	event_queue_t *eq = pr->event_head;
	while (eq != NULL) {
		event_queue_t *next = eq->next;
		free(eq);
		eq = next;
	}
	free(pr);
}

void postpone_event(pending_rule_t *pr, void *evt)
{
	event_queue_t *eq = make_event_queue(evt);
	if (pr->event_tail == NULL) {
		pr->event_head = pr->event_tail = eq;
	} else {
		pr->event_tail->next = eq;
		eq->prev = pr->event_tail;
		pr->event_tail = eq;
	}
}

event_queue_t *make_event_queue(void *evt)
{
    if (!evt) return NULL;

    event_queue_t *eq = calloc(1, sizeof(event_queue_t));
    if (!eq) return NULL;

    /* Copy up to 64 bytes of event data (fits any backend event) */
    memcpy(eq->data, evt, sizeof(eq->data));

    eq->prev = eq->next = NULL;
    return eq;
}

#define SET_CSQ_SPLIT_DIR(val) \
	do { \
		if (csq->split_dir == NULL) { \
			csq->split_dir = calloc(1, sizeof(direction_t)); \
		} \
		*(csq->split_dir) = (val); \
	} while (0)

#define SET_CSQ_STATE(val) \
	do { \
		if (csq->state == NULL) { \
			csq->state = calloc(1, sizeof(client_state_t)); \
		} \
		*(csq->state) = (val); \
	} while (0)

#define SET_CSQ_LAYER(val) \
	do { \
		if (csq->layer == NULL) { \
			csq->layer = calloc(1, sizeof(stack_layer_t)); \
		} \
		*(csq->layer) = (val); \
	} while (0)

void _apply_window_type(bspwm_wid_t win, rule_consequence_t *csq)
{
	bspwm_window_type_t type;
	if (!backend_get_window_type(win, &type))
		return;

	switch (type) {
		case BSP_WINDOW_TYPE_TOOLBAR:
		case BSP_WINDOW_TYPE_UTILITY:
			csq->focus = false;
			break;
		case BSP_WINDOW_TYPE_DIALOG:
			SET_CSQ_STATE(STATE_FLOATING);
			csq->center = true;
			break;
		case BSP_WINDOW_TYPE_DOCK:
		case BSP_WINDOW_TYPE_DESKTOP:
		case BSP_WINDOW_TYPE_NOTIFICATION:
			csq->manage = false;
			if (type == BSP_WINDOW_TYPE_DESKTOP)
				window_lower(win);
			break;
		default:
			break;
	}
}

void _apply_window_state(bspwm_wid_t win, rule_consequence_t *csq)
{
#ifdef BACKEND_X11
	/* Query _NET_WM_STATE to set initial fullscreen/above/below/sticky */
	xcb_ewmh_get_atoms_reply_t wm_state;
	if (xcb_ewmh_get_wm_state_reply(ewmh, xcb_ewmh_get_wm_state(ewmh, win), &wm_state, NULL) == 1) {
		for (unsigned int i = 0; i < wm_state.atoms_len; i++) {
			xcb_atom_t a = wm_state.atoms[i];
			if (a == ewmh->_NET_WM_STATE_FULLSCREEN) {
				SET_CSQ_STATE(STATE_FULLSCREEN);
			} else if (a == ewmh->_NET_WM_STATE_BELOW) {
				SET_CSQ_LAYER(LAYER_BELOW);
			} else if (a == ewmh->_NET_WM_STATE_ABOVE) {
				SET_CSQ_LAYER(LAYER_ABOVE);
			} else if (a == ewmh->_NET_WM_STATE_STICKY) {
				csq->sticky = true;
			}
		}
		xcb_ewmh_get_atoms_reply_wipe(&wm_state);
	}
#else
	(void)win;
	(void)csq;
#endif
}

void _apply_transient(bspwm_wid_t win, rule_consequence_t *csq)
{
	bspwm_wid_t transient_for = BSPWM_WID_NONE;
	if (backend_get_transient_for(win, &transient_for) && transient_for != BSPWM_WID_NONE) {
		SET_CSQ_STATE(STATE_FLOATING);
	}
}

void _apply_hints(bspwm_wid_t win, rule_consequence_t *csq)
{
	bspwm_size_hints_t size_hints;
	if (backend_get_size_hints(win, &size_hints)) {
		if ((size_hints.flags & (BSP_SIZE_HINT_P_MIN_SIZE | BSP_SIZE_HINT_P_MAX_SIZE)) &&
		    size_hints.min_width == size_hints.max_width && size_hints.min_height == size_hints.max_height) {
			SET_CSQ_STATE(STATE_FLOATING);
		}
	}
}

void _apply_class(bspwm_wid_t win, rule_consequence_t *csq)
{
	backend_get_window_class(win, csq->class_name, csq->instance_name, sizeof(csq->class_name));
}

void _apply_name(bspwm_wid_t win, rule_consequence_t *csq)
{
	backend_get_window_name(win, csq->name, sizeof(csq->name));
}

void parse_keys_values(char *buf, rule_consequence_t *csq)
{
    if (buf == NULL)
        return;

    size_t len = strnlen(buf, BUFSIZ);
    if (len >= BUFSIZ)
        return;

    char *buf_copy = malloc(len + 1);
    if (buf_copy == NULL)
        return;

    memcpy(buf_copy, buf, len);
    buf_copy[len] = '\0';

    char *key = strtok(buf_copy, CSQ_BLK);
    char *value = strtok(NULL, CSQ_BLK);

    while (key != NULL && value != NULL) {
        if (strnlen(key, MAXLEN) < MAXLEN && strnlen(value, MAXLEN) < MAXLEN) {
            parse_key_value(key, value, csq);
        }
        key = strtok(NULL, CSQ_BLK);
        value = strtok(NULL, CSQ_BLK);
    }

    free(buf_copy);
}


void apply_rules(bspwm_wid_t win, rule_consequence_t *csq)
{
	/* Query window properties via backend */
	_apply_window_type(win, csq);
	_apply_window_state(win, csq);
	_apply_transient(win, csq);
	_apply_hints(win, csq);
	_apply_class(win, csq);
	_apply_name(win, csq);

	rule_t *rule = rule_head;
	while (rule != NULL) {
		rule_t *next = rule->next;
		if ((streq(rule->class_name, MATCH_ANY) || streq(rule->class_name, csq->class_name)) &&
		    (streq(rule->instance_name, MATCH_ANY) || streq(rule->instance_name, csq->instance_name)) &&
		    (streq(rule->name, MATCH_ANY) || streq(rule->name, csq->name))) {
			char effect[MAXLEN];
			snprintf(effect, sizeof(effect), "%s", rule->effect);
			parse_keys_values(effect, csq);
			if (rule->one_shot) {
				remove_rule(rule);
				break;
			}
		}
		rule = next;
	}
}

bool schedule_rules(bspwm_wid_t win, rule_consequence_t *csq)
{
	if (external_rules_command[0] == '\0') {
		return false;
	}
	resolve_rule_consequence(csq);
	int fds[2];
	if (pipe(fds) == -1) {
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		int dpy_fd = backend_get_fd();
		if (dpy_fd >= 0) {
			close(dpy_fd);
		}
		dup2(fds[1], 1);
		close(fds[0]);
		char wid[SMALEN];
		char *csq_buf;
		print_rule_consequence(&csq_buf, csq);
		snprintf(wid, sizeof(wid), "%i", win);
		setsid();
		execl(external_rules_command, external_rules_command, wid, csq->class_name, csq->instance_name, csq_buf, NULL);
		free(csq_buf);
		err("Couldn't spawn rule command.\n");
	} else if (pid > 0) {
		close(fds[1]);
		pending_rule_t *pr = make_pending_rule(fds[0], win, csq);
		add_pending_rule(pr);
	}
	return (pid != -1);
}

void parse_rule_consequence(int fd, rule_consequence_t *csq)
{
    if (fd == -1) {
        return;
    }
    char data[BUFSIZ];
    int nb;
    int total_read = 0;

    while ((nb = read(fd, data + total_read, sizeof(data) - total_read - 1)) > 0) {
        total_read += nb;
        if ((size_t)total_read >= sizeof(data) - 1) {

            break;
        }
    }

    if (total_read > 0) {
        data[total_read] = '\0';
        parse_keys_values(data, csq);
    }
}

void parse_key_value(char *key, char *value, rule_consequence_t *csq)
{
	bool v;
	if (streq("monitor", key)) {
		snprintf(csq->monitor_desc, sizeof(csq->monitor_desc), "%s", value);
	} else if (streq("desktop", key)) {
		snprintf(csq->desktop_desc, sizeof(csq->desktop_desc), "%s", value);
	} else if (streq("node", key)) {
		snprintf(csq->node_desc, sizeof(csq->node_desc), "%s", value);
	} else if (streq("split_dir", key)) {
		direction_t dir;
		if (parse_direction(value, &dir)) {
			SET_CSQ_SPLIT_DIR(dir);
		}
	} else if (streq("state", key)) {
		client_state_t cst;
		if (parse_client_state(value, &cst)) {
			SET_CSQ_STATE(cst);
		}
	} else if (streq("layer", key)) {
		stack_layer_t lyr;
		if (parse_stack_layer(value, &lyr)) {
			SET_CSQ_LAYER(lyr);
		}
	} else if (streq("split_ratio", key)) {
		double rat;
		if (sscanf(value, "%lf", &rat) == 1 && rat > 0 && rat < 1) {
			csq->split_ratio = rat;
		}
	} else if (streq("rectangle", key)) {
		if (csq->rect == NULL) {
			csq->rect = calloc(1, sizeof(bspwm_rect_t));
		}
		if (!parse_rectangle(value, csq->rect)) {
			free(csq->rect);
			csq->rect = NULL;
		}
	} else if (streq("honor_size_hints", key)) {
		if (!parse_honor_size_hints_mode(value, &csq->honor_size_hints)) {
			csq->honor_size_hints = HONOR_SIZE_HINTS_DEFAULT;
		}
	} else if (parse_bool(value, &v)) {
		if (streq("hidden", key)) {
			csq->hidden = v;
		}
#define SETCSQ(name) \
		else if (streq(#name, key)) { \
			csq->name = v; \
		}
		SETCSQ(sticky)
		SETCSQ(private)
		SETCSQ(locked)
		SETCSQ(marked)
		SETCSQ(center)
		SETCSQ(follow)
		SETCSQ(manage)
		SETCSQ(focus)
		SETCSQ(border)
#undef SETCSQ
	}
}

#undef SET_CSQ_LAYER
#undef SET_CSQ_STATE

void list_rules(FILE *rsp)
{
	for (rule_t *r = rule_head; r != NULL; r = r->next) {
		fprintf(rsp, "%s:%s:%s %c> %s\n", r->class_name, r->instance_name, r->name, r->one_shot?'-':'=', r->effect);
	}
}
