/* Export the current OBS setup into a bundle; see export.h. */
#include "export.h"

#include "devices-identity.h"
#include "ini.h"
#include "manifest.h"
#include "scrub.h"
#include "sha256.h"
#include "show-fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t err_size, const char *fmt, const char *arg)
{
	if (err && err_size)
		snprintf(err, err_size, fmt, arg ? arg : "");
	return false;
}

static bool default_hash(void *opaque, const char *path, char sha256[65], int64_t *size)
{
	(void)opaque;
	return sha256_file(path, sha256, size, NULL, NULL);
}

static const char *or_empty(const char *s)
{
	return s ? s : "";
}

/* ---- devices ---- */

static const char *settings_get(void *ctx, const char *key)
{
	return cj_get_str((const cj *)ctx, key);
}

static void add_identity_field(cj *identity, const char *key, const char *value)
{
	if (value[0])
		cj_set_str(identity, key, value);
}

static bool add_device(cj *devices, const cj *src, cj *warnings)
{
	const char *id = cj_get_str(src, "id");
	const char *versioned = cj_get_str(src, "versioned_id");
	const char *type = id;
	const cj *settings = cj_get(src, "settings");
	const char *name = cj_get_str(src, "name");
	const char *uuid = cj_get_str(src, "uuid");
	devices_identity_t ident;
	if (!type || !devices_identity_class(type))
		type = versioned;
	if (!type || !devices_identity_class(type) || !cj_is_object(settings))
		return true;
	if (!devices_identity(type, settings_get, (void *)settings, &ident))
		return true;
	if (!uuid) {
		char msg[320];
		snprintf(msg, sizeof(msg), "Device source %.200s has no uuid: not mapped", or_empty(name));
		if (warnings)
			cj_append(warnings, cj_new_string(msg));
		return true;
	}
	cj *d = cj_append(devices, cj_new_object());
	if (!d)
		return false;
	cj_set_str(d, "slot", uuid);
	cj_set_str(d, "sourceType", id ? id : type);
	cj_set_str(d, "class", ident.cls);
	cj *identity = cj_get_or_add_object(d, "identity");
	if (!identity)
		return false;
	add_identity_field(identity, "name", ident.name);
	add_identity_field(identity, "id", ident.id);
	add_identity_field(identity, "vidPid", ident.vid_pid);
	add_identity_field(identity, "serial", ident.serial);
	cj_set_str(d, "label", or_empty(name));
	return true;
}

/* sources[] and the global audio objects ("DesktopAudioDevice1"...): every source object that can be a device */
static bool build_devices(const cj *collection, cj *devices, cj *warnings)
{
	for (size_t i = 0; i < cj_size(collection); i++) {
		const cj *v = cj_at(collection, i);
		if (strcmp(cj_key_at(collection, i), "sources") == 0 && cj_is_array(v)) {
			for (size_t j = 0; j < cj_size(v); j++)
				if (cj_is_object(cj_at(v, j)) && !add_device(devices, cj_at(v, j), warnings))
					return false;
		} else if (cj_is_object(v) && cj_get_str(v, "id") && cj_is_object(cj_get(v, "settings"))) {
			if (!add_device(devices, v, warnings))
				return false;
		}
	}
	return true;
}

/* ---- warnings ---- */

static void missing_warnings(const cj *collection, const cj *missing, cj *warnings)
{
	for (size_t i = 0; warnings && i < cj_size(missing); i++) {
		const cj *m = cj_at(missing, i);
		const char *uuid = cj_get_str(m, "source");
		cj *src = uuid ? show_find_by_uuid((cj *)collection, uuid) : NULL;
		const char *name = src ? cj_get_str(src, "name") : NULL;
		char msg[1400];
		snprintf(msg, sizeof(msg), "Missing file for %.200s: %.1000s", name ? name : "a source",
			 or_empty(cj_get_str(m, "path")));
		cj_append(warnings, cj_new_string(msg));
	}
}

/* ---- profile files ---- */

