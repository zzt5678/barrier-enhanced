param(
    [string]$ServerHost = '100.76.98.15',
    [int]$ServerPort = 24800,
    [string]$ClientName = 'zyt',
    [string]$BuildDir = '',
    [string]$ClientExe = '',
    [int]$CheckIntervalMs = 5000,
    [string]$FastWatchdogTask = 'WeaveFastClientWatchdog',
    [string]$LegacyWatchdogTask = 'WeaveClientWatchdog'
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)

if ([string]::IsNullOrWhiteSpace($ClientExe)) {
    $buildRoot = if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        Join-Path $repoRoot 'build'
    } else {
        [System.IO.Path]::GetFullPath($BuildDir)
    }
    $clientCandidates = @(
        (Join-Path $buildRoot 'bin\weavec.exe'),
        (Join-Path $buildRoot 'bin\Release\weavec.exe'),
        (Join-Path $buildRoot 'bin\Debug\weavec.exe')
    )
    $clientExePath = $clientCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($clientExePath)) {
        $clientExePath = $clientCandidates[0]
    }
} else {
    $clientExePath = [System.IO.Path]::GetFullPath($ClientExe)
}

if (!(Test-Path $clientExePath)) {
    throw "Missing client executable: $clientExePath"
}

$localWeaveDir = Join-Path $env:LOCALAPPDATA 'Weave'
$profileDir = Join-Path $env:LOCALAPPDATA 'Barrier'
$clientLog = Join-Path $profileDir 'weave-debug.log'
$dropDir = Join-Path $env:APPDATA 'Weave\Weave\workflow\inbox'
$startVbs = Join-Path $localWeaveDir 'start-weavec-hidden.vbs'
$fastVbs = Join-Path $localWeaveDir 'weave-fast-watchdog.vbs'
$fastLog = Join-Path $localWeaveDir 'weave-fast-watchdog.log'

New-Item -ItemType Directory -Force -Path $localWeaveDir, $profileDir, $dropDir | Out-Null

$clientCommand =
    '"' + $clientExePath + '" -f --no-tray --debug INFO --name ' + $ClientName +
    ' --enable-drag-drop --drop-dir "' + $dropDir +
    '" --profile-dir "' + $profileDir +
    '" --log "' + $clientLog +
    '" ' + $ServerHost + ':' + $ServerPort

$escapedClientCommand = $clientCommand.Replace('"', '""')
$startVbsContent = @"
Set shell = CreateObject("WScript.Shell")
shell.Run "$escapedClientCommand", 0, False
"@
Set-Content -Path $startVbs -Value $startVbsContent -Encoding ASCII

$vbsClientExe = $clientExePath.Replace('"', '""')
$vbsServerAddress = ("{0}:{1}" -f $ServerHost, $ServerPort).Replace('"', '""')
$vbsClientName = $ClientName.Replace('"', '""')

$fastVbsContent = @"
Option Explicit

Dim shell, fso, wmi
Dim serverHost, serverPort, serverAddress, clientName, clientExe, startVbs, logPath, checkIntervalMs
Dim lastAliveLog

serverHost = "$ServerHost"
serverPort = "$ServerPort"
serverAddress = "$vbsServerAddress"
clientName = "$vbsClientName"
clientExe = "$vbsClientExe"
startVbs = "$startVbs"
logPath = "$fastLog"
checkIntervalMs = $CheckIntervalMs
lastAliveLog = Timer

Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
Set wmi = GetObject("winmgmts:\\.\root\cimv2")

Sub WriteLog(message)
    Dim file
    Set file = fso.OpenTextFile(logPath, 8, True)
    file.WriteLine "[" & Year(Now) & "-" & Right("0" & Month(Now), 2) & "-" & Right("0" & Day(Now), 2) & " " & _
        Right("0" & Hour(Now), 2) & ":" & Right("0" & Minute(Now), 2) & ":" & Right("0" & Second(Now), 2) & "] " & message
    file.Close
End Sub

Function IsManagedWeavec(proc)
    Dim commandLine
    IsManagedWeavec = False
    If IsNull(proc.CommandLine) Then
        Exit Function
    End If
    commandLine = proc.CommandLine
    If InStr(1, commandLine, clientExe, vbTextCompare) > 0 And _
        InStr(1, commandLine, serverAddress, vbTextCompare) > 0 And _
        InStr(1, commandLine, "--name " & clientName, vbTextCompare) > 0 Then
        IsManagedWeavec = True
    End If
End Function

Function WeavecCount()
    Dim count, proc
    count = 0
    For Each proc In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='weavec.exe'")
        If IsManagedWeavec(proc) Then
            count = count + 1
        End If
    Next
    WeavecCount = count
End Function

Sub StopWeavec()
    Dim proc
    For Each proc In wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='weavec.exe'")
        If IsManagedWeavec(proc) Then
            proc.Terminate()
        End If
    Next
End Sub

Sub StartClient()
    shell.Run "wscript.exe //B //NoLogo """ & startVbs & """", 0, False
End Sub

Sub RestartClient(reason)
    WriteLog "restart: " & reason
    StopWeavec
    WScript.Sleep 1500
    StartClient
    WScript.Sleep 3000
    WriteLog "after restart: start requested"
End Sub

WriteLog "fast watchdog started; interval_ms=" & checkIntervalMs

Do
    If WeavecCount() = 0 Then
        RestartClient "weavec missing"
    Else
        If Abs(Timer - lastAliveLog) >= 60 Then
            WriteLog "ok: weavec running"
            lastAliveLog = Timer
        End If
    End If
    WScript.Sleep checkIntervalMs
Loop
"@
Set-Content -Path $fastVbs -Value $fastVbsContent -Encoding ASCII

Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -match [regex]::Escape($fastVbs) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\wscript.exe" `
    -Argument ('//B //NoLogo "' + $fastVbs + '"')
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $FastWatchdogTask -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings -Force | Out-Null

Disable-ScheduledTask -TaskName $LegacyWatchdogTask -ErrorAction SilentlyContinue | Out-Null
Start-ScheduledTask -TaskName $FastWatchdogTask

Write-Output "installed fast watchdog task: $FastWatchdogTask"
Write-Output "disabled legacy watchdog task if present: $LegacyWatchdogTask"
Write-Output "start script: $startVbs"
Write-Output "watchdog script: $fastVbs"
Write-Output "watchdog log: $fastLog"
Write-Output "client executable: $clientExePath"
