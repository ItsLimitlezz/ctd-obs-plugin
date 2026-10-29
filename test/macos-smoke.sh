#!/bin/bash
# Smoke test on a Mac (CI's macos-26 runner): installs the built plugin into a real OBS, plays it a test feed over SRT,
# drops and restores the feed, quits OBS, and checks the log. Leaves screenshots and logs in test/out/macos.
#   macos-smoke.sh <the build's .pkg, .tar.xz or .zip>
#   OBS_VERSION      32.2.2 (the version buildspec.json builds against)
#   MEDIAMTX_VERSION v1.21.3
set -euo pipefail

package="$1"
obs_version="${OBS_VERSION:-32.2.2}"
mediamtx_version="${MEDIAMTX_VERSION:-v1.21.3}"
here="$(cd "$(dirname "$0")" && pwd)"
out="${here}/out/macos"
work="$(mktemp -d)"
config="${HOME}/Library/Application Support/obs-studio"
mkdir -p "${out}"

failures=0
fail() { echo "::error::$*"; echo "- ❌ $*" >> "${out}/summary.md"; failures=$((failures + 1)); }
pass() { echo "$*"; echo "- ✅ $*" >> "${out}/summary.md"; }
note() { echo "::warning::$*"; echo "- ⚠️ $*" >> "${out}/summary.md"; }
log_file() { ls -t "${config}/logs/"*.txt 2> /dev/null | head -n 1 || true; }
# waits up to $2 seconds for a line matching $1 in OBS's log
wait_for() {
  local pattern="$1" seconds="$2"
  for ((i = 0; i < seconds; i++)); do
    local log
    log="$(log_file)"
    if [[ -n "${log}" ]] && grep -E -q -- "${pattern}" "${log}"; then return 0; fi
    sleep 1
  done
  return 1
}
shot() { screencapture -x "${out}/$1.png" 2> /dev/null || note "couldn't take the $1 screenshot"; }

echo "## CTD Live on macOS ($(sw_vers -productVersion), $(uname -m)), OBS ${obs_version}" > "${out}/summary.md"

echo '::group::OBS, MediaMTX and FFmpeg'
arch="$(uname -m)"
dmg_arch=Apple
mtx_arch=arm64
if [[ "${arch}" == x86_64 ]]; then dmg_arch=Intel; mtx_arch=amd64; fi
curl -fsSL -o "${work}/obs.dmg" \
  "https://github.com/obsproject/obs-studio/releases/download/${obs_version}/OBS-Studio-${obs_version}-macOS-${dmg_arch}.dmg"
hdiutil attach -nobrowse -quiet -mountpoint "${work}/obs-dmg" "${work}/obs.dmg"
cp -R "${work}/obs-dmg/OBS.app" "${work}/OBS.app"
hdiutil detach -quiet "${work}/obs-dmg"
curl -fsSL "https://github.com/bluenviron/mediamtx/releases/download/${mediamtx_version}/mediamtx_${mediamtx_version}_darwin_${mtx_arch}.tar.gz" \
  | tar -xz -C "${work}" mediamtx
command -v ffmpeg > /dev/null || brew install --quiet ffmpeg
echo '::endgroup::'

echo '::group::Install the plugin'
mkdir -p "${config}/plugins"
case "${package}" in
  *.pkg)
    # the package installs for the current user only (distribution.in), as a person double-clicking it would
    installer -pkg "${package}" -target CurrentUserHomeDirectory
    ;;
  *.tar.xz | *.zip)
    mkdir -p "${work}/unpacked"
    if [[ "${package}" == *.zip ]]; then ditto -x -k "${package}" "${work}/unpacked"; else tar -xJf "${package}" -C "${work}/unpacked"; fi
    cp -R "$(find "${work}/unpacked" -name '*.plugin' -maxdepth 3 | head -n 1)" "${config}/plugins/"
    ;;
  *) echo "don't know how to install ${package}"; exit 2 ;;
