/* Unit tests for magnet.c: magnetic edges while dragging a window. */

#include <stddef.h>
#include <stdio.h>
#include "../src/magnet.h"

static int failures;

/* The work area of every case: a 1920x1080 monitor without padding. */
static const magnet_box_t area = {0, 0, 1920, 1080};

/* A full magnet pass over `count` other windows. */
static magnet_box_t snap(magnet_box_t free, unsigned int edges,
                         const magnet_box_t *others, size_t count, int threshold)
{
	magnet_t mg;
	magnet_begin(&mg, free, edges, area, threshold);
	for (size_t i = 0; i < count; i++)
		magnet_consider(&mg, others[i]);
	return magnet_result(&mg);
}

static void check(const char *desc, magnet_box_t expected, magnet_box_t actual)
{
	if (expected.x1 == actual.x1 && expected.y1 == actual.y1 &&
	    expected.x2 == actual.x2 && expected.y2 == actual.y2) {
		printf("  PASS: %s\n", desc);
		return;
	}
	printf("  FAIL: %s: expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}\n", desc,
	       expected.x1, expected.y1, expected.x2, expected.y2,
	       actual.x1, actual.y1, actual.x2, actual.y2);
	failures++;
}

int main(void)
{
	/* Moving against the work area. */
	check("a left edge 8 px from the area sticks to it",
	      (magnet_box_t) {0, 300, 404, 604},
	      snap((magnet_box_t) {8, 300, 412, 604}, MAGNET_ALL, NULL, 0, 20));
	check("an edge exactly at the threshold sticks",
	      (magnet_box_t) {0, 300, 404, 604},
	      snap((magnet_box_t) {20, 300, 424, 604}, MAGNET_ALL, NULL, 0, 20));
	check("an edge one pixel past the threshold stays free",
	      (magnet_box_t) {21, 300, 425, 604},
	      snap((magnet_box_t) {21, 300, 425, 604}, MAGNET_ALL, NULL, 0, 20));
	check("a threshold of 0 never sticks",
	      (magnet_box_t) {1, 300, 405, 604},
	      snap((magnet_box_t) {1, 300, 405, 604}, MAGNET_ALL, NULL, 0, 0));
	check("a right edge near the area sticks to it",
	      (magnet_box_t) {1516, 300, 1920, 604},
	      snap((magnet_box_t) {1510, 300, 1914, 604}, MAGNET_ALL, NULL, 0, 20));

	magnet_t mg;
	magnet_begin(&mg, (magnet_box_t) {2, 300, 498, 604}, MAGNET_ALL,
	             (magnet_box_t) {0, 0, 500, 1080}, 20);
	check("on a tie between both edges the left one wins",
	      (magnet_box_t) {0, 300, 496, 604}, magnet_result(&mg));

	/* Moving next to other windows. */
	const magnet_box_t right_of = {1000, 300, 1404, 604};
	check("a window moved next to another touches it",
	      (magnet_box_t) {596, 300, 1000, 604},
	      snap((magnet_box_t) {590, 300, 994, 604}, MAGNET_ALL, &right_of, 1, 20));

	const magnet_box_t below = {1000, 500, 1404, 804};
	check("a window above another lines up with it and touches it",
	      (magnet_box_t) {1000, 196, 1404, 500},
	      snap((magnet_box_t) {994, 180, 1398, 484}, MAGNET_ALL, &below, 1, 20));

	const magnet_box_t far_below = {1000, 850, 1304, 1054};
	check("a window far away on the other axis does not attract",
	      (magnet_box_t) {994, 100, 1398, 404},
	      snap((magnet_box_t) {994, 100, 1398, 404}, MAGNET_ALL, &far_below, 1, 20));

	const magnet_box_t close_right = {409, 300, 813, 604};
	check("an edge already in place holds against a farther candidate",
	      (magnet_box_t) {0, 300, 404, 604},
	      snap((magnet_box_t) {0, 300, 404, 604}, MAGNET_ALL, &close_right, 1, 20));

	/* Resizing. */
	check("a resized right edge touches the window next to it",
	      (magnet_box_t) {300, 300, 1000, 604},
	      snap((magnet_box_t) {300, 300, 993, 604}, MAGNET_RIGHT, &right_of, 1, 20));
	check("a resize leaves alone the edges it does not drag",
	      (magnet_box_t) {5, 300, 1000, 604},
	      snap((magnet_box_t) {5, 300, 993, 604}, MAGNET_RIGHT, &right_of, 1, 20));
	check("a corner resize sticks both of its edges",
	      (magnet_box_t) {300, 300, 1000, 1080},
	      snap((magnet_box_t) {300, 300, 995, 1072}, MAGNET_RIGHT | MAGNET_BOTTOM,
	           &right_of, 1, 20));
	const magnet_box_t above = {300, 300, 704, 604};
	check("a resized top edge touches the window above it",
	      (magnet_box_t) {300, 604, 704, 900},
	      snap((magnet_box_t) {300, 610, 704, 900}, MAGNET_TOP, &above, 1, 20));
	check("a resized edge pulled past the threshold is free again",
	      (magnet_box_t) {300, 300, 1043, 604},
	      snap((magnet_box_t) {300, 300, 1043, 604}, MAGNET_RIGHT, &right_of, 1, 20));

	/* Edges hidden under windows stacked above. */
	magnet_t hid;
	const magnet_box_t under = {500, 300, 1104, 704};
	const magnet_box_t cover = {300, 200, 1304, 904};
	magnet_begin(&hid, (magnet_box_t) {1110, 400, 1314, 554}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, &cover, 1);
	check("an edge under a window above does not attract",
	      (magnet_box_t) {1110, 400, 1314, 554}, magnet_result(&hid));

	magnet_begin(&hid, (magnet_box_t) {1110, 400, 1314, 554}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, NULL, 0);
	check("the same edge with nothing above attracts",
	      (magnet_box_t) {1104, 400, 1308, 554}, magnet_result(&hid));

	/* Only the top of the right edge of `under` shows: [300, 500). */
	const magnet_box_t lower_cover = {900, 500, 1300, 900};
	magnet_begin(&hid, (magnet_box_t) {1110, 400, 1314, 450}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, &lower_cover, 1);
	check("a window next to the visible part of an edge sticks to it",
	      (magnet_box_t) {1104, 400, 1308, 450}, magnet_result(&hid));

	magnet_begin(&hid, (magnet_box_t) {1110, 600, 1314, 680}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, &lower_cover, 1);
	check("a window next to the hidden part of an edge does not",
	      (magnet_box_t) {1110, 600, 1314, 680}, magnet_result(&hid));

	/* Monocle: a window of the same size on top hides all four edges,
	 * the right and bottom ones included. */
	magnet_begin(&hid, (magnet_box_t) {1110, 400, 1314, 554}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, &under, 1);
	check("a window of the same size above hides every edge",
	      (magnet_box_t) {1110, 400, 1314, 554}, magnet_result(&hid));

	/* A window above that only touches the edge from outside leaves it visible. */
	const magnet_box_t beside = {1104, 300, 1400, 704};
	magnet_begin(&hid, (magnet_box_t) {1110, 400, 1314, 554}, MAGNET_ALL, area, 20);
	magnet_consider_visible(&hid, under, &beside, 1);
	check("a window above next to an edge does not hide it",
	      (magnet_box_t) {1104, 400, 1308, 554}, magnet_result(&hid));

	return failures ? 1 : 0;
}
