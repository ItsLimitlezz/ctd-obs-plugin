# Show Profiles: Spike S2, binary file transfer

**Question:** how does the plugin stream files of up to 2 GB to and from CTD Relay?
**Options:** FFmpeg avio (what `src/http-client.c` uses for JSON today) with a chunked POST/PUT, or libcurl.
**Decision: libcurl.** It is already part of every OBS install the plugin targets, and avio can't report the
server's answer to an upload (evidence below).

Branch `sr/s2-binary-http`. Prototype only: nothing in the plugin calls the new functions yet.

## What is linkable from this plugin's build

| Platform | FFmpeg (avio) | libcurl |
| --- | --- | --- |
| Windows x64 | obs-deps `2026-07-15`: FFmpeg 8 (`avformat-62.dll`), already linked | obs-deps `2026-07-15`: **libcurl 8.12.1**, `bin/libcurl.dll` + `lib/libcurl_imp.lib` + CMake package `CURL::libcurl` (`lib/cmake/CURL`). Built with Schannel/SSPI (its `CURLConfig.cmake` doesn't depend on OpenSSL), threadsafe, http/https. OBS installs this same DLL next to `obs64.exe`. |
| macOS | obs-deps, already linked | **Not in obs-deps** (checked the `2026-07-15` universal tarball). `find_package(CURL)` finds the SDK's `libcurl` (`/usr/lib/libcurl.4.dylib`), which is part of macOS, so there is nothing to ship. |
| Ubuntu 24.04 | system FFmpeg, already linked | system `libcurl4t64` (OBS from the PPA already depends on it); build needs `libcurl4-openssl-dev`. |

Verified here: downloaded the buildspec's obs-deps archives (SHA-256 matches `buildspec.json`) and listed them. The OBS
32.2.2 source archive was blocked from this sandbox, so "OBS ships the same `libcurl.dll`" is from OBS's packaging, not
re-checked here; **PC to confirm** (see the end).

Build changes on this branch: `CMakeLists.txt` adds `find_package(CURL REQUIRED)` + `CURL::libcurl`; the Ubuntu CI
setup script and README add `libcurl4-openssl-dev`. Nothing is bundled, same as FFmpeg (REQUIREMENTS F5).

## Why not avio

FFmpeg's `http` protocol in write mode never reads the server's reply. `http_connect()` "pretends that it did work"
(sets `http_code = 200`) as soon as the request headers of a POST/PUT are sent, and `http_shutdown()` only drains the
socket without parsing it. So for an upload the caller gets no HTTP status at all. `test/s2/avio-put.c` shows it
(FFmpeg 6.1 here; the code path is the same in FFmpeg 7 and 8):

| Server's answer | What avio returns |
| --- | --- |
| 401 before the body (wrong token) | `avio_closep: -32 (Broken pipe)` and only because the server closed the socket: no 401 |
| reads all 8 MiB, then **500** | `avio_open2: 0`, `io->error: 0`, `avio_closep: 0`, i.e. **success** |

A 2 GB upload that the server rejected for a full disk, a checksum mismatch or a quota would look like it worked.
Other avio gaps, each a workaround at best: uploads are chunked only (no `Content-Length`, so Relay can't refuse an
oversized file up front); no `Expect: 100-continue` (a refused upload still sends its body); no upload progress (only
bytes we hand it); no response headers on a write (no server SHA-256 to compare).

libcurl gives all of these directly, plus `CURLOPT_FAILONERROR` (an error body never lands in the file), stall
detection, `CURLOPT_PROTOCOLS_STR "http,https"` and no redirects. The JSON path stays on avio: it works, and S2 doesn't
need to move it. (Moving it later would let the plugin drop the FFmpeg-version TLS quirks in `status_from_error`.)

## API (`src/http-client.h`)

```c
typedef void (*http_progress_cb)(void *opaque, uint64_t done, uint64_t total);

enum http_status http_get_file(const char *url, const char *bearer, const char *path, char sha256_out[65],
                               int (*interrupt)(void *), http_progress_cb progress, void *opaque);
enum http_status http_put_file(const char *url, const char *bearer, const char *path, char sha256_out[65],
                               int (*interrupt)(void *), http_progress_cb progress, void *opaque);
```

The ticket's six arguments, plus `opaque` (passed to both callbacks, as in `struct http_call`). New statuses
`HTTP_ABORTED` (interrupt said stop) and `HTTP_LOCAL_FILE` (local read/write/rename failed); the rest map as for JSON
calls (401 → `HTTP_UNAUTHORIZED`, 5xx → `HTTP_SERVER_ERROR`, cut connection or stall → `HTTP_UNREACHABLE`, TLS →
`HTTP_TLS_FAILED`, redirect or SHA-256 mismatch → `HTTP_BAD_RESPONSE`).

- **Streaming:** one 64 KiB curl buffer each way; sizes are 64-bit (`CURLOPT_INFILESIZE_LARGE`, `os_fseeki64`).
- **SHA-256 while streaming:** libavutil's `av_sha` (already linked), updated in the write/read callbacks;
  `sha256_out` gets lowercase hex, only on `HTTP_OK`.
- **Server checksum (proposal for Relay):** if a GET response, or the reply to a PUT, carries
  `X-Content-SHA256: <hex>`, it must equal the streamed hash or the call fails with `HTTP_BAD_RESPONSE`.
- **GET: temp file + atomic rename:** writes `<path>.<unique>.part` in the target's directory, `fflush` + `fsync`
  (`_commit` on Windows), then `os_rename` (libobs: `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` on Windows,
  `rename()` elsewhere). On any error, abort or checksum mismatch the `.part` is deleted and `path` is untouched.
  A crash mid-transfer can leave a `.part`: the profile store should sweep `*.part` in its directory on start.
- **PUT:** `Content-Length` (no chunked), `Expect: 100-continue` so Relay can refuse (401/403/413) before any body;
  `HTTP_OK` only for a 2xx after the whole body. Fails `HTTP_LOCAL_FILE` if the file changed size during the upload.
- **Abort:** `interrupt` is polled from curl's progress callback (at least once a second, and per buffer) →
  `HTTP_ABORTED` within one buffer.
- **Progress:** `(opaque, done, total)` on the calling thread, bytes on the wire; `total` is the file size for a PUT,
  `Content-Length` for a GET (0 if unknown); a final call with `done == total` on success.
- **Safety:** no redirects (the token would follow), http/https only, certificate + host checked, the
  `Authorization` header buffer is wiped after the call. The plugin must call `curl_global_init()` once in
  `obs_module_load` (and `curl_global_cleanup()` in unload) before these are used for real.

Design notes for the Show Profiles implementation: don't download over a file OBS has open (a media source playing
it): on Windows the rename fails (`HTTP_LOCAL_FILE`). Store downloads under content-addressed names (the SHA-256) and
switch references instead.

