# Netplay through the game's own menus, without the test mod: a host and a client on this PC (netplay_ui.ps1), the
# host's keys go from the title to a single battle on Rock Garden, then both players walk and drop bombs.
# Afterwards the client's log tells whether checks failed. Usage: netplay_menus.ps1 -HostDir <dir> -ClientDir <dir>
param(
    [Parameter(Mandatory)][string]$HostDir,
    [Parameter(Mandatory)][string]$ClientDir,
    [int]$PlaySeconds = 60,
    [string]$CheckLog = "1"
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Only this test's games: another BM64Recompiled may be the user's own.
Get-Process BM64Recompiled -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$HostDir\*" -or $_.Path -like "$ClientDir\*" } |
    ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
$env:BM64_NETPLAY_CHECKLOG = $CheckLog
& "$here\netplay_ui.ps1" -TestDir $HostDir -HostFor 2 | Out-Null
Start-Sleep -Seconds 2
& "$here\netplay_ui.ps1" -TestDir $ClientDir -Join 127.0.0.1:7777 | Out-Null
Remove-Item Env:BM64_NETPLAY_CHECKLOG

# Keys with their hardware scancodes (SDL reads scancodes): the default keyboard bindings of port 1.
$map = @{ w = @(0x57, 0x11); a = @(0x41, 0x1E); s = @(0x53, 0x1F); d = @(0x44, 0x20); space = @(0x20, 0x39); enter = @(0x0D, 0x1C) }
Add-Type -Namespace Win -Name K3 -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
function Send-Keys($dir, $keys, $hold = 120, $gap = 400) {
    $game = Get-Process BM64Recompiled | Where-Object { $_.Path -like "$dir\*" } | Select-Object -First 1
    (New-Object -ComObject WScript.Shell).AppActivate($game.Id) | Out-Null
    Start-Sleep -Milliseconds 300
    foreach ($name in $keys.Split(" ", [StringSplitOptions]::RemoveEmptyEntries)) {
        $k = $map[$name]
        [Win.K3]::keybd_event($k[0], $k[1], 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $hold
        [Win.K3]::keybd_event($k[0], $k[1], 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds $gap
    }
}
Start-Sleep -Seconds 25                          # boot, logos, intro
Send-Keys $HostDir "enter" 120 2000              # title
Send-Keys $HostDir "enter" 120 3000              # main menu appears
Send-Keys $HostDir "s space" 120 2500            # Battle
Send-Keys $HostDir "space" 120 2500              # Single Battle
Send-Keys $HostDir "space" 120 1000              # player 1 ready
Send-Keys $ClientDir "space" 120 2500            # player 2 ready
Send-Keys $HostDir "space" 120 2500              # stage
Send-Keys $HostDir "space" 120 4000              # go
$end = (Get-Date).AddSeconds($PlaySeconds)
while ((Get-Date) -lt $end) {
    Send-Keys $HostDir "d space s a" 600 150
    Send-Keys $ClientDir "a space w d" 600 150
}
$client = Get-Content "$ClientDir\out.txt" | Select-String "^NETPLAY" | ForEach-Object Line
"checks: " + @($client | Where-Object { $_ -match "check f=" }).Count + ", failed: " + @($client | Where-Object { $_ -match "differs" }).Count
$client | Where-Object { $_ -match "resync at|Lost|slow" } | Select-Object -First 10
