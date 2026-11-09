/* Show Profiles JSON document model; see json.h. */
#include "json.h"
#include "show-fs.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cj_member {
	char *key;
	cj *val;
};

struct cj {
	enum cj_type type;
	char *text; /* string value (UTF-8), or a number's source text */
	double num;
	size_t count, cap;
	struct cj_member *members; /* objects */
	cj **items;                /* arrays */
};

static char *dup_str(const char *s)
{
	size_t n = strlen(s);
	char *d = malloc(n + 1);
	if (d)
		memcpy(d, s, n + 1);
	return d;
}

static cj *new_node(enum cj_type type)
{
	cj *v = calloc(1, sizeof(*v));
	if (v)
		v->type = type;
	return v;
}

void cj_free(cj *v)
{
	if (!v)
		return;
	if (v->type == CJ_OBJECT) {
		for (size_t i = 0; i < v->count; i++) {
			free(v->members[i].key);
			cj_free(v->members[i].val);
		}
		free(v->members);
	} else if (v->type == CJ_ARRAY) {
		for (size_t i = 0; i < v->count; i++)
			cj_free(v->items[i]);
		free(v->items);
	}
	free(v->text);
	free(v);
}

void cj_free_str(char *s)
{
	free(s);
}

cj *cj_new_null(void)
{
	return new_node(CJ_NULL);
}

cj *cj_new_bool(bool b)
{
	return new_node(b ? CJ_TRUE : CJ_FALSE);
}

static void format_number(double n, char *buf, size_t size)
{
	if (!isfinite(n)) {
		snprintf(buf, size, "0"); /* JSON has no NaN/Infinity */
		return;
	}
	if (n == floor(n) && fabs(n) < 9007199254740992.0) {
		snprintf(buf, size, "%lld", (long long)n);
		return;
	}
	/* the shortest form that reads back as the same double */
	for (int prec = 15; prec <= 17; prec++) {
		snprintf(buf, size, "%.*g", prec, n);
		if (strtod(buf, NULL) == n)
			break;
	}
}

cj *cj_new_number(double n)
{
	char buf[40];
	cj *v = new_node(CJ_NUMBER);
	if (!v)
		return NULL;
	format_number(n, buf, sizeof(buf));
	v->num = n;
	v->text = dup_str(buf);
	if (!v->text) {
		free(v);
		return NULL;
	}
	return v;
}

cj *cj_new_int(int64_t n)
{
	char buf[32];
	cj *v = new_node(CJ_NUMBER);
	if (!v)
		return NULL;
	snprintf(buf, sizeof(buf), "%lld", (long long)n);
	v->num = (double)n;
	v->text = dup_str(buf);
	if (!v->text) {
		free(v);
		return NULL;
	}
	return v;
}

cj *cj_new_string(const char *s)
{
	cj *v = new_node(CJ_STRING);
	if (!v)
		return NULL;
	v->text = dup_str(s ? s : "");
	if (!v->text) {
		free(v);
		return NULL;
	}
	return v;
}

cj *cj_new_array(void)
{
	return new_node(CJ_ARRAY);
}

cj *cj_new_object(void)
{
	return new_node(CJ_OBJECT);
}

enum cj_type cj_kind(const cj *v)
{
	return v ? v->type : CJ_NULL;
}

bool cj_is_object(const cj *v)
{
	return v && v->type == CJ_OBJECT;
}

bool cj_is_array(const cj *v)
{
	return v && v->type == CJ_ARRAY;
}

bool cj_is_string(const cj *v)
{
	return v && v->type == CJ_STRING;
}

bool cj_is_number(const cj *v)
{
	return v && v->type == CJ_NUMBER;
}

bool cj_is_bool(const cj *v)
{
	return v && (v->type == CJ_TRUE || v->type == CJ_FALSE);
}

const char *cj_str(const cj *v)
{
	return v && v->type == CJ_STRING ? v->text : NULL;
}

double cj_num(const cj *v, double def)
{
	return v && v->type == CJ_NUMBER ? v->num : def;
}

int64_t cj_int(const cj *v, int64_t def)
{
	if (!v || v->type != CJ_NUMBER)
		return def;
	/* integers are read from the text, so values past 2^53 stay exact */
	char *end = NULL;
	errno = 0;
	long long n = strtoll(v->text, &end, 10);
	if (errno == 0 && end && *end == '\0')
		return (int64_t)n;
	if (v->num == floor(v->num) && fabs(v->num) < 9.2e18)
		return (int64_t)v->num;
	return def;
}

