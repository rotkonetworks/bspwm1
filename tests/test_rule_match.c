/* Unit tests for rule_match.c: how a rule condition matches a window. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../src/rule_match.h"

static int failures;

static void check(const char *desc, bool expected, bool actual)
{
	if (expected == actual) {
		printf("  PASS: %s\n", desc);
		return;
	}
	printf("  FAIL: %s: expected %s, got %s\n", desc,
	       expected ? "true" : "false", actual ? "true" : "false");
	failures++;
}

static void check_str(const char *desc, const char *expected, const char *actual)
{
	if (strcmp(expected, actual) == 0) {
		printf("  PASS: %s\n", desc);
		return;
	}
	printf("  FAIL: %s: expected '%s', got '%s'\n", desc, expected, actual);
	failures++;
}

/* Compile `value` for `prop` with `flags` and return whether `text` matches
 * it. The condition must compile. */
static bool matches_flags(rule_prop_t prop, const char *value, unsigned int flags,
                          const char *text)
{
	rule_cond_t cond;
	char err[256];
	if (!rule_cond_compile(&cond, prop, value, flags, err, sizeof(err))) {
		printf("  FAIL: could not compile '%s': %s\n", value, err);
		failures++;
		return false;
	}
	bool result = rule_cond_matches(&cond, text);
	rule_cond_free(&cond);
	return result;
}

/* matches_flags() with no flags: the old CLASS:INSTANCE:NAME syntax, exact
 * and case-sensitive. */
static bool matches(rule_prop_t prop, const char *value, const char *text)
{
	return matches_flags(prop, value, 0, text);
}

