/* Secret scrubber (ticket T10, design section 8): nothing secret leaves the machine in a bundle. Pure C, no libobs.
 *
 * What counts as secret:
 * - setting names: any containing password/passwd/passphrase/secret/token/credential/cookie/bearer/apikey, a "key"
 *   word right after stream/api/auth/access/private/secret/client/app/license/session/signing/encryption (stream_key,
 *   apiKey, ...), and the exact names "key", "pass", "pwd", "auth", "authorization", "sig", "signature";
 * - inside any string that is a URL: query parameters key, token, access_token, auth, secret, password, sig and the
 *   names above, and user:password@ credentials;
 * - values that look like a credential whatever their name: streaming keys (live_..., sk_live_..., FB-...,
 *   xxxx-xxxx-xxxx-xxxx-xxxx), JWTs (eyJ...xxx.yyy), "Bearer ..." and long base64/hex tokens in a secret position;
 * - CTD overlays (browser sources whose private settings carry ctd_overlay_slot): the URL, which holds ?key=, is
 *   replaced by an overlay ref {source, slot, canvas} (resolved from Relay at apply time, design 1.1);
 * - the stream service (service.json): reduced to an allow-list (type, service, server, protocol, bwtest...). The
 *   key, username, password and bearer token never stay.
 *
 * A scrubbed value becomes SHOW_SECRET_PLACEHOLDER (a whole value) or keeps its shape with the secret part replaced
 * (a URL keeps its host and path), and is recorded twice: in manifest.secrets[] as {source, key} (where), and in the
 * bundle's local half as the original value, so this machine restores it when it switches back (local-first). */
#ifndef CTD_SHOW_SCRUB_H
#define CTD_SHOW_SCRUB_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOW_OVERLAY_PLACEHOLDER "${ctd-overlay}"

struct show_scrub_report {
	int secrets;     /* values moved to the local secrets */
	int overlays;    /* overlay refs built */
	int stream_keys; /* stream keys / service credentials dropped from service.json */
};

/* Is a setting with this name (any case) secret? */
bool show_scrub_is_secret_name(const char *name);
/* Does this value look like a credential on its own (stream key, JWT, bearer)? */
bool show_scrub_is_secret_value(const char *value);
/* Copies url to out with secret query values and URL credentials replaced by SHOW_SECRET_PLACEHOLDER. Returns true
 * when something was replaced. out is malloc'd (free()) and NULL when nothing changed. */
bool show_scrub_url(const char *url, char **out);

/* Scrubs a scene collection in place. manifest gains secrets[] and overlays[] entries; local_secrets (an object)
 * gains "<uuid>|<pointer>" -> original value, where uuid is the nearest enclosing source/filter object that has a
 * "uuid" member ("" at the top level) and pointer is a JSON Pointer from that object. report may be NULL. */
bool show_scrub_collection(cj *collection, cj *manifest, cj *local_secrets, struct show_scrub_report *report);

/* Scrubs any other settings document in place (streamEncoder.json, recordEncoder.json), recording what it removed
 * in local_secrets under "<file>|<pointer>". */
bool show_scrub_settings(cj *settings, const char *file, cj *local_secrets, struct show_scrub_report *report);

/* Reduces service.json to its allow-listed members. *local_service (if not NULL) receives a copy of the original
 * document so this machine can put its own key back. */
bool show_scrub_service(cj *service, cj **local_service, struct show_scrub_report *report);

/* Removes secret-named keys from an INI text (basic.ini): returns a malloc'd copy without those lines. */
char *show_scrub_ini(const char *ini);

/* Apply side: puts the local secrets back into a collection (and settings files keyed "<file>|..."). Every placeholder
 * that stays (no local value: a bundle from another machine) is listed in missing (an array, may be NULL) as
 * {source, key}, for the dock's "needs re-entry". Returns how many values were restored. */
int show_secrets_restore(cj *collection, const cj *local_secrets, cj *missing);
int show_secrets_restore_settings(cj *settings, const char *file, const cj *local_secrets);

/* Finds the object whose "uuid" member equals uuid anywhere under root (depth-first), or NULL. */
cj *show_find_by_uuid(cj *root, const char *uuid);

#ifdef __cplusplus
}
#endif

#endif