bool cj_bool(const cj *v, bool def)
{
	if (v && v->type == CJ_TRUE)
		return true;
	if (v && v->type == CJ_FALSE)
		return false;
	return def;
}

size_t cj_size(const cj *v)
{
	return v && (v->type == CJ_ARRAY || v->type == CJ_OBJECT) ? v->count : 0;
}

cj *cj_at(const cj *v, size_t i)
{
	if (!v || i >= v->count)
		return NULL;
	if (v->type == CJ_ARRAY)
		return v->items[i];
	if (v->type == CJ_OBJECT)
		return v->members[i].val;
	return NULL;
}

const char *cj_key_at(const cj *v, size_t i)
{
	return v && v->type == CJ_OBJECT && i < v->count ? v->members[i].key : NULL;
}

cj *cj_get(const cj *obj, const char *key)
{
	if (!obj || obj->type != CJ_OBJECT || !key)
		return NULL;
	for (size_t i = 0; i < obj->count; i++)
		if (strcmp(obj->members[i].key, key) == 0)
			return obj->members[i].val;
	return NULL;
}

const char *cj_get_str(const cj *obj, const char *key)
{
	return cj_str(cj_get(obj, key));
}

int64_t cj_get_int(const cj *obj, const char *key, int64_t def)
{
	return cj_int(cj_get(obj, key), def);
}

bool cj_get_bool(const cj *obj, const char *key, bool def)
{
	return cj_bool(cj_get(obj, key), def);
}

static bool grow(cj *v)
{
	if (v->count < v->cap)
		return true;
	size_t cap = v->cap ? v->cap * 2 : 4;
	if (v->type == CJ_OBJECT) {
		struct cj_member *m = realloc(v->members, cap * sizeof(*m));
		if (!m)
			return false;
		v->members = m;
	} else {
		cj **items = realloc(v->items, cap * sizeof(*items));
		if (!items)
			return false;
		v->items = items;
	}
	v->cap = cap;
	return true;
}

static bool add_member(cj *obj, char *key, cj *val)
{
	if (!grow(obj))
		return false;
	obj->members[obj->count].key = key;
	obj->members[obj->count].val = val;
	obj->count++;
	return true;
}

cj *cj_set(cj *obj, const char *key, cj *val)
{
	if (!val)
		return NULL;
	if (!obj || obj->type != CJ_OBJECT || !key) {
		cj_free(val);
		return NULL;
	}
	for (size_t i = 0; i < obj->count; i++) {
		if (strcmp(obj->members[i].key, key) == 0) {
			cj_free(obj->members[i].val);
			obj->members[i].val = val;
			return val;
		}
	}
	char *k = dup_str(key);
	if (!k || !add_member(obj, k, val)) {
		free(k);
		cj_free(val);
		return NULL;
	}
	return val;
}

cj *cj_set_str(cj *obj, const char *key, const char *s)
{
	return cj_set(obj, key, cj_new_string(s));
}

cj *cj_set_int(cj *obj, const char *key, int64_t n)
{
	return cj_set(obj, key, cj_new_int(n));
}

cj *cj_set_bool(cj *obj, const char *key, bool b)
{
	return cj_set(obj, key, cj_new_bool(b));
}

size_t cj_remove(cj *obj, const char *key)
{
	size_t removed = 0;
	if (!obj || obj->type != CJ_OBJECT || !key)
		return 0;
	for (size_t i = 0; i < obj->count;) {
		if (strcmp(obj->members[i].key, key) == 0) {
			free(obj->members[i].key);
			cj_free(obj->members[i].val);
			memmove(&obj->members[i], &obj->members[i + 1], (obj->count - i - 1) * sizeof(*obj->members));
			obj->count--;
			removed++;
		} else {
			i++;
		}
	}
	return removed;
}

cj *cj_take(cj *obj, const char *key)
{
	if (!obj || obj->type != CJ_OBJECT || !key)
		return NULL;
	for (size_t i = 0; i < obj->count; i++) {
		if (strcmp(obj->members[i].key, key) == 0) {
			cj *val = obj->members[i].val;
			free(obj->members[i].key);
			memmove(&obj->members[i], &obj->members[i + 1], (obj->count - i - 1) * sizeof(*obj->members));
			obj->count--;
			return val;
		}
	}
	return NULL;
}

