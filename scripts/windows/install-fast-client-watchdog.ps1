param(
    [string]$ServerHost = '100.76.98.15',
    [int]$ServerPort = 24800,
    [string]$ClientName = 'zyt',
    [int]$CheckIntervalMs = 5000,
    [string]$FastWatchdogTask = 'WeaveFastClientWatchdog',
    [string]$LegacyWatchdogTask = 'WeaveClientWatchdog'
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$binDir = Join-Path $repoRoot 'build-final\bin\Release'
$clientExe = Join-Path $binDir 'weavec.exe'

if (!(Test-Path $clientExe)) {
    throw "Missing client executable: $clientExe"
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
    '"' + $clientExe + '" -f --no-tray --debug INFO --name ' + $ClientName +
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

$fastVbsContent = @"
Option Explicit

Dim shell, fso, wmi
Dim serverHost, serverPort, startVbs, logPath, checkIntervalMs
Dim disconnectedCount

serverHost = "$ServerHost"
serverPort = "$ServerPort"
startVbs = "$startVbs"
logPath = "$fastLog"
checkIntervalMs = $CheckIntervalMs
disconnectedCount = 0

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

Function WeavecCount()
    WeavecCount = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='weavec.exe'").Count
End Function

Function HasEstablishedConnection()
    Dim exec, line, upperLine
    HasEstablishedConnection = False
    Set exec = shell.Exec("%ComSpec% /c netstat -ano -p tcp")
    Do While exec.Status = 0
        WScript.Sleep 100
    Loop
    Do Until exec.StdOut.AtEndOfStream
        line = exec.StdOut.ReadLine
        upperLine = UCase(line)
        If InStr(line, serverHost & ":" & serverPort) > 0 And InStr(upperLine, "ESTABLISHED") > 0 Then
            HasEstablishedConnection = True
            Exit Function
        End If
    Loop
End Function

Sub StopWeavec()
    Dim proc
    For Each proc In wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name='weavec.exe'")
        proc.Terminate()
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
    If HasEstablishedConnection() Then
        WriteLog "after restart: connected"
        disconnectedCount = 0
    Else
        WriteLog "after restart: not connected"
    End If
End Sub

WriteLog "fast watchdog started; interval_ms=" & checkIntervalMs

Do
    If WeavecCount() = 0 Then
        RestartClient "weavec missing"
    ElseIf HasEstablishedConnection() Then
        disconnectedCount = 0
    Else
        disconnectedCount = disconnectedCount + 1
        If disconnectedCount >= 2 Then
            RestartClient "no established connection"
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
