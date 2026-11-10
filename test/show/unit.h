/* ctd-show-unit: a tiny test framework for the Show Profiles pure logic (no libobs).
 * Each suite is a function `void test_<name>(void)` registered in unit-main.c. */
#ifndef CTD_SHOW_UNIT_H
#define CTD_SHOW_UNIT_H

#include "json.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern int unit_failures;
extern int unit_checks;
extern const char *unit_fixtures; /* argv[1]: test/show/fixtures */
extern const char *unit_scratch;  /* argv[2]: an empty folder per run (suites make their own subfolders) */

#define CHECK(cond)                                                                      \
	do {                                                                             \
		unit_checks++;                                                           \
		if (!(cond)) {                                                           \
			unit_failures++;                                                 \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                        \
	} while (0)

#define CHECK_STR(a, b)                                                                                    \
	do {                                                                                               \
		const char *a_ = (a), *b_ = (b);                                                           \
		unit_checks++;                                                                             \
		if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                                   \
			unit_failures++;                                                                   \
			fprintf(stderr, "%s:%d: \"%s\" != \"%s\" (%s vs %s)\n", __FILE__, __LINE__,        \
				a_ ? a_ : "(null)", b_ ? b_ : "(null)", #a, #b);                           \
		}                                                                                          \
	} while (0)

/* <fixtures>/<name> parsed, or NULL (and a failure). */
cj *unit_fixture_json(const char *name);
/* <fixtures>/<name> as text (malloc'd), or NULL (and a failure). */
char *unit_fixture_text(const char *name);
/* Joins <fixtures>/<name> into out. */
void unit_fixture_path(const char *name, char *out, size_t size);
/* Makes an empty folder <scratch>/<name> (removing an old one) and writes its path into out. */
void unit_scratch_dir(const char *name, char *out, size_t size);
/* Does text contain needle? (for "no secret survives" greps) */
bool unit_contains(const char *text, const char *needle);

#endif
