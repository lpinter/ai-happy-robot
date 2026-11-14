# Happy: hearing and thinking (speech-to-text)

How to install and test Happy's ears (Whisper) and brain (Gemma in LM Studio), one step at a time:

```
 ESP32-S3 (body)                              Windows PC (brain)
 ───────────────                              ──────────────────
 "Happy, ..."
 INMP441 mic ──── 16 kHz audio over Wi-Fi ──► stt_server.py  POST /talk
                                                 1. Whisper     speech -> text    ◄── this guide
                                                 2. Gemma       text -> answer    ◄── this guide (LM Studio, port 1234)
                                                 3. Kokoro      answer -> speech  (text-to-speech guide)
 MAX98357A speaker ◄──── spoken answer ──────────┘
```

This folder has all the code: one server and one ESP32 sketch handle hearing, thinking and talking.
For Happy's **voice** (Kokoro and the speaker), follow this guide first, then the
[text-to-speech guide](../text-to-speech/README.md).

Each step ends with a **Test**. Don't move on until the test passes: when something breaks
later, you'll know the earlier parts are fine.

Run all commands in **PowerShell** (Start menu → "PowerShell"), not in the old Command Prompt.

## Files

| File | What it does |
|------|--------------|
| `brain/stt_server.py` | The PC server: Whisper + LM Studio + Kokoro |
| `brain/requirements.txt` | Python packages for the server |
| `brain/start_happy.ps1` | Starts LM Studio's server, then `stt_server.py`, and logs to `brain/logs/stt_server.log` |
| `brain/install_autostart.ps1` | Makes Windows run `start_happy.ps1` every time you log on |
| `firmware/stt_client/stt_client.ino` | ESP32 sketch: push-to-talk, sends audio, plays the answer |
| `../examples/personality.md` | Happy's character. Gemma reads it before every answer |

## What you need first

- The robot is wired as in the main [README](../README.md), and `mic_test` and `amp_test` both pass.
  At minimum you need the ESP32-S3, the INMP441 mic and the MAX98357A amp with a speaker.
- A Windows PC with an Nvidia graphics card. It also works without one, just much slower.
- LM Studio with the Gemma model downloaded (see pinter.org/archives/17888 and /18014).
- Arduino IDE with the "esp32 by Espressif Systems" boards package, version 3.x.
- The PC and the ESP32 on the **same Wi-Fi network**.

---

## Step 1: Install Python 3.12

```powershell
winget install Python.Python.3.12
```

Close PowerShell and open a new window so it picks up the new install.

**Test:**
```powershell
py -3.12 --version
```
Expected: `Python 3.12.x`

> Always use `py -3.12`, not `python`. On many Windows PCs, `python` opens the Microsoft Store
> instead of running Python.

## Step 2: Install PyTorch with GPU support

PyTorch is the engine that runs Whisper and Kokoro. Install it **before** the other packages, or
pip installs a CPU-only version.

```powershell
py -3.12 -m pip install torch --index-url https://download.pytorch.org/whl/cu126
```

**Test:**
```powershell
py -3.12 -c "import torch; print(torch.__version__, torch.cuda.is_available())"
```
Expected: something like `2.x.x+cu126 True`.
If it ends with `False`, PyTorch can't see the graphics card. Update the Nvidia driver and try
again. Everything still works on the CPU, just slower.

## Step 3: Install Whisper (and Kokoro)

```powershell
cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
py -3.12 -m pip install -r requirements.txt
```
This also installs Kokoro, Happy's voice, because the server loads both. The voice is covered in
the [text-to-speech guide](../text-to-speech/README.md).

**Test:**
```powershell
py -3.12 -c "import whisper, kokoro, soundfile; print('all good')"
```
Expected: `all good`

## Step 4: Start LM Studio's server

LM Studio has to be open **and** its API server has to be running. These are two separate things.

Either:
- In LM Studio, open the **Developer** tab and switch **Status** to **Running**, or
- run:
  ```powershell
  & "$env:USERPROFILE\.lmstudio\bin\lms.exe" server start
  ```

