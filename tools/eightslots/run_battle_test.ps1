# Runs the build's game in an isolated portable folder with the test mod and saves its TEST log lines.
# Usage: run_battle_test.ps1 -Label baseline1 -Seconds 90 -TestDir <folder> [-Exe <path>]
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 90,
    [Parameter(Mandatory)][string]$TestDir,
    [string]$Exe = "C:\Tools\BM64Recomp\build\BM64Recompiled.exe",
    [string]$Rom = "C:\Spiele\Bomberman64Recompiled\bm64_us.z64"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Split-Path -Parent $Exe

New-Item -ItemType Directory -Force "$TestDir\mods" | Out-Null
Get-ChildItem "$TestDir\mods" | Remove-Item -Recurse -Force -Confirm:$false
foreach ($file in "BM64Recompiled.exe", "SDL2.dll", "dxcompiler.dll", "dxil.dll") {
    Copy-Item "$buildDir\$file" $TestDir -Force
}
if (-not (Test-Path "$TestDir\assets")) { Copy-Item -Recurse "C:\Tools\BM64Recomp\assets" $TestDir }
Copy-Item "C:\Tools\BM64Recomp\recompcontrollerdb.txt" $TestDir -Force
if (-not (Test-Path "$TestDir\bm64_us.z64")) { Copy-Item $Rom "$TestDir\bm64_us.z64" }
if (-not (Test-Path "$TestDir\portable.txt")) { New-Item -ItemType File "$TestDir\portable.txt" | Out-Null }
Copy-Item "$here\testmod\build\bm64_test_battle.nrm" "$TestDir\mods\"

$out = Join-Path $here "logs\$Label.out.txt"
$err = Join-Path $here "logs\$Label.err.txt"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null

$env:BM64_AUTOSTART = "1"
$process = Start-Process -FilePath "$TestDir\BM64Recompiled.exe" -WorkingDirectory $TestDir -PassThru `
    -RedirectStandardOutput $out -RedirectStandardError $err
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $process.HasExited) { Start-Sleep -Seconds 1 }
$crashed = $process.HasExited
if (-not $crashed) { Stop-Process -Id $process.Id -Force -Confirm:$false }
Start-Sleep -Milliseconds 500

$lines = @(Select-String -Path $out -Pattern "^TEST" | ForEach-Object { $_.Line })
$lines | Set-Content (Join-Path $here "logs\$Label.test.txt") -Encoding utf8
"label=$Label exited_by_itself=$crashed exit_code=$(if ($crashed) { $process.ExitCode } else { 'n/a' }) test_lines=$($lines.Count)"
