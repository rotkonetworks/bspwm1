/*
 * X11 latency benchmark for bspwm
 * Measures actual window creation and command latency
 *
 * Build: gcc -O2 -o x11_latency_bench x11_latency_bench.c -lxcb -lxcb-icccm -lxcb-ewmh
 * Run:   ./x11_latency_bench [iterations]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_ewmh.h>

#define WARMUP_ITERATIONS 5
#define DEFAULT_ITERATIONS 50

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int send_bspc_command(const char *socket_path, const char *cmd, char *response, size_t resp_size) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Send command with null terminator */
    size_t len = strlen(cmd);
    char *msg = malloc(len + 1);
    memcpy(msg, cmd, len);
    msg[len] = '\0';

    /* Replace spaces with nulls for bspwm protocol */
    for (size_t i = 0; i < len; i++) {
        if (msg[i] == ' ') msg[i] = '\0';
    }

    write(fd, msg, len + 1);
    free(msg);

    /* Read response */
    if (response && resp_size > 0) {
        ssize_t n = read(fd, response, resp_size - 1);
        if (n > 0) response[n] = '\0';
        else response[0] = '\0';
    }

    close(fd);
    return 0;
}

static void benchmark_command_latency(const char *socket_path, int iterations) {
    char response[4096];
    uint64_t *times = malloc(iterations * sizeof(uint64_t));

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        send_bspc_command(socket_path, "query -T -d", response, sizeof(response));
    }

    /* Benchmark: query tree */
    for (int i = 0; i < iterations; i++) {
        uint64_t start = now_ns();
        send_bspc_command(socket_path, "query -T -d", response, sizeof(response));
        times[i] = now_ns() - start;
    }

    /* Calculate stats */
    uint64_t sum = 0, min = times[0], max = times[0];
    for (int i = 0; i < iterations; i++) {
        sum += times[i];
        if (times[i] < min) min = times[i];
        if (times[i] > max) max = times[i];
    }
    uint64_t avg = sum / iterations;

    printf("query -T -d          : %6lu μs (min: %lu, max: %lu)\n", avg / 1000, min / 1000, max / 1000);

    /* Benchmark: query monitors */
    for (int i = 0; i < iterations; i++) {
        uint64_t start = now_ns();
        send_bspc_command(socket_path, "query -M", response, sizeof(response));
        times[i] = now_ns() - start;
    }

    sum = 0; min = times[0]; max = times[0];
    for (int i = 0; i < iterations; i++) {
        sum += times[i];
        if (times[i] < min) min = times[i];
        if (times[i] > max) max = times[i];
    }
    avg = sum / iterations;

    printf("query -M             : %6lu μs (min: %lu, max: %lu)\n", avg / 1000, min / 1000, max / 1000);

    /* Benchmark: query desktops */
    for (int i = 0; i < iterations; i++) {
        uint64_t start = now_ns();
        send_bspc_command(socket_path, "query -D", response, sizeof(response));
        times[i] = now_ns() - start;
    }

    sum = 0; min = times[0]; max = times[0];
    for (int i = 0; i < iterations; i++) {
        sum += times[i];
        if (times[i] < min) min = times[i];
        if (times[i] > max) max = times[i];
    }
    avg = sum / iterations;

    printf("query -D             : %6lu μs (min: %lu, max: %lu)\n", avg / 1000, min / 1000, max / 1000);

    free(times);
}

static void benchmark_window_creation(xcb_connection_t *conn, xcb_ewmh_connection_t *ewmh,
                                       xcb_screen_t *screen, int iterations) {
    uint64_t *times = malloc(iterations * sizeof(uint64_t));
    xcb_window_t *windows = malloc(iterations * sizeof(xcb_window_t));

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        xcb_window_t win = xcb_generate_id(conn);
        uint32_t mask = XCB_CW_EVENT_MASK;
        uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, mask, values);
        xcb_map_window(conn, win);
        xcb_flush(conn);
        usleep(50000); /* Wait for WM to manage */
        xcb_destroy_window(conn, win);
        xcb_flush(conn);
        usleep(10000);
    }

    /* Benchmark window creation + management */
    for (int i = 0; i < iterations; i++) {
        xcb_window_t win = xcb_generate_id(conn);

        uint64_t start = now_ns();

        /* Register for StructureNotify to get MapNotify */
        uint32_t mask = XCB_CW_EVENT_MASK;
        uint32_t values[] = { XCB_EVENT_MASK_STRUCTURE_NOTIFY };
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          0, 0, 200, 150, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual, mask, values);

        /* Set WM_CLASS so bspwm applies rules */
        xcb_icccm_set_wm_class(conn, win, 12, "bench\0Bench");

        xcb_map_window(conn, win);
        xcb_flush(conn);

        /* Wait for MapNotify (window actually managed) with timeout */
        xcb_generic_event_t *ev;
        int timeout_count = 0;
        while (timeout_count < 100) { /* ~1 second timeout */
            ev = xcb_poll_for_event(conn);
            if (ev) {
                uint8_t type = ev->response_type & ~0x80;
                free(ev);
                if (type == XCB_MAP_NOTIFY) break;
            } else {
                usleep(10000); /* 10ms */
                timeout_count++;
            }
        }

        times[i] = now_ns() - start;
        windows[i] = win;
    }

    /* Calculate stats */
    uint64_t sum = 0, min = times[0], max = times[0];
    for (int i = 0; i < iterations; i++) {
        sum += times[i];
        if (times[i] < min) min = times[i];
        if (times[i] > max) max = times[i];
    }
    uint64_t avg = sum / iterations;

    printf("window create+map    : %6lu μs (min: %lu, max: %lu)\n", avg / 1000, min / 1000, max / 1000);

    /* Cleanup */
    for (int i = 0; i < iterations; i++) {
        xcb_destroy_window(conn, windows[i]);
    }
    xcb_flush(conn);

    free(times);
    free(windows);
}

int main(int argc, char **argv) {
    int iterations = DEFAULT_ITERATIONS;
    if (argc > 1) {
        iterations = atoi(argv[1]);
        if (iterations < 1) iterations = DEFAULT_ITERATIONS;
    }

    /* Connect to X */
    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "Cannot connect to X server\n");
        return 1;
    }

    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

    xcb_ewmh_connection_t ewmh;
    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(conn, &ewmh);
    xcb_ewmh_init_atoms_replies(&ewmh, cookies, NULL);

    /* Get bspwm socket path */
    char socket_path[256];
    const char *display = getenv("DISPLAY");
    if (!display) display = ":0";
    /* Format: /tmp/bspwm_:99_0_0-socket for DISPLAY=:99 */
    snprintf(socket_path, sizeof(socket_path), "/tmp/bspwm_%s_0_0-socket", display);

    printf("=== X11 Latency Benchmark ===\n");
    printf("Iterations: %d (+ %d warmup)\n", iterations, WARMUP_ITERATIONS);
    printf("Socket: %s\n\n", socket_path);

    /* Command latency */
    printf("Command latency (IPC round-trip):\n");
    benchmark_command_latency(socket_path, iterations);
    printf("\n");

    /* Window creation latency */
    printf("Window management latency:\n");
    benchmark_window_creation(conn, &ewmh, screen, iterations);

    xcb_ewmh_connection_wipe(&ewmh);
    xcb_disconnect(conn);

    return 0;
}
