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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include "types.h"
#include "desktop.h"
#include "monitor.h"
#include "settings.h"
#include "messages.h"
#include "pointer.h"
#include "events.h"
#include "common.h"
#include "window.h"
#include "history.h"
#include "ewmh.h"
#include "rule.h"
#include "restore.h"
#include "query.h"
#include "keybind.h"
#include "bspwm.h"

#ifdef BACKEND_X11
#include "backend_x11.h"
#endif

/* Global state */
int default_screen, screen_width, screen_height;
uint32_t clients_count;
bspwm_wid_t root;
char config_path[MAXLEN];

monitor_t *mon;
monitor_t *mon_head;
monitor_t *mon_tail;
monitor_t *pri_mon;
history_t *history_head;
history_t *history_tail;
history_t *history_needle;
rule_t *rule_head;
rule_t *rule_tail;
stacking_list_t *stack_head;
stacking_list_t *stack_tail;
subscriber_list_t *subscribe_head;
subscriber_list_t *subscribe_tail;
pending_rule_t *pending_rule_head;
pending_rule_t *pending_rule_tail;

bspwm_wid_t meta_window;
motion_recorder_t motion_recorder;
int exit_status;
int epoll_fd;

bool auto_raise;
bool sticky_still;
bool hide_sticky;
bool record_history;
volatile sig_atomic_t running;
volatile bool restart;
bool randr;