static cj *read_json(const char *path, char *err, size_t err_size)
{
	return cj_parse_file(path, err, err_size);
}

static bool add_text_file(struct show_bundle *out, cj *files_profile, const char *name, const char *text)
{
	if (!show_bundle_set_file(out, name, text, strlen(text)))
		return false;
	return cj_append(files_profile, cj_new_string(name)) != NULL;
}

static bool add_json_file(struct show_bundle *out, cj *files_profile, const char *name, const cj *doc)
{
	char *text = cj_print(doc, true);
	bool ok = text && add_text_file(out, files_profile, name, text);
	cj_free_str(text);
	return ok;
}

static bool export_profile(const struct show_export_input *in, struct show_bundle *out, cj *files_profile, cj *warnings,
			   char *err, size_t err_size)
{
	static const struct {
		const char *file, *path;
	} encoders[] = {{"streamEncoder.json", SHOW_FILE_STREAM_ENCODER},
			{"recordEncoder.json", SHOW_FILE_RECORD_ENCODER}};
	char path[1024], perr[256];
	cj *secrets = cj_get(out->local, "secrets");

	if (!in->profile_dir || !in->profile_dir[0])
		return true;

	if (sfs_join(path, sizeof(path), in->profile_dir, "basic.ini") && sfs_exists(path)) {
		char *text = NULL, *filtered, *scrubbed;
		size_t size = 0;
		if (!sfs_read(path, &text, &size))
			return fail(err, err_size, "can't read %s", path);
		filtered = show_ini_filter(text);
		free(text);
		scrubbed = filtered ? show_scrub_ini(filtered) : NULL;
		free(filtered);
		if (!scrubbed)
			return fail(err, err_size, "out of memory%s", "");
		bool ok = add_text_file(out, files_profile, SHOW_FILE_BASIC_INI, scrubbed);
		free(scrubbed);
		if (!ok)
			return fail(err, err_size, "out of memory%s", "");
	}

	for (size_t i = 0; i < sizeof(encoders) / sizeof(encoders[0]); i++) {
		if (!sfs_join(path, sizeof(path), in->profile_dir, encoders[i].file) || !sfs_exists(path))
			continue;
		cj *doc = read_json(path, perr, sizeof(perr));
		if (!doc) {
			char msg[600];
			snprintf(msg, sizeof(msg), "%s was not saved: %.300s", encoders[i].file, perr);
			if (warnings)
				cj_append(warnings, cj_new_string(msg));
			continue;
		}
		bool ok = show_scrub_settings(doc, encoders[i].path, secrets, NULL) &&
			  add_json_file(out, files_profile, encoders[i].path, doc);
		cj_free(doc);
		if (!ok)
			return fail(err, err_size, "can't export %s", encoders[i].file);
	}

	if (sfs_join(path, sizeof(path), in->profile_dir, "service.json") && sfs_exists(path)) {
		cj *doc = read_json(path, perr, sizeof(perr));
		if (!doc) {
			char msg[600];
			snprintf(msg, sizeof(msg), "service.json was not saved: %.300s", perr);
			if (warnings)
				cj_append(warnings, cj_new_string(msg));
		} else {
			cj *original = cj_clone(doc);
			cj *local_service = NULL;
			bool ok = original && show_scrub_service(doc, &local_service, NULL);
			if (ok) {
				if (!local_service) {
					local_service = original;
					original = NULL;
				}
				ok = cj_set(out->local, "service", local_service) != NULL;
				local_service = NULL;
			}
			ok = ok && add_json_file(out, files_profile, SHOW_FILE_SERVICE, doc);
			cj_free(local_service);
			cj_free(original);
			cj_free(doc);
			if (!ok)
				return fail(err, err_size, "can't export %s", "service.json");
		}
	}
	return true;
}

/* ---- manifest ---- */

