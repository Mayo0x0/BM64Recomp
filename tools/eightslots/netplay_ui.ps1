# Starts the build's game without mods in its own portable folder, for trying netplay by hand (several on one PC).
# Usage: netplay_ui.ps1 -TestDir <folder> [-Players 1] [-Join address] [-HostFor players]
# (the folder's port 1 is the keyboard, ports 2 to n controllers; -Join joins that host without the page,
# -HostFor hosts without the page and starts once that many players are in)
param(
    [Parameter(Mandatory)][string]$TestDir,
    [int]$Players = 1,
    [string]$Join = "",
    [int]$HostFor = 0,
    [string]$Exe = "C:\Tools\BM64Recomp\build\BM64Recompiled.exe",
    [string]$Rom = "C:\Spiele\Bomberman64Recompiled\bm64_us.z64"
)
$ErrorActionPreference = "Stop"
$buildDir = Split-Path -Parent $Exe
New-Item -ItemType Directory -Force "$TestDir\mods" | Out-Null
Get-ChildItem "$TestDir\mods" | Remove-Item -Recurse -Force -Confirm:$false
foreach ($file in "BM64Recompiled.exe", "SDL2.dll", "dxcompiler.dll", "dxil.dll") { Copy-Item "$buildDir\$file" $TestDir -Force }
if (Test-Path "$TestDir\assets") { Remove-Item -Recurse -Force "$TestDir\assets" -Confirm:$false }
Copy-Item -Recurse "C:\Tools\BM64Recomp\assets" $TestDir
Copy-Item "C:\Tools\BM64Recomp\recompcontrollerdb.txt" $TestDir -Force
if (-not (Test-Path "$TestDir\bm64_us.z64")) { Copy-Item $Rom "$TestDir\bm64_us.z64" }
if (-not (Test-Path "$TestDir\portable.txt")) { New-Item -ItemType File "$TestDir\portable.txt" | Out-Null }
$ports = 0..7 | ForEach-Object {
    $mode = if ($_ -eq 0) { "Keyboard" } elseif ($_ -lt $Players) { "Controller" } else { "Off" }
    [pscustomobject]@{ mode = $mode; pak_type = "ControllerPak"; keyboard = [pscustomobject]@{}; controller = [pscustomobject]@{} }
}
[pscustomobject]@{ ports = $ports } | ConvertTo-Json -Depth 8 | Set-Content "$TestDir\controls.json" -Encoding utf8
Remove-Item Env:BM64_AUTOSTART, Env:BM64_NETPLAY -ErrorAction SilentlyContinue
if ($Join) { $env:BM64_NETPLAY = "join:$Join"; $env:BM64_NETPLAY_NAME = "ClientPC" }
if ($HostFor) { $env:BM64_NETPLAY = "host"; $env:BM64_NETPLAY_PLAYERS = $HostFor; $env:BM64_NETPLAY_NAME = "HostPC" }
# With BM64_NETPLAY set, BM64_AUTOSTART only adds the crash report (main.cpp) to the log.
if ($Join -or $HostFor) { $env:BM64_AUTOSTART = "1" }
Start-Process -FilePath "$TestDir\BM64Recompiled.exe" -WorkingDirectory $TestDir -PassThru `
    -RedirectStandardOutput "$TestDir\out.txt" -RedirectStandardError "$TestDir\err.txt" | Select-Object Id
