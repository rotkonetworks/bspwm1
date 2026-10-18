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

/* Control socket clients.
 *
 * Requests are read without blocking, so a client that connects and stalls
 * cannot hold up the event loop. A request is complete when the client shuts
 * down its write side (bspc does). Clients that don't (polybar, scripts
 * talking to the socket directly) send the whole request in one write and
 * wait; for them a request ending in NUL is taken as complete once no more
 * data arrives for IPC_IDLE_MS. */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "bspwm.h"
#include "common.h"
#include "helpers.h"
#include "messages.h"
#include "ipc.h"

#define IPC_IDLE_MS     10
#define IPC_TIMEOUT_MS  2000

typedef struct ipc_client_t ipc_client_t;
struct ipc_client_t {
	int fd;
	char *buf;
	size_t len, cap;
	long long idle_at;     /* dispatch at this time, or 0 */
	long long expire_at;   /* drop at this time */
	ipc_client_t *next;
};

static ipc_client_t *clients;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void unlink_client(ipc_client_t *c)
{
	for (ipc_client_t **p = &clients; *p != NULL; p = &(*p)->next) {
		if (*p == c) {
			*p = c->next;
			break;
		}
	}
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
	free(c->buf);
	free(c);
}

static void drop_client(ipc_client_t *c)
{
	int fd = c->fd;
	unlink_client(c);
	close(fd);
}

static void dispatch(ipc_client_t *c)
{
	int fd = c->fd;
	char *msg = c->buf;
	size_t len = c->len;
	c->buf = NULL;
	unlink_client(c);

	/* Replies are written synchronously; the send timeout bounds a client
	 * that never reads. */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	struct timeval tv = {.tv_sec = IPC_TIMEOUT_MS / 1000};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	FILE *rsp = fdopen(fd, "w");
	if (rsp == NULL) {
		warn("Can't open the client socket as file.\n");
		close(fd);
	} else {
		handle_message(msg, len, rsp);
	}
	free(msg);
}

void ipc_accept(int sock_fd)
{
	int fd = accept4(sock_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd == -1) {
		return;
	}
	struct ucred cred;
	socklen_t len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1 ||
	    cred.uid != getuid()) {
		close(fd);
		return;
	}
	ipc_client_t *c = calloc(1, sizeof(ipc_client_t));
	if (c == NULL) {
		close(fd);
		return;
	}
	c->fd = fd;
	c->expire_at = now_ms() + IPC_TIMEOUT_MS;
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		free(c);
		close(fd);
		return;
	}
	c->next = clients;
	clients = c;
}

bool ipc_handle(int fd)
{
	ipc_client_t *c = clients;
	while (c != NULL && c->fd != fd) {
		c = c->next;
	}
	if (c == NULL) {
		return false;
	}

	while (true) {
		if (c->len >= IPC_MAX_MSG) {
			static const char too_long[] = FAILURE_MESSAGE "Message too long.\n";
			send(fd, too_long, sizeof(too_long) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
			drop_client(c);
			return true;
		}
		if (c->cap - c->len < BUFSIZ && c->cap < IPC_MAX_MSG + 1) {
			size_t cap = c->cap == 0 ? BUFSIZ : c->cap * 2;
			if (cap > IPC_MAX_MSG + 1) {
				cap = IPC_MAX_MSG + 1;
			}
			char *buf = realloc(c->buf, cap);
			if (buf == NULL) {
				drop_client(c);
				return true;
			}
			c->buf = buf;
			c->cap = cap;
		}
		/* Leave room for the terminating NUL. */
		ssize_t n = recv(fd, c->buf + c->len, c->cap - c->len - 1, 0);
		if (n > 0) {
			c->len += n;
			c->idle_at = 0;
			continue;
		}
		if (n == 0) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (c->len > 0 && c->buf[c->len - 1] == '\0') {
				c->idle_at = now_ms() + IPC_IDLE_MS;
			}
			return true;
		}
		drop_client(c);
		return true;
	}

	/* End of request. */
	if (c->len == 0) {
		drop_client(c);
		return true;
	}
	c->buf[c->len] = '\0';
	dispatch(c);
	return true;
}

int ipc_timeout(void)
{
	long long next = -1;
	for (ipc_client_t *c = clients; c != NULL; c = c->next) {
		long long t = c->idle_at != 0 ? c->idle_at : c->expire_at;
		if (next == -1 || t < next) {
			next = t;
		}
	}
	if (next == -1) {
		return -1;
	}
	long long d = next - now_ms();
	return d < 0 ? 0 : (int) d;
}

void ipc_expire(void)
{
	long long now = now_ms();
	ipc_client_t *c = clients;
	while (c != NULL) {
		ipc_client_t *next = c->next;
		if (c->idle_at != 0 && c->idle_at <= now) {
			c->buf[c->len] = '\0';
			dispatch(c);
		} else if (c->expire_at <= now) {
			drop_client(c);
		}
		c = next;
	}
}

void ipc_close_all(void)
{
	while (clients != NULL) {
		drop_client(clients);
	}
}
