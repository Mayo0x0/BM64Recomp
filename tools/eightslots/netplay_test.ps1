# Netplay on one PC: a host and clients in their own test folders, each with the test mod, joined over localhost.
# Every PC logs its RAM hash every 60 frames (BM64_NETPLAY_CHECKLOG) and its resyncs; the logs land in logs\.
# -HostPlayers n gives the host n players (ports 1 to n); -HoldAt s holds M on client 1 from second s for 4 seconds,
# bound to its stick right (build the test mod with -DTEST_HUMANS so that slot is a human).
# Usage: netplay_test.ps1 -Folder <base folder> [-Clients 1] [-Seconds 60] [-Label np] [-Relay host:port]
#        [-HostPlayers 1] [-HoldAt 0]
param(
    [Parameter(Mandatory)][string]$Folder,
    [int]$Clients = 1,
    [int]$Seconds = 60,
    [string]$Label = "np",
    [string]$Relay = "",
    [int]$HostPlayers = 1,
    [int]$HoldAt = 0
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$runner = "$here\run_battle_test.ps1"

# n players on the PC: port 1 on the keyboard (stick right on M), ports 2 to n on controllers that are not there.
function Set-Players($dir, $n) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $ports = 0..7 | ForEach-Object {
        $mode = if ($_ -eq 0) { "Keyboard" } elseif ($_ -lt $n) { "Controller" } else { "Off" }
        $keyboard = if ($_ -eq 0) { [pscustomobject]@{ X_AXIS_POS = @(@{ input_type = 1; input_id = 16 }, @{ input_type = 0; input_id = 0 }) } } else { [pscustomobject]@{} }
        [pscustomobject]@{ mode = $mode; pak_type = "ControllerPak"; keyboard = $keyboard; controller = [pscustomobject]@{} }
    }
    [pscustomobject]@{ ports = $ports } | ConvertTo-Json -Depth 8 | Set-Content "$dir\controls.json" -Encoding utf8
}

$jobs = @()
$hostDir = "$Folder\host"
Set-Players $hostDir $HostPlayers
$hostMode = if ($Relay) { "relayhost:$Relay" } else { "host:7777" }
$jobs += Start-Job {
    param($runner, $dir, $label, $seconds, $mode, $players)
    $env:BM64_NETPLAY = $mode; $env:BM64_NETPLAY_PLAYERS = $players; $env:BM64_NETPLAY_CHECKLOG = "1"; $env:BM64_NETPLAY_NAME = "Host"
    & $runner -Label "$label-host" -Seconds $seconds -TestDir $dir
} -ArgumentList $runner, $hostDir, $Label, $Seconds, $hostMode, ($HostPlayers + $Clients)

$room = ""
if ($Relay) {
    # The relay gives the host a room code; wait for it in the host's log.
    $out = "$here\logs\$Label-host.out.txt"
    for ($i = 0; $i -lt 40 -and -not $room; $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path $out) { $m = Select-String -Path $out -Pattern "Room code: (\d+)" | Select-Object -First 1; if ($m) { $room = $m.Matches[0].Groups[1].Value } }
    }
    if (-not $room) { throw "no room code from the relay" }
} else {
    Start-Sleep -Seconds 2
}
for ($c = 1; $c -le $Clients; $c++) {
    $dir = "$Folder\client$c"
    Set-Players $dir 1
    $mode = if ($Relay) { "relayjoin:${Relay}:$room" } else { "join:127.0.0.1:7777" }
    $jobs += Start-Job {
        param($runner, $dir, $label, $seconds, $mode, $name)
        $env:BM64_NETPLAY = $mode; $env:BM64_NETPLAY_CHECKLOG = "1"; $env:BM64_NETPLAY_NAME = $name
        & $runner -Label $label -Seconds $seconds -TestDir $dir
    } -ArgumentList $runner, $dir, "$Label-client$c", $Seconds, $mode, "Client$c"
}
if ($HoldAt -gt 0) {
    Add-Type -Namespace Win -Name Keys -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
    Start-Sleep -Seconds $HoldAt
    $game = Get-Process BM64Recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Folder\client1\*" } | Select-Object -First 1
    if ($game) {
        (New-Object -ComObject WScript.Shell).AppActivate($game.Id) | Out-Null
        Start-Sleep -Milliseconds 300
        [Win.Keys]::keybd_event(0x4D, 0x32, 0, [UIntPtr]::Zero)
        Start-Sleep -Seconds 4
        [Win.Keys]::keybd_event(0x4D, 0x32, 2, [UIntPtr]::Zero)
    } else { "client 1 not found" }
}
$jobs | Receive-Job -Wait
$jobs | Remove-Job
foreach ($name in @("host") + (1..$Clients | ForEach-Object { "client$_" })) {
    $lines = Select-String -Path "$here\logs\$Label-$name.out.txt" -Pattern "^NETPLAY" | ForEach-Object { $_.Line }
    $lines | Set-Content "$here\logs\$Label-$name.netplay.txt" -Encoding utf8
    "$name : $(@($lines).Count) netplay lines"
}
