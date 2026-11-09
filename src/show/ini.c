/* OBS profile basic.ini for Show Profiles; see ini.h.
 *
 * What a bundle carries (an allow-list, deliberately small; everything else stays on the machine):
 *   [Video]        BaseCX BaseCY OutputCX OutputCY FPSType FPSCommon FPSInt FPSNum FPSDen ScaleType ColorFormat
 *                  ColorSpace ColorRange SdrWhiteLevel HdrNominalPeakLevel
 *   [Audio]        SampleRate ChannelSetup            (the audio devices and monitoring device are local)
 *   [Output]       Mode
 *   [SimpleOutput] VBitrate ABitrate StreamEncoder RecQuality RecEncoder RecFormat2 Preset NVENCPreset2 QSVPreset
 *                  AMDPreset UseAdvanced x264Settings RecRB RecRBTime RecRBSize
 *   [AdvOut]       Encoder TrackIndex VodTrackIndex ApplyServiceSettings Rescale RescaleRes RescaleFilter RecType
 *                  RecEncoder RecFormat2 RecTracks RecRB RecRBTime RecRBSize Track1Bitrate..Track6Bitrate
 *                  Track1Name..Track6Name AudioEncoder RecAudioEncoder
 * Never carried, even when listed above by mistake: a key whose name has path/filename/directory/dir, hotkey,
 * monitor, device, key, token, password, secret in it, or that show_scrub_is_secret_name flags.
 * Section and key names are matched case-insensitively (OBS's config does the same). */
#include "ini.h"

#include "scrub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (lower(*a) != lower(*b))
			return false;
	return *a == *b;
}

static bool ieq_n(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

static bool icontains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && lower(hay[i]) == needle[i])
			i++;
		if (i == n)
			return true;
	}
	return false;
}

static char *dup_n(const char *s, size_t n)
{
	char *p = malloc(n + 1);
	if (p) {
		memcpy(p, s, n);
		p[n] = '\0';
	}
	return p;
}

/* ---- allow-list ---- */

static const char *const video_keys[] = {"BaseCX",     "BaseCY",        "OutputCX",
					 "OutputCY",   "FPSType",       "FPSCommon",
					 "FPSInt",     "FPSNum",        "FPSDen",
					 "ScaleType",  "ColorFormat",   "ColorSpace",
					 "ColorRange", "SdrWhiteLevel", "HdrNominalPeakLevel",
					 NULL};
static const char *const audio_keys[] = {"SampleRate", "ChannelSetup", NULL};
static const char *const output_keys[] = {"Mode", NULL};
static const char *const simple_keys[] = {"VBitrate",   "ABitrate",   "StreamEncoder", "RecQuality",
					  "RecEncoder", "RecFormat2", "Preset",        "NVENCPreset2",
					  "QSVPreset",  "AMDPreset",  "UseAdvanced",   "x264Settings",
					  "RecRB",      "RecRBTime",  "RecRBSize",     NULL};
static const char *const adv_keys[] = {"Encoder",    "TrackIndex", "VodTrackIndex", "ApplyServiceSettings",
				       "Rescale",    "RescaleRes", "RescaleFilter", "RecType",
				       "RecEncoder", "RecFormat2", "RecTracks",     "RecRB",
				       "RecRBTime",  "RecRBSize",  "AudioEncoder",  "RecAudioEncoder",
				       NULL};

static bool in_list(const char *const *list, const char *key)
{
	for (; *list; list++)
		if (ieq(*list, key))
			return true;
	return false;
}

/* Track1Bitrate..Track6Bitrate, Track1Name..Track6Name */
static bool adv_track_key(const char *key)
{
	if (strlen(key) < 6 || !ieq_n(key, "Track", 5) || key[5] < '1' || key[5] > '6')
		return false;
	return ieq(key + 6, "Bitrate") || ieq(key + 6, "Name");
}

bool show_ini_key_allowed(const char *section, const char *key)
{
	static const char *const never[] = {"path",   "filename", "directory", "dir",      "hotkey", "monitor",
					    "device", "key",      "token",     "password", "secret", NULL};
	if (!section || !key || !*key)
		return false;
	for (int i = 0; never[i]; i++)
		if (icontains(key, never[i]))
			return false;
	if (show_scrub_is_secret_name(key))
		return false;
	if (ieq(section, "Video"))
		return in_list(video_keys, key);
	if (ieq(section, "Audio"))
		return in_list(audio_keys, key);
	if (ieq(section, "Output"))
		return in_list(output_keys, key);
	if (ieq(section, "SimpleOutput"))
		return in_list(simple_keys, key);
	if (ieq(section, "AdvOut"))
		return in_list(adv_keys, key) || adv_track_key(key);
	return false;
}

