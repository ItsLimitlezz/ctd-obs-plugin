/* Show Profiles file helpers; see show-fs.h. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno, fsync with -std=c17 */
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "show-fs.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <wchar.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0)
		return NULL;
	wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
	if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	if (w)
		for (wchar_t *p = w; *p; p++)
			if (*p == L'/')
				*p = L'\\';
	return w;
}

static char *narrow(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 0)
		return NULL;
	char *s = malloc((size_t)n);
	if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
		free(s);
		return NULL;
	}
	return s;
}
#endif

FILE *sfs_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
	wchar_t *wp = wide(path), *wm = wide(mode);
	FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
	free(wp);
	free(wm);
	return f;
#else
	return fopen(path, mode);
#endif
}

bool sfs_exists(const char *path)
{
#ifdef _WIN32
	wchar_t *w = wide(path);
	DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
	free(w);
	return attr != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st;
	return stat(path, &st) == 0;
#endif
}

bool sfs_is_dir(const char *path)
{
#ifdef _WIN32
	wchar_t *w = wide(path);
	DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
	free(w);
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int64_t sfs_size(const char *path)
{
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA data;
	wchar_t *w = wide(path);
	BOOL ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &data);
	free(w);
	if (!ok || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return -1;
	return (int64_t)(((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow);
#else
	struct stat st;
	if (stat(path, &st) != 0 || S_ISDIR(st.st_mode))
		return -1;
	return (int64_t)st.st_size;
#endif
}

static bool mkdir_one(const char *path)
{
#ifdef _WIN32
	wchar_t *w = wide(path);
	BOOL ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
	free(w);
	return ok;
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

bool sfs_mkdirs(const char *path)
{
	size_t n = strlen(path);
	char *p = malloc(n + 1);
	if (!p)
		return false;
	memcpy(p, path, n + 1);
	while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'))
		p[--n] = '\0';
	for (size_t i = 1; i < n; i++) {
		if (p[i] != '/' && p[i] != '\\')
			continue;
		if (i == 2 && p[1] == ':')
			continue; /* "C:\" */
		char c = p[i];
		p[i] = '\0';
		if (p[i - 1] != '/' && p[i - 1] != '\\' && !sfs_is_dir(p))
			mkdir_one(p);
		p[i] = c;
	}
	bool ok = sfs_is_dir(p) || (mkdir_one(p) && sfs_is_dir(p));
	free(p);
	return ok;
}

bool sfs_read(const char *path, char **data, size_t *size)
{
	*data = NULL;
	*size = 0;
	FILE *f = sfs_fopen(path, "rb");
	if (!f)
		return false;
	size_t cap = 65536, len = 0;
	char *buf = malloc(cap);
	while (buf) {
		if (len + 1 >= cap) {
			char *grown = realloc(buf, cap * 2);
			if (!grown) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = grown;
			cap *= 2;
		}
		size_t got = fread(buf + len, 1, cap - len - 1, f);
		len += got;
		if (got == 0)
			break;
	}
	bool ok = buf && !ferror(f);
	fclose(f);
	if (!ok) {
		free(buf);
		return false;
	}
	buf[len] = '\0';
	*data = buf;
	*size = len;
	return true;
}

static bool flush_to_disk(FILE *f)
{
	if (fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

bool sfs_rename(const char *from, const char *to)
{
#ifdef _WIN32
	wchar_t *wf = wide(from), *wt = wide(to);
	BOOL ok = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	free(wf);
	free(wt);
	return ok;
#else
	return rename(from, to) == 0;
#endif
}

static bool temp_name(char *out, size_t size, const char *path)
{
	char rnd[33];
	sfs_random_hex(rnd);
	return (size_t)snprintf(out, size, "%s.%.12s.tmp", path, rnd) < size;
}

bool sfs_write_atomic(const char *path, const void *data, size_t size)
{
	size_t cap = strlen(path) + 32;
	char *tmp = malloc(cap);
	if (!tmp || !temp_name(tmp, cap, path)) {
		free(tmp);
		return false;
	}
	FILE *f = sfs_fopen(tmp, "wb");
	bool ok = f != NULL;
	if (ok && size)
		ok = fwrite(data, 1, size, f) == size;
	if (f) {
		ok = flush_to_disk(f) && ok;
		ok = fclose(f) == 0 && ok;
	}
	ok = ok && sfs_rename(tmp, path);
	if (!ok)
		sfs_remove(tmp);
	free(tmp);
	return ok;
}

bool sfs_remove(const char *path)
{
#ifdef _WIN32
	wchar_t *w = wide(path);
	if (w) {
		SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(w);
	}
	free(w);
#else
	unlink(path);
#endif
	return !sfs_exists(path);
}

struct tree_ctx {
	const char *dir;
	bool ok;
};

static bool remove_entry(void *opaque, const char *name, bool is_dir)
{
	struct tree_ctx *t = opaque;
	size_t n = strlen(t->dir) + strlen(name) + 2;
	char *child = malloc(n);
	if (!child) {
		t->ok = false;
		return false;
	}
	snprintf(child, n, "%s/%s", t->dir, name);
	if (!(is_dir ? sfs_remove_tree(child) : sfs_remove(child)))
		t->ok = false;
	free(child);
	return true;
}

bool sfs_remove_tree(const char *path)
{
	if (!sfs_exists(path))
		return true;
	if (!sfs_is_dir(path))
		return sfs_remove(path);
	struct tree_ctx t = {path, true};
	sfs_list(path, remove_entry, &t);
#ifdef _WIN32
	wchar_t *w = wide(path);
	if (w)
		RemoveDirectoryW(w);
	free(w);
#else
	rmdir(path);
#endif
	return !sfs_exists(path);
}

bool sfs_copy_file(const char *from, const char *to)
{
	FILE *in = sfs_fopen(from, "rb");
	if (!in)
		return false;
	size_t cap = strlen(to) + 32;
	char *tmp = malloc(cap);
	FILE *out = NULL;
	bool ok = tmp && temp_name(tmp, cap, to) && (out = sfs_fopen(tmp, "wb")) != NULL;
	char *buf = ok ? malloc(65536) : NULL;
	ok = ok && buf;
	while (ok) {
		size_t got = fread(buf, 1, 65536, in);
		if (got == 0) {
			ok = !ferror(in);
			break;
		}
		ok = fwrite(buf, 1, got, out) == got;
	}
	free(buf);
	fclose(in);
	if (out) {
		ok = flush_to_disk(out) && ok;
		ok = fclose(out) == 0 && ok;
	}
	ok = ok && sfs_rename(tmp, to);
	if (!ok && tmp && out)
		sfs_remove(tmp);
	free(tmp);
	return ok;
}

struct copy_ctx {
	const char *from, *to;
	bool ok;
};

static bool copy_entry(void *opaque, const char *name, bool is_dir)
{
	struct copy_ctx *c = opaque;
	size_t nf = strlen(c->from) + strlen(name) + 2, nt = strlen(c->to) + strlen(name) + 2;
	char *src = malloc(nf), *dst = malloc(nt);
	if (src && dst) {
		snprintf(src, nf, "%s/%s", c->from, name);
		snprintf(dst, nt, "%s/%s", c->to, name);
		if (!(is_dir ? sfs_copy_tree(src, dst) : sfs_copy_file(src, dst)))
			c->ok = false;
	} else {
		c->ok = false;
	}
	free(src);
	free(dst);
	return c->ok;
}

bool sfs_copy_tree(const char *from, const char *to)
{
	if (!sfs_is_dir(from) || sfs_exists(to) || !sfs_mkdirs(to))
		return false;
	struct copy_ctx c = {from, to, true};
	return sfs_list(from, copy_entry, &c) && c.ok;
}

bool sfs_list(const char *dir, sfs_list_cb cb, void *ctx)
{
#ifdef _WIN32
	size_t n = strlen(dir) + 3;
	char *pattern = malloc(n);
	if (!pattern)
		return false;
	snprintf(pattern, n, "%s/*", dir);
	wchar_t *w = wide(pattern);
	free(pattern);
	if (!w)
		return false;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	free(w);
	if (h == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
			continue;
		char *name = narrow(fd.cFileName);
		bool more = !name || cb(ctx, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
		free(name);
		if (!more)
			break;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return true;
#else
	DIR *d = opendir(dir);
	if (!d)
		return false;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		size_t n = strlen(dir) + strlen(e->d_name) + 2;
		char *child = malloc(n);
		bool is_dir = false;
		if (child) {
			snprintf(child, n, "%s/%s", dir, e->d_name);
			is_dir = sfs_is_dir(child);
			free(child);
		}
		if (!cb(ctx, e->d_name, is_dir))
			break;
	}
	closedir(d);
	return true;
#endif
}

bool sfs_join(char *dst, size_t size, const char *a, const char *b)
{
	size_t na = strlen(a);
	while (na && (a[na - 1] == '/' || a[na - 1] == '\\'))
		na--;
	while (*b == '/' || *b == '\\')
		b++;
	int n = snprintf(dst, size, "%.*s/%s", (int)na, a, b);
	return n >= 0 && (size_t)n < size;
}

void sfs_random_hex(char out[33])
{
	static const char hex[] = "0123456789abcdef";
	unsigned char bytes[16];
	bool got = false;
#ifdef _WIN32
	got = BCryptGenRandom(NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		got = read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
		close(fd);
	}
#endif
	if (!got) {
		static unsigned counter;
		unsigned seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)out ^ (++counter * 2654435761u);
		for (size_t i = 0; i < sizeof(bytes); i++) {
			seed = seed * 1103515245u + 12345u;
			bytes[i] = (unsigned char)(seed >> 16);
		}
	}
	for (size_t i = 0; i < sizeof(bytes); i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 15];
	}
	out[32] = '\0';
}