cj *cj_get_or_add_object(cj *obj, const char *key)
{
	cj *v = cj_get(obj, key);
	if (cj_is_object(v))
		return v;
	return cj_set(obj, key, cj_new_object());
}

cj *cj_get_or_add_array(cj *obj, const char *key)
{
	cj *v = cj_get(obj, key);
	if (cj_is_array(v))
		return v;
	return cj_set(obj, key, cj_new_array());
}

cj *cj_append(cj *arr, cj *val)
{
	if (!val)
		return NULL;
	if (!arr || arr->type != CJ_ARRAY || !grow(arr)) {
		cj_free(val);
		return NULL;
	}
	arr->items[arr->count++] = val;
	return val;
}

void cj_remove_at(cj *arr, size_t i)
{
	if (!arr || arr->type != CJ_ARRAY || i >= arr->count)
		return;
	cj_free(arr->items[i]);
	memmove(&arr->items[i], &arr->items[i + 1], (arr->count - i - 1) * sizeof(*arr->items));
	arr->count--;
}

bool cj_set_string_value(cj *v, const char *s)
{
	if (!v || v->type != CJ_STRING)
		return false;
	char *d = dup_str(s ? s : "");
	if (!d)
		return false;
	free(v->text);
	v->text = d;
	return true;
}

cj *cj_clone(const cj *v)
{
	if (!v)
		return NULL;
	cj *c = new_node(v->type);
	if (!c)
		return NULL;
	c->num = v->num;
	if (v->text && !(c->text = dup_str(v->text)))
		goto fail;
	if (v->type == CJ_OBJECT) {
		for (size_t i = 0; i < v->count; i++) {
			char *k = dup_str(v->members[i].key);
			cj *val = cj_clone(v->members[i].val);
			if (!k || !val || !add_member(c, k, val)) {
				free(k);
				cj_free(val);
				goto fail;
			}
		}
	} else if (v->type == CJ_ARRAY) {
		for (size_t i = 0; i < v->count; i++)
			if (!cj_append(c, cj_clone(v->items[i])))
				goto fail;
	}
	return c;
fail:
	cj_free(c);
	return NULL;
}

bool cj_equal(const cj *a, const cj *b)
{
	if (!a || !b)
		return a == b;
	if (a->type != b->type)
		return false;
	switch (a->type) {
	case CJ_NUMBER:
		return a->num == b->num;
	case CJ_STRING:
		return strcmp(a->text, b->text) == 0;
	case CJ_ARRAY:
		if (a->count != b->count)
			return false;
		for (size_t i = 0; i < a->count; i++)
			if (!cj_equal(a->items[i], b->items[i]))
				return false;
		return true;
	case CJ_OBJECT:
		if (a->count != b->count)
			return false;
		for (size_t i = 0; i < a->count; i++)
			if (strcmp(a->members[i].key, b->members[i].key) != 0 ||
			    !cj_equal(a->members[i].val, b->members[i].val))
				return false;
		return true;
	default:
		return true;
	}
}

/* ---- parser ---- */

struct parser {
	const char *p, *end;
	char *err;
	size_t err_size;
	bool failed;
};

static void fail(struct parser *ps, const char *fmt, ...)
{
	if (ps->failed)
		return;
	ps->failed = true;
	if (ps->err && ps->err_size) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(ps->err, ps->err_size, fmt, ap);
		va_end(ap);
	}
}

