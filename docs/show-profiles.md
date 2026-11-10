# Show Profiles in the plugin (M2: local-first, M3: sync with CTD Relay, M4: device mapping)

Design: hub `epics/show-profiles-design.md` (sections 1, 2, 5, 6, 7, 8). This page is the code map and the interfaces
M3 (sync client, T11) and M4 (fix-mapping dialog, T13) build on.

## Code map (`src/show/`)

| File | What | libobs |
| --- | --- | --- |
| `json.c/h` | `cj`: JSON model that keeps member order and number text (bundles stay byte-stable) | no |
| `show-fs.c/h` | UTF-8 file helpers (wide APIs on Windows), atomic writes | no |
| `sha256.c/h` | SHA-256 of buffers and files | no |
| `manifest.c/h` | Schema 1 validation, path/name/uuid/sha checks, `${ctd-asset:…}` placeholders | no |
| `bundle.c/h` | In-memory bundle: manifest + collection + profile files + this machine's `local` half | no |
| `scrub.c/h` | T10 secret scrubber and the apply-side restore | no |
| `assets.c/h` | T9 asset scanner/rewriter (paths ↔ `${ctd-asset:<sha256>}`) | no |
| `ini.c/h` | T9 `basic.ini` allow-list and merge | no |
| `export.c/h` | T9 current setup → bundle | no |
| `store.c/h` | T7 local store: versioned bundles, checksums, asset cache, pruning, device maps | no |
| `apply.c` | T7 guard, staged apply → swap → verify → rollback | yes |
| `show.c/h` | The API for the dock, M3 and M4; state, worker, hooks | yes |
| `src/ui/show-section.cpp` | T12 dock section in CTD Relay (+ M3: Save to CTD Relay, downloads, conflict, progress) | Qt |
| `tar.c/h` | M3: the bundle as Relay stores it: a deterministic ustar of collection.json + profile files | no |
| `sync-core.c/h` | M3: sync status, request bodies, conflict plans, the offline queue (`show-sync.json`) | no |
| `sync-flow.c/h` | M3: save up / pull / Keep mine · Take theirs · Save as new, against a table of Relay operations | no |
| `sync.c/h` | M3: the Relay operations over HTTP (JSON + S2 libcurl transfers), jobs, automatic pulls and retries | yes |
| `devices.c/h` | M4: device scoring (design section 5), greedy mapping, remembered map, aliases | no |
| `device-map.c/h` | M4: lists this machine's devices, maps on switch (stage hook), remembers, dialog API | yes |
| `src/ui/device-map-dialog.cpp` | T13 "Fix mapping" dialog | Qt |

Everything marked "no" is tested by `ctd-show-unit` (`test/show/`), run in CI by `.github/workflows/show-tests.yaml`
on Ubuntu, Windows and macOS.

## On disk

```
<plugin config>/show-profiles/<id>/meta.json
<plugin config>/show-profiles/<id>/revisions/<seq>/{manifest.json, collection.json, profile/…, checksums.json}
<plugin config>/show-profiles/<id>/local.json                (this machine's secrets + stream service; never uploaded)
<plugin config>/show-profiles/<id>/device-map.<machine>.json (M4)
<plugin config>/show-profiles/device-aliases.<machine>.json  (M4: a captured device -> this machine's device, all profiles)
<plugin config>/show-sync.json                              (M3: save-ups waiting for CTD Relay)
<plugin config>/sync-tmp/                                   (M3: a bundle tar being sent or received)
<plugin config>/assets/<ab>/<sha256>.<ext>                   (content-addressed cache)
<plugin config>/staging/                                     (a switch in progress)
basic/scenes/ctd-show-<id>.json + basic/profiles/ctd-show-<id>/   (named "CTD · <name>" in OBS)
```

`seq` is a local sequence; `manifest.revision` is Relay's number (0 until Relay accepts it) and
`manifest.parentRevision` the Relay revision a save was based on.

## Core procs (CTD OBS, C1)

The plugin calls these on the global proc handler and turns Show Profiles on only when `ctd_show_refresh` answers
(checked at `OBS_FRONTEND_EVENT_FINISHED_LOADING`):

```
void ctd_show_apply(in string collection_path, in string profile_path, out bool success, out string error)
void ctd_show_refresh()
```

`collection_path` is the `.json` file, `profile_path` the profile folder. A core that sets no `success` is judged by
the plugin's verify step.

## For M3 (sync client, T11)

- Save up: `show_store_load(id, seq, &bundle, …)` gives the verified bundle; `show_store_revision_dir()` is the folder to
  tar; `manifest.assets[]` + `show_store_asset_path()` are the blobs to `POST /blobs/check` and `PUT`. After Relay
  answers, `show_store_set_relay_revision(id, seq, revision)` (clears "Not backed up" for the head).
- Pull: build a `struct show_bundle` from Relay's bundle, download blobs into the cache with
  `show_store_add_asset()` (or straight to `show_store_asset_path()` + `.part` + rename), then
  `show_store_save(id, &bundle, SHOW_SAVE_RELAY, &seq, …)`. A new profile first: `show_store_create(id, name, …)`.
