/* ctd-show-tests: device identity extractor against test/show/identity-fixtures (ticket T6).
 * Usage: ctd-show-tests <fixture.json>...   (CMake passes the fixture list). No libobs, no JSON library: the
 * fixtures are objects of strings with one nesting level, flattened here to "settings.key" style paths. */
#include "../../src/show/devices-identity.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KV 32
#define STR 600

typedef struct {
	char key[128];
	char val[STR];
} kv_t;

typedef struct {
	kv_t kv[MAX_KV];
	int n;
} doc_t;

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

/* Parses a JSON string at *pp (which points at the opening quote) into dst. */
static int parse_string(const char **pp, char *dst, size_t cap)
{
	const char *p = *pp;
	size_t w = 0;
	if (*p != '"')
		return 0;
	p++;
	while (*p && *p != '"') {
		char c = *p++;
		if (c == '\\') {
			char e = *p++;
			c = e == 'n' ? '\n' : e == 't' ? '\t' : e; /* \\ \" \/ pass through */
		}
		if (w + 1 < cap)
			dst[w++] = c;
	}
	if (*p != '"')
		return 0;
	dst[w] = '\0';
	*pp = p + 1;
	return 1;
}

static int parse_object(const char **pp, const char *prefix, doc_t *d)
{
	const char *p = skip_ws(*pp);
	if (*p != '{')
		return 0;
	p = skip_ws(p + 1);
	while (*p && *p != '}') {
		char key[64], path[64];
		if (!parse_string(&p, key, sizeof(key)))
			return 0;
		p = skip_ws(p);
		if (*p++ != ':')
			return 0;
		p = skip_ws(p);
		snprintf(path, sizeof(path), "%s%s", prefix, key);
		if (*p == '{') {
			char sub[80];
			snprintf(sub, sizeof(sub), "%s.", path);
			if (!parse_object(&p, sub, d))
				return 0;
		} else if (*p == '"') {
			if (d->n >= MAX_KV)
				return 0;
			snprintf(d->kv[d->n].key, sizeof(d->kv[0].key), "%s", path);
			if (!parse_string(&p, d->kv[d->n].val, STR))
				return 0;
			d->n++;
		} else { /* true / false / number: keep the raw token */
			const char *s = p;
			while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
				p++;
			if (d->n >= MAX_KV)
				return 0;
			snprintf(d->kv[d->n].key, sizeof(d->kv[0].key), "%s", path);
			snprintf(d->kv[d->n].val, STR, "%.*s", (int)(p - s), s);
			d->n++;
		}
		p = skip_ws(p);
		if (*p == ',')
			p = skip_ws(p + 1);
	}
	if (*p != '}')
		return 0;
	*pp = p + 1;
	return 1;
}

static const char *doc_get(const doc_t *d, const char *key)
{
	for (int i = 0; i < d->n; i++)
		if (strcmp(d->kv[i].key, key) == 0)
			return d->kv[i].val;
	return NULL;
}

static const char *settings_get(void *ctx, const char *key)
{
	char path[160];
	snprintf(path, sizeof(path), "settings.%s", key);
	return doc_get((const doc_t *)ctx, path);
}

static int failures;

static void check(const char *file, const char *field, const char *want, const char *got)
{
	if (strcmp(want, got) != 0) {
		fprintf(stderr, "FAIL %s: %s expected \"%s\" got \"%s\"\n", file, field, want, got);
		failures++;
	}
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf)
		buf[n] = '\0';
	return buf;
}

static void run_fixture(const char *path)
{
	char *text = slurp(path);
	if (!text) {
		fprintf(stderr, "FAIL %s: cannot read\n", path);
		failures++;
		return;
	}
	static doc_t doc;
	doc.n = 0;
	const char *p = text;
	if (!parse_object(&p, "", &doc)) {
		fprintf(stderr, "FAIL %s: bad fixture JSON\n", path);
		failures++;
		free(text);
		return;
	}
	const char *type = doc_get(&doc, "sourceType");
	devices_identity_t id;
	int known = devices_identity(type, settings_get, &doc, &id);

	if (doc_get(&doc, "expectUnknown")) {
		if (known || id.cls[0] || devices_identity_class(type)) {
			fprintf(stderr, "FAIL %s: type should be unknown\n", path);
			failures++;
		}
	} else if (!known) {
		fprintf(stderr, "FAIL %s: type \"%s\" not in the table\n", path, type ? type : "(null)");
		failures++;
	} else {
		static const struct {
			const char *key;
			const char *field;
		} fields[] = {{"expect.class", "class"},
			      {"expect.id", "id"},
			      {"expect.name", "name"},
			      {"expect.vidPid", "vidPid"},
			      {"expect.serial", "serial"}};
		const char *got[] = {id.cls, id.id, id.name, id.vid_pid, id.serial};
		for (int i = 0; i < 5; i++) {
			const char *want = doc_get(&doc, fields[i].key);
			if (want) /* only the fields a fixture lists are checked */
				check(path, fields[i].field, want, got[i]);
		}
		/* devices_identity_class must agree with the filled class */
		check(path, "class()", id.cls, devices_identity_class(type));
	}
	free(text);
}

static void unit_tests(void)
{
	devices_identity_t o;
	memset(&o, 0, sizeof(o));
	if (devices_identity_parse_dshow(NULL, &o) || devices_identity_parse_dshow("", &o)) {
		fprintf(stderr, "FAIL dshow parser accepted empty input\n");
		failures++;
	}
	if (devices_identity_parse_dshow("X:\\\\?\\usb#vid_zzzz&pid_0001#i#{g}", &o)) {
		fprintf(stderr, "FAIL dshow parser accepted non-hex VID\n");
		failures++;
	}
	if (devices_identity_parse_avcapture("0x1234", &o) || devices_identity_parse_avcapture("FaceTime", &o)) {
		fprintf(stderr, "FAIL avcapture parser accepted a non-USB id\n");
		failures++;
	}
	/* Oversized id must truncate, not overflow. */
	char big[2000];
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	devices_identity_parse_dshow(big, &o);
	if (devices_identity(NULL, NULL, NULL, &o) || devices_identity("dshow_input", NULL, NULL, &o) == 0) {
		fprintf(stderr, "FAIL null handling\n");
		failures++;
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s fixture.json...\n", argv[0]);
		return 2;
	}
	for (int i = 1; i < argc; i++)
		run_fixture(argv[i]);
	unit_tests();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("ctd-show-tests: %d fixtures ok\n", argc - 1);
	return 0;
}
