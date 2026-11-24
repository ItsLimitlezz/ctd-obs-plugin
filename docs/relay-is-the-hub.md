# CTD Relay is the hub

Rule (Juan, 2026-11-18): **there is no "CTD Hub". CTD Relay is the hub, always.** Every CTD app pairs with CTD Relay
and talks to it; nothing in the CTD family is named, designed or documented as a separate hub product, dock, menu or
server.

This page says what that changed in the plugin, what still talks to a separate feed server today, and the plan for
that to go away.

## What changed in the plugin

| Before | Now |
|---|---|
| **CTD Hub** dock | **CTD Relay · Feeds** dock, with the sections **Incoming feeds**, **Issues** and **Sync groups**. Opened from **Incoming feeds…** in the CTD Relay dock, from OBS's Docks menu, or from CTD OBS's CTD menu |
| **Tools ▸ CTD Hub servers…** | **Feed servers…** in the CTD Relay dock and the Feeds dock (and in CTD OBS: CTD menu, Settings ▸ CTD ▸ CTD Relay). Stock OBS also keeps a Tools item, **CTD Relay feed servers…** |
| "Allow CTD Hub to change this MediaMTX" | "Allow CTD Relay · Feeds to change this MediaMTX" |
| Log lines `[CTD] hub: …` | `[CTD] feeds: …` |

Nothing was removed: the stream list, Add all live streams and the grids, auto-add, Create path, kick and remove,
the issues panel with its fixes, the sync groups and the sync clip all work as before.

It is a second dock rather than more sections inside the CTD Relay dock because that dock already holds the
connection, the profile, Show Profiles, destinations, overlays and chat; adding a stream table, an issues list and a
sync tree would make it too tall to dock beside the preview. Both docks belong to CTD Relay and are named so.

### What stays the same under the surface

Kept on purpose, so nobody's set-up breaks:

- **Dock id `ctd-hub`.** OBS saves window layouts by dock id, so the Feeds dock comes back where the old dock was.
  CTD OBS's CTD menu also finds the dock by this id. Users never see it.
- **`servers.json`** (the feed server profiles) and **`sync-groups.json`**: unchanged.
- **`hub.json` → `feeds.json`** (auto-add per profile). The first time the Feeds dock reads its settings it copies
  `hub.json` to `feeds.json` and logs it. `hub.json` is left in place, so going back to an older plugin still works.
- **obs-websocket vendor `ctd`, webhook and Relay events** ([api.md](api.md)): no request, event or field was
  renamed. `GetServers`, `GetStreams`, `serverId` and the rest are as they were.
- Locale keys `Hub.*` are now `Feeds.*` (plugin-internal; only en-US exists).

## What still talks to a separate feed server today

"Feed server" = a MediaMTX the cameras publish to. Today the user types its address once, in **Feed servers…**, and
it is saved in `servers.json` with its logins. It may be the same machine as CTD Relay or a different one; the plugin
doesn't know or care, and that is the problem: two addresses to set up, and a second thing that looks like a hub.

| What | Talks to | Protocol | Code |
|---|---|---|---|
| **CTD Live Source / CTD Live Audio Source** (playback) | the feed server | SRT read (`read:<path>[:user:pass]`), optional passphrase | `src/obs/ctd-live-source.c`, `src/engine/` |
| **Feed list, link stats, server version** (one poller per profile, every 2 s) | the feed server's MediaMTX API | HTTP (`/v3/paths/list`, `/v3/srtconns/list`) | `src/api/poller.c`, `src/api/mediamtx-api.c` |
| **Create path, remove path, kick publisher** (only with "Allow … to change this MediaMTX") | the feed server's MediaMTX API | HTTP | `src/ui/feeds-dock.cpp`, `src/obs/issues.c`, `src/obs/ws-vendor.c` |
| **Publish URLs** copied for a camera app (SRT, RTMP, WHIP) | built from the profile's address | none (text) | `src/ui/feeds-dock.cpp`, `GetPublishUrl` |
| **Issues** "camera offline", "path not on server", link loss | read from the poller | none of their own | `src/obs/issues.c` |
| **Webhook** per server profile | a URL the user gives | HTTP POST | `src/obs/webhook.c` |
| **Sync groups, timecode, audio repair, delays** | nothing: worked out from the feeds inside OBS | — | `src/engine/`, `src/obs/sync-groups.c` |

