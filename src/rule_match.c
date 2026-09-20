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
#include <string.h>
#include <strings.h>
#include "rule_match.h"

static const char *prop_names[RULE_PROP_COUNT] = {
	"class", "instance", "name", "type", "role", "transient",
};

/* The values `type` and `transient` accept; NULL ends each list. */
static const char *type_values[] = {
	"normal", "dock", "desktop", "notification", "dialog", "utility", "toolbar", NULL,
};
static const char *transient_values[] = {"on", "off", "true", "false", NULL};

bool rule_prop_from_key(const char *key, rule_prop_t *prop)
{
	if (key == NULL) {
		return false;
	}
	for (int i = 0; i < RULE_PROP_COUNT; i++) {
		if (strcmp(prop_names[i], key) == 0) {
			if (prop != NULL) {
				*prop = (rule_prop_t) i;
			}
			return true;
		}
	}
	return false;
}

const char *rule_prop_name(rule_prop_t prop)
{
	if (prop < 0 || prop >= RULE_PROP_COUNT) {
		return "?";
	}
	return prop_names[prop];
}

/* Whether `value` is one of the NULL-terminated `list`. */
static bool value_in(const char *const *list, const char *value)
{
	for (; *list != NULL; list++) {
		if (strcmp(*list, value) == 0) {
			return true;
		}
	}
	return false;
}

/* GCC cannot know the real size of `err`/`buf`: they are plain pointer
 * parameters, not local arrays. Once the caller-supplied `len` is checked
 * against 0 and found nonzero, -Wformat-truncation=2 under -D_FORTIFY_SOURCE=3
 * falls back to treating that lower bound (1) as if it were the destination's
 * actual size, and flags every longer message below as a truncation risk.
 * There is nothing to fix: `snprintf` is exactly what bounds the write to
 * `len`, on purpose, for a message that may not fit. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

bool rule_cond_compile(rule_cond_t *cond, rule_prop_t prop, const char *value,
                       unsigned int flags, char *err, size_t len)
{
	if (cond == NULL || value == NULL || err == NULL || len == 0) {
		return false;
	}
	memset(cond, 0, sizeof(*cond));
	err[0] = '\0';

	if (strlen(value) >= sizeof(cond->pattern)) {
		snprintf(err, len, "the pattern is longer than %zu characters",
		         sizeof(cond->pattern) - 1);
		return false;
	}
	snprintf(cond->pattern, sizeof(cond->pattern), "%s", value);
	snprintf(cond->text, sizeof(cond->text), "%s", value);

	cond->is_regex = (flags & RULE_COND_REGEX) != 0;
	cond->ignore_case = (flags & RULE_COND_ICASE) != 0;

	if (prop == RULE_PROP_TYPE || prop == RULE_PROP_TRANSIENT) {
		const char *const *list = (prop == RULE_PROP_TYPE) ? type_values : transient_values;
		if (cond->is_regex) {
			snprintf(err, len, "%s takes no regular expression", rule_prop_name(prop));
			return false;
		}
		/* Their values are a closed list compared as written: a case
		 * marker would do nothing, and `rule -l` could not print it back. */
		if (cond->ignore_case) {
			snprintf(err, len, "%s takes no case marker", rule_prop_name(prop));
			return false;
		}
		if (!value_in(list, cond->text)) {
			snprintf(err, len, "'%s' is not a %s", cond->text, rule_prop_name(prop));
			return false;
		}
		/* `true` and `false` are the same as `on` and `off`. */
		if (prop == RULE_PROP_TRANSIENT) {
			bool on = strcmp(cond->text, "on") == 0 || strcmp(cond->text, "true") == 0;
			snprintf(cond->text, sizeof(cond->text), "%s", on ? "on" : "off");
		}
	}

	if (cond->is_regex) {
		int regex_flags = REG_EXTENDED | REG_NOSUB | (cond->ignore_case ? REG_ICASE : 0);
		int status = regcomp(&cond->preg, cond->text, regex_flags);
		if (status != 0) {
			/* POSIX leaves `preg` undefined when regcomp() fails, so
			 * there is nothing to regfree(); `used` stays false, and
			 * rule_cond_free() will not try either. */
			regerror(status, &cond->preg, err, len);
			return false;
		}
	}

	cond->used = true;
	return true;
}

#pragma GCC diagnostic pop

bool rule_cond_matches(const rule_cond_t *cond, const char *value)
{
	if (cond == NULL || !cond->used) {
		return true;
	}
	if (value == NULL) {
		value = "";
	}
	if (cond->is_regex) {
		return regexec(&cond->preg, value, 0, NULL, 0) == 0;
	}
	if (strcmp(cond->text, "*") == 0) {
		return true;
	}
	if (cond->ignore_case) {
		return strcasecmp(cond->text, value) == 0;
	}
	return strcmp(cond->text, value) == 0;
}

bool rule_conds_match(const rule_cond_t *conds, const char *const *values)
{
	if (conds == NULL || values == NULL) {
		return false;
	}
	for (int i = 0; i < RULE_PROP_COUNT; i++) {
		if (!rule_cond_matches(&conds[i], values[i])) {
			return false;
		}
	}
	return true;
}

const char *rule_cond_pattern(const rule_cond_t *cond)
{
	if (cond == NULL || !cond->used) {
		return "*";
	}
	return cond->pattern;
}

/* Same false positive as rule_cond_compile() above: `len` checked against 0
 * makes GCC assume `buf` is 1 byte long for the rest of the function. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

void rule_cond_print(const rule_cond_t *cond, rule_prop_t prop, char *buf, size_t len)
{
	if (buf == NULL || len == 0) {
		return;
	}
	if (cond == NULL || !cond->used) {
		buf[0] = '\0';
		return;
	}
	/* The `~` marks a regex right on the operator; a trailing `/i` on the
	 * value marks case-insensitivity. Neither lives in cond->pattern: both
	 * come from the flags the caller compiled this condition with. */
	snprintf(buf, len, "%s%s=%s%s", rule_prop_name(prop), cond->is_regex ? "~" : "",
	         cond->pattern, cond->ignore_case ? "/i" : "");
}

#pragma GCC diagnostic pop

void rule_cond_free(rule_cond_t *cond)
{
	if (cond == NULL || !cond->used) {
		return;
	}
	if (cond->is_regex) {
		regfree(&cond->preg);
	}
	memset(cond, 0, sizeof(*cond));
}
