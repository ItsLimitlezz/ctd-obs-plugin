/* Deterministic ustar writer and a strict reader for show-profile bundles. See tar.h. */
#include "tar.h"
#include "manifest.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 512

static bool fail(char *err, size_t size, const char *fmt, ...)
{
	if (err && size) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err, size, fmt, ap);
		va_end(ap);
	}
	return false;
}

struct entry {
	const char *path;
	const char *data;
	size_t size;
};

static int compare_entries(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path);
}

static void octal(char *field, size_t width, uint64_t value)
{
	/* width - 1 digits, zero padded, then NUL */
	field[width - 1] = '\0';
	for (size_t i = width - 1; i-- > 0;) {
		field[i] = (char)('0' + (value & 7));
		value >>= 3;
	}
}

/* ustar name (100) + prefix (155) split at a '/' */
static bool put_name(char *header, const char *path)
{
	size_t n = strlen(path);
	if (n <= 100) {
		memcpy(header, path, n);
		return true;
	}
	for (size_t i = n; i-- > 0;) {
		if (path[i] != '/')
			continue;
		if (i <= 155 && n - i - 1 <= 100 && n - i - 1 > 0) {
			memcpy(header + 345, path, i);
			memcpy(header, path + i + 1, n - i - 1);
			return true;
		}
	}
	return false;
}

static bool write_header(char *header, const char *path, size_t size)
{
	memset(header, 0, BLOCK);
	if (!put_name(header, path))
		return false;
	octal(header + 100, 8, 0644);     /* mode */
	octal(header + 108, 8, 0);        /* uid */
	octal(header + 116, 8, 0);        /* gid */
	octal(header + 124, 12, size);    /* size */
	octal(header + 136, 12, 0);       /* mtime */
	header[156] = '0';                /* regular file */
	memcpy(header + 257, "ustar", 6); /* magic + NUL */
	memcpy(header + 263, "00", 2);    /* version */
	memset(header + 148, ' ', 8);
	unsigned sum = 0;
	for (size_t i = 0; i < BLOCK; i++)
		sum += (unsigned char)header[i];
	octal(header + 148, 7, sum);
	header[155] = ' ';
	return true;
}

bool show_tar_write(const struct show_bundle *b, char **out, size_t *size)
{
	*out = NULL;
	*size = 0;
	if (!b || !b->collection)
		return false;
	char *collection = cj_print(b->collection, true);
	if (!collection)
		return false;
	size_t count = b->file_count + 1;
	struct entry *entries = calloc(count, sizeof(*entries));
	bool ok = false;
	char *buf = NULL;
	if (!entries)
		goto done;
	entries[0] = (struct entry){SHOW_FILE_COLLECTION, collection, strlen(collection)};
	for (size_t i = 0; i < b->file_count; i++)
		entries[i + 1] = (struct entry){b->files[i].path, b->files[i].data, b->files[i].size};
	qsort(entries, count, sizeof(*entries), compare_entries);

	size_t total = 2 * BLOCK;
	for (size_t i = 0; i < count; i++) {
		if (!show_path_is_safe(entries[i].path) || (i && strcmp(entries[i].path, entries[i - 1].path) == 0))
			goto done;
		total += BLOCK + (entries[i].size + BLOCK - 1) / BLOCK * BLOCK;
	}
	buf = calloc(1, total);
	if (!buf)
		goto done;
	size_t at = 0;
	for (size_t i = 0; i < count; i++) {
		if (!write_header(buf + at, entries[i].path, entries[i].size))
			goto done;
		at += BLOCK;
		if (entries[i].size)
			memcpy(buf + at, entries[i].data, entries[i].size);
		at += (entries[i].size + BLOCK - 1) / BLOCK * BLOCK;
	}
	*out = buf;
	*size = total;
	buf = NULL;
	ok = true;
done:
	free(buf);
	free(entries);
	cj_free_str(collection);
	return ok;
}

