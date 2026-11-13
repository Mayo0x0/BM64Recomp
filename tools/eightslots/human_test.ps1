# Player 5 as a human on port 5: binds port 5's stick right to a key, holds it during the battle and saves the log.
# The test mod must be built with -DTEST_HUMANS=1. The game window takes the focus while the key is held.
# Usage: human_test.ps1 -TestDir <folder> [-Label human1] [-HoldAt 20] [-HoldFor 4]
param(
    [Parameter(Mandatory)][string]$TestDir,
    [string]$Label = "human1",
    [int]$Seconds = 40,
    [int]$HoldAt = 20,
    [int]$HoldFor = 4
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$KEY_M = 0x4D         # virtual key
$SCAN_M = 0x32        # its hardware scancode: SDL reads that one
$SCANCODE_M = 16      # SDL scancode

# Ports 5 to 8 after the 4 the config has: port 5 on the keyboard, stick right on M.
New-Item -ItemType Directory -Force $TestDir | Out-Null
$controls = "$TestDir\controls.json"
$config = if (Test-Path $controls) { Get-Content $controls -Raw | ConvertFrom-Json } else { [pscustomobject]@{ ports = @() } }
$ports = @($config.ports)
while ($ports.Count -lt 8) { $ports += [pscustomobject]@{ mode = "Off"; pak_type = "ControllerPak"; keyboard = [pscustomobject]@{}; controller = [pscustomobject]@{} } }
$ports[4].mode = "Keyboard"
$ports[4].keyboard = [pscustomobject]@{ X_AXIS_POS = @(@{ input_type = 1; input_id = $SCANCODE_M }, @{ input_type = 0; input_id = 0 }) }
$config.ports = $ports
$config | ConvertTo-Json -Depth 8 | Set-Content $controls -Encoding utf8

Add-Type -Namespace Win -Name Keys -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
$job = Start-Job { param($script, $label, $seconds, $dir) & $script -Label $label -Seconds $seconds -TestDir $dir } `
    -ArgumentList "$here\run_battle_test.ps1", $Label, $Seconds, $TestDir
Start-Sleep -Seconds $HoldAt
$game = Get-Process BM64Recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$TestDir*" } | Select-Object -First 1
if ($game) {
    (New-Object -ComObject WScript.Shell).AppActivate($game.Id) | Out-Null
    Start-Sleep -Milliseconds 300
    [Win.Keys]::keybd_event($KEY_M, $SCAN_M, 0, [UIntPtr]::Zero)
    Start-Sleep -Seconds $HoldFor
    [Win.Keys]::keybd_event($KEY_M, $SCAN_M, 2, [UIntPtr]::Zero)
} else {
    "game not found"
}
Receive-Job $job -Wait
