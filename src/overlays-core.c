/* See overlays-core.h. */
#include "overlays-core.h"
#include "overlay-scenes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct version {
	bool valid;
	long nums[3];
	const char *pre; /* the pre-release part (after '-', up to '+' or the end), or NULL */
	size_t pre_len;
};

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static struct version parse_version(const char *s)
{
	struct version v = {0};
	if (!s)
		return v;
	if (*s == 'v' || *s == 'V')
		s++;
	for (int i = 0; i < 3; i++) {
		if (!is_digit(*s)) {
			if (i == 0)
				return v;
			break;
		}
		long n = 0;
		while (is_digit(*s)) {
			if (n < 100000000)
				n = n * 10 + (*s - '0');
			s++;
		}
		v.nums[i] = n;
		if (i < 2 && *s == '.' && is_digit(s[1]))
			s++;
		else
			break;
	}
	if (*s == '-') {
		v.pre = ++s;
		while (*s && *s != '+')
			s++;
		v.pre_len = (size_t)(s - v.pre);
		if (!v.pre_len)
			v.pre = NULL;
	}
	if (*s && *s != '+')
		return v; /* trailing text: not a version */
	v.valid = true;
	return v;
}

/* one dot-separated identifier of a pre-release: numeric ones compare as numbers and sort before text */
static int compare_ident(const char *a, size_t an, const char *b, size_t bn)
{
	bool a_num = an > 0, b_num = bn > 0;
	for (size_t i = 0; i < an; i++)
		a_num = a_num && is_digit(a[i]);
	for (size_t i = 0; i < bn; i++)
		b_num = b_num && is_digit(b[i]);
	if (a_num && b_num) {
		while (an > 1 && *a == '0') {
			a++;
			an--;
		}
		while (bn > 1 && *b == '0') {
			b++;
			bn--;
		}
		if (an != bn)
			return an < bn ? -1 : 1;
		const int c = memcmp(a, b, an);
		return c < 0 ? -1 : c > 0;
	}
	if (a_num != b_num)
		return a_num ? -1 : 1;
	const size_t n = an < bn ? an : bn;
	const int c = memcmp(a, b, n);
	if (c)
		return c < 0 ? -1 : 1;
	return an == bn ? 0 : an < bn ? -1 : 1;
}

static int compare_pre(const struct version *x, const struct version *y)
{
	if (!x->pre || !y->pre)
		return !x->pre && !y->pre ? 0 : x->pre ? -1 : 1; /* a release sorts after its pre-releases */
	const char *a = x->pre, *a_end = x->pre + x->pre_len;
	const char *b = y->pre, *b_end = y->pre + y->pre_len;
	while (a < a_end && b < b_end) {
		const char *ad = memchr(a, '.', (size_t)(a_end - a));
		const char *bd = memchr(b, '.', (size_t)(b_end - b));
		const char *ae = ad ? ad : a_end, *be = bd ? bd : b_end;
		const int c = compare_ident(a, (size_t)(ae - a), b, (size_t)(be - b));
		if (c)
			return c;
		a = ad ? ad + 1 : a_end;
		b = bd ? bd + 1 : b_end;
	}
	return a < a_end ? 1 : b < b_end ? -1 : 0;
}

int ctd_semver_compare(const char *a, const char *b)
{
	const struct version x = parse_version(a), y = parse_version(b);
	if (!x.valid || !y.valid)
		return x.valid == y.valid ? 0 : x.valid ? 1 : -1;
	for (int i = 0; i < 3; i++)
		if (x.nums[i] != y.nums[i])
			return x.nums[i] < y.nums[i] ? -1 : 1;
	return compare_pre(&x, &y);
}

enum ctd_update_kind ctd_update_kind_of(const char *from, const char *to)
{
	const struct version x = parse_version(from), y = parse_version(to);
	if (!x.valid || !y.valid || ctd_semver_compare(to, from) <= 0)
		return CTD_UPDATE_NONE;
	if (x.nums[0] != y.nums[0] || (x.nums[0] == 0 && x.nums[1] != y.nums[1]) || x.pre || y.pre)
		return CTD_UPDATE_MAJOR;
	return x.nums[1] != y.nums[1] ? CTD_UPDATE_MINOR : CTD_UPDATE_PATCH;
}

enum ctd_update_kind ctd_update_kind_parse(const char *text)
{
	if (!text)
		return CTD_UPDATE_NONE;
	if (strcmp(text, "patch") == 0)
		return CTD_UPDATE_PATCH;
	if (strcmp(text, "minor") == 0)
		return CTD_UPDATE_MINOR;
	if (strcmp(text, "major") == 0)
		return CTD_UPDATE_MAJOR;
	return CTD_UPDATE_NONE;
}

static bool unreserved(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit((char)c) || c == '-' || c == '.' ||
	       c == '_' || c == '~';
}

/* "http(s)://host[:port]" (a path is kept, a trailing '/' dropped); no query, fragment, spaces or credentials */
static bool usable_base(const char *ui, size_t *len)
{
	if (!ui)
		return false;
	const char *rest = NULL;
	if (strncmp(ui, "http://", 7) == 0)
		rest = ui + 7;
	else if (strncmp(ui, "https://", 8) == 0)
		rest = ui + 8;
	if (!rest || !*rest || *rest == '/')
		return false;
	for (const char *c = rest; *c; c++)
		if (*c == '?' || *c == '#' || *c == '@' || (unsigned char)*c <= ' ' || *c == '"' || *c == '\\')
			return false;
	size_t n = strlen(ui);
	while (n > 0 && ui[n - 1] == '/')
		n--;
	*len = n;
	return true;
}