/* ---- model ---- */

struct kv {
	char *key, *val;
};

struct section {
	char *name; /* "" for lines before the first header */
	struct kv *kv;
	size_t count, cap;
};

struct ini {
	struct section *sec;
	size_t count, cap;
	bool oom;
};

static void ini_free(struct ini *ini)
{
	for (size_t i = 0; i < ini->count; i++) {
		for (size_t j = 0; j < ini->sec[i].count; j++) {
			free(ini->sec[i].kv[j].key);
			free(ini->sec[i].kv[j].val);
		}
		free(ini->sec[i].kv);
		free(ini->sec[i].name);
	}
	free(ini->sec);
}

static struct section *ini_section(struct ini *ini, const char *name, bool create)
{
	for (size_t i = 0; i < ini->count; i++)
		if (ieq(ini->sec[i].name, name))
			return &ini->sec[i];
	if (!create)
		return NULL;
	if (ini->count == ini->cap) {
		size_t cap = ini->cap ? ini->cap * 2 : 8;
		struct section *p = realloc(ini->sec, cap * sizeof(*p));
		if (!p) {
			ini->oom = true;
			return NULL;
		}
		ini->sec = p;
		ini->cap = cap;
	}
	struct section *s = &ini->sec[ini->count];
	memset(s, 0, sizeof(*s));
	s->name = dup_n(name, strlen(name));
	if (!s->name) {
		ini->oom = true;
		return NULL;
	}
	ini->count++;
	return s;
}

/* Sets key (a later duplicate or a merge replaces the value in place). */
static void ini_set(struct ini *ini, struct section *s, const char *key, size_t key_len, const char *val,
		    size_t val_len)
{
	char *v = dup_n(val, val_len);
	if (!v) {
		ini->oom = true;
		return;
	}
	for (size_t i = 0; i < s->count; i++) {
		if (strlen(s->kv[i].key) == key_len) {
			size_t j = 0;
			while (j < key_len && lower(s->kv[i].key[j]) == lower(key[j]))
				j++;
			if (j == key_len) {
				free(s->kv[i].val);
				s->kv[i].val = v;
				return;
			}
		}
	}
	if (s->count == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 8;
		struct kv *p = realloc(s->kv, cap * sizeof(*p));
		if (!p) {
			free(v);
			ini->oom = true;
			return;
		}
		s->kv = p;
		s->cap = cap;
	}
	s->kv[s->count].key = dup_n(key, key_len);
	s->kv[s->count].val = v;
	if (!s->kv[s->count].key) {
		free(v);
		ini->oom = true;
		return;
	}
	s->count++;
}

static const struct kv *ini_find(struct ini *ini, const char *section, const char *key)
{
	struct section *s = ini_section(ini, section, false);
	for (size_t i = 0; s && i < s->count; i++)
		if (ieq(s->kv[i].key, key))
			return &s->kv[i];
	return NULL;
}

/* Parses text (LF, CRLF or a stray CR; a UTF-8 BOM; lines that are neither [Section] nor key=value are dropped). */
static bool ini_parse(struct ini *ini, const char *text)
{
	struct section *cur;
	memset(ini, 0, sizeof(*ini));
	if (!text)
		text = "";
	if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text += 3;
	cur = ini_section(ini, "", true);
	while (*text && !ini->oom) {
		const char *end = text;
		while (*end && *end != '\n' && *end != '\r')
			end++;
		const char *a = text, *b = end;
		while (a < b && (*a == ' ' || *a == '\t'))
			a++;
		while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
			b--;
		if (a < b && *a == '[' && b[-1] == ']') {
			char *name = dup_n(a + 1, (size_t)(b - a) - 2);
			if (!name) {
				ini->oom = true;
				break;
			}
			cur = ini_section(ini, name, true);
			free(name);
			if (!cur)
				break;
		} else if (a < b && *a != ';' && *a != '#') {
			const char *eq = memchr(a, '=', (size_t)(end - a));
			if (eq) {
				const char *k = eq;
				while (k > a && (k[-1] == ' ' || k[-1] == '\t'))
					k--;
				if (k > a)
					ini_set(ini, cur, a, (size_t)(k - a), eq + 1, (size_t)(end - eq - 1));
			}
		}
		text = end;
		while (*text == '\n' || *text == '\r')
			text++;
	}
	if (ini->oom) {
		ini_free(ini);
		return false;
	}
	return true;
}