**Test 1: is the server there?**
```powershell
curl.exe http://localhost:1234/v1/models
```
Expected: a list that includes `google/gemma-4-26b-a4b-qat`.
If you see "Unable to connect" or "Failed to connect", the server isn't running.

**Test 2: does Gemma answer?**
```powershell
'{"model":"google/gemma-4-26b-a4b-qat","messages":[{"role":"user","content":"Say hi in one sentence"}],"reasoning_effort":"none"}' | curl.exe http://localhost:1234/v1/chat/completions -H "Content-Type: application/json" --data-binary "@-"
```
Expected: a JSON answer with `"content": "Hi there! ..."`. The first call can take up to a
minute while the model loads.

> In PowerShell, always type `curl.exe`. Plain `curl` is a PowerShell alias for a different
> command that doesn't understand `-H`.
>
> JSON is **piped** into curl (`'{...}' | curl.exe ... --data-binary "@-"`) instead of passed
> with `-d`. Windows PowerShell breaks `-d` text that contains both quotes and spaces into
> separate words, and curl then fails with `Could not resolve host`.

## Step 5: Start the Happy server by hand

```powershell
cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
py -3.12 stt_server.py
```

The **first start downloads the models** (Whisper ~460 MB, Kokoro ~330 MB, plus a small language
pack), which takes a few minutes. Later starts take about 20 seconds. It's ready when it prints:
```
Listening on http://0.0.0.0:8000  (POST /talk /stt /chat /say, GET /health)
```

Leave this window open: it shows everything Happy hears and says.
**Open a second PowerShell window** for the tests below.

Useful options (add them after `stt_server.py`):

| Option | Default | Meaning |
|--------|---------|---------|
| `--model` | `small` | Whisper size: `base.en` (faster), `small`, `turbo` (more accurate, more GPU memory) |
| `--language` | `en` | Language you speak to Happy, or `auto` |
| `--port` | `8000` | Port the ESP32 connects to |
| `--no-tts` | off | Skip loading Happy's voice. Starts faster, but the robot gets no spoken answer |

The voice options (`--voice`, `--speed`) are in the [text-to-speech guide](../text-to-speech/README.md).

**Test:**
```powershell
curl.exe http://localhost:8000/health
```
Expected:
```json
{"ok": true, "model": "small", "cuda": true, "llm": "google/gemma-4-26b-a4b-qat", "voice": "af_heart"}
```
If the server window says `Port 8000 is busy`, another copy is already running, for example the
autostart task from Step 10. Stop it first (see Step 10).

## Step 6: Test hearing and thinking on the PC

Run these in the second PowerShell window, from any folder you like (e.g. `cd $env:TEMP`).
Each test checks one more piece.

### 6a. The brain (LM Studio) — `/chat`
```powershell
'{"text":"Hi Happy, who are you?"}' | curl.exe -X POST http://localhost:8000/chat -H "Content-Type: application/json" --data-binary "@-"
```
Expected: `"reply": "I am Happy, a little robot made from spare parts. ..."`.
If you get `llm_error` instead, go back to Step 4.

### 6b. Make a test recording
Windows can speak a sentence into a WAV file, so you don't need a microphone yet:
```powershell
Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; $s.SetOutputToWaveFile("$PWD\test.wav"); $s.Speak('Hello Happy, how are you today?'); $s.Dispose()
```
Paste this straight into PowerShell. Don't wrap it in `powershell -c "..."`, or the `$` variables
get emptied and you'll get a parser error.

To use your own voice instead, record a **16-bit WAV** with Audacity or a similar program.
Windows Voice Recorder saves `.m4a`, which the server can't read.

