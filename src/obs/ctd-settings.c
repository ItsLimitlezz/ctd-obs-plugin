/*
 * Where the plugin keeps its settings, and CTD OBS's one CTD settings folder (integration gap 17).
 *
 * Settings files: multistream.json (CTD Relay: address, pairing, profile), servers.json (CTD Hub servers), hub.json
 * (CTD Hub dock) and sync-groups.json, each with its .bak. The Show Profiles store and the LUT cache are data, not
 * settings, and stay in the plugin's own config folder.
 *
 * In CTD OBS, core answers `ctd_config_dir(out string path)` with <config>/ctd-obs/ctd/, and the settings live
 * there. The first time, the files are COPIED there from the plugin's own config folder (plugin_config/ctd-live-source/),
 * which is left as it is, as a backup. A file already in the CTD folder is never overwritten. If any copy fails, the
 * files copied in that run are removed again and the old folder stays in use, so a failed move loses nothing and is
 * retried at the next start. A marker file (.live-source-moved) records that the move is done.
 *
 * Elsewhere (stock OBS, or a CTD OBS without that proc) the settings stay in the plugin's own config folder.
 *
 * Profile ▸ Export / Import in CTD OBS carry the CTD settings, without secrets (procs, UI thread):
 *   void ctd_settings_export(in string dir, out bool success, out string message)
 *       relay.json (CTD Relay address and profile; no pairing token), servers.json (CTD Hub servers without
 *       passwords, tokens, passphrases or webhook URLs), hub.json, sync-groups.json.
 *   void ctd_settings_import(in string dir, out bool success, out string message)
 *       Adds the Hub servers this CTD OBS doesn't have (same ids, passwords to be entered again), sets the CTD Relay
 *       address and profile only when this CTD OBS isn't connected to a CTD Relay, and takes hub.json /
 *       sync-groups.json only when this CTD OBS has none (they apply at the next start). Never touches a pairing.
 */

#include "ctd-settings.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdio.h>
#include <string.h>

#include "../multistream.h"
#include "servers.h"

#define LOG_PREFIX "[CTD] live-source: "

static const char *const settings_files[] = {"multistream.json", "servers.json", "hub.json", "sync-groups.json"};
static const char *const settings_exts[] = {"", ".bak"};
static const char *const moved_marker = ".live-source-moved";

static char *settings_dir; /* with a trailing slash */

static void ensure_slash(struct dstr *path)
{
	if (path->len && path->array[path->len - 1] != '/' && path->array[path->len - 1] != '\\')
		dstr_cat_ch(path, '/');
}

/* core's CTD settings folder, or NULL outside CTD OBS */
static char *core_ctd_dir(void)
{
	calldata_t cd = {0};
	char *dir = NULL;
	if (proc_handler_call(obs_get_proc_handler(), "ctd_config_dir", &cd)) {
		const char *path = calldata_string(&cd, "path");
		if (path && *path)
			dir = bstrdup(path);
	}
	calldata_free(&cd);
	return dir;
}

/* Copies the settings files from `from` to `to` (both ending in '/'). False if any copy failed; then nothing that this
 * run created is left in `to`. */
static bool copy_settings(const char *from, const char *to, int *copied)
{
	DARRAY(char *) created = {0};
	bool ok = true;
	*copied = 0;
	for (size_t f = 0; ok && f < sizeof(settings_files) / sizeof(*settings_files); f++) {
		for (size_t e = 0; ok && e < sizeof(settings_exts) / sizeof(*settings_exts); e++) {
			struct dstr src = {0}, dst = {0};
			dstr_printf(&src, "%s%s%s", from, settings_files[f], settings_exts[e]);
			dstr_printf(&dst, "%s%s%s", to, settings_files[f], settings_exts[e]);
			if (os_file_exists(src.array) && !os_file_exists(dst.array)) {
				const int64_t size = os_get_file_size(src.array);
				if (os_copyfile(src.array, dst.array) != 0 || os_get_file_size(dst.array) != size) {
					blog(LOG_WARNING, LOG_PREFIX "couldn't copy %s to %s", src.array, dst.array);
					if (os_file_exists(dst.array))
						os_unlink(dst.array);
					ok = false;
				} else {
					char *path = bstrdup(dst.array);
					da_push_back(created, &path);
					(*copied)++;
				}
			}
			dstr_free(&src);
			dstr_free(&dst);
		}
	}
	for (size_t i = 0; i < created.num; i++) {
		if (!ok)
			os_unlink(created.array[i]);
		bfree(created.array[i]);
	}
	da_free(created);
	return ok;
}