esac
bundle="$(find "${config}/plugins" -name '*.plugin' -maxdepth 1 | head -n 1)"
if [[ -z "${bundle}" ]]; then fail "the package didn't install a .plugin into ${config}/plugins"; exit 1; fi
pass "installed $(basename "${bundle}")"
binary="${bundle}/Contents/MacOS/$(basename "${bundle}" .plugin)"
file "${binary}" | tee "${out}/plugin-file.txt"
otool -L "${binary}" | tee "${out}/plugin-libraries.txt"
lipo -archs "${binary}" | grep -q arm64 && lipo -archs "${binary}" | grep -q x86_64 \
  && pass "universal binary ($(lipo -archs "${binary}"))" || fail "not universal: $(lipo -archs "${binary}")"
codesign -dv "${bundle}" > "${out}/plugin-codesign.txt" 2>&1 || true
echo '::endgroup::'

echo '::group::Test feed'
# the plugin always reads over SRT; the publisher uses RTSP if this FFmpeg has no SRT (the runner's doesn't)
publish_url='srt://127.0.0.1:8890?streamid=publish:cam1&pkt_size=1316'
publish_format=mpegts
sed -e 's/^rtsp: false/rtsp: true/' "${here}/mediamtx.yml" > "${work}/mediamtx.yml"
echo 'rtspTransports: [tcp]' >> "${work}/mediamtx.yml"
if ! ffmpeg -hide_banner -protocols 2> /dev/null | grep -q -x ' *srt'; then
  publish_url='rtsp://127.0.0.1:8554/cam1'
  publish_format=rtsp
  note "this FFmpeg ($(ffmpeg -version | head -n 1 | cut -d ' ' -f 3)) can't send SRT: the test feed is published over RTSP"
fi
(cd "${work}" && exec ./mediamtx mediamtx.yml) > "${out}/mediamtx.log" 2>&1 &
mediamtx_pid=$!
sleep 2
publish() {
  PUBLISH_URL="${publish_url}" PUBLISH_FORMAT="${publish_format}" sh "${here}/publish.sh" \
    >> "${out}/publisher.log" 2>&1 &
  publisher_pid=$!
}
publish
echo '::endgroup::'

echo '::group::OBS settings: no first-run wizard or dialogs, one scene with a CTD Live source'
mkdir -p "${config}/basic/scenes" "${config}/basic/profiles/CTD_Smoke"
# FirstRun=true means "has run before" to OBS; LastVersion (32.2.2 as libobs packs it) skips What's New
for ini in global.ini user.ini; do
  cat > "${config}/${ini}" << 'EOF'
