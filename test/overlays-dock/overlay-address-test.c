/* src/overlay-address.c: an overlay's URL at its local network or public address, the "Switch to public / local"
 * rewrite of existing browser sources, older CTD Overlays without meta.addresses, and the https:// rule for the
 * public address. Only example names (overlay.example.com) and documentation addresses (RFC 5737); "k1" stands for
 * an overlay key. */
#include "overlay-address.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void str_is(const char *what, char *got, const char *want)
{
	if ((got == NULL) != (want == NULL) || (got && strcmp(got, want) != 0)) {
		fprintf(stderr, "FAIL %s: got %s, want %s\n", what, got ? got : "NULL", want ? want : "NULL");
		failures++;
	}
	free(got);
}

static void check(const char *what, bool got, bool want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
		failures++;
	}
}

static void int_is(const char *what, int got, int want)
{
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
		failures++;
	}
}

static void address_is(const char *what, enum ctd_address got, enum ctd_address want)
{
	int_is(what, (int)got, (int)want);
}

/* one switch: its result, and the new URL when it changes */
static void switch_is(const char *what, const struct ctd_overlay_origins *o, const char *overlay_url,
		      const char *source_url, enum ctd_address want, enum ctd_switch result, const char *url)
{
	char *out = (char *)"untouched";
	const enum ctd_switch got = ctd_overlay_switch_url(o, overlay_url, source_url, want, &out);
	int_is(what, (int)got, (int)result);
	if (got != CTD_SWITCH_CHANGED && out) {
		fprintf(stderr, "FAIL %s: *out set without a change\n", what);
		failures++;
		return;
	}
	if (got == CTD_SWITCH_CHANGED)
		str_is(what, out, url);
}

#define LAN "http://192.0.2.10:4850"
#define PUB "https://overlay.example.com"