static bool fill_manifest(const struct show_export_input *in, cj *m, cj *files_profile)
{
	cj *files = cj_get_or_add_object(m, "files");
	cj *saved_by = cj_get_or_add_object(m, "savedBy");
	cj *versions = cj_get_or_add_object(m, "versions");
	if (!files || !saved_by || !versions)
		return false;
	if (in->previous_manifest) {
		const cj *relay = cj_get(in->previous_manifest, "relayProfileId");
		/* Relay assigns revision numbers (design 1.3): a local save is revision 0 ("not on Relay yet") and names
		 * the Relay revision it was based on. M3 sets revision once Relay accepts it. */
		int64_t rev = cj_get_int(in->previous_manifest, "revision", 0);
		int64_t base = rev > 0 ? rev : cj_get_int(in->previous_manifest, "parentRevision", 0);
		if (relay && !cj_set(m, "relayProfileId", cj_clone(relay)))
			return false;
		cj_set_int(m, "parentRevision", base);
		cj_set_int(m, "revision", 0);
	}
	cj_set_str(m, "savedAt", or_empty(in->saved_at));
	cj_set_str(saved_by, "appId", or_empty(in->app_id));
	cj_set_str(saved_by, "os", or_empty(in->os));
	cj_set_str(versions, "obs", or_empty(in->obs_version));
	cj_set_str(versions, "ctdObs", or_empty(in->ctd_obs_version));
	cj_set_str(versions, "plugin", or_empty(in->plugin_version));
	cj_set_str(files, "collection", SHOW_FILE_COLLECTION);
	return cj_set(files, "profile", files_profile) != NULL;
}

bool show_export(const struct show_export_input *in, struct show_bundle *out, cj *warnings, char *err, size_t err_size)
{
	char perr[256];
	cj *missing = NULL, *files_profile = NULL, *secrets = NULL;
	struct show_scrub_report report;
	bool ok = false;

	if (err && err_size)
		err[0] = '\0';
	if (!in || !out || !in->id || !in->name || !in->collection_path)
		return fail(err, err_size, "export: missing input%s", "");

	out->manifest = show_manifest_new(in->id, in->name);
	out->collection = cj_parse_file(in->collection_path, perr, sizeof(perr));
	out->local = cj_new_object();
	out->asset_sources = cj_new_object();
	missing = cj_new_array();
	files_profile = cj_new_array();
	secrets = out->local ? cj_get_or_add_object(out->local, "secrets") : NULL;
	if (!out->collection) {
		fail(err, err_size, "can't read the scene collection: %s", perr);
		goto done;
	}
	if (!cj_is_object(out->collection)) {
		fail(err, err_size, "the scene collection is not a JSON object%s", "");
		goto done;
	}
	if (!out->manifest || !out->local || !out->asset_sources || !missing || !files_profile || !secrets) {
		fail(err, err_size, "out of memory%s", "");
		goto done;
	}

	/* the tie to a show profile (show.h SHOW_MODULE_KEY) is this machine's apply state, not content */
	cj_remove(cj_get(out->collection, "modules"), "ctd-show-profile");

	memset(&report, 0, sizeof(report));
	if (!show_scrub_collection(out->collection, out->manifest, secrets, &report)) {
		fail(err, err_size, "can't scrub the scene collection%s", "");
		goto done;
	}
	if (!show_assets_rewrite(out->collection, out->manifest, out->asset_sources, missing,
				 in->hash ? in->hash : default_hash, in->hash ? in->hash_opaque : NULL, err, err_size))
		goto done;
	missing_warnings(out->collection, missing, warnings);
	if (!build_devices(out->collection, cj_get_or_add_array(out->manifest, "devices"), warnings)) {
		fail(err, err_size, "out of memory%s", "");
		goto done;
	}
	if (!export_profile(in, out, files_profile, warnings, err, err_size))
		goto done;
	if (!fill_manifest(in, out->manifest, files_profile)) {
		fail(err, err_size, "out of memory%s", "");
		goto done;
	}
	files_profile = NULL; /* owned by the manifest now */
	ok = show_manifest_validate(out->manifest, err, err_size);

done:
	cj_free(files_profile);
	cj_free(missing);
	return ok;
}
