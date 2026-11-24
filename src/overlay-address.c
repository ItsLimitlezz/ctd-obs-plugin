/* See overlay-address.h. */
#include "overlay-address.h"

#include <stdlib.h>
#include <string.h>

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool same_text(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

/* the length of origin with at most one trailing '/' dropped, or 0 when it isn't an http(s) origin */
static size_t origin_len(const char *origin, bool https_only)
{
	if (!origin)
		return 0;
	const char *host = NULL;
	if (strncmp(origin, "https://", 8) == 0)
		host = origin + 8;
	else if (!https_only && strncmp(origin, "http://", 7) == 0)
		host = origin + 7;
	if (!host || !*host || *host == '/' || *host == ':')
		return 0;
	size_t n = strlen(origin);
	if (origin[n - 1] == '/')
		n--;
	if (n >= CTD_ORIGIN_MAX || origin + n <= host)
		return 0;
	const char *end = origin + n;
	for (const char *c = host; c < end; c++) {
		const unsigned char u = (unsigned char)*c;
		if (u <= ' ' || u >= 0x7f || *c == '/' || *c == '?' || *c == '#' || *c == '@' || *c == '\\' ||
		    *c == '"' || *c == '<' || *c == '>')
			return 0;
	}
	/* the host: "[ipv6]" (brackets around the whole of it, nothing but ":port" after), else a name or IPv4
	 * address with no brackets and at most one ':' (its port) */
	const char *colon = NULL;
	if (*host == '[') {
		const char *close = memchr(host, ']', (size_t)(end - host));
		if (!close || close == host + 1 || memchr(host + 1, '[', (size_t)(close - host - 1)) ||
		    (close + 1 < end && close[1] != ':'))
			return 0;
		colon = close + 1 < end ? close + 1 : NULL;
		if (colon && memchr(colon + 1, ':', (size_t)(end - colon - 1)))
			return 0;
	} else {
		if (memchr(host, '[', (size_t)(end - host)) || memchr(host, ']', (size_t)(end - host)))
			return 0;
		colon = memchr(host, ':', (size_t)(end - host));
		if (colon && memchr(colon + 1, ':', (size_t)(end - colon - 1)))
			return 0; /* "host:bogus:443" */
	}
	if (colon) { /* a port: digits only, at least one */
		if (colon + 1 == end)
			return 0;
		for (const char *c = colon + 1; c < end; c++)
			if (*c < '0' || *c > '9')
				return 0;
	}
	return n;
}

bool ctd_origin_ok(const char *origin, bool https_only)
{
	return origin_len(origin, https_only) > 0;
}

/* copies origin (without a trailing '/') into out when it is one; else out is "" */
static bool take_origin(char out[CTD_ORIGIN_MAX], const char *origin, bool https_only)
{
	const size_t n = origin_len(origin, https_only);
	memcpy(out, origin ? origin : "", n);
	out[n] = '\0';
	return n > 0;
}

/* the length of url's origin when url is at `origin` (it starts with it, ignoring case, then '/', '?', '#' or the
 * end), else 0 */
static size_t at_origin(const char *url, const char *origin)
{
	const size_t n = origin ? strlen(origin) : 0;
	if (!url || !n || strlen(url) < n || !same_text(url, origin, n))
		return 0;
	const char next = url[n];
	return next == '\0' || next == '/' || next == '?' || next == '#' ? n : 0;
}

/* the length of url's own origin ("http(s)://host[:port]"), or 0 */
static size_t own_origin(const char *url)
{
	if (!url)
		return 0;
	const char *host = NULL;
	if (strncmp(url, "https://", 8) == 0)
		host = url + 8;
	else if (strncmp(url, "http://", 7) == 0)
		host = url + 7;
	if (!host)
		return 0;
	const char *end = host;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	const size_t n = (size_t)(end - url);
	char buf[CTD_ORIGIN_MAX];
	if (n >= CTD_ORIGIN_MAX)
		return 0;
	memcpy(buf, url, n);
	buf[n] = '\0';
	return origin_len(buf, false) == n ? n : 0;
}

static void legacy(struct ctd_overlay_origins *o, const char *url)
{
	const size_t n = own_origin(url);
	memset(o, 0, sizeof(*o));
	o->legacy = true;
	if (n) {
		memcpy(o->at, url, n);
		o->at[n] = '\0';
	}
}

void ctd_overlay_origins_read(struct ctd_overlay_origins *o, const char *url, bool has_addresses, const char *local,
			      const char *pub, const char *def)
{
	legacy(o, url);
	if (!has_addresses || !o->at[0])
		return;
	struct ctd_overlay_origins r;
	memset(&r, 0, sizeof(r));
	take_origin(r.local, local, false);
	take_origin(r.pub, pub, true);
	/* named, but not https://: never used, though the url may be at it (then only the local address is offered) */
	char refused[CTD_ORIGIN_MAX] = "";
	if (pub && *pub && !r.pub[0]) {
		r.refused_public = true;
		take_origin(refused, pub, false);
	}
	/* the default origin: the one def names, else whichever the url is at */
	const char *named = NULL;
	if (def && strcmp(def, "local") == 0)
		named = r.local;
	else if (def && strcmp(def, "public") == 0)
		named = r.pub[0] ? r.pub : refused;
	else if (at_origin(url, r.local))
		named = r.local;
	else if (at_origin(url, r.pub))
		named = r.pub;
	else if (at_origin(url, refused))
		named = refused;
	if (!named || !named[0] || at_origin(url, named) != strlen(o->at))
		return; /* the url isn't where meta.addresses says: nothing can be swapped safely */
	if (!r.local[0] && !r.pub[0] && !r.refused_public)
		return;
	memcpy(r.at, o->at, sizeof(r.at));
	*o = r;
}

bool ctd_overlay_has_address(const struct ctd_overlay_origins *o, enum ctd_address a)
{
	if (!o)
		return false;
	if (a == CTD_ADDRESS_LOCAL)
		return o->legacy ? o->at[0] != '\0' : o->local[0] != '\0';
	if (a == CTD_ADDRESS_PUBLIC)
		return !o->legacy && o->pub[0] != '\0';
	return false;
}

enum ctd_address ctd_overlay_address_of(const struct ctd_overlay_origins *o, const char *url)
{
	if (!o || o->legacy)
		return CTD_ADDRESS_NONE;
	if (at_origin(url, o->local))
		return CTD_ADDRESS_LOCAL;
	if (at_origin(url, o->pub))
		return CTD_ADDRESS_PUBLIC;
	return CTD_ADDRESS_NONE;
}

/* the length of url's origin when it is at one of the overlay's origins, else 0 */
static size_t known_origin(const struct ctd_overlay_origins *o, const char *url)
{
	size_t n = at_origin(url, o->at);
	if (!n && !o->legacy) {
		n = at_origin(url, o->local);
		if (!n)
			n = at_origin(url, o->pub);
	}
	return n;
}

static char *with_origin(const char *origin, const char *rest)
{
	const size_t a = strlen(origin), b = strlen(rest);
	char *out = malloc(a + b + 1);
	if (!out)
		return NULL;
	memcpy(out, origin, a);
	memcpy(out + a, rest, b + 1);
	return out;
}

static const char *target_of(const struct ctd_overlay_origins *o, enum ctd_address want)
{
	if (!ctd_overlay_has_address(o, want))
		return NULL;
	if (o->legacy)
		return o->at;
	return want == CTD_ADDRESS_PUBLIC ? o->pub : o->local;
}

char *ctd_overlay_url_for(const struct ctd_overlay_origins *o, const char *url, enum ctd_address want)
{
	if (!o || !url)
		return NULL;
	const char *target = target_of(o, want);
	const size_t n = known_origin(o, url);
	if (!target || !n)
		return NULL;
	return with_origin(target, url + n);
}

char *ctd_overlay_url_at_default(const struct ctd_overlay_origins *o, const char *url)
{
	if (!o || !url || !o->at[0])
		return NULL;
	const size_t n = known_origin(o, url);
	return n ? with_origin(o->at, url + n) : NULL;
}

/* the length of the path after an origin: up to '?', '#' or the end */
static size_t path_len(const char *rest)
{
	size_t n = 0;
	while (rest[n] && rest[n] != '?' && rest[n] != '#')
		n++;
	return n;
}

enum ctd_switch ctd_overlay_switch_url(const struct ctd_overlay_origins *o, const char *overlay_url,
				       const char *source_url, enum ctd_address want, char **out)
{
	if (out)
		*out = NULL;
	if (!o || !overlay_url || !source_url)
		return CTD_SWITCH_NOT_OURS;
	const size_t on = at_origin(overlay_url, o->at);
	const size_t sn = known_origin(o, source_url);
	if (!on || !sn)
		return CTD_SWITCH_NOT_OURS;
	const char *opath = overlay_url + on, *spath = source_url + sn;
	const size_t op = path_len(opath), sp = path_len(spath);
	/* the overlay's own path, exactly ("/o/lower" is not "/o/lower2" nor "/o/lower/x") */
	if (op == 0 || op != sp || memcmp(opath, spath, op) != 0)
		return CTD_SWITCH_NOT_OURS;
	const char *target = target_of(o, want);
	if (!target)
		return CTD_SWITCH_UNAVAILABLE;
	if (at_origin(source_url, target)) /* letter case aside, it is there */
		return CTD_SWITCH_ALREADY;
	char *url = with_origin(target, spath);
	if (!url)
		return CTD_SWITCH_NOT_OURS;
	if (out)
		*out = url;
	else
		free(url);
	return CTD_SWITCH_CHANGED;
}
