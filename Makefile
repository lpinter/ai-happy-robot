.PHONY: restart check-server upload

# Robot firmware upload (override on the command line, e.g. make upload PORT=COM7)
PORT   ?= COM5
FQBN   ?= esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M
SKETCH := speech-to-text/firmware/stt_client

# Restart the Happy Brain server (scheduled task) and free port 8000
restart:
	powershell -NoProfile -Command "Stop-ScheduledTask -TaskName 'Happy Brain'; Get-NetTCPConnection -LocalPort 8000 -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id \$$_.OwningProcess -Force }; Start-Sleep 2; Start-ScheduledTask -TaskName 'Happy Brain'"

check-server:
	curl http://localhost:8000/health

BAUD   ?= 115200

# Compile and upload the stt_client sketch to the robot (ESP32-S3).
# A serial monitor holding the port is stopped first and reopened after the upload.
upload:
	powershell -NoProfile -ExecutionPolicy Bypass -File tools/upload.ps1 -Port $(PORT) -Fqbn "$(FQBN)" -Sketch $(SKETCH) -Baud $(BAUD)
