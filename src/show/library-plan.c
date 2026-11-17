/* See library-plan.h. */
#include "library-plan.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool library_valid_sha256(const char *sha256)
{
	if (!sha256 || strlen(sha256) != LIBRARY_SHA256_LEN)
		return false;
	for (int i = 0; i < LIBRARY_SHA256_LEN; i++) {
		if (!isxdigit((unsigned char)sha256[i]))
			return false;
	}
	return true;
}

static bool ends_with_nocase(const char *text, const char *suffix)
{
	const size_t t = strlen(text), s = strlen(suffix);
	if (t < s)
		return false;
	for (size_t i = 0; i < s; i++) {
		if (tolower((unsigned char)text[t - s + i]) != tolower((unsigned char)suffix[i]))
			return false;
	}
	return true;
}

const char *library_extension(const char *kind, const char *name)
{
	if (kind && *kind && strcmp(kind, "lut") != 0)
		return NULL;
	return name && ends_with_nocase(name, ".png") ? ".png" : ".cube";
}

bool library_cache_name(const char *sha256, const char *ext, char *out, size_t size)
{
	if (!library_valid_sha256(sha256) || !ext || !size)
		return false;
	size_t n = 0;
	for (int i = 0; i < LIBRARY_SHA256_LEN && n + 1 < size; i++)
		out[n++] = (char)tolower((unsigned char)sha256[i]);
	out[n] = 0;
	return snprintf(out + n, size - n, "%s", ext) < (int)(size - n);
}

static bool is_hash_file(const char *name)
{
	if (strlen(name) < LIBRARY_SHA256_LEN + 1)
		return false;
	for (int i = 0; i < LIBRARY_SHA256_LEN; i++) {
		const char c = name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return name[LIBRARY_SHA256_LEN] == '.';
}

bool library_is_stale(const char *name, const char *const *wanted, size_t wanted_count)
{
	if (!name || !*name || name[0] == '.')
		return false;
	if (ends_with_nocase(name, ".part") || ends_with_nocase(name, ".dl"))
		return true; /* an interrupted download */
	if (!is_hash_file(name))
		return false; /* index.json, or something the user put there */
	for (size_t i = 0; i < wanted_count; i++) {
		if (strcmp(wanted[i], name) == 0)
			return false;
	}
	return true;
}

void library_display_name(const char *name, char *out, size_t size)
{
	if (!size)
		return;
	const char *base = name ? name : "";
	for (const char *c = base; *c; c++) {
		if (*c == '/' || *c == '\\')
			base = c + 1;
	}
	size_t n = 0;
	for (const char *c = base; *c && n < 100 && n + 1 < size; c++) {
		out[n++] = (unsigned char)*c < 0x20 || *c == 0x7f ? '_' : *c;
	}
	while (n && (out[n - 1] == ' ' || out[n - 1] == '.'))
		n--;
	out[n] = 0;
	if (!n)
		snprintf(out, size, "untitled");
}
