# Starts Happy's brain on the PC:
#   1. LM Studio's API server (port 1234)
#   2. stt_server.py         (port 8000)  - Whisper + Gemma
#
# Run by hand:   powershell -ExecutionPolicy Bypass -File start_happy.ps1
# Or at logon:   see install_autostart.ps1
# Log file:      brain\logs\stt_server.log

param(
    [string]$WhisperModel = "small",
    [string]$Language     = "en",
    [int]   $Port         = 8000
)

$ErrorActionPreference = "Stop"
$here   = $PSScriptRoot
$logDir = Join-Path $here "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir "stt_server.log"

function Say($msg) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $msg"
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding utf8
}

Set-Content $log -Value "" -Encoding utf8   # fresh log every start
Say "Starting Happy brain"

# --- 1. LM Studio server ------------------------------------------------------
$lms = Join-Path $env:USERPROFILE ".lmstudio\bin\lms.exe"
if (Test-Path $lms) {
    # lms prints even its success message on stderr; let cmd merge it so PowerShell doesn't treat it as an error
    cmd /c "`"$lms`" server start 2>&1" | ForEach-Object { Say "lms: $_" }
    # wait until it answers (LM Studio may need a moment after logon)
    # (curl.exe, not Invoke-RestMethod, which can hang on localhost in Windows PowerShell 5.1)
    $up = $false
    for ($i = 0; $i -lt 60 -and -not $up; $i++) {
        $code = curl.exe -s -m 3 -o NUL -w "%{http_code}" http://localhost:1234/v1/models
        if ($code -eq "200") { $up = $true } else { Start-Sleep -Seconds 2 }
    }
    if ($up) { Say "LM Studio is up" } else { Say "LM Studio did not answer within 2 minutes - continuing anyway" }
} else {
    Say "lms.exe not found - start LM Studio's server yourself (Developer tab)"
}

# --- 2. stt_server.py ---------------------------------------------------------
if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
    Say "Port $Port is already in use - stt_server.py is probably running already. Exiting."
    exit 0
}

Say "Starting stt_server.py on port $Port (Whisper '$WhisperModel', language '$Language')"
Set-Location $here
$env:PYTHONIOENCODING = "utf-8"
# -u = unbuffered, so lines appear in the log immediately.
# cmd does the redirection so the log stays plain UTF-8 (PowerShell 5.1 would write UTF-16).
cmd /c "py -3.12 -u stt_server.py --model $WhisperModel --language $Language --port $Port --save-dir `"$logDir`" >> `"$log`" 2>&1"
Say "stt_server.py exited with code $LASTEXITCODE"
exit $LASTEXITCODE
