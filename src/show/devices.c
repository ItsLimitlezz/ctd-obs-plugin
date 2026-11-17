/* Device mapping: scoring, greedy assignment, remembered map and aliases (design section 5). See devices.h. */
#include "devices.h"
#include "devices-identity.h"
#include "scrub.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str_or_empty(const char *s)
{
	return s ? s : "";
}

/* ------------------------------------------------------------- names --- */

static bool ends_with_word(const char *s, size_t n, const char *word)
{
	size_t w = strlen(word);
	return n >= w + 1 && memcmp(s + n - w, word, w) == 0 && s[n - w - 1] == ' ';
}

void show_device_normalize_name(const char *name, char *out, size_t size)
{
	if (!size)
		return;
	size_t n = 0;
	bool space = false;
	for (const char *c = str_or_empty(name); *c && n + 1 < size; c++) {
		unsigned char ch = (unsigned char)*c;
		if (ch == '(' || ch == '#') {
			/* "(2)", "#2": an instance number OBS or Windows added */
			const char *d = c + 1;
			while (*d == ' ')
				d++;
			const char *digits = d;
			while (isdigit((unsigned char)*d))
				d++;
			if (d > digits && (ch == '#' || *d == ')')) {
				c = ch == '(' ? d : d - 1;
				space = n > 0;
				continue;
			}
		}
		if (isalnum(ch) || ch >= 0x80) {
			if (space && n + 2 < size)
				out[n++] = ' ';
			space = false;
			out[n++] = (char)tolower(ch);
		} else {
			space = n > 0;
		}
	}
	out[n] = '\0';
	/* a trailing " video" (dshow names some interfaces "<device> Video") */
	if (ends_with_word(out, n, "video"))
		out[n - 6] = '\0';
}

const char *show_device_brand(const char *name)
{
	static const struct {
		const char *needle, *brand;
	} brands[] = {
		{"elgato", "elgato"},       {"cam link", "elgato"},      {"game capture hd", "elgato"},
		{"magewell", "magewell"},   {"usb capture", "magewell"}, {"blackmagic", "blackmagic"},
		{"decklink", "blackmagic"}, {"intensity", "blackmagic"}, {"ultrastudio", "blackmagic"},
		{"avermedia", "avermedia"}, {"live gamer", "avermedia"}, {"logitech", "logitech"},
		{"brio", "logitech"},       {"c920", "logitech"},        {"c922", "logitech"},
		{"razer", "razer"},         {"kiyo", "razer"},           {"obs virtual", "obs"},
		{"rode", "rode"},           {"focusrite", "focusrite"},  {"scarlett", "focusrite"},
		{"shure", "shure"},         {"behringer", "behringer"},  {"motu", "motu"},
		{"yamaha", "yamaha"},       {"atem", "blackmagic"},      {"insta360", "insta360"},
	};
	char norm[256];
	show_device_normalize_name(name, norm, sizeof(norm));
	if (!norm[0])
		return NULL;
	for (size_t i = 0; i < sizeof(brands) / sizeof(brands[0]); i++)
		if (strstr(norm, brands[i].needle))
			return brands[i].brand;
	return NULL;
}

/* ----------------------------------------------------------- scoring --- */

struct kv {
	const char *id_key, *name_key, *id, *name;
};

static const char *kv_get(void *ctx, const char *key)
{
	const struct kv *kv = ctx;
	if (kv->id_key && strcmp(key, kv->id_key) == 0)
		return kv->id;
	if (kv->name_key && strcmp(key, kv->name_key) == 0)
		return kv->name;
	return NULL;
}

/* A candidate's identity, read the way T6 reads a source (so a dshow id gives its name and USB ids) */
static void candidate_identity(const char *source_type, const char *dev_id, const char *dev_name,
			       devices_identity_t *out)
{
	struct kv kv = {NULL, NULL, str_or_empty(dev_id), str_or_empty(dev_name)};
	memset(out, 0, sizeof(*out));
	if (!devices_identity_keys(source_type, &kv.id_key, &kv.name_key) ||
	    !devices_identity(source_type, kv_get, &kv, out)) {
		snprintf(out->id, sizeof(out->id), "%s", kv.id);
		snprintf(out->name, sizeof(out->name), "%s", kv.name);
	}
	if (!out->name[0])
		snprintf(out->name, sizeof(out->name), "%s", kv.name);
}

static bool same_text(const char *a, const char *b)
{
	return a && b && *a && strcmp(a, b) == 0;
}