static char *join(const char *ui, size_t ui_len, const char *route, const char *part, const char *tail)
{
	static const char hex[] = "0123456789ABCDEF";
	const size_t part_len = strlen(part);
	char *out = malloc(ui_len + strlen(route) + part_len * 3 + strlen(tail) + 1);
	if (!out)
		return NULL;
	memcpy(out, ui, ui_len);
	char *p = out + ui_len;
	const size_t route_len = strlen(route);
	memcpy(p, route, route_len);
	p += route_len;
	for (const unsigned char *c = (const unsigned char *)part; *c; c++) {
		if (unreserved(*c)) {
			*p++ = (char)*c;
		} else {
			*p++ = '%';
			*p++ = hex[*c >> 4];
			*p++ = hex[*c & 15];
		}
	}
	memcpy(p, tail, strlen(tail) + 1);
	return out;
}

char *ctd_overlay_admin_url(const char *ui, const char *slug, bool upgrade)
{
	size_t len;
	if (!usable_base(ui, &len) || !slug || !*slug)
		return NULL;
	return join(ui, len, "/#/overlays/", slug, upgrade ? "?tab=version" : "");
}

char *ctd_template_admin_url(const char *ui, const char *template_id)
{
	size_t len;
	if (!usable_base(ui, &len) || !template_id || !*template_id)
		return NULL;
	return join(ui, len, "/#/templates/", template_id, "");
}

static bool repo_char(char c, bool owner)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' ||
	       (!owner && (c == '.' || c == '_'));
}

char *ctd_github_releases_url(const char *repo, const char *tag_prefix, const char *version)
{
	if (!repo)
		return NULL;
	const char *slash = strchr(repo, '/');
	if (!slash || slash == repo || slash - repo > 39 || !slash[1] || strlen(slash + 1) > 100)
		return NULL;
	for (const char *c = repo; c < slash; c++)
		if (!repo_char(*c, true))
			return NULL;
	for (const char *c = slash + 1; *c; c++)
		if (!repo_char(*c, false))
			return NULL;
	if (strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0)
		return NULL;
	static const char base[] = "https://github.com/";
	const struct version v = parse_version(version);
	if (!version || !*version || !v.valid) {
		const size_t n = strlen(base) + strlen(repo) + strlen("/releases") + 1;
		char *out = malloc(n);
		if (out)
			snprintf(out, n, "%s%s/releases", base, repo);
		return out;
	}
	/* the tag: the prefix, else "v" (unless the version already starts with one), then the version */
	const char *bare = (*version == 'v' || *version == 'V') ? version + 1 : version;
	const char *prefix = tag_prefix && *tag_prefix ? tag_prefix : "v";
	const size_t head = strlen(base) + strlen(repo) + strlen("/releases/tag/");
	char *out = malloc(head + (strlen(prefix) + strlen(bare)) * 3 + 1);
	if (!out)
		return NULL;
	snprintf(out, head + 1, "%s%s/releases/tag/", base, repo);
	char *p = out + head;
	static const char hex[] = "0123456789ABCDEF";
	for (int part = 0; part < 2; part++)
		for (const unsigned char *c = (const unsigned char *)(part ? bare : prefix); *c; c++) {
			if (unreserved(*c)) {
				*p++ = (char)*c;
			} else {
				*p++ = '%';
				*p++ = hex[*c >> 4];
				*p++ = hex[*c & 15];
			}
		}
	*p = '\0';
	return out;
}

/* the URL without scene= and without its #fragment */
static char *base_of(const char *url)
{
	char *plain = ctd_overlay_scene_url(url, NULL);
	if (plain) {
		char *hash = strchr(plain, '#');
		if (hash)
			*hash = '\0';
	}
	return plain;
}

bool ctd_overlay_url_same(const char *source_url, const char *overlay_url)
{
	if (!source_url || !overlay_url || !*source_url || !*overlay_url)
		return false;
	char *a = base_of(source_url), *b = base_of(overlay_url);
	const bool same = a && b && strcmp(a, b) == 0;
	free(a);
	free(b);
	return same;
}

char *ctd_url_origin(const char *url)
{
	if (!url)
		return NULL;
	const char *host = NULL;
	if (strncmp(url, "http://", 7) == 0)
		host = url + 7;
	else if (strncmp(url, "https://", 8) == 0)
		host = url + 8;
	if (!host)
		return NULL;
	const char *end = host;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	if (end == host)
		return NULL;
	for (const char *c = host; c < end; c++)
		if (*c == '@' || (unsigned char)*c <= ' ')
			return NULL;
	const size_t n = (size_t)(end - url);
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	memcpy(out, url, n);
	out[n] = '\0';
	return out;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *ctd_overlay_slug_of_url(const char *url)
{
	char *origin = ctd_url_origin(url);
	if (!origin)
		return NULL;
	const char *path = url + strlen(origin);
	free(origin);
	if (strncmp(path, "/o/", 3) != 0)
		return NULL;
	const char *start = path + 3, *end = start;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	if (end == start)
		return NULL;
	char *out = malloc((size_t)(end - start) + 1);
	if (!out)
		return NULL;
	char *o = out;
	for (const char *c = start; c < end; c++) {
		if (*c == '%' && end - c >= 3 && hex_digit(c[1]) >= 0 && hex_digit(c[2]) >= 0) {
			*o++ = (char)(hex_digit(c[1]) * 16 + hex_digit(c[2]));
			c += 2;
		} else {
			*o++ = *c;
		}
	}
	*o = '\0';
	return out;
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool ctd_text_contains(const char *text, const char *needle)
{
	if (!needle || !*needle)
		return true;
	if (!text)
		return false;
	const size_t n = strlen(needle);
	for (const char *t = text; *t; t++) {
		size_t i = 0;
		while (i < n && t[i] && lower(t[i]) == lower(needle[i]))
			i++;
		if (i == n)
			return true;
	}
	return false;
}