static bool parse_octal(const char *field, size_t width, uint64_t *value)
{
	uint64_t v = 0;
	size_t i = 0;
	while (i < width && (field[i] == ' ' || field[i] == '\0'))
		i++;
	bool any = false;
	for (; i < width && field[i] >= '0' && field[i] <= '7'; i++) {
		if (v > (UINT64_MAX >> 3))
			return false;
		v = (v << 3) | (uint64_t)(field[i] - '0');
		any = true;
	}
	for (; i < width; i++)
		if (field[i] != ' ' && field[i] != '\0')
			return false;
	*value = v;
	return any;
}

static bool all_zero(const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

static size_t field_len(const char *p, size_t max)
{
	size_t n = 0;
	while (n < max && p[n])
		n++;
	return n;
}

bool show_tar_read(const char *data, size_t size, struct show_bundle *b, char *err, size_t err_size)
{
	size_t at = 0, total = 0;
	if (size > SHOW_BUNDLE_MAX_BYTES + 64 * BLOCK)
		return fail(err, err_size, "the bundle is bigger than 16 MB");
	while (at + BLOCK <= size) {
		const char *h = data + at;
		if (all_zero(h, BLOCK))
			break;
		uint64_t file_size = 0, sum = 0;
		if (!parse_octal(h + 124, 12, &file_size) || !parse_octal(h + 148, 8, &sum))
			return fail(err, err_size, "the bundle is not a tar");
		unsigned computed = 0;
		for (size_t i = 0; i < BLOCK; i++)
			computed += (i >= 148 && i < 156) ? ' ' : (unsigned char)h[i];
		if (computed != sum)
			return fail(err, err_size, "the bundle is damaged (tar checksum)");
		char path[260];
		size_t prefix_len = memcmp(h + 257, "ustar", 5) == 0 ? field_len(h + 345, 155) : 0;
		size_t name_len = field_len(h, 100);
		if (prefix_len) {
			memcpy(path, h + 345, prefix_len);
			path[prefix_len] = '/';
			memcpy(path + prefix_len + 1, h, name_len);
			path[prefix_len + 1 + name_len] = '\0';
		} else {
			memcpy(path, h, name_len);
			path[name_len] = '\0';
		}
		const char type = h[156];
		at += BLOCK;
		if (file_size > size - at)
			return fail(err, err_size, "the bundle is cut short");
		size_t padded = (size_t)((file_size + BLOCK - 1) / BLOCK * BLOCK);
		if (type == '5') { /* a directory */
			at += padded;
			continue;
		}
		if (type != '0' && type != '\0')
			return fail(err, err_size, "the bundle holds something that isn't a file (%s)", path);
		total += (size_t)file_size;
		if (total > SHOW_BUNDLE_MAX_BYTES)
			return fail(err, err_size, "the bundle is bigger than 16 MB");
		/* "./x" from other tar tools */
		const char *rel = strncmp(path, "./", 2) == 0 ? path + 2 : path;
		if (!show_path_is_safe(rel) || strcmp(rel, SHOW_FILE_MANIFEST) == 0 ||
		    strcmp(rel, SHOW_FILE_CHECKSUMS) == 0)
			return fail(err, err_size, "the bundle holds a file it may not (%s)", path);
		if (strcmp(rel, SHOW_FILE_COLLECTION) == 0) {
			if (b->collection)
				return fail(err, err_size, "the bundle holds collection.json twice");
			b->collection = cj_parse(data + at, (size_t)file_size, err, err_size);
			if (!cj_is_object(b->collection))
				return fail(err, err_size, "the bundle's collection.json is not a scene collection");
		} else {
			if (show_bundle_file(b, rel))
				return fail(err, err_size, "the bundle holds %s twice", rel);
			if (!show_bundle_set_file(b, rel, data + at, (size_t)file_size))
				return fail(err, err_size, "out of memory");
		}
		at += padded;
	}
	if (!b->collection)
		return fail(err, err_size, "the bundle has no collection.json");
	return true;
}
