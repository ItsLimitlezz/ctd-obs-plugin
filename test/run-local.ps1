# The harness scenarios on Windows without Docker: MediaMTX, ffmpeg and ctd-live-probe as local processes, same checks
# as scenarios.sh/check.py. Everything listens on 127.0.0.1 only.
#
#   test/run-local.ps1 -MediaMtx C:\tools\mediamtx.exe -FFmpeg C:\tools\ffmpeg.exe -Probe build\ctd-live-probe.exe
#   test/run-local.ps1 ... -Scenarios T1,T11 -T6Minutes 3
#
# The probe needs FFmpeg's DLLs (built with libsrt) on PATH, for example OBS's own: -FFmpegDlls 'C:\Program Files\obs-studio\bin\64bit'.
param(
    [Parameter(Mandatory)] [string] $MediaMtx,
    [Parameter(Mandatory)] [string] $FFmpeg,
    [Parameter(Mandatory)] [string] $Probe,
    [string] $FFmpegDlls,
    [string[]] $Scenarios = @('T1', 'T2', 'T6', 'T11', 'T15', 'T17', 'T18', 'T19', 'T20', 'T21'),
    [string] $LtcGen, # ltc-gen.exe; by default next to the probe
    [int] $T6Minutes = 15,
    [string] $Size = '1280x720'
)
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Out = Join-Path $Here 'out'
New-Item -ItemType Directory -Force $Out | Out-Null
if ($FFmpegDlls) { $env:PATH = "$FFmpegDlls;$env:PATH" }
$RecoverMs = 3000
$Api = 'http://127.0.0.1:9997/v3/paths/get/cam1'

# the test server config, bound to loopback so Windows doesn't ask about the firewall
$config = Join-Path $Out 'mediamtx-local.yml'
$text = (Get-Content (Join-Path $Here 'mediamtx.yml') -Raw).Replace('apiAddress: :9997', 'apiAddress: 127.0.0.1:9997').Replace(
    'srtAddress: :8890', 'srtAddress: 127.0.0.1:8890')
[IO.File]::WriteAllText($config, $text, (New-Object Text.UTF8Encoding $false)) # MediaMTX refuses a byte-order mark

