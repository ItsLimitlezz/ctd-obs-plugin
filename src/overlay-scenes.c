/* See overlay-scenes.h. */
#include "overlay-scenes.h"

#include <stdlib.h>
#include <string.h>

struct buf {
	char *s;
	size_t len, cap;
	bool failed;
};

static void put(struct buf *b, const char *s, size_t n)
{
	if (b->failed)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (b->len + n + 1 > cap)
			cap *= 2;
		char *grown = realloc(b->s, cap);
		if (!grown) {
			b->failed = true;
			return;
		}
		b->s = grown;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static char *done(struct buf *b)
{
	if (!b->failed && !b->s)
		put(b, "", 0);
	if (b->failed) {
		free(b->s);
		return NULL;
	}
	return b->s;
}

static bool unreserved(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
	       c == '_' || c == '~';
}

/* is [p, end) the parameter `scene` (with or without a value)? */
static bool is_scene_param(const char *p, const char *end)
{
	static const char name[] = "scene";
	const size_t n = sizeof(name) - 1;
	return (size_t)(end - p) >= n && memcmp(p, name, n) == 0 && (p + n == end || p[n] == '=');
}

char *ctd_overlay_scene_url(const char *url, const char *scene)
{
	if (!url)
		return NULL;
	const char *hash = strchr(url, '#');
	const char *end = hash ? hash : url + strlen(url);
	const char *q = memchr(url, '?', (size_t)(end - url));
	struct buf b = {0};
	put(&b, url, (size_t)((q ? q : end) - url));
	bool first = true;
	if (q) {
		for (const char *p = q + 1; p <= end;) {
			const char *amp = memchr(p, '&', (size_t)(end - p));
			const char *stop = amp ? amp : end;
			if (stop > p && !is_scene_param(p, stop)) {
				put(&b, first ? "?" : "&", 1);
				put(&b, p, (size_t)(stop - p));
				first = false;
			}
			if (!amp)
				break;
			p = amp + 1;
		}
	}
	if (scene && *scene) {
		static const char hex[] = "0123456789ABCDEF";
		put(&b, first ? "?scene=" : "&scene=", 7);
		for (const unsigned char *c = (const unsigned char *)scene; *c; c++) {
			if (unreserved(*c)) {
				put(&b, (const char *)c, 1);
			} else {
				const char esc[3] = {'%', hex[*c >> 4], hex[*c & 15]};
				put(&b, esc, 3);
			}
		}
	}
	if (hash)
		put(&b, hash, strlen(hash));
	return done(&b);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

char *ctd_overlay_scene_of_url(const char *url)
{
	if (!url)
		return NULL;
	const char *hash = strchr(url, '#');
	const char *end = hash ? hash : url + strlen(url);
	const char *q = memchr(url, '?', (size_t)(end - url));
	const char *found = NULL, *found_end = NULL;
	for (const char *p = q ? q + 1 : end; p < end;) {
		const char *amp = memchr(p, '&', (size_t)(end - p));
		const char *stop = amp ? amp : end;
		if (is_scene_param(p, stop) && stop > p + 6) {
			found = p + 6; /* the first, as the overlay page reads it (URLSearchParams.get) */
			found_end = stop;
			break;
		}
		if (!amp)
			break;
		p = amp + 1;
	}
	if (!found)
		return NULL;
	struct buf b = {0};
	for (const char *c = found; c < found_end; c++) {
		char out = *c;
		if (*c == '+') {
			out = ' ';
		} else if (*c == '%' && found_end - c >= 3 && hexval(c[1]) >= 0 && hexval(c[2]) >= 0) {
			out = (char)(hexval(c[1]) * 16 + hexval(c[2]));
			c += 2;
		}
		put(&b, &out, 1);
	}
	return done(&b);
}

/* the next letter or digit of s (lowercased), or 0 at the end */
static unsigned char next_key(const unsigned char **s)
{
	for (; **s; (*s)++) {
		unsigned char c = **s;
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
			(*s)++;
			return c;
		}
	}
	return 0;
}

static bool same_key(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
	unsigned char cx = next_key(&x), cy = next_key(&y);
	if (!cx || !cy)
		return false; /* nothing to compare: never a match */
	while (cx && cx == cy) {
		cx = next_key(&x);
		cy = next_key(&y);
	}
	return cx == 0 && cy == 0;
}

bool ctd_overlay_scene_matches(const char *obs_scene, const char *id, const char *name)
{
	return same_key(obs_scene, name) || same_key(obs_scene, id);
}
