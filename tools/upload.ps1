# Compile and upload a sketch. If a serial monitor is holding the port, stop it
# first and open a monitor again after the upload.
#   powershell -File tools/upload.ps1 -Port COM5 -Fqbn <fqbn> -Sketch <dir> [-Baud 115200]
param(
    [Parameter(Mandatory)][string]$Port,
    [Parameter(Mandatory)][string]$Fqbn,
    [Parameter(Mandatory)][string]$Sketch,
    [int]$Baud = 115200
)

# Serial monitors: the Arduino IDE's (serial-monitor.exe, run by its arduino-cli daemon)
# and any standalone "arduino-cli monitor" window.
$monitors = Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'serial-monitor.exe' -or
    ($_.Name -eq 'arduino-cli.exe' -and $_.CommandLine -match '\smonitor(\s|$)')
}

if ($monitors) {
    Write-Host "Stopping serial monitor so the upload can use $Port..."
    $monitors | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 1
}

arduino-cli compile --fqbn $Fqbn --upload --port $Port $Sketch
$result = $LASTEXITCODE

if ($monitors) {
    Write-Host "Opening serial monitor on $Port at $Baud baud..."
    Start-Sleep -Seconds 2   # let the board reboot and the USB port come back
    Start-Process arduino-cli -ArgumentList 'monitor', '--port', $Port, '--config', "baudrate=$Baud"
}

exit $result