static void skip_ws(struct parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

struct buf {
	char *data;
	size_t len, cap;
};

static bool buf_put(struct buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (cap < b->len + n + 1)
			cap *= 2;
		char *d = realloc(b->data, cap);
		if (!d)
			return false;
		b->data = d;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return true;
}

static bool buf_putc(struct buf *b, char c)
{
	return buf_put(b, &c, 1);
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

static bool read_hex4(struct parser *ps, unsigned *out)
{
	if (ps->end - ps->p < 4)
		return false;
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		int d = hex_digit(ps->p[i]);
		if (d < 0)
			return false;
		v = (v << 4) | (unsigned)d;
	}
	ps->p += 4;
	*out = v;
	return true;
}

static bool put_utf8(struct buf *b, unsigned cp)
{
	char out[4];
	size_t n;
	if (cp < 0x80) {
		out[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	return buf_put(b, out, n);
}

static char *parse_string_raw(struct parser *ps)
{
	struct buf b = {0};
	if (ps->p >= ps->end || *ps->p != '"') {
		fail(ps, "expected a string");
		return NULL;
	}
	ps->p++;
	if (!buf_put(&b, "", 0))
		goto oom;
	while (ps->p < ps->end && *ps->p != '"') {
		unsigned char c = (unsigned char)*ps->p;
		if (c < 0x20) {
			fail(ps, "control character in a string");
			goto error;
		}
		if (c != '\\') {
			/* copy a run of plain bytes at once */
			const char *start = ps->p;
			while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20)
				ps->p++;
			if (!buf_put(&b, start, (size_t)(ps->p - start)))
				goto oom;
			continue;
		}
		ps->p++;
		if (ps->p >= ps->end)
			break;
		char e = *ps->p++;
		char out;
		switch (e) {
		case '"':
		case '\\':
		case '/':
			out = e;
			break;
		case 'b':
			out = '\b';
			break;
		case 'f':
			out = '\f';
			break;
		case 'n':
			out = '\n';
			break;
		case 'r':
			out = '\r';
			break;
		case 't':
			out = '\t';
			break;
		case 'u': {
			unsigned cp;
			if (!read_hex4(ps, &cp)) {
				fail(ps, "bad \\u escape");
				goto error;
			}
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				unsigned lo;
				if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
					ps->p += 2;
					if (!read_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
						fail(ps, "bad surrogate pair");
						goto error;
					}
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else {
					cp = 0xFFFD; /* a lone high surrogate */
				}
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				cp = 0xFFFD;
			}
			if (cp == 0)
				cp = 0xFFFD; /* keep strings NUL-free */
			if (!put_utf8(&b, cp))
				goto oom;
			continue;
		}
		default:
			fail(ps, "bad escape \\%c", e);
			goto error;
		}
		if (!buf_putc(&b, out))
			goto oom;
	}
	if (ps->p >= ps->end) {
		fail(ps, "unterminated string");
		goto error;
	}
	ps->p++;
	return b.data;
oom:
	fail(ps, "out of memory");
error:
	free(b.data);
	return NULL;
}

static cj *parse_value(struct parser *ps, int depth);

static cj *parse_number(struct parser *ps)
{
	const char *start = ps->p;
	if (ps->p < ps->end && *ps->p == '-')
		ps->p++;
	if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9')) {
		fail(ps, "bad number");
		return NULL;
	}
	if (*ps->p == '0') {
		ps->p++;
	} else {
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if (ps->p < ps->end && *ps->p == '.') {
		ps->p++;
		if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9')) {
			fail(ps, "bad number");
			return NULL;
		}
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
		ps->p++;
		if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
			ps->p++;
		if (ps->p >= ps->end || !(*ps->p >= '0' && *ps->p <= '9')) {
			fail(ps, "bad number");
			return NULL;
		}
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	size_t n = (size_t)(ps->p - start);
	cj *v = new_node(CJ_NUMBER);
	char *text = malloc(n + 1);
	if (!v || !text) {
		free(v);
		free(text);
		fail(ps, "out of memory");
		return NULL;
	}
	memcpy(text, start, n);
	text[n] = '\0';
	v->text = text;
	v->num = strtod(text, NULL);
	return v;
}

static bool match_word(struct parser *ps, const char *word)
{
	size_t n = strlen(word);
	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
		return false;
	ps->p += n;
	return true;
}

static cj *parse_object(struct parser *ps, int depth)
{
	cj *obj = new_node(CJ_OBJECT);
	if (!obj) {
		fail(ps, "out of memory");
		return NULL;
	}
	ps->p++; /* { */
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		return obj;
	}
	for (;;) {
		skip_ws(ps);
		char *key = parse_string_raw(ps);
		if (!key)
			goto error;
		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':') {
			free(key);
			fail(ps, "expected ':'");
			goto error;
		}
		ps->p++;
		cj *val = parse_value(ps, depth + 1);
		if (!val) {
			free(key);
			goto error;
		}
		if (!add_member(obj, key, val)) {
			free(key);
			cj_free(val);
			fail(ps, "out of memory");
			goto error;
		}
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == '}') {
			ps->p++;
			return obj;
		}
		fail(ps, "expected ',' or '}'");
		goto error;
	}