- Apply-time Relay data: `show_add_stage_hook(fn, opaque)`. The hook gets `struct show_stage_ctx` on the worker thread
  before anything is written: fill the ingest key into `ctx->service`, and point overlay sources (manifest
  `overlays[]`, URL currently `${ctd-overlay}` or this machine's last value) from `GET /profiles/{id}/overlays`. Return
  false to abort the switch; OBS stays as it was.
- Secrets from Relay (`GET …/secrets`, `show:secrets`): write them into the profile's `local.json` `secrets` with the
  same `"<uuid>|<pointer>"` keys as `manifest.secrets[]` (`show_store_save` merges `bundle.local`).
- State shown in the dock comes from `meta.json` (`dirty`, `baseRevision`, `relayProfileId`).

## For M4 (fix-mapping dialog, T13)

- `manifest.devices[]` (from export): `{slot (source uuid), sourceType, class, identity{name,id,vidPid,serial}, label}`.
- Map at apply time with a stage hook: rewrite device settings in `ctx->collection` (find the source with
  `show_find_by_uuid`), using `show_store_device_map(id, ctx->machine)`; append to `ctx->warnings`.
- After the switch: `show_add_applied_hook(fn, opaque)` runs on the UI thread with `(id, seq, manifest)`: open the
  dialog when a slot scored below 70. Save confirmed choices with `show_store_write_device_map(id, machine, map)`;
  `show_machine_name()` gives `machine`. Changing a device is not a switch and is allowed while live.

## M3: sync with CTD Relay (T11)

Relay API v1.9 (ctd-relay `docs/API.md` section 14). The flows are pure C (`sync-flow.c`) and run in `ctd-show-unit`
against an in-memory Relay; `sync.c` plugs in HTTP.

- **Pairing** asks for `show:read`, `show:write` and `show:secrets` too. A machine paired before this needs to pair
  again (scopes can't be added to a token); until then a sync says so in the dock.
- **Save to CTD Relay** (never automatic). For the active profile the current setup is saved locally first (Update).
  Then the newest local revision: tar it (`tar.c`, deterministic, so the same revision has the same `bundleSha256`),
  `POST /blobs/check` (bundle + every `manifest.assets[]`), `PUT /blobs/{sha}?kind=` for what's missing (S2
  `http_put_file`), `POST /profiles/{id}/show/revisions` with `parentRevision = baseRevision` and the values of
  `manifest.secrets[]` from `local.json` in `secrets[]`. A profile not linked yet is linked to the Relay profile chosen
  in the dock (`show_store_set_relay_link`).
- **Conflict.** FFmpeg's HTTP doesn't return the body of a 4xx, so a refused save asks `GET /profiles/{id}/show`: a head
  other than the parent is a conflict (409), otherwise a limit (413). The dock asks:
  - *Keep mine*: saved again with `parentRevision = head`, so theirs stays in Relay's history.
  - *Take theirs*: Relay's head is downloaded into this profile; yours stays as an older local revision.
  - *Save as new profile*: yours is copied into a new local-only profile "<name> (mine)", then this one takes theirs.
  - *Decide later*: nothing changes; it stays "Not backed up".
- **Offline** (unreachable, 5xx, 429): local-first, nothing changes; the save-up is queued in `show-sync.json` and
  retried with back-off (15 s, 30 s … 10 min) while the dock is connected.
- **Pull.** Relay profiles with a show profile that isn't on this machine are listed under "On CTD Relay": Download, or
  Download and switch. Every 2 s the client compares `GET /profiles` items' `show.headRevision` (refetched on
  `show.changed`, now in the event types) with each local copy. A copy that is behind and has nothing unsaved is
  downloaded automatically (bundle, its sha256 checked, then the blobs it lacks, then `…/secrets` when the token has
  `show:secrets`) and saved with `SHOW_SAVE_RELAY`. Nothing is applied to OBS: the active profile shows "Update ready"
  and "Update now" switches to it (refused while live). A copy that is behind *and* has unsaved changes shows "Changed on
  Relay" and goes through the conflict choice on the next save.
- **Progress**: step + bytes in the dock while a transfer runs.

## M4: device mapping (T11 scoring + T13 dialog)

- Scoring, assignment and the remembered map are in `devices.c` (design section 5): remembered and present always
  wins; then id 100 · alias 98 · VID:PID + serial 95 · VID:PID only one here 80 · name 70 · normalized name 60 ·
  brand 30, greedy, one device per slot unless the bundle shared it. ≥ 70 auto, 30–69 suggested, < 30 missing (the
  source keeps its settings). Window/game capture are never mapped.
- On a switch, the stage hook lists this machine's devices (the source types' device list properties, on the UI
  thread), maps the bundle's `devices[]`, and writes the chosen device ids into the staged collection.
- After the switch, auto and remembered slots are saved into `device-map.<machine>.json`, and the dialog opens when a
  slot is suggested, missing, or remembered but unplugged. "Devices…" in the dock opens it at any time.
- A choice in the dialog applies to the source at once (allowed while live), is saved as confirmed in the profile's map
  and in the machine's alias table, so the same camera also maps in other profiles.
