/* JSON model (src/show/json.c): round trips that keep numbers and order, escapes, pointers, limits. */
#include "unit.h"

#include <stdlib.h>

static void round_trip(const char *in, const char *expected)
{
	char err[128];
	cj *v = cj_parse(in, strlen(in), err, sizeof(err));
	CHECK(v != NULL);
	char *out = cj_print(v, false);
	CHECK_STR(out, expected);
	cj_free_str(out);
	cj_free(v);
}

static void bad(const char *in)
{
	char err[128];
	cj *v = cj_parse(in, strlen(in), err, sizeof(err));
	CHECK(v == NULL);
	CHECK(err[0] != '\0');
	cj_free(v);
}

void test_json(void)
{
	round_trip("{\"b\":1,\"a\":[0.1,1e3,-0,true,false,null]}", "{\"b\":1,\"a\":[0.1,1e3,-0,true,false,null]}");
	round_trip("\"\\u00e9\\ud83d\\ude00 \\\"q\\\" \\n\\t\\u0001\"",
		   "\"\xC3\xA9\xF0\x9F\x98\x80 \\\"q\\\" \\n\\t\\u0001\"");
	round_trip("\xEF\xBB\xBF {\"x\" : \"\xE2\x82\xAC\"}", "{\"x\":\"\xE2\x82\xAC\"}");
	round_trip("{\"k\":1,\"k\":2}", "{\"k\":1,\"k\":2}");
	round_trip("9007199254740993", "9007199254740993");
	bad("{\"a\":1,}");
	bad("[1 2]");
	bad("{\"a\":01}");
	bad("\"unterminated");
	bad("{} trailing");
	bad("\"ctl \x01 char\"");

	/* nesting limit */
	char deep[CJ_MAX_DEPTH * 2 + 8];
	size_t n = 0;
	for (int i = 0; i < CJ_MAX_DEPTH + 2; i++)
		deep[n++] = '[';
	deep[n] = '\0';
	bad(deep);

	/* integers past 2^53 stay exact */
	cj *big = cj_parse("9007199254740993", 16, NULL, 0);
	CHECK(cj_int(big, 0) == 9007199254740993LL);
	cj_free(big);

	/* building and editing */
	cj *o = cj_new_object();
	cj_set_str(o, "name", "x");
	cj_set_int(o, "n", 3);
	cj_set_str(o, "name", "y"); /* replaces in place */
	cj *arr = cj_get_or_add_array(o, "list");
	cj_append(arr, cj_new_number(0.25));
	cj_append(arr, cj_new_string("a/b~c"));
	char *text = cj_print(o, false);
	CHECK_STR(text, "{\"name\":\"y\",\"n\":3,\"list\":[0.25,\"a/b~c\"]}");
	cj_free_str(text);
	CHECK_STR(cj_str(cj_pointer(o, "/list/1")), "a/b~c");
	CHECK(cj_pointer(o, "/list/2") == NULL);
	CHECK(cj_pointer(o, "/list/01") == NULL);
	CHECK(cj_pointer(o, "") == o);
	char ptr[64] = "";
	CHECK(cj_pointer_append(ptr, sizeof(ptr), "a/b~c"));
	CHECK_STR(ptr, "/a~1b~0c");
	cj *inner = cj_set(o, "a/b~c", cj_new_bool(true));
	CHECK(cj_pointer(o, ptr) == inner);

	cj *copy = cj_clone(o);
	CHECK(cj_equal(o, copy));
	cj_set_int(copy, "n", 4);
	CHECK(!cj_equal(o, copy));
	CHECK(cj_remove(copy, "n") == 1);
	CHECK(cj_get(copy, "n") == NULL);
	cj *taken = cj_take(copy, "list");
	CHECK(cj_size(taken) == 2);
	cj_free(taken);
	cj_free(copy);

	/* pretty output reads back the same */
	char *pretty = cj_print(o, true);
	cj *back = cj_parse(pretty, strlen(pretty), NULL, 0);
	CHECK(cj_equal(o, back));
	CHECK(unit_contains(pretty, "\n    \"name\": \"y\""));
	cj_free(back);
	cj_free_str(pretty);
	cj_free(o);
}
