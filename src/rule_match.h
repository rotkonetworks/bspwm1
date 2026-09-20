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

#ifndef BSPWM_RULE_MATCH_H
#define BSPWM_RULE_MATCH_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>

/* How a rule picks the windows it applies to.
 *
 * A condition is one property matched against a fixed piece of text. The
 * caller says through `flags` whether that text is a POSIX extended regular
 * expression and whether case counts; the module never reads the text itself
 * to guess — parsing `key[/i][~]=value` into those flags is the caller's
 * job, not this module's. Pure text handling with no backend, so the unit
 * tests drive it directly. */

#define RULE_PATTERN_MAXLEN 256

/* Flags for rule_cond_compile(). */
#define RULE_COND_REGEX  (1u << 0)
#define RULE_COND_ICASE  (1u << 1)

typedef enum {
	RULE_PROP_CLASS,
	RULE_PROP_INSTANCE,
	RULE_PROP_NAME,
	RULE_PROP_TYPE,
	RULE_PROP_ROLE,
	RULE_PROP_TRANSIENT,
	RULE_PROP_COUNT,
} rule_prop_t;

typedef struct {
	bool used;
	bool is_regex;
	bool ignore_case;
	/* The value as the caller passed it, with no operator in it. */
	char pattern[RULE_PATTERN_MAXLEN];
	/* What is compared: the same value, except type/transient normalize
	 * true/false to on/off. */
	char text[RULE_PATTERN_MAXLEN];
	regex_t preg;
} rule_cond_t;

/* The property `key` names, if it names one. */
bool rule_prop_from_key(const char *key, rule_prop_t *prop);
const char *rule_prop_name(rule_prop_t prop);

/* Compile `value` into `cond`, taken as it is: `flags` says whether it is a
 * regular expression (RULE_COND_REGEX) and whether case counts
 * (RULE_COND_ICASE). Flags 0 is the old CLASS:INSTANCE:NAME syntax: exact,
 * case-sensitive, with `~` and `/i` as plain characters. On failure it fills
 * `err` with the reason and leaves nothing to free. Compiling again over a
 * `cond` that already holds a compiled condition, without an intervening
 * rule_cond_free(), leaks its regex, the same caveat regcomp() itself has. */
bool rule_cond_compile(rule_cond_t *cond, rule_prop_t prop, const char *value,
                       unsigned int flags, char *err, size_t len);
/* Whether `value` satisfies `cond`. An unused condition and the `*` pattern
 * match anything; a NULL value counts as empty. */
bool rule_cond_matches(const rule_cond_t *cond, const char *value);
/* Whether every condition in `conds` holds for `values`, indexed by property. */
bool rule_conds_match(const rule_cond_t *conds, const char *const *values);
/* The pattern as it was written, or "*" when the condition is unused. */
const char *rule_cond_pattern(const rule_cond_t *cond);
/* The condition written the way the new syntax expects it — "property=pattern",
 * "property~=pattern", "property=pattern/i" or "property~=pattern/i", the
 * `~` marking a regular expression and a trailing `/i` on the value marking
 * case-insensitivity — into `buf`; the empty string when the condition is
 * unused. */
void rule_cond_print(const rule_cond_t *cond, rule_prop_t prop, char *buf, size_t len);
void rule_cond_free(rule_cond_t *cond);

#endif