static void move_to(const char *old_dir, char *ctd_dir)
{
	struct dstr marker = {0};
	dstr_printf(&marker, "%s%s", ctd_dir, moved_marker);
	if (os_file_exists(marker.array)) {
		dstr_free(&marker);
		bfree(settings_dir);
		settings_dir = ctd_dir;
		return;
	}

	int copied = 0;
	if (!copy_settings(old_dir, ctd_dir, &copied)) {
		blog(LOG_WARNING, LOG_PREFIX "settings stay in %s this time (moving them to %s failed; nothing was lost)",
		     old_dir, ctd_dir);
		dstr_free(&marker);
		bfree(ctd_dir);
		return;
	}

	struct dstr note = {0};
	dstr_printf(&note,
		    "CTD Live Source settings moved here from %s (copied; the files there are kept as a backup).\n",
		    old_dir);
	if (!os_quick_write_utf8_file(marker.array, note.array, note.len, false))
		blog(LOG_WARNING, LOG_PREFIX "couldn't write %s; the move is checked again at the next start",
		     marker.array);
	dstr_free(&note);
	dstr_free(&marker);

	blog(LOG_INFO, LOG_PREFIX "settings now in %s (%d file(s) copied from %s, which is kept as a backup)", ctd_dir,
	     copied, old_dir);
	bfree(settings_dir);
	settings_dir = ctd_dir;
}

/* ---------------------------------------------------------------- export / import --- */

static bool write_json(obs_data_t *data, const char *dir, const char *file)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", dir, file);
	const bool ok = obs_data_save_json_safe(data, path.array, "tmp", "bak");
	dstr_free(&path);
	return ok;
}

static void copy_plain(const char *file, const char *from_dir, const char *to_dir, bool only_if_missing, int *count)
{
	struct dstr src = {0}, dst = {0};
	dstr_printf(&src, "%s%s%s", from_dir, (from_dir[strlen(from_dir) - 1] == '/') ? "" : "/", file);
	dstr_printf(&dst, "%s%s%s", to_dir, (to_dir[strlen(to_dir) - 1] == '/') ? "" : "/", file);
	if (os_file_exists(src.array) && !(only_if_missing && os_file_exists(dst.array)) &&
	    os_copyfile(src.array, dst.array) == 0)
		(*count)++;
	dstr_free(&src);
	dstr_free(&dst);
}

static void proc_export(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	const char *dir = calldata_string(cd, "dir");
	if (!dir || !*dir || os_mkdirs(dir) == MKDIR_ERROR) {
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "message", obs_module_text("Settings.Export.Failed"));
		return;
	}

	bool ok = true;

	/* CTD Relay: where and which profile, never the token */
	struct ms_snapshot s;
	ms_snapshot_get(&s);
	obs_data_t *relay = obs_data_create();
	obs_data_set_string(relay, "server", s.server ? s.server : "");
	obs_data_set_string(relay, "profile_id", s.profile_id ? s.profile_id : "");
	ms_snapshot_free(&s);
	ok = write_json(relay, dir, "relay.json") && ok;
	obs_data_release(relay);

	/* CTD Hub servers without their secrets */
	obs_data_t *root = obs_data_create();
	obs_data_array_t *items = obs_data_array_create();
	const size_t count = servers_count();
	for (size_t i = 0; i < count; i++) {
		struct ctd_server server = {0};
		if (!servers_get_index(i, &server))
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", server.id);
		obs_data_set_string(item, "name", server.name ? server.name : "");
		obs_data_set_string(item, "host", server.host ? server.host : "");
		obs_data_set_int(item, "srt_port", server.srt_port);
		obs_data_set_string(item, "api_url", server.api_url ? server.api_url : "");
		obs_data_set_string(item, "api_user", server.api_user ? server.api_user : "");
		obs_data_set_string(item, "read_user", server.read_user ? server.read_user : "");
		obs_data_set_bool(item, "default", server.is_default);
		obs_data_set_bool(item, "allow_writes", server.allow_writes);
		obs_data_array_push_back(items, item);
		obs_data_release(item);
		ctd_server_free(&server);
	}
	obs_data_set_array(root, "servers", items);
	obs_data_array_release(items);
	ok = write_json(root, dir, "servers.json") && ok;
	obs_data_release(root);

	int copied = 0;
	copy_plain("hub.json", settings_dir, dir, false, &copied);
	copy_plain("sync-groups.json", settings_dir, dir, false, &copied);

	calldata_set_bool(cd, "success", ok);
	calldata_set_string(cd, "message", obs_module_text(ok ? "Settings.Export.Done" : "Settings.Export.Failed"));
	blog(ok ? LOG_INFO : LOG_WARNING, LOG_PREFIX "CTD settings %s to %s (no pairing token, no server secrets)",
	     ok ? "exported" : "partly exported", dir);
}