error:
	cj_free(obj);
	return NULL;
}

static cj *parse_array(struct parser *ps, int depth)
{
	cj *arr = new_node(CJ_ARRAY);
	if (!arr) {
		fail(ps, "out of memory");
		return NULL;
	}
	ps->p++; /* [ */
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		return arr;
	}
	for (;;) {
		cj *val = parse_value(ps, depth + 1);
		if (!val)
			goto error;
		if (!cj_append(arr, val)) {
			fail(ps, "out of memory");
			goto error;
		}
		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
			return arr;
		}
		fail(ps, "expected ',' or ']'");
		goto error;
	}
error:
	cj_free(arr);
	return NULL;
}

static cj *parse_value(struct parser *ps, int depth)
{
	if (depth > CJ_MAX_DEPTH) {
		fail(ps, "nested too deeply");
		return NULL;
	}
	skip_ws(ps);
	if (ps->p >= ps->end) {
		fail(ps, "unexpected end");
		return NULL;
	}
	switch (*ps->p) {
	case '{':
		return parse_object(ps, depth);
	case '[':
		return parse_array(ps, depth);
	case '"': {
		char *s = parse_string_raw(ps);
		if (!s)
			return NULL;
		cj *v = new_node(CJ_STRING);
		if (!v) {
			free(s);
			fail(ps, "out of memory");
			return NULL;
		}
		v->text = s;
		return v;
	}
	case 't':
		if (match_word(ps, "true"))
			return new_node(CJ_TRUE);
		break;
	case 'f':
		if (match_word(ps, "false"))
			return new_node(CJ_FALSE);
		break;
	case 'n':
		if (match_word(ps, "null"))
			return new_node(CJ_NULL);
		break;
	default:
		if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
			return parse_number(ps);
	}
	fail(ps, "unexpected character '%c'", *ps->p);
	return NULL;
}

cj *cj_parse(const char *text, size_t len, char *err, size_t err_size)
{
	struct parser ps = {text, text + len, err, err_size, false};
	if (err && err_size)
		err[0] = '\0';
	if (!text) {
		fail(&ps, "no text");
		return NULL;
	}
	if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF)
		ps.p += 3;
	cj *v = parse_value(&ps, 0);
	if (!v) {
		if (err && err_size && ps.failed) {
			size_t used = strlen(err);
			snprintf(err + used, err_size - used, " at byte %zu", (size_t)(ps.p - text));
		}
		return NULL;
	}
	skip_ws(&ps);
	if (ps.p != ps.end) {
		fail(&ps, "trailing text at byte %zu", (size_t)(ps.p - text));
		cj_free(v);
		return NULL;
	}
	return v;
}

/* ---- printer ---- */

static bool print_string(struct buf *b, const char *s)
{
	static const char hex[] = "0123456789abcdef";
	if (!buf_putc(b, '"'))
		return false;
	const char *run = s;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 0x20 && c != '"' && c != '\\')
			continue;
		if (!buf_put(b, run, (size_t)(s - run)))
			return false;
		char esc[7] = {'\\', 0};
		size_t n = 2;
		switch (c) {
		case '"':
			esc[1] = '"';
			break;
		case '\\':
			esc[1] = '\\';
			break;
		case '\n':
			esc[1] = 'n';
			break;
		case '\r':
			esc[1] = 'r';
			break;
		case '\t':
			esc[1] = 't';
			break;
		case '\b':
			esc[1] = 'b';
			break;
		case '\f':
			esc[1] = 'f';
			break;
		default:
			esc[1] = 'u';
			esc[2] = '0';
			esc[3] = '0';
			esc[4] = hex[c >> 4];
			esc[5] = hex[c & 15];
			n = 6;
		}
		if (!buf_put(b, esc, n))
			return false;
		run = s + 1;
	}
	return buf_put(b, run, (size_t)(s - run)) && buf_putc(b, '"');
}

static bool indent(struct buf *b, bool pretty, int depth)
{
	if (!pretty)
		return true;
	if (!buf_putc(b, '\n'))
		return false;
	for (int i = 0; i < depth; i++)
		if (!buf_put(b, "    ", 4))
			return false;
	return true;
}