### What Relay's file endpoints need (to match)

`GET` with `Content-Length` and `X-Content-SHA256`; `PUT` that checks auth on `Expect: 100-continue`, needs
`Content-Length`, writes to a temp file, verifies the length, fsyncs and renames, deletes the temp file on an aborted or
short body, and answers 2xx with `X-Content-SHA256` only after the rename. `test/s2/server.mjs` is a working reference
(about 150 lines of Node, no dependencies).

## Tests

`test/s2/`:

| File | What |
| --- | --- |
| `s2-test.c` | Harness: compiles the real `src/http-client.c` unmodified; links only libcurl + libavutil (+ libavformat for the file's JSON half) |
| `shim/` | Stand-ins for the five libobs headers `http-client.c` includes, implemented on libc (test-only) |
| `server.mjs` | One-file Node server: generates a 1 GiB file, GET/PUT with SHA-256, fault injection, `/debug/partials` |
| `avio-put.c` | The avio evidence above |
| `run.sh` | Linux/cloud: build with gcc + pkg-config, start the server on `127.0.0.1`, run everything |
| `CMakeLists.txt`, `run-s2.ps1` | PC (Windows): build with the plugin's obs-deps libcurl/FFmpeg; run against the local server, or a dev Relay with `-GetUrl/-PutUrl/-Token` |

| Test | Checks |
| --- | --- |
| T1 GET | `HTTP_OK`; streamed SHA-256 = SHA-256 of the file on disk; progress called, monotonic, ends at size/size; no `.part` |
| T2 PUT | `HTTP_OK` (server's `X-Content-SHA256` matched); same SHA-256 as T1; progress as T1 |
| T3 round trip | GET the uploaded copy back: SHA-256 and size equal the original |
| T4 abort GET at 50% | `HTTP_ABORTED`; the existing target file is untouched; no `.part`; no SHA-256 |
| T5 abort PUT at 50% | `HTTP_ABORTED`; GET of that name → 404; the server has no temp file left |
| T6 connection cut halfway (GET) | `HTTP_UNREACHABLE`; no file, no `.part` |
| T7 wrong `X-Content-SHA256` | `HTTP_BAD_RESPONSE`; no file, no `.part` |
| T10 500 after the whole PUT body | `HTTP_SERVER_ERROR`; no SHA-256 (what avio reports as success) |
| T8 wrong token, PUT | `HTTP_UNAUTHORIZED` with 0 body bytes sent |
| T9 wrong token, GET | `HTTP_UNAUTHORIZED`; no file |
| Memory | peak RSS < 64 MiB |

T6, T7, T10 and the server temp-file check need `server.mjs` (`--debug`); against Relay the rest run.

### Results (cloud: Ubuntu 24.04, gcc 13, libcurl 8.5.0/OpenSSL, FFmpeg 6.1, Node 22, loopback)

`bash test/s2/run.sh 1024`: **all passed (0 failed)**, final code:

| Test | Result |
| --- | --- |
| T1 GET 1 GiB | OK, 6.1 s (167 MiB/s); streamed SHA-256 `6ce2f84b…94a89a` = on-disk hash; 337 progress calls, monotonic, last 1073741824/1073741824; no `.part` |
| T2 PUT 1 GiB | OK, 5.8 s (177 MiB/s); server's `X-Content-SHA256` matched; same hash; 32785 progress calls, last = size/size |
| T3 round trip | OK; GET → PUT → GET hash and size equal the original |
| T4 abort GET at 50% | `HTTP_ABORTED` at 539284299 of 1073741824 bytes; old target untouched; no `.part` |
| T5 abort PUT at 50% | `HTTP_ABORTED` at 536870912 of 1073741824; GET → 404; server `{"partials":[]}` |
| T6 cut connection | `HTTP_UNREACHABLE` after 535822336 bytes; no file, no `.part` |
| T7 wrong server hash | `HTTP_BAD_RESPONSE`; no file, no `.part` |
| T10 500 after body | `HTTP_SERVER_ERROR` after all 1073741824 bytes |
| T8 / T9 wrong token | `HTTP_UNAUTHORIZED`; PUT sent 0 body bytes; GET left no file |
| Memory | peak RSS 29.3 MiB for the 1 GiB file |

`bash test/s2/run.sh 2200` (2.2 GiB, past the 2^31 boundary): **all passed**: GET 16.2 s (136 MiB/s), PUT 12.6 s
(174 MiB/s), aborts at 1158216794 / 1153433600 of 2306867200 bytes, peak RSS 29.4 MiB. (Run on the build just before
upload progress switched from bytes-read to curl's bytes-sent counter; the 1 GiB run above is on the final code.)

Note for the UI: curl calls progress every 32 KiB on uploads (32785 calls per GiB). The dock should throttle what it
posts to Qt (e.g. at most every 250 ms).

The plugin itself was **not** built here (no OBS/obs-deps toolchain in the cloud; AGENTS.md), hence `needs-build:pc`.

## What the PC must verify

1. `cmake --preset windows-x64` finds `CURL::libcurl` in `.deps/obs-deps-2026-07-15-x64` and the plugin builds and
   links (`cmake --build --preset windows-x64`), with no new warnings.
2. The OBS 32.2.2 install has `bin/64bit/libcurl.dll`, and the plugin loads in OBS (no "module not found").
3. `pwsh test/s2/run-s2.ps1` passes against the local server (Windows: `MoveFileExW` rename, `_commit`,
   `_fseeki64` past 2 GiB, Schannel build). Try `-SizeMb 2200` once for the 2 GiB boundary.
4. Once Relay has file endpoints: `run-s2.ps1 -GetUrl ... -PutUrl ... -Token ...` against the dev Relay
   (`10.20.30.40`-style LAN address), over http and, if the dev Relay runs TLS, https (Schannel checks revocation:
   note whether a LAN certificate passes).
5. Mac (`needs-build:mac`, later): the plugin links the SDK's libcurl; `cmake -S test/s2 -B build-s2
   -DCMAKE_PREFIX_PATH=<.deps/obs-deps-...-universal>` and run `s2-test` the same way.
