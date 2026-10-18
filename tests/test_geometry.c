/* Rectangles that reach past INT16_MAX are still rectangles. */
#include <stdio.h>
#include "../src/types.h"
#include "../src/settings.h"
#include "../src/geometry.h"

tightness_t directional_focus_tightness = TIGHTNESS_HIGH;

static int failures;

static void check(const char *desc, int ok)
{
	printf("  %s: %s\n", ok ? "PASS" : "FAIL", desc);
	failures += !ok;
}

int main(void)
{
	bspwm_rect_t far = {32000, 0, 2000, 100};
	bspwm_rect_t inner = {32100, 10, 100, 10};
	check("point inside a rect crossing x=32767", is_inside((bspwm_point_t) {32100, 50}, far));
	check("rect crossing x=32767 contains a smaller one", contains(far, inner));
	check("area of a rect crossing x=32767", area(far) == 200000);
	check("area of 65535x65535 does not overflow",
	      area((bspwm_rect_t) {0, 0, 65535, 65535}) == 65535u * 65535u);

	bspwm_rect_t left = {31000, 0, 1000, 100};
	check("boundary distance east to a rect past x=32767",
	      boundary_distance(left, far, DIR_EAST) == 1);
	check("rect past x=32767 is on the east side", on_dir_side(left, far, DIR_EAST));
	return failures != 0;
}
