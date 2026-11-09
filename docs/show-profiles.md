# Show Profiles in the plugin (M2: local-first)

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
| `src/ui/show-section.cpp` | T12 dock section in CTD Relay | Qt |

Everything marked "no" is tested by `ctd-show-unit` (`test/show/`), run in CI by `.github/workflows/show-tests.yaml`
on Ubuntu, Windows and macOS.

## On disk

```
<plugin config>/show-profiles/<id>/meta.json
<plugin config>/show-profiles/<id>/revisions/<seq>/{manifest.json, collection.json, profile/…, checksums.json}
<plugin config>/show-profiles/<id>/local.json                (this machine's secrets + stream service; never uploaded)
<plugin config>/show-profiles/<id>/device-map.<machine>.json (M4)
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
