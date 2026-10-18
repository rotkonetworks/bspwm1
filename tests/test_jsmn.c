/* jsmn nesting limit: deep enough for any tree bspwm can dump, bounded for
 * restore.c's recursive descent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/jsmn.h"

static int failures;

static int parse_nested(int depth)
{
	size_t len = (size_t) depth * 6 + 2;
	char *js = malloc(len);
	char *p = js;
	for (int i = 0; i < depth; i++) {
		memcpy(p, "{\"a\":", 5);
		p += 5;
	}
	*p++ = '1';
	memset(p, '}', depth);
	p[depth] = '\0';

	unsigned int ntok = 2 * depth + 1;
	jsmntok_t *tok = malloc(ntok * sizeof(jsmntok_t));
	jsmn_parser parser;
	jsmn_init(&parser);
	int r = jsmn_parse(&parser, js, strlen(js), tok, ntok);
	free(tok);
	free(js);
	return r;
}

static void check(const char *desc, int ok)
{
	printf("  %s: %s\n", ok ? "PASS" : "FAIL", desc);
	failures += !ok;
}

int main(void)
{
	check("200 levels parse (the old cap of 128 rejected these)", parse_nested(200) == 401);
	check("1024 levels parse", parse_nested(1024) == 2049);
	check("1025 levels are rejected", parse_nested(1025) == JSMN_ERROR_INVAL);
	return failures != 0;
}
