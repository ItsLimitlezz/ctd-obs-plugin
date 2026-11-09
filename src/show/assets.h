/* Asset scanner/rewriter (ticket T9, design section 1.1): file paths in a scene collection become
 * "${ctd-asset:<sha256>}" on export, and cache paths again on apply. Pure C, no libobs.
 *
 * Known path keys (design T9): image_source.file, ffmpeg_source.local_file, vlc_source.playlist[].value,
 * slideshow.files[].value, browser_source.local_file, clut_filter.image_path, text_gdiplus/text_ft2_source "file"
 * (and their versioned ids), obs_stinger_transition.path / track_matte_path. Fallback: any other string setting that
 * is an existing absolute path to a regular file.
 *
 * An asset ref is {source, filter, key}: source is the uuid of the source (scene, input, transition), filter the uuid
 * of the filter (or null), and key a JSON Pointer from that source/filter object ("/settings/file",
 * "/settings/playlist/0/value"). */
#ifndef CTD_SHOW_ASSETS_H
#define CTD_SHOW_ASSETS_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hashes a file. Export passes one with a cache keyed by path+size+mtime (design section 4). */
typedef bool (*show_hash_fn)(void *opaque, const char *path, char sha256[65], int64_t *size);

/* The design 1.1 kind of a file ("lut", "image", "video", "audio", "font", "html", "other"), from the source/filter
 * type and the extension. */
const char *show_asset_kind(const char *source_type, const char *path);

/* Export: rewrites every asset path in the collection (sources, groups, transitions, their filters; scene items are
 * not assets) to its placeholder, and fills manifest.assets[] (one entry per sha256, refs merged) and asset_sources
 * ({sha256: path}). A known key whose file is missing is left as it is and reported in missing (an array of
 * {source, filter, key, path}; may be NULL). Fails only when hashing an existing file fails. */
bool show_assets_rewrite(cj *collection, cj *manifest, cj *asset_sources, cj *missing, show_hash_fn hash, void *opaque,
			 char *err, size_t err_size);

/* Apply: where the asset with this manifest entry lives on this machine (the store's cache path). */
typedef bool (*show_asset_path_fn)(void *opaque, const cj *asset, char *out, size_t size);

/* Apply: replaces every "${ctd-asset:<sha256>}" string anywhere in the collection with path_for's answer. A
 * placeholder whose sha256 is not in manifest.assets, or that path_for can't place, fails the call (nothing is half
 * rewritten from the caller's point of view: it works on a copy it then discards). */
bool show_assets_resolve(cj *collection, const cj *manifest, show_asset_path_fn path_for, void *opaque, char *err,
			 size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