static void proc_import(void *unused, calldata_t *cd)
{
	UNUSED_PARAMETER(unused);
	const char *dir = calldata_string(cd, "dir");
	struct dstr message = {0};
	int servers_added = 0;
	bool relay_set = false;
	int files = 0;

	/* CTD Hub servers this CTD OBS doesn't have, with the same ids (sources refer to them) */
	struct dstr path = {0};
	dstr_printf(&path, "%s/servers.json", dir ? dir : "");
	obs_data_t *root = dir ? obs_data_create_from_json_file(path.array) : NULL;
	obs_data_array_t *items = root ? obs_data_get_array(root, "servers") : NULL;
	const bool had_servers = servers_count() > 0;
	for (size_t i = 0; items && i < obs_data_array_count(items); i++) {
		obs_data_t *item = obs_data_array_item(items, i);
		const char *id = obs_data_get_string(item, "id");
		struct ctd_server existing = {0};
		if (*id && !servers_get(id, &existing)) {
			struct ctd_server server = {0};
			snprintf(server.id, sizeof(server.id), "%s", id);
			server.name = (char *)obs_data_get_string(item, "name");
			server.host = (char *)obs_data_get_string(item, "host");
			server.srt_port = (int)obs_data_get_int(item, "srt_port");
			server.api_url = (char *)obs_data_get_string(item, "api_url");
			server.api_user = (char *)obs_data_get_string(item, "api_user");
			server.api_pass = (char *)"";
			server.api_token = (char *)"";
			server.read_user = (char *)obs_data_get_string(item, "read_user");
			server.read_pass = (char *)"";
			server.passphrase = (char *)"";
			server.webhook_url = (char *)"";
			server.is_default = !had_servers && obs_data_get_bool(item, "default");
			server.allow_writes = obs_data_get_bool(item, "allow_writes");
			if (servers_import(&server))
				servers_added++;
		} else if (*id) {
			ctd_server_free(&existing);
		}
		obs_data_release(item);
	}
	obs_data_array_release(items);
	obs_data_release(root);

	/* CTD Relay address and profile, only when this CTD OBS isn't connected to one */
	dstr_printf(&path, "%s/relay.json", dir ? dir : "");
	obs_data_t *relay = dir ? obs_data_create_from_json_file(path.array) : NULL;
	if (relay && *obs_data_get_string(relay, "server")) {
		struct ms_snapshot s;
		ms_snapshot_get(&s);
		const bool free_to_set = s.link == MS_DISCONNECTED && (!s.server || !*s.server);
		ms_snapshot_free(&s);
		if (free_to_set) {
			ms_set_server(obs_data_get_string(relay, "server"));
			if (*obs_data_get_string(relay, "profile_id"))
				ms_set_profile(obs_data_get_string(relay, "profile_id"));
			relay_set = true;
		}
	}
	obs_data_release(relay);
	dstr_free(&path);

	/* Hub dock and sync groups, only when this CTD OBS has none (read at the next start) */
	if (dir) {
		copy_plain("hub.json", dir, settings_dir, true, &files);
		copy_plain("sync-groups.json", dir, settings_dir, true, &files);
	}

	dstr_printf(&message, obs_module_text("Settings.Import.Done"), servers_added);
	if (relay_set) {
		dstr_cat(&message, "\n");
		dstr_cat(&message, obs_module_text("Settings.Import.Relay"));
	}
	if (files) {
		dstr_cat(&message, "\n");
		dstr_cat(&message, obs_module_text("Settings.Import.Restart"));
	}
	calldata_set_bool(cd, "success", true);
	calldata_set_string(cd, "message", message.array);
	blog(LOG_INFO, LOG_PREFIX "CTD settings imported from %s: %d Hub server(s), relay address %s, %d file(s)",
	     dir ? dir : "", servers_added, relay_set ? "set" : "unchanged", files);
	dstr_free(&message);
}

/* --------------------------------------------------------------------------- init --- */

void ctd_settings_init(void)
{
	char *own = obs_module_config_path("");
	struct dstr old_dir = {0};
	dstr_copy(&old_dir, own ? own : "");
	bfree(own);
	ensure_slash(&old_dir);
	os_mkdirs(old_dir.array);
	settings_dir = bstrdup(old_dir.array);

	char *ctd = core_ctd_dir();
	if (ctd) {
		struct dstr ctd_dir = {0};
		dstr_copy(&ctd_dir, ctd);
		bfree(ctd);
		ensure_slash(&ctd_dir);
		if (os_mkdirs(ctd_dir.array) == MKDIR_ERROR) {
			blog(LOG_WARNING, LOG_PREFIX "can't create %s; settings stay in %s", ctd_dir.array,
			     old_dir.array);
			dstr_free(&ctd_dir);
		} else {
			move_to(old_dir.array, ctd_dir.array); /* takes ctd_dir.array */
		}
	}
	dstr_free(&old_dir);

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void ctd_settings_export(in string dir, out bool success, out string message)",
			 proc_export, NULL);
	proc_handler_add(ph, "void ctd_settings_import(in string dir, out bool success, out string message)",
			 proc_import, NULL);
}

void ctd_settings_shutdown(void)
{
	bfree(settings_dir);
	settings_dir = NULL;
}

char *ctd_settings_path(const char *file)
{
	if (!settings_dir)
		return obs_module_config_path(file);
	os_mkdirs(settings_dir);
	struct dstr path = {0};
	dstr_printf(&path, "%s%s", settings_dir, file);
	return path.array;
}