static bool print_value(struct buf *b, const cj *v, bool pretty, int depth)
{
	switch (v->type) {
	case CJ_NULL:
		return buf_put(b, "null", 4);
	case CJ_TRUE:
		return buf_put(b, "true", 4);
	case CJ_FALSE:
		return buf_put(b, "false", 5);
	case CJ_NUMBER:
		return buf_put(b, v->text, strlen(v->text));
	case CJ_STRING:
		return print_string(b, v->text);
	case CJ_ARRAY:
		if (!buf_putc(b, '['))
			return false;
		for (size_t i = 0; i < v->count; i++) {
			if ((i && !buf_putc(b, ',')) || !indent(b, pretty, depth + 1) ||
			    !print_value(b, v->items[i], pretty, depth + 1))
				return false;
		}
		if (v->count && !indent(b, pretty, depth))
			return false;
		return buf_putc(b, ']');
	case CJ_OBJECT:
		if (!buf_putc(b, '{'))
			return false;
		for (size_t i = 0; i < v->count; i++) {
			if ((i && !buf_putc(b, ',')) || !indent(b, pretty, depth + 1) ||
			    !print_string(b, v->members[i].key) || !buf_put(b, pretty ? ": " : ":", pretty ? 2 : 1) ||
			    !print_value(b, v->members[i].val, pretty, depth + 1))
				return false;
		}
		if (v->count && !indent(b, pretty, depth))
			return false;
		return buf_putc(b, '}');
	}
	return false;
}

char *cj_print(const cj *v, bool pretty)
{
	struct buf b = {0};
	if (!v || !buf_put(&b, "", 0) || !print_value(&b, v, pretty, 0)) {
		free(b.data);
		return NULL;
	}
	return b.data;
}

/* ---- JSON Pointer ---- */

cj *cj_pointer(const cj *root, const char *pointer)
{
	if (!root || !pointer)
		return NULL;
	const cj *cur = root;
	const char *p = pointer;
	char token[512];
	while (*p) {
		if (*p != '/')
			return NULL;
		p++;
		size_t n = 0;
		while (*p && *p != '/') {
			char c = *p++;
			if (c == '~') {
				if (*p == '0')
					c = '~';
				else if (*p == '1')
					c = '/';
				else
					return NULL;
				p++;
			}
			if (n + 1 >= sizeof(token))
				return NULL;
			token[n++] = c;
		}
		token[n] = '\0';
		if (cur->type == CJ_OBJECT) {
			cur = cj_get(cur, token);
		} else if (cur->type == CJ_ARRAY) {
			char *end = NULL;
			if (!n || (n > 1 && token[0] == '0'))
				return NULL;
			unsigned long long i = strtoull(token, &end, 10);
			if (!end || *end || i >= cur->count)
				return NULL;
			cur = cur->items[i];
		} else {
			return NULL;
		}
		if (!cur)
			return NULL;
	}
	return (cj *)cur;
}

bool cj_pointer_append(char *buf, size_t size, const char *token)
{
	size_t len = strlen(buf);
	if (len + 1 >= size)
		return false;
	buf[len++] = '/';
	for (const char *s = token; *s; s++) {
		const char *rep = *s == '~' ? "~0" : *s == '/' ? "~1" : NULL;
		size_t n = rep ? 2 : 1;
		if (len + n >= size)
			return false;
		if (rep) {
			buf[len++] = rep[0];
			buf[len++] = rep[1];
		} else {
			buf[len++] = *s;
		}
	}
	buf[len] = '\0';
	return true;
}

/* ---- files ---- */

cj *cj_parse_file(const char *path, char *err, size_t err_size)
{
	char *data = NULL;
	size_t size = 0;
	if (!sfs_read(path, &data, &size)) {
		if (err && err_size)
			snprintf(err, err_size, "can't read %s", path);
		return NULL;
	}
	cj *v = cj_parse(data, size, err, err_size);
	free(data);
	return v;
}

bool cj_write_file(const char *path, const cj *v, bool pretty)
{
	char *text = cj_print(v, pretty);
	if (!text)
		return false;
	bool ok = sfs_write_atomic(path, text, strlen(text));
	free(text);
	return ok;
}
