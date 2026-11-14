# Registers a Windows scheduled task that runs start_happy.ps1 (hidden) every time you log on.
# If the server crashes, Windows restarts it (up to 3 times, 1 minute apart).
#
#   Install:    powershell -ExecutionPolicy Bypass -File install_autostart.ps1
#   Remove:     powershell -ExecutionPolicy Bypass -File install_autostart.ps1 -Uninstall
#   Start now:  Start-ScheduledTask -TaskName "Happy Brain"
#   Stop:       Stop-ScheduledTask  -TaskName "Happy Brain"
#   Log:        brain\logs\stt_server.log

param([switch]$Uninstall)

$taskName = "Happy Brain"

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    "Removed scheduled task '$taskName'."
    return
}

$script = Join-Path $PSScriptRoot "start_happy.ps1"
$user   = "$env:USERDOMAIN\$env:USERNAME"

$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`"" `
    -WorkingDirectory $PSScriptRoot

$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$trigger.Delay = "PT30S"   # give Windows and the network 30 s after logon

$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew

# Interactive = runs in your session, as you, only while you're logged on (no password stored)
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

"Installed scheduled task '$taskName'. It starts 30 s after you log on."
"Start it now with:  Start-ScheduledTask -TaskName '$taskName'"
