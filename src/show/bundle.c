/* In-memory show-profile bundle; see bundle.h. */
#include "bundle.h"
#include "manifest.h"

#include <stdlib.h>
#include <string.h>

void show_bundle_init(struct show_bundle *b)
{
	memset(b, 0, sizeof(*b));
}

void show_bundle_free(struct show_bundle *b)
{
	if (!b)
		return;
	cj_free(b->manifest);
	cj_free(b->collection);
	for (size_t i = 0; i < b->file_count; i++) {
		free(b->files[i].path);
		free(b->files[i].data);
	}
	free(b->files);
	cj_free(b->local);
	cj_free(b->asset_sources);
	memset(b, 0, sizeof(*b));
}

const struct show_file *show_bundle_file(const struct show_bundle *b, const char *path)
{
	for (size_t i = 0; b && i < b->file_count; i++)
		if (strcmp(b->files[i].path, path) == 0)
			return &b->files[i];
	return NULL;
}

bool show_bundle_set_file(struct show_bundle *b, const char *path, const void *data, size_t size)
{
	if (!show_path_is_safe(path))
		return false;
	char *copy = malloc(size + 1);
	if (!copy)
		return false;
	if (size)
		memcpy(copy, data, size);
	copy[size] = '\0';
	for (size_t i = 0; i < b->file_count; i++) {
		if (strcmp(b->files[i].path, path) == 0) {
			free(b->files[i].data);
			b->files[i].data = copy;
			b->files[i].size = size;
			return true;
		}
	}
	struct show_file *files = realloc(b->files, (b->file_count + 1) * sizeof(*files));
	size_t n = strlen(path);
	char *p = malloc(n + 1);
	if (!files || !p) {
		if (files)
			b->files = files;
		free(p);
		free(copy);
		return false;
	}
	memcpy(p, path, n + 1);
	b->files = files;
	b->files[b->file_count].path = p;
	b->files[b->file_count].data = copy;
	b->files[b->file_count].size = size;
	b->file_count++;
	return true;
}
