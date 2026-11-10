/* A small JSON document model for Show Profiles (design section 1).
 *
 * Pure C, no libobs: the bundle code (manifest, scrubber, asset rewriter, store) runs in the plugin and in
 * ctd-show-tests. Unlike obs_data it keeps what a bundle needs to stay stable across a load and save:
 * - object members keep their order, and duplicate keys are kept (cj_get returns the first);
 * - numbers keep the text they were parsed from, so 0.1 stays 0.1 and 1e3 stays 1e3;
 * - strings are UTF-8; \uXXXX escapes (surrogate pairs too) are decoded on parse.
 * Nesting is limited to CJ_MAX_DEPTH levels so a hostile bundle can't exhaust the stack. */
#ifndef CTD_SHOW_JSON_H
#define CTD_SHOW_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CJ_MAX_DEPTH 256

typedef struct cj cj;

enum cj_type { CJ_NULL, CJ_FALSE, CJ_TRUE, CJ_NUMBER, CJ_STRING, CJ_ARRAY, CJ_OBJECT };

/* Parses len bytes (need not be NUL-terminated). Returns NULL on error, with a message in err (may be NULL). A UTF-8
 * byte order mark is skipped. Trailing non-space text is an error. */
cj *cj_parse(const char *text, size_t len, char *err, size_t err_size);
/* Serializes; pretty uses 4-space indents like OBS's own files. Returns malloc'd text (free with cj_free_str). */
char *cj_print(const cj *v, bool pretty);
void cj_free_str(char *s);
void cj_free(cj *v);
cj *cj_clone(const cj *v);
/* Deep equality: same kinds, same members in the same order, numbers compared by value. */
bool cj_equal(const cj *a, const cj *b);

cj *cj_new_null(void);
cj *cj_new_bool(bool b);
cj *cj_new_number(double n);
cj *cj_new_int(int64_t n);
cj *cj_new_string(const char *s);
cj *cj_new_array(void);
cj *cj_new_object(void);

enum cj_type cj_kind(const cj *v); /* CJ_NULL for a NULL pointer */
bool cj_is_object(const cj *v);
bool cj_is_array(const cj *v);
bool cj_is_string(const cj *v);
bool cj_is_number(const cj *v);
bool cj_is_bool(const cj *v);

const char *cj_str(const cj *v); /* NULL unless a string */
double cj_num(const cj *v, double def);
int64_t cj_int(const cj *v, int64_t def); /* def unless a number that is a whole value in int64 range */
bool cj_bool(const cj *v, bool def);

/* Arrays and objects: number of elements/members, the i-th value, and (objects) the i-th key. */
size_t cj_size(const cj *v);
cj *cj_at(const cj *v, size_t i);
const char *cj_key_at(const cj *v, size_t i);

/* Objects. cj_get returns the first member named key, or NULL. */
cj *cj_get(const cj *obj, const char *key);
const char *cj_get_str(const cj *obj, const char *key);
int64_t cj_get_int(const cj *obj, const char *key, int64_t def);
bool cj_get_bool(const cj *obj, const char *key, bool def);
/* Sets key to val (ownership passes to obj; val is freed if obj isn't an object). Replaces the first member with that
 * key in place (keeping its position) or appends. Returns val, or NULL on failure. */
cj *cj_set(cj *obj, const char *key, cj *val);
cj *cj_set_str(cj *obj, const char *key, const char *s);
cj *cj_set_int(cj *obj, const char *key, int64_t n);
cj *cj_set_bool(cj *obj, const char *key, bool b);
/* Removes every member named key. Returns how many were removed. */
size_t cj_remove(cj *obj, const char *key);
/* Detaches the first member named key and returns its value (caller owns it), or NULL. */
cj *cj_take(cj *obj, const char *key);
/* Gets the object member key, creating an empty object there if it is missing or not an object. */
cj *cj_get_or_add_object(cj *obj, const char *key);
cj *cj_get_or_add_array(cj *obj, const char *key);

/* Arrays. Ownership of val passes to arr. */
cj *cj_append(cj *arr, cj *val);
void cj_remove_at(cj *arr, size_t i);

/* Replaces the value of a string node in place (returns false if v isn't a string). */
bool cj_set_string_value(cj *v, const char *s);

/* JSON Pointer (RFC 6901, "/a/0/b", with ~0 and ~1 escapes) from root; "" is root itself. NULL when absent. */
cj *cj_pointer(const cj *root, const char *pointer);
/* Appends one escaped JSON Pointer segment ("/" + token) to buf. Returns false if it doesn't fit. */
bool cj_pointer_append(char *buf, size_t size, const char *token);

/* Files (UTF-8 paths, through show-fs). cj_write_file is atomic (temp file + rename). */
cj *cj_parse_file(const char *path, char *err, size_t err_size);
bool cj_write_file(const char *path, const cj *v, bool pretty);

#ifdef __cplusplus
}
#endif

#endif
