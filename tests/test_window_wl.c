/* Minimal Wayland test window for bspwm TDD.
 * Creates an xdg-toplevel with a given app_id, maps it, waits for close. */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

/* xdg-shell protocol - we include the generated header or define inline */
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *shm;
static bool running = true;
static bool configured = false;

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
	int32_t width, int32_t height, struct wl_array *states)
{
	(void)data; (void)toplevel; (void)width; (void)height; (void)states;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)data; (void)toplevel;
	running = false;
}

static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
	int32_t width, int32_t height)
{
	(void)data; (void)toplevel; (void)width; (void)height;
}

static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
	struct wl_array *capabilities)
{
	(void)data; (void)toplevel; (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
	.configure_bounds = xdg_toplevel_configure_bounds,
	.wm_capabilities = xdg_toplevel_wm_capabilities,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(surface, serial);
	configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void registry_global(void *data, struct wl_registry *registry,
	uint32_t name, const char *interface, uint32_t version)
{
	(void)data; (void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 3);
		xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char **argv)
{
	const char *app_id = "test";
	if (argc > 1) app_id = argv[1];

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "Can't connect to Wayland display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !xdg_wm_base) {
		fprintf(stderr, "Missing required globals\n");
		wl_display_disconnect(display);
		return 1;
	}

	/* Create surface */
	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);

	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, app_id);
	xdg_toplevel_set_title(toplevel, "bspwm test window");

	wl_surface_commit(surface);
	wl_display_roundtrip(display);

	/* Wait for configure then commit with a buffer */
	while (!configured && wl_display_dispatch(display) != -1)
		;

	/* Attach a minimal shm buffer so the surface maps */
	if (shm) {
		int width = 64, height = 64, stride = width * 4;
		int size = stride * height;
		int fd = -1;
		char name[] = "/tmp/bspwm-test-XXXXXX";
		fd = mkstemp(name);
		if (fd >= 0) {
			unlink(name);
			ftruncate(fd, size);
			struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
			struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
				width, height, stride, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
			wl_surface_attach(surface, buffer, 0, 0);
			wl_surface_commit(surface);
		}
	}

	/* Event loop — wait for close */
	while (running && wl_display_dispatch(display) != -1)
		;

	xdg_toplevel_destroy(toplevel);
	xdg_surface_destroy(xdg_surface);
	wl_surface_destroy(surface);
	wl_display_disconnect(display);

	return 0;
}