static size_t put(char *out, size_t w, const char *s)
{
	size_t n = strlen(s);
	memcpy(out + w, s, n);
	return w + n;
}

/* LF line endings, a blank line between sections. */
static char *ini_print(const struct ini *ini)
{
	size_t need = 1;
	for (size_t i = 0; i < ini->count; i++) {
		const struct section *s = &ini->sec[i];
		if (s->count == 0)
			continue;
		need += strlen(s->name) + 4;
		for (size_t j = 0; j < s->count; j++)
			need += strlen(s->kv[j].key) + strlen(s->kv[j].val) + 2;
	}
	char *out = malloc(need);
	size_t w = 0;
	if (!out)
		return NULL;
	for (size_t i = 0; i < ini->count; i++) {
		const struct section *s = &ini->sec[i];
		if (s->count == 0)
			continue;
		if (s->name[0]) {
			if (w)
				out[w++] = '\n';
			out[w++] = '[';
			w = put(out, w, s->name);
			out[w++] = ']';
			out[w++] = '\n';
		}
		for (size_t j = 0; j < s->count; j++) {
			w = put(out, w, s->kv[j].key);
			out[w++] = '=';
			w = put(out, w, s->kv[j].val);
			out[w++] = '\n';
		}
	}
	out[w] = '\0';
	return out;
}

/* ---- API ---- */

char *show_ini_filter(const char *ini_text)
{
	struct ini in, out;
	char *text;
	if (!ini_parse(&in, ini_text))
		return NULL;
	memset(&out, 0, sizeof(out));
	for (size_t i = 0; i < in.count && !out.oom; i++) {
		struct section *dst = NULL;
		for (size_t j = 0; j < in.sec[i].count && !out.oom; j++) {
			const struct kv *kv = &in.sec[i].kv[j];
			if (!show_ini_key_allowed(in.sec[i].name, kv->key))
				continue;
			if (!dst)
				dst = ini_section(&out, in.sec[i].name, true);
			if (dst)
				ini_set(&out, dst, kv->key, strlen(kv->key), kv->val, strlen(kv->val));
		}
	}
	ini_free(&in);
	if (out.oom) {
		ini_free(&out);
		return NULL;
	}
	text = ini_print(&out);
	ini_free(&out);
	return text;
}

char *show_ini_merge(const char *bundle_ini, const char *local_ini, const char *profile_name)
{
	struct ini bundle, local;
	char *text;
	if (!ini_parse(&bundle, bundle_ini))
		return NULL;
	if (!ini_parse(&local, local_ini)) {
		ini_free(&bundle);
		return NULL;
	}
	for (size_t i = 0; i < bundle.count && !local.oom; i++) {
		for (size_t j = 0; j < bundle.sec[i].count && !local.oom; j++) {
			const struct kv *kv = &bundle.sec[i].kv[j];
			if (!show_ini_key_allowed(bundle.sec[i].name, kv->key))
				continue;
			struct section *dst = ini_section(&local, bundle.sec[i].name, true);
			if (dst)
				ini_set(&local, dst, kv->key, strlen(kv->key), kv->val, strlen(kv->val));
		}
	}
	if (!local.oom && profile_name) {
		struct section *general = ini_section(&local, "General", true);
		if (general)
			ini_set(&local, general, "Name", 4, profile_name, strlen(profile_name));
	}
	ini_free(&bundle);
	if (local.oom) {
		ini_free(&local);
		return NULL;
	}
	text = ini_print(&local);
	ini_free(&local);
	return text;
}

bool show_ini_get(const char *ini_text, const char *section, const char *key, char *out, unsigned size)
{
	struct ini ini;
	bool found = false;
	if (!out || size == 0)
		return false;
	out[0] = '\0';
	if (!section || !key || !ini_parse(&ini, ini_text))
		return false;
	const struct kv *kv = ini_find(&ini, section, key);
	if (kv && strlen(kv->val) < size) {
		memcpy(out, kv->val, strlen(kv->val) + 1);
		found = true;
	}
	ini_free(&ini);
	return found;
}