[General]
FirstRun=true
LastVersion=537001986
LicenseAccepted=true
MacOSPermissionsDialogLastShown=99
InfoIncrement=999
[Basic]
Profile=CTD Smoke
ProfileDir=CTD_Smoke
SceneCollection=CTD Smoke
SceneCollectionFile=CTD_Smoke
EOF
done
cat > "${config}/basic/profiles/CTD_Smoke/basic.ini" << 'EOF'
[General]
Name=CTD Smoke
[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSCommon=30
EOF
cat > "${config}/basic/scenes/CTD_Smoke.json" << 'EOF'
{
  "name": "CTD Smoke",
  "current_scene": "Scene",
  "current_program_scene": "Scene",
  "scene_order": [{ "name": "Scene" }],
  "sources": [
    {
      "id": "scene", "versioned_id": "scene", "name": "Scene", "uuid": "5f1a2b8e-0000-4000-8000-000000000001",
      "settings": {
        "id_counter": 1,
        "items": [{
          "name": "CTD Test", "source_uuid": "5f1a2b8e-0000-4000-8000-000000000002", "id": 1, "visible": true,
          "pos": { "x": 0.0, "y": 0.0 }, "scale": { "x": 1.0, "y": 1.0 }, "align": 5,
          "bounds_type": 2, "bounds_align": 0, "bounds": { "x": 1280.0, "y": 720.0 }
        }]
      }
    },
    {
      "id": "ctd_live_source", "versioned_id": "ctd_live_source", "name": "CTD Test",
      "uuid": "5f1a2b8e-0000-4000-8000-000000000002",
      "settings": { "host": "127.0.0.1", "port": 8890, "path": "cam1" }
    }
  ]
}
EOF
echo '::endgroup::'

echo '::group::Run OBS'
"${work}/OBS.app/Contents/MacOS/OBS" --disable-updater --disable-shutdown-check --multi > "${out}/obs-stdout.log" 2>&1 &
obs_pid=$!

if wait_for '\[ctd-live\] plugin loaded' 60; then
  pass "plugin loaded: $(grep -h -o '\[ctd-live\] plugin loaded.*' "$(log_file)" | head -n 1)"
else
  fail "the plugin didn't load"
  grep -i -E 'ctd-live|dlopen|failed to load' "$(log_file)" 2> /dev/null | head -n 20 || true
fi
if wait_for '\[ctd-live:CTD Test\] connected:' 60; then
  line="$(grep -h -o 'connected: video=.*' "$(log_file)" | head -n 1)"
  pass "the source connected: ${line}"
  [[ "${line}" == *videotoolbox* ]] || note "video isn't on VideoToolbox: ${line}"
else
  fail "the source didn't connect to the test feed"
fi
sleep 8
shot live
grep -q 'decoding the video on the CPU' "$(log_file)" \
  && note "VideoToolbox fell back to the CPU: $(grep -h -o '\] .*decoding the video on the CPU' "$(log_file)" | head -n 1)"

kill "${publisher_pid}" 2> /dev/null || true
if wait_for '\[ctd-live:CTD Test\] no signal for' 30; then
  pass "feed dropped: $(grep -h -o 'no signal for.*' "$(log_file)" | head -n 1)"
else
  fail "the source didn't notice the feed dropping"
fi
sleep 3
shot loss-card
publish
if wait_for '\[ctd-live:CTD Test\] back live after' 45; then
  pass "feed restored: $(grep -h -o 'back live after.*' "$(log_file)" | head -n 1)"
else
  fail "the source didn't come back after the feed returned"
fi
sleep 3
shot back-live

osascript -e 'tell application id "com.obsproject.obs-studio" to quit' 2> /dev/null || kill "${obs_pid}" 2> /dev/null || true
for ((i = 0; i < 30; i++)); do kill -0 "${obs_pid}" 2> /dev/null || break; sleep 1; done
if kill -0 "${obs_pid}" 2> /dev/null; then
  fail "OBS didn't quit within 30 s"
  kill -9 "${obs_pid}" 2> /dev/null || true
fi
wait "${obs_pid}" 2> /dev/null && status=0 || status=$?
echo '::endgroup::'

log="$(log_file)"
cp "${log}" "${out}/obs-log.txt"
if grep -q 'Number of memory leaks' "${log}"; then
  pass "OBS quit cleanly ($(grep -h -o 'Number of memory leaks: [0-9]*' "${log}" | tail -n 1))"
else
  fail "OBS didn't shut down cleanly (exit ${status})"
fi
if grep -E -q '\[ctd-live[^]]*\].*(crash|assert)' "${log}"; then fail "the log mentions a crash or an assert"; fi
crashes="$(find "${HOME}/Library/Logs/DiagnosticReports" -name 'OBS*' -newer "${work}/obs.dmg" 2> /dev/null || true)"
if [[ -n "${crashes}" ]]; then
  fail "OBS left a crash report"
  cp ${crashes} "${out}/" 2> /dev/null || true
fi
grep -h '\[ctd-live' "${log}" > "${out}/ctd-lines.txt" || true

kill "${publisher_pid}" "${mediamtx_pid}" 2> /dev/null || true
cat "${out}/summary.md"
[[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && cat "${out}/summary.md" >> "${GITHUB_STEP_SUMMARY}"
exit $((failures > 0))