### 6c. The ears (Whisper) — `/stt`
```powershell
curl.exe -X POST -H "Content-Type: audio/wav" --data-binary "@test.wav" http://localhost:8000/stt
```
Expected: `"text": "Hello Happy, how are you today?"` plus Happy's `"reply"`.
The server window shows what it heard, how loud it was (`peak`), and how long each part took:
```
[127.0.0.1] 2.98s audio, peak 0.947, stt 0.6s
  you    : Hello Happy, how are you today?
  happy  : I am doing great because my gears are spinning smoothly today. How are you feeling?
```
Keep `test.wav`: the text-to-speech guide uses it too.

## Step 7: Let the robot reach the PC (firewall)

Windows Firewall blocks other devices from reaching port 8000. Allow it once, in a PowerShell
started with **Run as administrator**:
```powershell
New-NetFirewallRule -DisplayName "Happy STT" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow
```

Find your PC's address:
```powershell
ipconfig
```
Look for the **IPv4 Address** of your Wi-Fi or Ethernet adapter, e.g. `192.168.1.50`.

**Test:** on your phone, connected to the **same Wi-Fi**, open `http://<PC-IP>:8000/health` in
the browser. You should see the same JSON as in Step 5. If the phone can't load it, the ESP32
can't reach the PC either.

> Tip: give the PC a fixed address in your router (a DHCP reservation). Otherwise the address can
> change and the robot stops finding it.

## Step 8: Program the robot

1. Open `firmware/stt_client/stt_client.ino` in the Arduino IDE.
2. Edit the three lines at the top:
   ```cpp
   const char* WIFI_SSID = "your-wifi";
   const char* WIFI_PASS = "your-password";
   const char* STT_URL   = "http://192.168.1.50:8000/talk";   // your PC's IPv4, path /talk
   ```
   The ESP32-S3 only connects to **2.4 GHz** Wi-Fi.
3. Choose these settings in the **Tools** menu:
   - Board: **ESP32S3 Dev Module**
   - Flash Size: **16MB**
   - PSRAM: **OPI PSRAM**. This one is required: recordings and answers are kept in PSRAM.
   - Port: your board's COM port, e.g. `COM5`
4. Plug in USB-C on the **TTL** (right) port and click **Upload**.

**Test:** open the **Serial Monitor** at **115200** baud and press **RST** on the board. Expected:
```
Wi-Fi: connecting to your-wifi....
Wi-Fi: connected, IP 192.168.1.xx
Ready. Say "Happy, ..." (or hold BOOT, talk, release).
```

## Step 9: Talk to Happy

Just talk: say something like "Hello Happy, what's your favorite color?" Happy starts recording
when it hears speech and stops after a short pause (0.8 s). It only answers when Whisper hears
its name, so ordinary talk in the room is transcribed but ignored:
```
Listening...
Recorded 1.9 s, sending...
You:   Can you pass me the salt?
(not talking to Happy - say "Happy" to wake it)
```
For 8 seconds after an answer you can reply without saying "Happy" again, so a conversation flows.

Holding the **BOOT** button still works as push-to-talk (hold, talk, let go), and then the name
isn't needed.

Expected in the Serial Monitor:
```
Listening...
Recorded 2.8 s, sending...
You:   Hello Happy, what's your favorite color?
Happy: I really love the bright color of shiny copper. Do you have a favorite color too?
Speaking 5.1 s...
```
The `You:` line proves that hearing works, and the `Happy:` line proves that thinking works.
The server window shows the same conversation. If the speaker is wired, you also hear the answer;
setting up and tuning the sound is in the [text-to-speech guide](../text-to-speech/README.md).

Microphone settings you can adjust in the sketch:

| Setting | Default | Change it when... |
|---------|---------|-------------------|
| `MIC_SLOT` | `0` | Whisper always hears nothing and `peak` in the server window is near 0: try `1` |
| `GAIN_SHIFT` | `14` | The recording is too quiet (`13`) or distorted (`15`) |
| `MAX_SECONDS` | `10` | You want longer questions |
| `HANDS_FREE` | `true` | You want the BOOT button only (`false`) |
| `MIN_LEVEL` | `300` | Happy starts recording on background noise (raise it) or misses quiet speech (lower it). Set `SHOW_LEVELS = true` to see the levels in the Serial Monitor |
| `SILENCE_MS` | `800` | Happy cuts you off mid-sentence (raise it) or waits too long after you stop (lower it) |
| `FOLLOW_UP_MS` | `8000` | Happy keeps answering things not meant for it (`0` = always need the name) |