function Now-Ms { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

$script:mtx = $null
$script:pub = $null
function Start-Server { $script:mtx = Start-Process $MediaMtx -ArgumentList "`"$config`"" -WorkingDirectory $Out -WindowStyle Hidden -PassThru; Wait-Api }
function Stop-Server { if ($script:mtx) { Stop-Process -Id $script:mtx.Id -Force -ErrorAction SilentlyContinue; $script:mtx = $null } }
function Start-Publisher([string] $AudioBsf, [string] $AudioOffset = '0') {
    $fps = 30
    $bsf = if ($AudioBsf) { @('-bsf:a', $AudioBsf) } else { @() }
    $a = @('-hide_banner', '-loglevel', 'error', '-re',
        '-f', 'lavfi', '-i', "testsrc2=size=${Size}:rate=$fps,drawbox=c=white:t=fill:enable='lt(mod(t\,1)\,1/$fps)'",
        '-itsoffset', $AudioOffset, '-f', 'lavfi', '-i', "aevalsrc='if(lt(mod(t\,1)\,0.05)\,sin(2*PI*1000*t)\,0)':s=48000:c=stereo",
        '-c:v', 'libx264', '-preset', 'veryfast', '-bf', '2', '-g', "$($fps * 2)", '-b:v', '3M', '-pix_fmt', 'yuv420p',
        '-c:a', 'aac', '-b:a', '128k') + $bsf + @('-f', 'mpegts', 'srt://127.0.0.1:8890?streamid=publish:cam1&pkt_size=1316')
    $script:pub = Start-Process $FFmpeg -ArgumentList $a -WorkingDirectory $Out -WindowStyle Hidden -PassThru
}
# T17: ltc-gen piped into ffmpeg through cmd, so stopping it takes the whole tree
$script:pubs = @()
function Start-LtcPublisher([string] $LtcOptions, [int] $Kbps, [string] $Path = 'cam1') {
    $gen = if ($LtcGen) { $LtcGen } else { Join-Path (Split-Path $Probe) 'ltc-gen.exe' }
    $line = "`"$gen`" $LtcOptions | `"$FFmpeg`" -hide_banner -loglevel error -re -f lavfi -i testsrc2=size=${Size}:rate=30 " +
        "-re -f s16le -ar 48000 -ac 2 -i pipe:0 -map 0:v -map 1:a -c:v libx264 -preset veryfast -bf 2 -g 60 -b:v 3M " +
        "-pix_fmt yuv420p -c:a aac -b:a ${Kbps}k -f mpegts `"srt://127.0.0.1:8890?streamid=publish:${Path}&pkt_size=1316`""
    $p = Start-Process cmd.exe -ArgumentList '/c', "`"$line`"" -WorkingDirectory $Out -WindowStyle Hidden -PassThru
    $script:pubs += $p
    $p
}
function Stop-One($p) { if ($p) { cmd /c "taskkill /T /F /PID $($p.Id) >nul 2>nul" } }
function Stop-Publisher {
    Stop-One $script:pub; $script:pub = $null
    foreach ($p in $script:pubs) { Stop-One $p }
    $script:pubs = @()
}
function Wait-Api {
    foreach ($i in 1..100) {
        if ($script:mtx.HasExited) { throw 'MediaMTX exited (check test/out/mediamtx-local.yml)' }
        try { $null = Invoke-WebRequest 'http://127.0.0.1:9997/v3/paths/list' -UseBasicParsing -TimeoutSec 1; return } catch { Start-Sleep -Milliseconds 100 }
    }
    throw 'the API never answered'
}
function Wait-Ready([string] $Path = 'cam1') {
    foreach ($i in 1..300) {
        try { if ((Invoke-RestMethod "http://127.0.0.1:9997/v3/paths/get/$Path" -TimeoutSec 1).ready) { return Now-Ms } } catch {}
        Start-Sleep -Milliseconds 100
    }
    throw 'the stream never became ready'
}
function Start-Probe([string] $name, [string[]] $arguments) {
    $file = Join-Path $Out "$name.jsonl"
    $all = @('--host', '127.0.0.1', '--path', 'cam1', '--json') + $arguments
    Start-Process $Probe -ArgumentList $all -RedirectStandardOutput $file -RedirectStandardError (Join-Path $Out "$name.err") -WindowStyle Hidden -PassThru
}

# check.py, in PowerShell
function Check([string] $name, [double] $bound, [double[]] $readies) {
    $lines = Get-Content (Join-Path $Out "$name.jsonl") | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json }
    $start = ($lines | Where-Object type -eq 'start').epochMs
    $result = $lines | Where-Object type -eq 'result'
    if (-not $start -or -not $result) { "FAIL: the probe didn't start or didn't finish"; return $false }
    $states = $lines | Where-Object type -eq 'state' | ForEach-Object { [pscustomobject]@{ At = $start + $_.t * 1000; State = $_.state } }
    $ok = [bool]$result.pass
    if (-not $ok) { "FAIL: probe bounds: $($result.why)" }
    foreach ($ready in $readies) {
        $before = @($states | Where-Object At -le $ready)
        $label = 'ready at +{0:N2} s' -f (($ready - $start) / 1000)
        if ($before.Count -and $before[-1].State -eq 'LIVE') { "  ${label}: never left LIVE"; continue }
        $after = @($states | Where-Object { $_.At -gt $ready -and $_.State -eq 'LIVE' })
        if (-not $after.Count) { "  ${label}: never LIVE again"; $ok = $false; continue }
        $delay = $after[0].At - $ready
        "  ${label}: LIVE {0:N2} s later ({1})" -f ($delay / 1000), $(if ($delay -le $bound) { 'ok' } else { 'TOO SLOW' })
        $ok = $ok -and $delay -le $bound
    }
    '{0}  first LIVE {1} s, worst lateness {2} ms, gaps {3} (worst {4} s), reconnects {5}, resyncs {6}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }),
    $result.firstLiveS, $result.worstLatenessMs, $result.gaps, $result.worstGapS, $result.reconnects, $result.resyncs
    return $ok
}

function T1 {
    Start-Server; Start-Publisher; $null = Wait-Ready
    (Start-Probe 'T1' @('--seconds', '10', '--live-within', '3', '--monotonic', '--max-lateness', '700')).WaitForExit()
    Check 'T1' $RecoverMs @()
}
function T2 {
    Start-Server
    $p = Start-Probe 'T2' @('--seconds', '20', '--monotonic', '--max-lateness', '700')
    Start-Sleep 6
    Start-Publisher; $ready = Wait-Ready
    $p.WaitForExit()
    Check 'T2' $RecoverMs @($ready)
}
function T6 {
    Start-Server; Start-Publisher; $null = Wait-Ready
    $p = Start-Probe 'T6' @('--seconds', "$($T6Minutes * 60 + 15)", '--max-lateness', '700')
    $readies = @()
    foreach ($m in 1..$T6Minutes) {
        Start-Sleep 57
        Stop-Publisher; Start-Sleep 3; Start-Publisher
        $readies += Wait-Ready
    }
    $p.WaitForExit()
    Check 'T6' $RecoverMs $readies
}
function T11 {
    Start-Server; Start-Publisher; $null = Wait-Ready
    $p = Start-Probe 'T11' @('--seconds', '40', '--max-lateness', '700')
    Start-Sleep 10
    Stop-Server; Start-Sleep 1; Start-Server
    # the local publisher doesn't restart itself the way the Docker one does
    Stop-Publisher; Start-Publisher
    $ready = Wait-Ready
    $p.WaitForExit()
    Check 'T11' ($RecoverMs + 1000) @($ready)
}
# T15 (A2): every 20th AAC packet dropped; the holes are filled with silence and the feed stays LIVE
function T15 {
    Start-Server; Start-Publisher 'noise=drop=not(mod(n\,20))'; $null = Wait-Ready
    (Start-Probe 'T15' @('--seconds', '15', '--live-within', '3', '--max-lateness', '700', '--min-gaps-filled', '10')).WaitForExit()
    Check 'T15' $RecoverMs @()
}
# T17 (S5, S8): LTC through AAC at 64, 128 and 256 kbps at every rate; locks at the right rate, under 0.1% lost
function T17 {
    $ok = $true
    foreach ($kbps in 64, 128, 256) {
        foreach ($rate in '23.976', '24', '25', '29.97 --drop', '29.97', '30') {
            Start-Server; Start-LtcPublisher "--fps $rate" $kbps; $null = Wait-Ready
            Start-Sleep 3 # a fresh AAC encoder mangles a word or two in its first seconds; a camera's has been running
            $fps = $rate.Split(' ')[0]
            $name = "T17-$kbps-$($rate.Replace(' ', ''))"
            $a = @('--seconds', '15', '--live-within', '3', '--timecode', '2', '--expect-tc-fps', $fps, '--max-tc-errors', '0.1')
            if ($rate -ne $fps) { $a += '--expect-drop' }
            (Start-Probe $name $a).WaitForExit()
            "-- $kbps kbps, $rate"
            $r = Check $name $RecoverMs @()
            $result = Get-Content (Join-Path $Out "$name.jsonl") | Where-Object { $_ -match '"result"' } | ConvertFrom-Json
            "   timecode {0:N3} fps{1}, errors {2:N3}%" -f $result.tcFps, $(if ($result.tcDrop) { ' drop' } else { '' }), $result.tcErrors
            $ok = $ok -and ($r | Select-Object -Last 1)
            $r | Where-Object { $_ -is [string] }
            Stop-Publisher; Stop-Server
        }
    }
    # S7: with no timecode input set, the LTC is found on its channel
    Start-Server; Start-LtcPublisher '--fps 25' 128; $null = Wait-Ready
    (Start-Probe 'T17-detect' @('--seconds', '6', '--expect-detected', '2')).WaitForExit()
    $r = Check 'T17-detect' $RecoverMs @()
    '-- LTC found with the input set to None'
    $r | Where-Object { $_ -is [string] }
    $ok = $ok -and ($r | Select-Object -Last 1)
    Stop-Publisher; Stop-Server
    $ok
}

# the group probe's result line
function Check-Group([string] $name) {
    $r = Get-Content (Join-Path $Out "$name.jsonl") | Where-Object { $_ -match '"result"' } | ConvertFrom-Json
    if (-not $r) { "FAIL: the probe didn't finish"; return $false }
    '{0}  all locked at {1} s, worst spread {2} ms ({3} frames), jumps after lock {4}, freewheeled {5} {6}' -f $(if ($r.pass) { 'PASS' } else { 'FAIL' }),
    $r.allLockedS, $r.worstSpreadMs, $r.worstSpreadFrames, $r.jumpsAfterLock, $r.freewheeled, $r.why
    return [bool]$r.pass
}

# T18 (S9 to S12): three feeds with time-of-day LTC, arriving 0, 180 and 420 ms late. All lock within 10 s, then stay
# within a frame of each other on screen, with no jump after the first lock.
function T18 {
    Start-Server
    $offsets = @{ cam1 = 0; cam2 = 180; cam3 = 420 }
    foreach ($path in 'cam1', 'cam2', 'cam3') { $null = Start-LtcPublisher "--fps 25 --time-of-day --offset-ms $($offsets[$path])" 128 $path }
    foreach ($path in 'cam1', 'cam2', 'cam3') { $null = Wait-Ready $path }
    Start-Sleep 3
    (Start-Probe 'T18' @('--group', 'cam1,cam2,cam3', '--seconds', '30', '--lock-within', '10', '--max-spread-frames', '1',
        '--no-jumps-after-lock', '--all-locked-at-end')).WaitForExit()
    Check-Group 'T18'
}

# T19 (S13): the 180 ms member loses its LTC for 7 s, then its link for 3 s; both come back. It freewheels (sync lost),
# then relocks without a jump, and the group ends locked and within a frame. The restarted test publisher comes back
# later than it was (its ffmpeg starting up), hence a 3 s max delay and the time to ramp there.
function T19 {
    Start-Server
    $null = Start-LtcPublisher '--fps 25 --time-of-day' 128 'cam1'
    $b = Start-LtcPublisher '--fps 25 --time-of-day --offset-ms 180 --mute-from 18 --mute-to 25' 128 'cam2'
    $null = Start-LtcPublisher '--fps 25 --time-of-day --offset-ms 420' 128 'cam3'
    foreach ($path in 'cam1', 'cam2', 'cam3') { $null = Wait-Ready $path }
    Start-Sleep 3
    $p = Start-Probe 'T19' @('--group', 'cam1,cam2,cam3', '--seconds', '75', '--max-delay', '3000', '--max-spread-frames', '1', '--no-jumps-after-lock',
        '--expect-freewheel', '--all-locked-at-end')
    Start-Sleep 27 # LTC gone from about 14 s into the probe until about 21 s
    Stop-One $b; Start-Sleep 3
    $null = Start-LtcPublisher '--fps 25 --time-of-day --offset-ms 180' 128 'cam2'
    $p.WaitForExit()
    Check-Group 'T19'
}

# T20 (S1, S2): delays 0 → 1500 ms video and 0 → 800 ms audio while live; smooth, reached, inside the memory cap
function T20 {
    Start-Server; Start-Publisher; $null = Wait-Ready
    (Start-Probe 'T20' @('--seconds', '45', '--live-within', '3', '--monotonic', '--max-lateness', '700', '--change-delay-at', '10',
        '--video-delay-to', '1500', '--audio-delay-to', '800', '--reach-delays', '--no-gaps-filled')).WaitForExit()
    Check 'T20' $RecoverMs @()
}

# T21 (S16): the publisher's beep comes 200 ms after its flash; a sync check measures 200 ms (within 40), and 0 without
function T21 {
    $ok = $true
    foreach ($case in @(@{ Offset = '0'; Expect = '0' }, @{ Offset = '0.2'; Expect = '200' })) {
        Start-Server; Start-Publisher '' $case.Offset; $null = Wait-Ready
        $name = "T21-$($case.Expect)"
        (Start-Probe $name @('--seconds', '16', '--sync-check-at', '3', '--expect-av-offset', $case.Expect)).WaitForExit()
        $r = Get-Content (Join-Path $Out "$name.jsonl") | Where-Object { $_ -match '"result"' } | ConvertFrom-Json
        '{0}  beep {1} ms after the flash: measured {2} ms from {3} pairs {4}' -f $(if ($r.pass) { 'PASS' } else { 'FAIL' }),
            $case.Expect, $r.syncCheckOffsetMs, $r.syncCheckPairs, $r.why
        $ok = $ok -and [bool]$r.pass
        Stop-Publisher; Stop-Server
    }
    $ok
}

$failed = 0
foreach ($t in $Scenarios) {
    "== $t"
    try {
        $output = & $t
        $output | Where-Object { $_ -is [string] } | ForEach-Object { $_ }
        if (-not ($output | Where-Object { $_ -is [bool] } | Select-Object -Last 1)) { $failed++ }
    } catch { "FAIL: $_"; $failed++ }
    finally { Stop-Publisher; Stop-Server }
}
"== $failed of $($Scenarios.Count) scenarios failed"
exit $failed