int main(int argc, char *argv[])
{
	char socket_path[MAXLEN];
	char state_path[MAXLEN] = {0};
	int run_level = 0;
	config_path[0] = '\0';
	int sock_fd = -1, cli_fd, dpy_fd, n;
	struct sockaddr_un sock_address;
	char msg[BUFSIZ] = {0};
	char *end;
	int opt;

	while ((opt = getopt(argc, argv, "hvc:s:o:")) != -1) {
		switch (opt) {
			case 'h':
				printf(WM_NAME " [-h|-v|-c CONFIG_PATH]\n");
				exit(EXIT_SUCCESS);
				break;
			case 'v':
				printf("%s\n", VERSION);
				exit(EXIT_SUCCESS);
				break;
			case 'c':
				snprintf(config_path, sizeof(config_path), "%s", optarg);
				break;
			case 's':
				run_level |= 1;
				snprintf(state_path, sizeof(state_path), "%s", optarg);
				break;
			case 'o':
				run_level |= 2;
				sock_fd = strtol(optarg, &end, 0);
				if (*end != '\0') {
					sock_fd = -1;
				}
				break;
		}
	}

	if (config_path[0] == '\0') {
		char *config_home = getenv(CONFIG_HOME_ENV);
		if (config_home != NULL) {
			snprintf(config_path, sizeof(config_path), "%s/%s/%s", config_home, WM_NAME, CONFIG_NAME);
		} else {
			snprintf(config_path, sizeof(config_path), "%s/%s/%s/%s", getenv("HOME"), ".config", WM_NAME, CONFIG_NAME);
		}
	}

	if (backend_init(&default_screen) != 0) {
		err("Can't open display.\n");
	}

	if (!backend_check_connection()) {
		exit(EXIT_FAILURE);
	}

	load_settings();
	setup();

	if (state_path[0] != '\0') {
		restore_state(state_path);
		unlink(state_path);
		adopt_orphans();
	}

	dpy_fd = backend_get_fd();

	if (sock_fd == -1) {
		char *sp = getenv(SOCKET_ENV_VAR);
		if (sp != NULL) {
			snprintf(socket_path, sizeof(socket_path), "%s", sp);
		} else {
			char *host = NULL;
			int dn = 0, sn = 0;
			if (backend_parse_display(&host, &dn, &sn)) {
				snprintf(socket_path, sizeof(socket_path), SOCKET_PATH_TPL, host, dn, sn);
			}
			free(host);
		}

		sock_address.sun_family = AF_UNIX;

		size_t socket_path_len = strlen(socket_path);
		if (socket_path_len >= sizeof(sock_address.sun_path)) {
			err("Socket path too long (%zu >= %zu): %s\n",
			    socket_path_len, sizeof(sock_address.sun_path), socket_path);
		}

		strcpy(sock_address.sun_path, socket_path);

		sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

		if (sock_fd == -1) {
			err("Couldn't create the socket.\n");
		}

		unlink(socket_path);

		if (bind(sock_fd, (struct sockaddr *) &sock_address, sizeof(sock_address)) == -1) {
			err("Couldn't bind a name to the socket.\n");
		}

		chmod(socket_path, 0600);

		if (listen(sock_fd, SOMAXCONN) == -1) {
			err("Couldn't listen to the socket.\n");
		}
	}

	fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(sock_fd, F_GETFD));

	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd == -1) {
		err("Couldn't create epoll instance.\n");
	}

	struct epoll_event ep_ev;
	ep_ev.events = EPOLLIN;
	ep_ev.data.fd = sock_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ep_ev);
	ep_ev.data.fd = dpy_fd;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dpy_fd, &ep_ev);

	struct sigaction sigact;
	sigact.sa_handler = sig_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGHUP, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
	run_config(run_level);
	backend_grab_keys();
	running = true;

	#define MAX_EPOLL_EVENTS 16
	struct epoll_event ep_events[MAX_EPOLL_EVENTS];

	while (running) {

		backend_flush();

		int nfds = epoll_wait(epoll_fd, ep_events, MAX_EPOLL_EVENTS, -1);
		if (nfds == -1) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (int i = 0; i < nfds; i++) {
			int fd = ep_events[i].data.fd;

			/* Check pending rules */
			bool handled = false;
			pending_rule_t *pr = pending_rule_head;
			while (pr != NULL) {
				pending_rule_t *next = pr->next;
				if (pr->fd == fd) {
					if (manage_window(pr->win, pr->csq, pr->fd)) {
						for (event_queue_t *eq = pr->event_head; eq != NULL; eq = eq->next) {
							handle_event(eq->data);
						}
					}
					remove_pending_rule(pr);
					handled = true;
					break;
				}
				pr = next;
			}
			if (handled)
				continue;

			if (fd == sock_fd) {
				cli_fd = accept(sock_fd, NULL, 0);
				if (cli_fd > 0) {
					struct ucred cred;
					socklen_t len = sizeof(cred);
					if (getsockopt(cli_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1 ||
					    cred.uid != getuid()) {
						close(cli_fd);
						continue;
					}
				}
				if (cli_fd > 0 && (n = recv(cli_fd, msg, sizeof(msg)-1, 0)) > 0) {
					msg[n] = '\0';
					FILE *rsp = fdopen(cli_fd, "w");
					if (rsp != NULL) {
						handle_message(msg, n, rsp);
						scratch_reset();
					} else {
						warn("Can't open the client socket as file.\n");
						close(cli_fd);
					}
				}
			} else if (fd == dpy_fd) {
#ifdef BACKEND_X11
				xcb_aux_sync(dpy);
				xcb_generic_event_t *event;
				while ((event = xcb_poll_for_event(dpy)) != NULL) {
					handle_event(event);
					free(event);
				}
#else
				backend_dispatch_events();
#endif
			}
		}

		if (!backend_check_connection()) {
			running = false;
		}

		static unsigned int prune_counter;
		if (++prune_counter >= 64) {
			prune_counter = 0;
			prune_dead_subscribers();
		}
	}

	if (restart) {
		char *host = NULL;
		int dn = 0, sn = 0;
		if (backend_parse_display(&host, &dn, &sn)) {
			snprintf(state_path, sizeof(state_path), STATE_PATH_TPL, host, dn, sn);
		}
		free(host);
		FILE *f = fopen(state_path, "w");
		query_state(f);
		fclose(f);
	}

	cleanup();
	scratch_destroy();
	ungrab_buttons();
	backend_destroy_window(meta_window);
	backend_destroy_window(motion_recorder.id);
	backend_destroy();

	if (restart) {
		fcntl(sock_fd, F_SETFD, ~FD_CLOEXEC & fcntl(sock_fd, F_GETFD));

		int rargc;
		for (rargc = 0; rargc < argc; rargc++) {
			if (streq("-s", argv[rargc])) {
				break;
			}
		}

		size_t len = (size_t)rargc + 5;
		char **rargv = safe_malloc_array(len, sizeof(char *));
		if (rargv != NULL) {
			for (int i = 0; i < rargc; i++) {
				rargv[i] = argv[i];
			}

			char sock_fd_arg[SMALEN];
			snprintf(sock_fd_arg, sizeof(sock_fd_arg), "%i", sock_fd);

			rargv[rargc] = "-s";
			rargv[rargc + 1] = state_path;
			rargv[rargc + 2] = "-o";
			rargv[rargc + 3] = sock_fd_arg;
			rargv[rargc + 4] = 0;

			execvp(*rargv, rargv);
			free(rargv);
		} else {
			warn("Failed to allocate restart argv\n");
		}
		exit_status = 1;
	}

	close(epoll_fd);
	close(sock_fd);
	unlink(socket_path);

	return exit_status;
}