The name Happy answers to is set on the server: `py stt_server.py --wake-words happy,robot`
(`--wake-words ""` answers everything it hears).

To hear exactly what the microphone picked up, play `last_recording.wav`. It's in the `brain`
folder when you start the server by hand, and in `brain\logs` when it runs from autostart.

## Step 10: Start everything automatically at logon

When Steps 1–9 work, let Windows start LM Studio's server and the Happy server for you.

1. Stop the server you started by hand in Step 5 (press **Ctrl+C** in its window).
2. Install the scheduled task:
   ```powershell
   cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
   powershell -ExecutionPolicy Bypass -File install_autostart.ps1
   ```
3. Start it now, without logging off:
   ```powershell
   Start-ScheduledTask -TaskName "Happy Brain"
   ```

**Test** (wait about 30 seconds for it to load):
```powershell
curl.exe http://localhost:8000/health
Get-Content .\logs\stt_server.log
```
The log should end with `Listening on http://0.0.0.0:8000 ...`. After that, every conversation
is written to this log.

The task runs hidden, starts 30 seconds after logon, and Windows restarts it up to 3 times if it
crashes.

**Restart it** after you change `stt_server.py` or `personality.md`. Stopping the task can leave
Python running, so the second command frees port 8000:
```powershell
Stop-ScheduledTask -TaskName "Happy Brain"
Get-NetTCPConnection -LocalPort 8000 -State Listen | ForEach-Object { Stop-Process -Id $_.OwningProcess }
Start-ScheduledTask -TaskName "Happy Brain"
```

**Change the Whisper model or language** for the autostart version with the `$WhisperModel` and
`$Language` settings at the top of `start_happy.ps1`. Then restart as above. To change the voice,
see the [text-to-speech guide](../text-to-speech/README.md), Step 9.

**Remove the autostart:**
```powershell
powershell -ExecutionPolicy Bypass -File install_autostart.ps1 -Uninstall
```

---

## Troubleshooting

| What you see | Likely cause and fix |
|--------------|----------------------|
| `python` opens the Microsoft Store | Use `py -3.12` instead |
| `curl : A parameter cannot be found that matches parameter name 'H'` | Use `curl.exe`, not `curl` |
| `Unable to connect` on port 1234 | LM Studio's server isn't running: Step 4 |
| `"reply": ""` and `llm_error` in the JSON | Same: LM Studio's server isn't running, or the model name is wrong (`--lm-model`) |
| `Port 8000 is busy` | Another copy is running (often the autostart task). Stop it as in Step 10 |
| ESP32: `HTTP error -1` (connection refused) | Wrong PC IP in `STT_URL`, the firewall rule from Step 7 is missing, or the server isn't running |
| ESP32: `HTTP error -11` (read timeout) | The PC is too slow or the model is still loading. Try again, or use `--model base.en` |
| ESP32 stuck on `Wi-Fi: connecting....` | Wrong Wi-Fi name or password, or a 5 GHz-only network |
| `No PSRAM!` | Tools → PSRAM → **OPI PSRAM**, then upload again |
| `You:` is empty / `peak` near 0 | Mic wiring (SD on GPIO 1, L/R to GND) or set `MIC_SLOT = 1` |
| Whisper gets the words wrong | Play `last_recording.wav` and tune `GAIN_SHIFT`; speak 20–30 cm from the mic; try `--model turbo` |
| Answers are slow | Gemma and Whisper share the GPU. Use `--model base.en` or a smaller Gemma |
| Happy calls itself by the wrong name | Edit `../examples/personality.md`, then restart the server |
| Text is right but there's no sound, or the sound is bad | See the [text-to-speech guide](../text-to-speech/README.md) |