int main(void)
{
	rule_prop_t prop;

	/* Property names. */
	check("class is a property", true, rule_prop_from_key("class", &prop));
	check("the property is the class", true, prop == RULE_PROP_CLASS);
	check("transient is a property", true, rule_prop_from_key("transient", &prop));
	check("state is not a property", false, rule_prop_from_key("state", &prop));
	check_str("the name of the role property", "role", rule_prop_name(RULE_PROP_ROLE));

	/* Exact, the way it has always worked. */
	check("an exact pattern matches", true, matches(RULE_PROP_CLASS, "kitty", "kitty"));
	check("an exact pattern is case sensitive", false,
	      matches(RULE_PROP_CLASS, "kitty", "Kitty"));
	check("a star matches anything", true, matches(RULE_PROP_CLASS, "*", "kitty"));

	/* Case insensitive: RULE_COND_ICASE, not a `/i` read from the value. */
	check("with the icase flag the case does not count", true,
	      matches_flags(RULE_PROP_CLASS, "Pavucontrol", RULE_COND_ICASE, "pavucontrol"));
	check("with the icase flag the rest still has to match", false,
	      matches_flags(RULE_PROP_CLASS, "Pavucontrol", RULE_COND_ICASE, "pavucontrolx"));

	/* Regular expressions: RULE_COND_REGEX, not a `~` read from the value. */
	check("a regex matches a prefix", true,
	      matches_flags(RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX, "crx_abcdef"));
	check("a regex anchored at the start does not match in the middle", false,
	      matches_flags(RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX, "google-crx_abcdef"));
	check("the real case: a plain Chrome window is not a web app", false,
	      matches_flags(RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX, "google-chrome"));
	check("an alternation matches every branch", true,
	      matches_flags(RULE_PROP_CLASS, "^(eog|feh|ristretto)$", RULE_COND_REGEX, "feh"));
	check("an alternation matches nothing else", false,
	      matches_flags(RULE_PROP_CLASS, "^(eog|feh|ristretto)$", RULE_COND_REGEX, "firefox"));
	check("a regex with the icase flag ignores the case", true,
	      matches_flags(RULE_PROP_CLASS, "^(eog|feh)$", RULE_COND_REGEX | RULE_COND_ICASE, "FEH"));

	/* Empty properties: a window with no WM_WINDOW_ROLE. */
	check("an empty property matches an empty regex", true,
	      matches_flags(RULE_PROP_ROLE, "^$", RULE_COND_REGEX, ""));
	check("an empty property does not match a pattern", false,
	      matches(RULE_PROP_ROLE, "pop-up", ""));
	check("a NULL property counts as empty", true,
	      matches_flags(RULE_PROP_ROLE, "^$", RULE_COND_REGEX, NULL));

	/* The old CLASS:INSTANCE:NAME syntax compiles with no flags: `~` and
	 * `/i` are then just characters in the pattern, never an operator —
	 * this is what keeps an old-style rule working exactly as before. */
	check("with no flags, ~ is a literal character, not a regex anchor", false,
	      matches(RULE_PROP_INSTANCE, "~^crx_", "crx_abc"));
	check("with no flags, ~^crx_ matches only its own literal text", true,
	      matches(RULE_PROP_INSTANCE, "~^crx_", "~^crx_"));
	check("with no flags, /i is literal text and case still counts", false,
	      matches(RULE_PROP_CLASS, "algo/i", "ALGO/I"));
	check("with no flags, algo/i matches only its own literal text", true,
	      matches(RULE_PROP_CLASS, "algo/i", "algo/i"));
	rule_cond_t literal;
	char literal_err[256];
	check("an invalid regex compiles fine with no flags: it is literal text", true,
	      rule_cond_compile(&literal, RULE_PROP_NAME, "~(pendiente", 0, literal_err, sizeof(literal_err)));
	rule_cond_free(&literal);

	/* Closed value lists. */
	rule_cond_t cond;
	char err[256];
	check("dialog is a window type", true,
	      rule_cond_compile(&cond, RULE_PROP_TYPE, "dialog", 0, err, sizeof(err)));
	rule_cond_free(&cond);
	check("popup is not a window type", false,
	      rule_cond_compile(&cond, RULE_PROP_TYPE, "popup", 0, err, sizeof(err)));
	check("a regex is not allowed for the type", false,
	      rule_cond_compile(&cond, RULE_PROP_TYPE, "dia", RULE_COND_REGEX, err, sizeof(err)));
	check("on is a transient value", true,
	      rule_cond_compile(&cond, RULE_PROP_TRANSIENT, "on", 0, err, sizeof(err)));
	rule_cond_free(&cond);
	check("maybe is not a transient value", false,
	      rule_cond_compile(&cond, RULE_PROP_TRANSIENT, "maybe", 0, err, sizeof(err)));
	/* Both are compared as written, so a case marker on them would be
	 * inert, and `rule -l` could not print it back: it is refused. */
	check("a case marker is not allowed for the type", false,
	      rule_cond_compile(&cond, RULE_PROP_TYPE, "dialog", RULE_COND_ICASE, err, sizeof(err)));
	check("and the error names the property", true, strstr(err, "type") != NULL);
	check("a case marker is not allowed for transient", false,
	      rule_cond_compile(&cond, RULE_PROP_TRANSIENT, "on", RULE_COND_ICASE, err, sizeof(err)));

	/* A broken regex is refused, with the reason. */
	check("an unbalanced regex does not compile", false,
	      rule_cond_compile(&cond, RULE_PROP_CLASS, "^(eog", RULE_COND_REGEX, err, sizeof(err)));
	check("and the error says something", true, err[0] != '\0');
	/* After a failed regcomp() the regex_t is undefined: the condition must
	 * not claim to hold one, so rule_cond_free() leaves it alone. */
	check("a condition that failed to compile is not in use", false, cond.used);
	rule_cond_free(&cond);

	/* The pattern is kept as it was written, and printing puts the
	 * operator and the /i back the new-syntax way. */
	check("a regex with a case marker compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX | RULE_COND_ICASE, err, sizeof(err)));
	check_str("the pattern keeps its shape", "^crx_", rule_cond_pattern(&cond));
	char printed[512];
	rule_cond_print(&cond, RULE_PROP_INSTANCE, printed, sizeof(printed));
	check_str("printing gives back the condition", "instance~=^crx_/i", printed);
	rule_cond_free(&cond);

	/* An exact, case-sensitive condition prints with neither mark. */
	check("an exact condition compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_CLASS, "kitty", 0, err, sizeof(err)));
	rule_cond_print(&cond, RULE_PROP_CLASS, printed, sizeof(printed));
	check_str("an exact condition prints with no operator", "class=kitty", printed);
	rule_cond_free(&cond);

	/* Recompiling over a `cond` that still holds a regex leaks it: free it
	 * first. The new condition replaces the old one entirely. */
	check("the first regex compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX, err, sizeof(err)));
	rule_cond_free(&cond);
	check("the second regex compiles over the freed condition", true,
	      rule_cond_compile(&cond, RULE_PROP_INSTANCE, "^tab_", RULE_COND_REGEX, err, sizeof(err)));
	check("the old pattern no longer matches after recompiling", false,
	      rule_cond_matches(&cond, "crx_abc"));
	check("the new pattern matches after recompiling", true,
	      rule_cond_matches(&cond, "tab_abc"));
	rule_cond_free(&cond);

	/* The pattern buffer holds 255 characters: that many compile, one more
	 * is refused and the reason gives the limit. */
	char edge[RULE_PATTERN_MAXLEN + 1];
	memset(edge, 'c', RULE_PATTERN_MAXLEN - 1);
	edge[RULE_PATTERN_MAXLEN - 1] = '\0';
	check("a 255-character pattern compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_CLASS, edge, 0, err, sizeof(err)));
	check("and it matches itself whole", true, rule_cond_matches(&cond, edge));
	rule_cond_free(&cond);
	memset(edge, 'c', RULE_PATTERN_MAXLEN);
	edge[RULE_PATTERN_MAXLEN] = '\0';
	check("a 256-character pattern is refused", false,
	      rule_cond_compile(&cond, RULE_PROP_CLASS, edge, 0, err, sizeof(err)));
	check("and the error gives the limit", true, strstr(err, "255") != NULL);

	/* transient takes true/false as well as on/off: it compares the
	 * normalized value, and still lists the one that was written. */
	check("transient=true compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_TRANSIENT, "true", 0, err, sizeof(err)));
	check("transient=true matches a child window", true, rule_cond_matches(&cond, "on"));
	check("transient=true does not match a window with no parent", false,
	      rule_cond_matches(&cond, "off"));
	check_str("transient=true keeps the value it was written with", "true",
	          rule_cond_pattern(&cond));
	rule_cond_print(&cond, RULE_PROP_TRANSIENT, printed, sizeof(printed));
	check_str("transient=true prints as it was written", "transient=true", printed);
	rule_cond_free(&cond);
	check("transient=false compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_TRANSIENT, "false", 0, err, sizeof(err)));
	check("transient=false matches a window with no parent", true,
	      rule_cond_matches(&cond, "off"));
	check("transient=false does not match a child window", false,
	      rule_cond_matches(&cond, "on"));
	rule_cond_free(&cond);

	/* An empty pattern is a real condition — it matches only an empty
	 * property — and prints as such; an unused one prints nothing. */
	check("an empty pattern compiles", true,
	      rule_cond_compile(&cond, RULE_PROP_ROLE, "", 0, err, sizeof(err)));
	check("an empty pattern matches an empty property", true, rule_cond_matches(&cond, ""));
	check("an empty pattern does not match a set one", false,
	      rule_cond_matches(&cond, "pop-up"));
	rule_cond_print(&cond, RULE_PROP_ROLE, printed, sizeof(printed));
	check_str("an empty pattern prints with nothing after the =", "role=", printed);
	rule_cond_free(&cond);
	rule_cond_print(&cond, RULE_PROP_ROLE, printed, sizeof(printed));
	check_str("an unused condition prints nothing", "", printed);

	/* A whole rule: every condition has to hold. */
	rule_cond_t conds[RULE_PROP_COUNT];
	memset(conds, 0, sizeof(conds));
	check("the class condition of the rule compiles", true,
	      rule_cond_compile(&conds[RULE_PROP_CLASS], RULE_PROP_CLASS, "Google-chrome", 0, err, sizeof(err)));
	check("the instance condition of the rule compiles", true,
	      rule_cond_compile(&conds[RULE_PROP_INSTANCE], RULE_PROP_INSTANCE, "^crx_", RULE_COND_REGEX, err, sizeof(err)));
	const char *web_app[RULE_PROP_COUNT] = {"Google-chrome", "crx_abc", "Mail", "normal", "", "off"};
	const char *browser[RULE_PROP_COUNT] = {"Google-chrome", "google-chrome", "News", "normal", "", "off"};
	check("both conditions hold for the web app", true, rule_conds_match(conds, web_app));
	check("the instance rules the browser out", false, rule_conds_match(conds, browser));
	for (int i = 0; i < RULE_PROP_COUNT; i++) {
		rule_cond_free(&conds[i]);
	}

	rule_cond_t none[RULE_PROP_COUNT];
	memset(none, 0, sizeof(none));
	check("a rule with no conditions matches anything", true, rule_conds_match(none, browser));

	return failures ? 1 : 0;
}