What already goes through CTD Relay and needs no other address: pairing, the profile, where OBS streams to
(`/profiles/{id}/ingest`), destinations and alerts, overlays, chat, the shared timecode clock, Show Profiles, OBS's
own events (`obs.feed.*`, `obs.issue.*`, `obs.sync.*`, outputs, scenes) and the shared WebSocket settings.

So the only things outside CTD Relay are the **incoming** feeds: where they are played from, and where their list and
link stats come from.

## The plan: feeds come from the Relay profile

Hub ROADMAP **Relay F1** ("take in a video feed and relay it to OBS") ↔ **CTD OBS F1** ("take in the video feed from
CTD Relay and add it as a source"). Not built yet; this is the shape, so the two sides are built to meet.

**Goal:** a user who has paired OBS with CTD Relay and picked a profile sees that profile's incoming feeds in the
Feeds dock and can add them to a scene, without typing a second address or a second login.

### Relay side (Relay F1, in ctd-relay)

1. Relay takes camera feeds in for a profile (its own ingest, or a MediaMTX it runs and manages itself).
2. A new scope, e.g. `feeds:read`, and one endpoint per profile, e.g. `GET /profiles/{id}/feeds`, answering for each
   feed: a stable id, its name, whether it is live, publisher kind, tracks, bitrate, link stats (round trip, loss),
   and **how to play it**: an SRT read URL (host, port, stream id) with short-lived read credentials, the same way
   `/profiles/{id}/ingest` hands out the publish side today. Credentials are secrets: never logged, never in events.
3. Feed changes arrive on the events stream the plugin already polls (`feed.live`, `feed.gone`, stats), so no second
   poller is needed.
4. Optional, behind their own scope (`feeds:write`): create a feed slot, remove one, kick a publisher, and the
   publish URL for a camera app. These replace the MediaMTX API writes.

### Plugin side (CTD OBS F1, this repo)

1. A feed server profile gets a **kind**: `mediamtx` (today's) or `relay`. One built-in `relay` profile appears by
   itself while the plugin is paired; it has no address field (it uses the pairing) and can't be removed, only
   hidden.
2. `src/api/poller.c` gets a second back end that fills the same `mtx_path_list` shape from
   `/profiles/{id}/feeds`, so the Feeds dock, the issues and the vendor requests don't change.
3. CTD Live Source's **Server** list offers "CTD Relay (profile name)" first. The source asks the plugin for the
   play URL and credentials when it connects, so an expired credential is renewed on reconnect. The engine itself
   doesn't change (still SRT, still no libobs).
4. Create path / kick / publish URL go to Relay's endpoints for a `relay` profile; the MediaMTX API code stays for
   `mediamtx` profiles.
5. Pairing asks for the new scopes. An older token keeps working without them (the Relay profile just isn't
   offered), like the other scope additions.
6. When the Relay profile is in use and no `mediamtx` profile exists, **Feed servers…** is hidden behind an
   "Other feed servers (advanced)…" item. Manual MediaMTX profiles stay supported for people who run their own
   server without Relay: playback must never depend on Relay being up for them.

### Rules that carry over

- Playback never depends on an API: if Relay's API is down, sources already playing keep playing and reconnect with
  the last play URL they were given.
- No address, key or login of a real server in the repo, a PR, an issue or a log.
- Existing scenes keep working: a source saved with a `mediamtx` profile is never moved to Relay by itself.

### Order of work

1. Relay F1: ingest + `GET /profiles/{id}/feeds` + events (Relay repo; contract posted on the hub's
   AGENT-REQUESTS.md first).
2. Plugin: profile kind + Relay poller back end + source play-URL lookup (this repo, `main` and `ctd-obs-in-tree`).
3. Plugin: writes through Relay, hide the manual server list by default.
4. CTD OBS first-run: pairing alone is enough to see feeds; drop "Feed Servers…" from the top of the CTD menu.

Until step 2 ships, **Feed servers…** stays, as a part of CTD Relay's set-up rather than as a hub of its own.
