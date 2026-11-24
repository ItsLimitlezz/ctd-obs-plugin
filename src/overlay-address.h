/*
 * Which address a CTD Overlay overlay is opened at: its local network one or its public (internet) one. CTD Overlay
 * shares both with CTD Relay on each overlay resource (ctd-overlay docs/standard/06-integrations.md §6.11):
 * meta.addresses = { local?, public?, default }, origins only ("scheme://host[:port]"): local its LAN address
 * (http:// or https://), public its SKP_PUBLIC_URL (always https://), and default the one the resource's url and its
 * scenes' URLs are at. Every URL is <origin><path and query>, so the other address is the same URL with the origin
 * swapped. An older CTD Overlay sends no meta.addresses: its one url is used as it is.
 *
 * Pure C: no libobs, so test/overlays-dock checks it on every platform. Nothing here logs; overlay URLs carry the
 * overlay's key.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ctd_address {
	CTD_ADDRESS_NONE = 0, /* not one the overlay is known at */
	CTD_ADDRESS_LOCAL = 1,
	CTD_ADDRESS_PUBLIC = 2,
};

#define CTD_ORIGIN_MAX 512

/* One overlay resource's addresses, checked (ctd_overlay_origins_read). Empty strings when unknown. */
struct ctd_overlay_origins {
	bool legacy;             /* no usable meta.addresses: the resource's url is used as it is, for Local only */
	bool refused_public;     /* meta.addresses.public was given but isn't https://: never used */
	char at[CTD_ORIGIN_MAX]; /* the origin the resource's url is at (the default address) */
	char local[CTD_ORIGIN_MAX];
	char pub[CTD_ORIGIN_MAX]; /* always https:// */
};

/* Is `origin` exactly "http(s)://host[:port]" (no path, query, fragment, credentials or spaces; one trailing '/' is
 * allowed)? https_only: only "https://". The scheme must be lowercase, as CTD Overlay sends it. */
bool ctd_origin_ok(const char *origin, bool https_only);

/* Reads one resource's addresses. url: the resource's url. has_addresses: whether meta.addresses is an object;
 * local, pub, def: its "local", "public" and "default" strings (NULL or "" when absent). A public origin that isn't
 * https:// is refused (refused_public); a local one must be http(s). The url must be at the default origin (the one
 * def names; without def, whichever of local and public it is at), else nothing can be swapped and the resource
 * counts as legacy. */
void ctd_overlay_origins_read(struct ctd_overlay_origins *o, const char *url, bool has_addresses, const char *local,
			      const char *pub, const char *def);

/* Can a URL be made at address a? Local: a local origin, or a legacy resource (its url as it is). Public: a public
 * https:// origin. */
bool ctd_overlay_has_address(const struct ctd_overlay_origins *o, enum ctd_address a);

/* Which of the overlay's addresses `url` is at (compared on its origin, ignoring case): LOCAL, PUBLIC, or NONE (any
 * other origin, a refused http:// public one, or a legacy resource, whose address isn't known). */
enum ctd_address ctd_overlay_address_of(const struct ctd_overlay_origins *o, const char *url);

/* `url` (the resource's url, one of its scenes' URLs, or a browser source's URL at one of the overlay's origins) at
 * address `want`: the same path, query (scene=, key=, canvas=) and fragment, byte for byte, at want's origin. A
 * legacy resource gives url as it is for LOCAL. A new string (free()), or NULL when want isn't known or url isn't at
 * one of the overlay's origins. */
char *ctd_overlay_url_for(const struct ctd_overlay_origins *o, const char *url, enum ctd_address want);

/* `url` (a browser source's URL at any of the overlay's origins) at the origin of the resource's own url (o->at), so it
 * can be compared with the resource's URLs. A new string (free()), or NULL when url isn't at one of its origins. */
char *ctd_overlay_url_at_default(const struct ctd_overlay_origins *o, const char *url);

enum ctd_switch {
	CTD_SWITCH_NOT_OURS = 0, /* not this overlay's URL at one of its origins: leave it alone */
	CTD_SWITCH_ALREADY,      /* already at the wanted address */
	CTD_SWITCH_UNAVAILABLE,  /* this overlay's URL, but the overlay isn't known at the wanted address */
	CTD_SWITCH_CHANGED,      /* *out is the new URL */
};

/* "Switch to public / local" for one browser source: source_url is rewritten only when its origin is one of the
 * overlay's (the default, local or public one) and its path is the overlay's (overlay_url's: "/o/<slug>"); its query
 * and fragment are kept as they are. *out (free()) is set only for CHANGED. */
enum ctd_switch ctd_overlay_switch_url(const struct ctd_overlay_origins *o, const char *overlay_url,
				       const char *source_url, enum ctd_address want, char **out);

#ifdef __cplusplus
}
#endif
