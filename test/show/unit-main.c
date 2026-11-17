/* ctd-show-unit: runs every Show Profiles pure-logic suite.
 * Usage: ctd-show-unit <fixtures folder> <scratch folder> [suite...] */
#include "unit.h"
#include "show-fs.h"

#include <stdlib.h>

int unit_failures;
int unit_checks;
const char *unit_fixtures;
const char *unit_scratch;

void test_json(void);
void test_manifest(void);
void test_scrub(void);
void test_store(void);
void test_assets(void);
void test_ini(void);
void test_export(void);
void test_devices(void);
void test_sync(void);

static const struct {
	const char *name;
	void (*fn)(void);
} suites[] = {
	{"json", test_json},     {"manifest", test_manifest}, {"scrub", test_scrub},
	{"store", test_store},   {"assets", test_assets},     {"ini", test_ini},
	{"export", test_export}, {"devices", test_devices},   {"sync", test_sync},
};

void unit_fixture_path(const char *name, char *out, size_t size)
{
	sfs_join(out, size, unit_fixtures, name);
}

cj *unit_fixture_json(const char *name)
{
	char path[1024], err[256];
	unit_fixture_path(name, path, sizeof(path));
	cj *v = cj_parse_file(path, err, sizeof(err));
	if (!v) {
		unit_failures++;
		fprintf(stderr, "fixture %s: %s\n", name, err);
	}
	return v;
}

char *unit_fixture_text(const char *name)
{
	char path[1024];
	char *data = NULL;
	size_t size = 0;
	unit_fixture_path(name, path, sizeof(path));
	if (!sfs_read(path, &data, &size)) {
		unit_failures++;
		fprintf(stderr, "fixture %s: can't read\n", name);
		return NULL;
	}
	return data;
}

void unit_scratch_dir(const char *name, char *out, size_t size)
{
	sfs_join(out, size, unit_scratch, name);
	sfs_remove_tree(out);
	if (!sfs_mkdirs(out)) {
		unit_failures++;
		fprintf(stderr, "can't make %s\n", out);
	}
}

bool unit_contains(const char *text, const char *needle)
{
	return text && needle && strstr(text, needle) != NULL;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <fixtures> <scratch> [suite...]\n", argv[0]);
		return 2;
	}
	unit_fixtures = argv[1];
	unit_scratch = argv[2];
	if (!sfs_mkdirs(unit_scratch)) {
		fprintf(stderr, "can't make %s\n", unit_scratch);
		return 2;
	}
	for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
		bool wanted = argc == 3;
		for (int a = 3; a < argc; a++)
			wanted = wanted || strcmp(argv[a], suites[i].name) == 0;
		if (!wanted)
			continue;
		int before = unit_failures;
		suites[i].fn();
		printf("%-10s %s\n", suites[i].name, unit_failures == before ? "ok" : "FAILED");
	}
	printf("ctd-show-unit: %d checks, %d failed\n", unit_checks, unit_failures);
	return unit_failures ? 1 : 0;
}
