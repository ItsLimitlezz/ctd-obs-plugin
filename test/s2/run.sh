#!/usr/bin/env bash
# Spike S2: build the harness on Linux (gcc, libcurl and FFmpeg dev packages), start test/s2/server.mjs on 127.0.0.1,
# run every test against it, and the avio comparison. Usage: bash test/s2/run.sh [size-MiB, default 1024]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
size_mb="${1:-1024}"
work="${S2_WORK:-$(mktemp -d)}"
port="${S2_PORT:-8787}"
mkdir -p "$work/build" "$work/server" "$work/client"

cflags="-std=c11 -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
gcc $cflags -I"$here/shim" -I"$root/src" "$here/s2-test.c" "$root/src/http-client.c" "$here/shim/shim.c" \
  -o "$work/build/s2-test" $(pkg-config --cflags --libs libcurl libavformat libavutil)
gcc $cflags "$here/avio-put.c" -o "$work/build/avio-put" $(pkg-config --cflags --libs libavformat libavutil)

node "$here/server.mjs" --dir "$work/server" --port "$port" --size-mb "$size_mb" --token test-token \
  > "$work/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true; rm -rf "$work/server" "$work/client"' EXIT
for _ in $(seq 1 600); do grep -q "s2 server on" "$work/server.log" 2>/dev/null && break; sleep 0.5; done
cat "$work/server.log"

base="http://127.0.0.1:$port"
status=0
"$work/build/s2-test" --get "$base/files/big.bin" --put "$base/files/up.bin" --token test-token \
  --dir "$work/client" --debug "$base/debug/partials" || status=$?
echo
echo "== avio comparison 1: PUT with a wrong token (server answers 401 before the body) =="
"$work/build/avio-put" "$base/files/avio.bin" wrong-token 2>&1 | grep -v "^\[" || true
echo "== avio comparison 2: PUT the server reads in full, then answers 500 =="
"$work/build/avio-put" "$base/files/avio.bin?fail=end" test-token 2>&1 | grep -v "^\[" || true
exit $status
