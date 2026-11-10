# Runs one battle test, judges its log and names the function of a crash.
# Usage: test_battle.ps1 -Label m2 -TestDir <folder> [-Seconds 100] [-Players 4]
param(
    [Parameter(Mandatory)][string]$Label,
    [Parameter(Mandatory)][string]$TestDir,
    [int]$Seconds = 100,
    [int]$Players = 4
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$started = Get-Date
& "$here\run_battle_test.ps1" -Label $Label -Seconds $Seconds -TestDir $TestDir
python -I "$here\check_log.py" "$here\logs\$Label.test.txt" $Players
$failed = $LASTEXITCODE
$frames = @(Select-String -Path "$here\logs\$Label.err.txt" -Pattern '^(FRAME|CRASH)' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
if ($frames.Count) {
    python -I "$here\crash_at.py" @($frames | ForEach-Object { [regex]::Match($_, '(?:FRAME |at=)([0-9A-F]+)').Groups[1].Value } | Where-Object { $_ }) | Select-Object -First 14
}
$crash = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $started } -MaxEvents 1 -ErrorAction SilentlyContinue
if ($crash) {
    $offset = [regex]::Match($crash.Message, '0x[0-9a-f]{16}').Value
    "CRASH at $offset"
    python -I "$here\crash_at.py" $offset
    $failed = 1
}
"RESULT $(if ($failed) { 'FAILED' } else { 'PASSED' })"
Get-Content "$here\logs\$Label.test.txt" -TotalCount 4 | ForEach-Object { $_.Substring(0, [Math]::Min(150, $_.Length)) }