void init(void)
{
	clients_count = 0;
	mon = mon_head = mon_tail = pri_mon = NULL;
	history_head = history_tail = history_needle = NULL;
	rule_head = rule_tail = NULL;
	stack_head = stack_tail = NULL;
	subscribe_head = subscribe_tail = NULL;
	pending_rule_head = pending_rule_tail = NULL;
	auto_raise = sticky_still = hide_sticky = record_history = true;
	exit_status = 0;
	restart = false;
}

void setup(void)
{
	init();
	scratch_init();
	keybind_init();

#ifdef BACKEND_X11
	backend_ewmh_init();
	pointer_init();
	x11_setup_screen();
	root = backend_get_root();
	if (!backend_register_root_events()) {
		backend_destroy();
		err("Another window manager is already running.\n");
	}
	backend_get_screen_size(&screen_width, &screen_height);

	/* Create internal windows via backend */
	bspwm_rect_t meta_rect = {-1, -1, 1, 1};
	meta_window = backend_create_internal_window("meta", meta_rect, true);

	bspwm_rect_t mr_rect = {0, 0, 1, 1};
	motion_recorder.id = backend_create_internal_window("motion_recorder", mr_rect, true);
	motion_recorder.sequence = 0;
	motion_recorder.enabled = false;

	x11_setup_ewmh_supported();
	ewmh_set_supporting(meta_window);
	x11_setup_atoms();
	x11_setup_randr();

	if (randr && update_monitors()) {
		/* monitors set up via RandR */
	} else if (!randr || !mon_head) {
		randr = false;
		warn("Couldn't retrieve monitors via RandR.\n");
		/* Fallback: single screen */
		bspwm_rect_t rect = {0, 0, screen_width, screen_height};
		monitor_t *m = make_monitor(NULL, &rect, BSPWM_WID_NONE);
		add_monitor(m);
		add_desktop(m, make_desktop(NULL, BSPWM_WID_NONE));
	}
#endif

#ifndef BACKEND_X11
	/* Wayland: outputs were created during backend_init, but setup
	 * wasn't ready yet. Re-query outputs now. */
	root = backend_get_root();
	backend_get_screen_size(&screen_width, &screen_height);
	if (!update_monitors()) {
		bspwm_rect_t rect = {0, 0, screen_width, screen_height};
		monitor_t *m = make_monitor(NULL, &rect, BSPWM_WID_NONE);
		add_monitor(m);
		add_desktop(m, make_desktop(NULL, BSPWM_WID_NONE));
	}
	if (mon_head && !mon)
		mon = mon_head;
#endif

	ewmh_update_number_of_desktops();
	ewmh_update_desktop_names();
	ewmh_update_desktop_viewport();
	ewmh_update_current_desktop();
	clear_input_focus();
}

void cleanup(void)
{
	mon = NULL;

	while (mon_head != NULL) {
		remove_monitor(mon_head);
	}
	while (rule_head != NULL) {
		remove_rule(rule_head);
	}
	while (subscribe_head != NULL) {
		remove_subscriber(subscribe_head);
	}
	while (pending_rule_head != NULL) {
		remove_pending_rule(pending_rule_head);
	}

	empty_history();
}

void sig_handler(int sig)
{
	if (sig == SIGCHLD) {
		while (waitpid(-1, 0, WNOHANG) > 0)
			;
	} else if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM) {
		running = 0;
	}
}