static bool same_text_nocase(const char *a, const char *b)
{
	if (!a || !b || !*a)
		return false;
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

int show_device_score(const char *source_type, const cj *identity, const char *dev_id, const char *dev_name,
		      int same_vidpid)
{
	devices_identity_t c;
	candidate_identity(source_type, dev_id, dev_name, &c);
	const char *id = cj_get_str(identity, "id"), *name = cj_get_str(identity, "name");
	const char *vid_pid = cj_get_str(identity, "vidPid"), *serial = cj_get_str(identity, "serial");

	if (same_text(id, c.id))
		return 100;
	int score = 0;
	if (vid_pid && *vid_pid && same_text_nocase(vid_pid, c.vid_pid)) {
		if (serial && *serial && same_text_nocase(serial, c.serial))
			return 95;
		if (same_vidpid == 1)
			score = 80;
	}
	if (score < 70 && (same_text(name, c.name) || same_text(name, dev_name)))
		score = 70;
	if (score < 60 && name && *name) {
		char a[256], b[256], b2[256];
		show_device_normalize_name(name, a, sizeof(a));
		show_device_normalize_name(c.name, b, sizeof(b));
		show_device_normalize_name(dev_name, b2, sizeof(b2));
		if (a[0] && (strcmp(a, b) == 0 || strcmp(a, b2) == 0))
			score = 60;
	}
	if (score < 30 && name && *name) {
		const char *brand = show_device_brand(name);
		const char *mine = show_device_brand(c.name);
		if (!mine)
			mine = show_device_brand(dev_name);
		if (brand && mine && strcmp(brand, mine) == 0)
			score = 30;
	}
	return score;
}

void show_device_identity_key(const char *source_type, const cj *identity, char *out, size_t size)
{
	const char *cls = devices_identity_class(source_type);
	const char *id = cj_get_str(identity, "id"), *name = cj_get_str(identity, "name");
	const char *vid_pid = cj_get_str(identity, "vidPid"), *serial = cj_get_str(identity, "serial");
	cls = cls ? cls : "other";
	if (vid_pid && *vid_pid && serial && *serial) {
		snprintf(out, size, "%s|usb:%s:%s", cls, vid_pid, serial);
	} else if (id && *id) {
		snprintf(out, size, "%s|id:%s", cls, id);
	} else if (name && *name) {
		char norm[256];
		show_device_normalize_name(name, norm, sizeof(norm));
		snprintf(out, size, "%s|name:%s", cls, norm);
	} else if (size) {
		out[0] = '\0';
	}
}

/* ----------------------------------------------------------- mapping --- */

struct pair {
	size_t slot, cand;
	int score;
};

static int compare_pairs(const void *a, const void *b)
{
	const struct pair *x = a, *y = b;
	if (x->score != y->score)
		return x->score > y->score ? -1 : 1;
	if (x->slot != y->slot)
		return x->slot < y->slot ? -1 : 1;
	if (x->cand != y->cand)
		return x->cand < y->cand ? -1 : 1;
	return 0;
}

static bool mappable(const cj *device)
{
	const char *type = cj_get_str(device, "sourceType");
	const char *cls = devices_identity_class(type);
	return cls && strcmp(cls, "window") != 0 && cj_get_str(device, "slot");
}

static const cj *find_candidate(const cj *list, const char *id)
{
	for (size_t i = 0; id && *id && i < cj_size(list); i++)
		if (same_text(cj_get_str(cj_at(list, i), "id"), id))
			return cj_at(list, i);
	return NULL;
}

static void set_choice(cj *entry, const char *status, int score, const char *id, const char *name)
{
	cj_set_str(entry, "status", status);
	cj_set_int(entry, "score", score);
	cj_set_str(entry, "id", str_or_empty(id));
	cj_set_str(entry, "name", str_or_empty(name));
}

static void use_key(char *out, size_t size, const char *type, const char *id)
{
	snprintf(out, size, "%s|%s", str_or_empty(type), str_or_empty(id));
}

cj *show_devices_map(const cj *devices, const cj *candidates, const cj *remembered, const cj *aliases)
{
	cj *mapping = cj_new_array();
	size_t n = cj_size(devices);
	const cj **dev = calloc(n ? n : 1, sizeof(*dev));
	cj **entry = calloc(n ? n : 1, sizeof(*entry));
	cj *used = cj_new_object(); /* "<type>|<device id>" -> slot index */
	struct pair *pairs = NULL;
	size_t pair_count = 0, pair_cap = 0;
	size_t m = 0;
	char key[1024];
	if (!mapping || !dev || !entry || !used)
		goto done;

	for (size_t i = 0; i < n; i++) {
		const cj *d = cj_at(devices, i);
		if (!cj_is_object(d) || !mappable(d))
			continue;
		const cj *identity = cj_get(d, "identity");
		const char *was = cj_get_str(identity, "name");
		cj *e = cj_append(mapping, cj_new_object());
		if (!e)
			goto done;
		cj_set_str(e, "slot", cj_get_str(d, "slot"));
		cj_set_str(e, "sourceType", cj_get_str(d, "sourceType"));
		cj_set_str(e, "class", devices_identity_class(cj_get_str(d, "sourceType")));
		cj_set_str(e, "label", str_or_empty(cj_get_str(d, "label")));
		cj_set_str(e, "was", was && *was ? was : str_or_empty(cj_get_str(d, "label")));
		set_choice(e, SHOW_DEVICE_MISSING, 0, "", "");
		cj_set_bool(e, "rememberedMissing", false);
		dev[m] = d;
		entry[m] = e;
		m++;
	}

	/* 1. remembered on this machine, and present */
	for (size_t i = 0; i < m; i++) {
		const char *slot = cj_get_str(dev[i], "slot"), *type = cj_get_str(dev[i], "sourceType");
		const cj *rem = cj_get(remembered, slot);
		const char *rem_id = cj_get_str(rem, "id");
		if (!rem_id || !*rem_id)
			continue;
		const cj *c = find_candidate(cj_get(candidates, type), rem_id);
		if (c) {
			set_choice(entry[i], SHOW_DEVICE_REMEMBERED, 100, rem_id, cj_get_str(c, "name"));
			use_key(key, sizeof(key), type, rem_id);
			cj_set_int(used, key, (int64_t)i);
		} else {
			cj_set_bool(entry[i], "rememberedMissing", true);
		}
	}

	/* 2. every (slot, device) pair worth considering */
	for (size_t i = 0; i < m; i++) {
		if (strcmp(cj_get_str(entry[i], "status"), SHOW_DEVICE_MISSING) != 0)
			continue;
		const char *type = cj_get_str(dev[i], "sourceType");
		const cj *identity = cj_get(dev[i], "identity");
		const cj *list = cj_get(candidates, type);
		char alias_key[600];
		show_device_identity_key(type, identity, alias_key, sizeof(alias_key));
		const char *alias_id = alias_key[0] ? cj_get_str(cj_get(aliases, alias_key), "id") : NULL;
		for (size_t j = 0; j < cj_size(list); j++) {
			const cj *c = cj_at(list, j);
			const char *cid = cj_get_str(c, "id");
			if (!cid || !*cid)
				continue;
			devices_identity_t ci;
			candidate_identity(type, cid, cj_get_str(c, "name"), &ci);
			int same_vidpid = 0;
			for (size_t k = 0; ci.vid_pid[0] && k < cj_size(list); k++) {
				devices_identity_t ck;
				candidate_identity(type, cj_get_str(cj_at(list, k), "id"),
						   cj_get_str(cj_at(list, k), "name"), &ck);
				if (strcmp(ck.vid_pid, ci.vid_pid) == 0)
					same_vidpid++;
			}
			int score = show_device_score(type, identity, cid, cj_get_str(c, "name"), same_vidpid);
			if (same_text(alias_id, cid) && score < SHOW_DEVICE_SCORE_ALIAS)
				score = SHOW_DEVICE_SCORE_ALIAS;
			if (score < SHOW_DEVICE_SCORE_SUGGEST)
				continue;
			if (pair_count == pair_cap) {
				size_t cap = pair_cap ? pair_cap * 2 : 16;
				struct pair *grown = realloc(pairs, cap * sizeof(*pairs));
				if (!grown)
					goto done;
				pairs = grown;
				pair_cap = cap;
			}
			pairs[pair_count++] = (struct pair){i, j, score};
		}
	}

	/* 3. greedy, highest score first; a device fills one slot unless the bundle shared it between slots */
	if (pair_count)
		qsort(pairs, pair_count, sizeof(*pairs), compare_pairs);
	for (size_t p = 0; p < pair_count; p++) {
		size_t i = pairs[p].slot;
		if (strcmp(cj_get_str(entry[i], "status"), SHOW_DEVICE_MISSING) != 0)
			continue;
		const char *type = cj_get_str(dev[i], "sourceType");
		const cj *c = cj_at(cj_get(candidates, type), pairs[p].cand);
		const char *cid = cj_get_str(c, "id");
		use_key(key, sizeof(key), type, cid);
		const cj *owner = cj_get(used, key);
		if (owner) {
			size_t o = (size_t)cj_int(owner, 0);
			const char *mine = cj_get_str(cj_get(dev[i], "identity"), "id");
			const char *theirs = o < m ? cj_get_str(cj_get(dev[o], "identity"), "id") : NULL;
			if (!same_text(mine, theirs))
				continue;
		}
		const int score = pairs[p].score;
		set_choice(entry[i], score >= SHOW_DEVICE_SCORE_AUTO ? SHOW_DEVICE_AUTO : SHOW_DEVICE_SUGGESTED, score,
			   cid, cj_get_str(c, "name"));
		if (!owner)
			cj_set_int(used, key, (int64_t)i);
	}

done:
	free(pairs);
	free(dev);
	free(entry);
	cj_free(used);
	return mapping;
}

bool show_devices_needs_fix(const cj *mapping)
{
	for (size_t i = 0; i < cj_size(mapping); i++) {
		const cj *e = cj_at(mapping, i);
		const char *status = cj_get_str(e, "status");
		if (cj_get_bool(e, "rememberedMissing", false))
			return true;
		if (status && (strcmp(status, SHOW_DEVICE_SUGGESTED) == 0 || strcmp(status, SHOW_DEVICE_MISSING) == 0))
			return true;
	}
	return false;
}

int show_devices_apply(cj *collection, const cj *mapping)
{
	int changed = 0;
	for (size_t i = 0; i < cj_size(mapping); i++) {
		const cj *e = cj_at(mapping, i);
		const char *id = cj_get_str(e, "id"), *name = cj_get_str(e, "name");
		const char *id_key = NULL, *name_key = NULL;
		if (!id || !*id || !devices_identity_keys(cj_get_str(e, "sourceType"), &id_key, &name_key) || !id_key)
			continue;
		cj *src = show_find_by_uuid(collection, cj_get_str(e, "slot"));
		if (!cj_is_object(src))
			continue;
		cj *settings = cj_get_or_add_object(src, "settings");
		bool differs = !same_text(cj_get_str(settings, id_key), id);
		if (differs)
			cj_set_str(settings, id_key, id);
		if (name_key && name && *name && !same_text(cj_get_str(settings, name_key), name)) {
			cj_set_str(settings, name_key, name);
			differs = true;
		}
		if (differs)
			changed++;
	}
	return changed;
}

bool show_devices_remember_auto(cj *map, const cj *mapping)
{
	bool changed = false;
	if (!cj_is_object(map))
		return false;
	for (size_t i = 0; i < cj_size(mapping); i++) {
		const cj *e = cj_at(mapping, i);
		const char *status = cj_get_str(e, "status"), *slot = cj_get_str(e, "slot");
		const char *id = cj_get_str(e, "id");
		if (!slot || !id || !*id || !status)
			continue;
		if (strcmp(status, SHOW_DEVICE_AUTO) != 0 && strcmp(status, SHOW_DEVICE_REMEMBERED) != 0)
			continue;
		const cj *have = cj_get(map, slot);
		if (same_text(cj_get_str(have, "id"), id))
			continue;
		cj *rem = cj_set(map, slot, cj_new_object());
		cj_set_str(rem, "id", id);
		cj_set_str(rem, "name", str_or_empty(cj_get_str(e, "name")));
		cj_set_bool(rem, "confirmed", false);
		changed = true;
	}
	return changed;
}

bool show_devices_remember(cj *map, const char *slot, const char *id, const char *name)
{
	if (!cj_is_object(map) || !slot || !*slot)
		return false;
	if (!id || !*id)
		return cj_remove(map, slot) > 0;
	cj *rem = cj_set(map, slot, cj_new_object());
	if (!rem)
		return false;
	cj_set_str(rem, "id", id);
	cj_set_str(rem, "name", str_or_empty(name));
	cj_set_bool(rem, "confirmed", true);
	return true;
}

bool show_devices_alias(cj *aliases, const char *source_type, const cj *identity, const char *id, const char *name)
{
	char key[600];
	if (!cj_is_object(aliases) || !id || !*id)
		return false;
	show_device_identity_key(source_type, identity, key, sizeof(key));
	if (!key[0])
		return false;
	cj *a = cj_set(aliases, key, cj_new_object());
	if (!a)
		return false;
	cj_set_str(a, "id", id);
	cj_set_str(a, "name", str_or_empty(name));
	return true;
}