int main(void)
{
	struct ctd_overlay_origins o;

	/* ---- the origins themselves, and the https:// rule ---- */
	check("origin http", ctd_origin_ok(LAN, false), true);
	check("origin https", ctd_origin_ok(PUB, true), true);
	check("origin trailing slash", ctd_origin_ok(PUB "/", true), true);
	check("origin ipv6", ctd_origin_ok("http://[2001:db8::1]:4850", false), true);
	check("public must be https", ctd_origin_ok("http://overlay.example.com", true), false);
	check("public must be https (lan)", ctd_origin_ok(LAN, true), false);
	check("origin with path", ctd_origin_ok(PUB "/o/lower", false), false);
	check("origin with query", ctd_origin_ok(PUB "?key=k1", false), false);
	check("origin with credentials", ctd_origin_ok("https://u:p@overlay.example.com", false), false);
	check("origin bad port", ctd_origin_ok("http://192.0.2.10:48x0", false), false);
	check("origin empty port", ctd_origin_ok("http://192.0.2.10:", false), false);
	check("origin no host", ctd_origin_ok("https://", false), false);
	check("origin other scheme", ctd_origin_ok("ftp://overlay.example.com", false), false);
	check("origin upper scheme", ctd_origin_ok("HTTPS://overlay.example.com", true), false);
	check("origin space", ctd_origin_ok("https://overlay example.com", false), false);
	check("origin NULL", ctd_origin_ok(NULL, false), false);

	/* ---- building the URL at each address: CTD Overlay with both, default public (SKP_PUBLIC_URL set) ---- */
	const char *land = PUB "/o/lower?key=k1";
	ctd_overlay_origins_read(&o, land, true, LAN, PUB, "public");
	check("both: not legacy", o.legacy, false);
	check("both: local known", ctd_overlay_has_address(&o, CTD_ADDRESS_LOCAL), true);
	check("both: public known", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), true);
	str_is("landscape local", ctd_overlay_url_for(&o, land, CTD_ADDRESS_LOCAL), LAN "/o/lower?key=k1");
	str_is("landscape public", ctd_overlay_url_for(&o, land, CTD_ADDRESS_PUBLIC), PUB "/o/lower?key=k1");
	const char *vert = PUB "/o/lower?key=k1&canvas=vertical";
	str_is("vertical local", ctd_overlay_url_for(&o, vert, CTD_ADDRESS_LOCAL),
	       LAN "/o/lower?key=k1&canvas=vertical");
	str_is("vertical public", ctd_overlay_url_for(&o, vert, CTD_ADDRESS_PUBLIC), vert);
	const char *scene = PUB "/o/lower?key=k1&canvas=vertical&scene=brb";
	str_is("scene local", ctd_overlay_url_for(&o, scene, CTD_ADDRESS_LOCAL),
	       LAN "/o/lower?key=k1&canvas=vertical&scene=brb");
	str_is("scene public", ctd_overlay_url_for(&o, scene, CTD_ADDRESS_PUBLIC), scene);
	str_is("fragment kept", ctd_overlay_url_for(&o, PUB "/o/lower?key=k1#x", CTD_ADDRESS_LOCAL),
	       LAN "/o/lower?key=k1#x");
	str_is("from the local one back", ctd_overlay_url_for(&o, LAN "/o/lower?key=k1", CTD_ADDRESS_PUBLIC), land);
	str_is("not at its origins", ctd_overlay_url_for(&o, "https://198.51.100.7/o/lower?key=k1", CTD_ADDRESS_LOCAL),
	       NULL);
	str_is("NONE", ctd_overlay_url_for(&o, land, CTD_ADDRESS_NONE), NULL);
	address_is("address of local", ctd_overlay_address_of(&o, LAN "/o/lower?key=k1&scene=brb"), CTD_ADDRESS_LOCAL);
	address_is("address of public", ctd_overlay_address_of(&o, land), CTD_ADDRESS_PUBLIC);
	address_is("address of public, other case", ctd_overlay_address_of(&o, "https://Overlay.Example.com/o/lower"),
		   CTD_ADDRESS_PUBLIC);
	address_is("address of other port", ctd_overlay_address_of(&o, "http://192.0.2.10:48500/o/lower"),
		   CTD_ADDRESS_NONE);
	address_is("address of longer host",
		   ctd_overlay_address_of(&o, "https://overlay.example.com.example.net/o/lower"), CTD_ADDRESS_NONE);

	/* a source at either address compared with the resource's own URLs */
	str_is("at default from local", ctd_overlay_url_at_default(&o, LAN "/o/lower?key=k1&scene=brb"),
	       PUB "/o/lower?key=k1&scene=brb");
	str_is("at default from public", ctd_overlay_url_at_default(&o, land), land);
	str_is("at default from elsewhere", ctd_overlay_url_at_default(&o, "http://198.51.100.7:4850/o/lower"), NULL);

	/* default local (CTD Overlay with the LAN address only before, the public one added): same either way */
	const char *lanUrl = LAN "/o/lower?key=k1";
	ctd_overlay_origins_read(&o, lanUrl, true, LAN, PUB, "local");
	str_is("default local: public", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_PUBLIC), PUB "/o/lower?key=k1");
	str_is("default local: local", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_LOCAL), lanUrl);
	/* a trailing '/' on an origin is dropped */
	ctd_overlay_origins_read(&o, lanUrl, true, LAN "/", PUB "/", "local");
	str_is("trailing slash", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_PUBLIC), PUB "/o/lower?key=k1");

	/* ---- the switch: existing browser sources ---- */
	ctd_overlay_origins_read(&o, land, true, LAN, PUB, "public");
	switch_is("local → public", &o, land, LAN "/o/lower?key=k1", CTD_ADDRESS_PUBLIC, CTD_SWITCH_CHANGED,
		  PUB "/o/lower?key=k1");
	switch_is("public → local, scene and canvas kept", &o, vert, PUB "/o/lower?canvas=vertical&key=k1&scene=brb",
		  CTD_ADDRESS_LOCAL, CTD_SWITCH_CHANGED, LAN "/o/lower?canvas=vertical&key=k1&scene=brb");
	switch_is("an older key is kept as it is", &o, land, LAN "/o/lower?key=k0", CTD_ADDRESS_PUBLIC,
		  CTD_SWITCH_CHANGED, PUB "/o/lower?key=k0");
	switch_is("already public", &o, land, land, CTD_ADDRESS_PUBLIC, CTD_SWITCH_ALREADY, NULL);
	switch_is("already local", &o, land, LAN "/o/lower?key=k1", CTD_ADDRESS_LOCAL, CTD_SWITCH_ALREADY, NULL);
	switch_is("already, other case", &o, land, "https://OVERLAY.example.com/o/lower?key=k1", CTD_ADDRESS_PUBLIC,
		  CTD_SWITCH_ALREADY, NULL);
	/* what must not be touched */
	switch_is("another overlay", &o, land, LAN "/o/lower2?key=k1", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS, NULL);
	switch_is("a page under it", &o, land, LAN "/o/lower/x?key=k1", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS, NULL);
	switch_is("CTD Overlay's admin UI", &o, land, LAN "/#/overlays/lower", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS,
		  NULL);
	switch_is("another server, same path", &o, land, "http://198.51.100.7:4850/o/lower?key=k1", CTD_ADDRESS_PUBLIC,
		  CTD_SWITCH_NOT_OURS, NULL);
	switch_is("another port", &o, land, "http://192.0.2.10:4851/o/lower?key=k1", CTD_ADDRESS_PUBLIC,
		  CTD_SWITCH_NOT_OURS, NULL);
	switch_is("a longer host", &o, land, "https://overlay.example.com.example.net/o/lower?key=k1",
		  CTD_ADDRESS_LOCAL, CTD_SWITCH_NOT_OURS, NULL);
	switch_is("credentials", &o, land, "http://u@192.0.2.10:4850/o/lower?key=k1", CTD_ADDRESS_PUBLIC,
		  CTD_SWITCH_NOT_OURS, NULL);
	switch_is("http at the public host", &o, land, "http://overlay.example.com/o/lower?key=k1", CTD_ADDRESS_LOCAL,
		  CTD_SWITCH_NOT_OURS, NULL);
	switch_is("a local file", &o, land, "file:///o/lower", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS, NULL);
	switch_is("empty", &o, land, "", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS, NULL);
	switch_is("NULL", &o, land, NULL, CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS, NULL);
	/* only a local address: Public can't be used */
	ctd_overlay_origins_read(&o, lanUrl, true, LAN, NULL, "local");
	check("local only: public unknown", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	str_is("local only: public url", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_PUBLIC), NULL);
	switch_is("local only: to public", &o, lanUrl, lanUrl, CTD_ADDRESS_PUBLIC, CTD_SWITCH_UNAVAILABLE, NULL);
	switch_is("local only: other overlay", &o, lanUrl, LAN "/o/other", CTD_ADDRESS_PUBLIC, CTD_SWITCH_NOT_OURS,
		  NULL);
	/* only a public address (no LAN one in SKP_HOSTS): Local can't be used */
	ctd_overlay_origins_read(&o, land, true, NULL, PUB, "public");
	check("public only: local unknown", ctd_overlay_has_address(&o, CTD_ADDRESS_LOCAL), false);
	str_is("public only: local url", ctd_overlay_url_for(&o, land, CTD_ADDRESS_LOCAL), NULL);
	switch_is("public only: to local", &o, land, land, CTD_ADDRESS_LOCAL, CTD_SWITCH_UNAVAILABLE, NULL);

	/* ---- missing or old meta ---- */
	ctd_overlay_origins_read(&o, lanUrl, false, NULL, NULL, NULL);
	check("old: legacy", o.legacy, true);
	check("old: local is its url", ctd_overlay_has_address(&o, CTD_ADDRESS_LOCAL), true);
	check("old: no public", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	str_is("old: local url", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_LOCAL), lanUrl);
	str_is("old: scene url", ctd_overlay_url_for(&o, LAN "/o/lower?key=k1&scene=brb", CTD_ADDRESS_LOCAL),
	       LAN "/o/lower?key=k1&scene=brb");
	str_is("old: public url", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_PUBLIC), NULL);
	address_is("old: address unknown", ctd_overlay_address_of(&o, lanUrl), CTD_ADDRESS_NONE);
	switch_is("old: to public", &o, lanUrl, lanUrl, CTD_ADDRESS_PUBLIC, CTD_SWITCH_UNAVAILABLE, NULL);
	switch_is("old: to local", &o, lanUrl, lanUrl, CTD_ADDRESS_LOCAL, CTD_SWITCH_ALREADY, NULL);
	str_is("old: at default", ctd_overlay_url_at_default(&o, lanUrl), lanUrl);
	/* an older CTD Overlay with SKP_PUBLIC_URL: its one url is used as it is, nothing is swapped */
	ctd_overlay_origins_read(&o, land, false, NULL, NULL, NULL);
	str_is("old public: as it is", ctd_overlay_url_for(&o, land, CTD_ADDRESS_LOCAL), land);
	switch_is("old public: not switched", &o, land, LAN "/o/lower?key=k1", CTD_ADDRESS_LOCAL, CTD_SWITCH_NOT_OURS,
		  NULL);
	/* meta.addresses there but empty, or of no use */
	ctd_overlay_origins_read(&o, lanUrl, true, NULL, NULL, NULL);
	check("empty addresses: legacy", o.legacy, true);
	ctd_overlay_origins_read(&o, lanUrl, true, "", "", "local");
	check("empty strings: legacy", o.legacy, true);
	ctd_overlay_origins_read(&o, lanUrl, true, "ftp://192.0.2.10", NULL, "local");
	check("unusable local: legacy", o.legacy, true);
	str_is("unusable local: url as it is", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_LOCAL), lanUrl);
	/* the url isn't at the default the meta names: nothing is swapped */
	ctd_overlay_origins_read(&o, "http://198.51.100.7:4850/o/lower?key=k1", true, LAN, PUB, "local");
	check("url elsewhere: legacy", o.legacy, true);
	check("url elsewhere: no public", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	ctd_overlay_origins_read(&o, lanUrl, true, LAN, PUB, "public");
	check("default public, url local: legacy", o.legacy, true);
	/* no default: whichever the url is at */
	ctd_overlay_origins_read(&o, land, true, LAN, PUB, NULL);
	check("no default: not legacy", o.legacy, false);
	str_is("no default: local", ctd_overlay_url_for(&o, land, CTD_ADDRESS_LOCAL), LAN "/o/lower?key=k1");
	ctd_overlay_origins_read(&o, land, true, LAN, PUB, "lan");
	str_is("unknown default: local", ctd_overlay_url_for(&o, land, CTD_ADDRESS_LOCAL), LAN "/o/lower?key=k1");
	/* no url */
	ctd_overlay_origins_read(&o, NULL, true, LAN, PUB, "public");
	check("no url: local", ctd_overlay_has_address(&o, CTD_ADDRESS_LOCAL), false);
	check("no url: public", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	str_is("NULL origins", ctd_overlay_url_for(NULL, land, CTD_ADDRESS_LOCAL), NULL);

	/* ---- an http:// public address is never used ---- */
	const char *plainPub = "http://overlay.example.com/o/lower?key=k1";
	ctd_overlay_origins_read(&o, lanUrl, true, LAN, "http://overlay.example.com", "local");
	check("http public: refused", o.refused_public, true);
	check("http public: not offered", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	str_is("http public: no url", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_PUBLIC), NULL);
	str_is("http public: local still works", ctd_overlay_url_for(&o, lanUrl, CTD_ADDRESS_LOCAL), lanUrl);
	switch_is("http public: no switch to it", &o, lanUrl, lanUrl, CTD_ADDRESS_PUBLIC, CTD_SWITCH_UNAVAILABLE, NULL);
	/* the resource's url at the refused origin: only the local address is offered, and a source there moves to it */
	ctd_overlay_origins_read(&o, plainPub, true, LAN, "http://overlay.example.com", "public");
	check("url at http public: refused", o.refused_public, true);
	check("url at http public: public not offered", ctd_overlay_has_address(&o, CTD_ADDRESS_PUBLIC), false);
	str_is("url at http public: local", ctd_overlay_url_for(&o, plainPub, CTD_ADDRESS_LOCAL), lanUrl);
	address_is("url at http public: address unknown", ctd_overlay_address_of(&o, plainPub), CTD_ADDRESS_NONE);
	switch_is("url at http public: switched to local", &o, plainPub, plainPub, CTD_ADDRESS_LOCAL,
		  CTD_SWITCH_CHANGED, lanUrl);
	/* and without a local one: nothing can be used, not even the url as it is */
	ctd_overlay_origins_read(&o, plainPub, true, NULL, "http://overlay.example.com", "public");
	check("only http public: not legacy", o.legacy, false);
	check("only http public: no local", ctd_overlay_has_address(&o, CTD_ADDRESS_LOCAL), false);
	str_is("only http public: no local url", ctd_overlay_url_for(&o, plainPub, CTD_ADDRESS_LOCAL), NULL);
	str_is("only http public: no public url", ctd_overlay_url_for(&o, plainPub, CTD_ADDRESS_PUBLIC), NULL);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("overlay-address: all passed\n");
	return 0;
}
