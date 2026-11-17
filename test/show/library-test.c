/* ctd-library-tests: the pure parts of the Relay library mirror (ticket T14). No libobs. Run with ctest. */
#include "../../src/show/library-plan.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                \
	do {                                                                       \
		if (!(cond)) {                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			failures++;                                                \
		}                                                                  \
	} while (0)

#define CHECK_STR(actual, expected)                                                                          \
	do {                                                                                                 \
		if (strcmp((actual), (expected)) != 0) {                                                     \
			printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
			failures++;                                                                          \
		}                                                                                            \
	} while (0)

#define HASH_A "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HASH_B "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

int main(void)
{
	CHECK(library_valid_sha256(HASH_A));
	CHECK(library_valid_sha256("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"));
	CHECK(!library_valid_sha256(NULL));
	CHECK(!library_valid_sha256(""));
	CHECK(!library_valid_sha256("abc"));
	CHECK(!library_valid_sha256(HASH_A "0"));
	CHECK(!library_valid_sha256("../../../../../../../../../../../../../../../../../../../../../../.."));
	CHECK(!library_valid_sha256("g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

	CHECK_STR(library_extension("lut", "Warm.cube"), ".cube");
	CHECK_STR(library_extension("lut", "Warm.CUBE"), ".cube");
	CHECK_STR(library_extension("lut", "Warm.PNG"), ".png");
	CHECK_STR(library_extension("", "Warm look"), ".cube");
	CHECK_STR(library_extension(NULL, NULL), ".cube");
	CHECK(library_extension("image", "logo.png") == NULL);
	CHECK(library_extension("video", "a.mp4") == NULL);

	char name[128];
	CHECK(library_cache_name(HASH_A, ".cube", name, sizeof(name)));
	CHECK_STR(name, HASH_A ".cube");
	CHECK(library_cache_name("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", ".png", name,
				 sizeof(name)));
	CHECK_STR(name, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.png");
	CHECK(!library_cache_name("nope", ".cube", name, sizeof(name)));
	CHECK(!library_cache_name(HASH_A, NULL, name, sizeof(name)));
	CHECK(!library_cache_name(HASH_A, ".cube", name, 20)); /* too small: refused, not cut */

	/* stale files */
	const char *wanted[] = {HASH_A ".cube"};
	CHECK(!library_is_stale(HASH_A ".cube", wanted, 1));
	CHECK(library_is_stale(HASH_B ".cube", wanted, 1));
	CHECK(library_is_stale(HASH_A ".png", wanted, 1)); /* same hash under another extension: not wanted */
	CHECK(library_is_stale(HASH_B ".cube", NULL, 0));
	CHECK(!library_is_stale("index.json", wanted, 1));
	CHECK(!library_is_stale("my-own-lut.cube", wanted, 1)); /* not ours */
	CHECK(!library_is_stale(".DS_Store", wanted, 1));
	CHECK(!library_is_stale("", wanted, 1));
	CHECK(!library_is_stale(NULL, wanted, 1));
	CHECK(library_is_stale(HASH_B ".dl", wanted, 1));
	CHECK(library_is_stale(HASH_B ".dl.17a3f2.part", wanted, 1));
	CHECK(library_is_stale("anything.part", wanted, 1));
	CHECK(!library_is_stale("ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789.cube", wanted, 1));

	/* display names */
	char shown[128];
	library_display_name("Warm Look.cube", shown, sizeof(shown));
	CHECK_STR(shown, "Warm Look.cube");
	library_display_name("../../etc/passwd", shown, sizeof(shown));
	CHECK_STR(shown, "passwd");
	library_display_name("C:\\Users\\x\\Film.cube", shown, sizeof(shown));
	CHECK_STR(shown, "Film.cube");
	library_display_name("a\nb\tc", shown, sizeof(shown));
	CHECK_STR(shown, "a_b_c");
	library_display_name("", shown, sizeof(shown));
	CHECK_STR(shown, "untitled");
	library_display_name(NULL, shown, sizeof(shown));
	CHECK_STR(shown, "untitled");
	library_display_name("   ", shown, sizeof(shown));
	CHECK_STR(shown, "untitled");
	library_display_name("dir/", shown, sizeof(shown));
	CHECK_STR(shown, "untitled");
	library_display_name("small", shown, 4);
	CHECK_STR(shown, "sma");

	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("library mirror: all checks passed\n");
	return 0;
}
