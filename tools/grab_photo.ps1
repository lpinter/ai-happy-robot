# Asks the bot (running firmware/tests/cam_snap) for one photo over USB serial
# and saves it as photo_<time>.jpg in the current folder. Usage: .\tools\grab_photo.ps1 [COM3]
param([string]$Port = "COM3")

$dir = (Get-Location).Path

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true
try { $sp.Open() }
catch {
    Write-Error "Can't open $Port. Close the Arduino IDE Serial Monitor (or anything else using $Port), or open a new PowerShell window, and try again. Details: $($_.Exception.InnerException.Message)"
    exit 1
}

# The port is always closed at the end, even if something fails, so the next
# run can open it again.
try {
    # Opening the port can reset the board, so keep sending "s" every 3 s and
    # show whatever the bot prints until the "JPEG <bytes>" header arrives.
    $sp.ReadTimeout = 500
    $deadline = (Get-Date).AddSeconds(20)
    $nextSend = Get-Date
    $len = $null
    while ((Get-Date) -lt $deadline) {
        if ((Get-Date) -ge $nextSend) { $sp.Write("s"); $nextSend = (Get-Date).AddSeconds(3) }
        try { $line = $sp.ReadLine().Trim() } catch { continue }
        if ($line -match '^JPEG (\d+)$') { $len = [int]$Matches[1]; break }
        if ($line) { Write-Host "Bot: $line" }
        if ($line -match '^ERROR') { throw "Bot says: $line" }
    }
    if ($null -eq $len) {
        throw "No photo from the bot on $Port within 20 s. Is the cam_snap sketch uploaded? Any 'Bot:' lines above show what it said."
    }

    $sp.ReadTimeout = 10000
    $buf = New-Object byte[] $len
    $got = 0
    while ($got -lt $len) { $got += $sp.Read($buf, $got, $len - $got) }
}
finally {
    $sp.Close()
    $sp.Dispose()
}

$file = Join-Path $dir ("photo_" + (Get-Date -Format "HHmmss") + ".jpg")
[System.IO.File]::WriteAllBytes($file, $buf)
Write-Output "Saved $len bytes to $((Resolve-Path $file).Path)"
