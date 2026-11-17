<#
Spike S2 on the PC (Windows): builds the harness with the plugin's obs-deps libcurl (Schannel) and FFmpeg, then runs it.

  # 1. against the local test server (needs Node 22+; generates a 1 GiB file in the work dir)
  pwsh test/s2/run-s2.ps1

  # 2. against a dev CTD Relay, once it has file endpoints (no fault-injection tests then)
  pwsh test/s2/run-s2.ps1 -GetUrl http://10.20.30.40:8080/api/v1/files/big.bin `
                          -PutUrl http://10.20.30.40:8080/api/v1/files/s2-up.bin -Token <dev token>

Configure the plugin once first (cmake --preset windows-x64) so .deps holds obs-deps. Needs ~3x the file size free.
#>
param(
  [string]$GetUrl,
  [string]$PutUrl,
  [string]$Token = 'test-token',
  [int]$SizeMb = 1024,
  [int]$Port = 8787,
  [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) 'ctd-s2')
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
$deps = Get-ChildItem (Join-Path $root '.deps') -Directory -Filter 'obs-deps-*-x64' |
  Where-Object { $_.Name -notlike '*qt6*' } | Select-Object -First 1
if (-not $deps) { throw "No .deps/obs-deps-*-x64: run 'cmake --preset windows-x64' in the repo first." }

$build = Join-Path $Work 'build'
$client = Join-Path $Work 'client'
New-Item -ItemType Directory -Force $build, $client | Out-Null
cmake -S (Join-Path $root 'test/s2') -B $build -DCMAKE_PREFIX_PATH="$($deps.FullName)"
if ($LASTEXITCODE) { throw 'configure failed' }
cmake --build $build --config RelWithDebInfo
if ($LASTEXITCODE) { throw 'build failed' }
$exe = Join-Path $build 'RelWithDebInfo/s2-test.exe'
$env:PATH = "$(Join-Path $deps.FullName 'bin');$env:PATH" # libcurl.dll, avutil, avformat from obs-deps

$server = $null
$testArgs = @('--token', $Token, '--dir', $client)
if (-not $GetUrl) {
  $store = Join-Path $Work 'server'
  $log = Join-Path $Work 'server.log'
  $server = Start-Process node -PassThru -NoNewWindow -RedirectStandardOutput $log -ArgumentList @(
    (Join-Path $root 'test/s2/server.mjs'), '--dir', $store, '--port', $Port, '--size-mb', $SizeMb, '--token', $Token)
  for ($i = 0; $i -lt 600 -and -not (Select-String -Quiet -Path $log -Pattern 's2 server on' -ErrorAction SilentlyContinue); $i++) {
    Start-Sleep -Milliseconds 500
  }
  Get-Content $log
  $base = "http://127.0.0.1:$Port"
  $testArgs += @('--get', "$base/files/big.bin", '--put', "$base/files/up.bin", '--debug', "$base/debug/partials")
} else {
  if (-not $PutUrl) { throw '-PutUrl is required with -GetUrl' }
  $testArgs += @('--get', $GetUrl, '--put', $PutUrl)
}

try {
  & $exe @testArgs
  $status = $LASTEXITCODE
} finally {
  if ($server) { Stop-Process -Id $server.Id -ErrorAction SilentlyContinue }
}
exit $status
