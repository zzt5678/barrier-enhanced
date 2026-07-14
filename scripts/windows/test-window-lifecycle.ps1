param(
    [string]$ResultPath = "$env:TEMP\weave-window-lifecycle-result.json",
    [int]$ObservationSeconds = 10
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class WeaveWindowApi {
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(IntPtr window, int command);

    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr window);
}
"@

function Get-TopLevelWindow([int]$ProcessId) {
    $script:windowResult = [IntPtr]::Zero
    $callback = [WeaveWindowApi+EnumWindowsProc] {
        param([IntPtr]$window, [IntPtr]$parameter)

        [uint32]$owner = 0
        [void][WeaveWindowApi]::GetWindowThreadProcessId($window, [ref]$owner)
        if ($owner -eq $ProcessId) {
            $script:windowResult = $window
            return $false
        }
        return $true
    }
    [void][WeaveWindowApi]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:windowResult
}

$sessionId = (Get-Process -Id $PID).SessionId
$gui = Get-Process -Name weave | Where-Object SessionId -eq $sessionId | Select-Object -First 1
$client = Get-Process -Name weavec | Where-Object SessionId -eq $sessionId | Select-Object -First 1
if ($null -eq $gui -or $null -eq $client) {
    throw "Weave GUI and client must both be running in the interactive session"
}

$window = Get-TopLevelWindow $gui.Id
if ($window -eq [IntPtr]::Zero) {
    throw "No top-level Weave window was found in the interactive session"
}

$clientId = $client.Id
$remoteBefore = @(Get-NetTCPConnection -OwningProcess $clientId -State Established |
    Where-Object { $_.RemotePort -eq 24800 }).Count
$ipcBefore = @(Get-NetTCPConnection -OwningProcess $clientId -State Established |
    Where-Object { $_.RemotePort -eq 24801 }).Count

[void][WeaveWindowApi]::ShowWindowAsync($window, 9)
Start-Sleep -Milliseconds 500
[void][WeaveWindowApi]::ShowWindowAsync($window, 6)
Start-Sleep -Seconds $ObservationSeconds

$guiAfter = Get-Process -Id $gui.Id -ErrorAction SilentlyContinue
$clientAfter = Get-Process -Id $clientId -ErrorAction SilentlyContinue
$remoteAfter = @(Get-NetTCPConnection -OwningProcess $clientId -State Established -ErrorAction SilentlyContinue |
    Where-Object { $_.RemotePort -eq 24800 }).Count
$ipcAfter = @(Get-NetTCPConnection -OwningProcess $clientId -State Established -ErrorAction SilentlyContinue |
    Where-Object { $_.RemotePort -eq 24801 }).Count
$wasMinimized = [WeaveWindowApi]::IsIconic($window)

$passed = $null -ne $guiAfter -and
    $null -ne $clientAfter -and
    $wasMinimized -and
    $remoteBefore -gt 0 -and $remoteAfter -gt 0 -and
    $ipcBefore -gt 0 -and $ipcAfter -gt 0

[ordered]@{
    passed = $passed
    guiPid = $gui.Id
    clientPid = $clientId
    minimized = $wasMinimized
    remoteConnectionsBefore = $remoteBefore
    remoteConnectionsAfter = $remoteAfter
    ipcConnectionsBefore = $ipcBefore
    ipcConnectionsAfter = $ipcAfter
    observationSeconds = $ObservationSeconds
} | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding UTF8

[void][WeaveWindowApi]::ShowWindowAsync($window, 9)

if (-not $passed) {
    exit 1
}
