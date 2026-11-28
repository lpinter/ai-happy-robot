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
| `firmware/stt_client/stt_client.ino` | ESP32 sketch: listens, sends audio, plays the answer, shows Happy's face on the OLED |
| `../examples/personality.md` | Happy's character. Gemma reads it before every answer |
| `brain/memory.md` | What Happy has learned from you. Gemma reads it before every answer, and Happy adds to it |

## What you need first

- The robot is wired as in the main [README](../README.md), and `mic_test` and `amp_test` both pass.
  At minimum you need the ESP32-S3, the INMP441 mic and the MAX98357A amp with a speaker.
- A Windows PC with an Nvidia graphics card. It also works without one, just much slower.
- LM Studio with the Gemma model downloaded (see pinter.org/archives/17888 and /18014).
- Arduino IDE with the "esp32 by Espressif Systems" boards package, version 4.0.0 or newer.
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

## Step 2: Create a virtual environment and install PyTorch with GPU support

The packages go into a virtual environment in `brain\.venv`, not into the global Python. That keeps
them from clashing with other programs that use Python, such as FreeCAD.

```powershell
cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
py -3.12 -m venv .venv
```

From now on, run Python as `.venv\Scripts\python.exe` from the `brain` folder. Or activate the
environment once per PowerShell window with `.venv\Scripts\Activate.ps1`, after which `python`
uses the environment.

PyTorch is the engine that runs Whisper and Kokoro. Install it **before** the other packages, or
pip installs a CPU-only version.

```powershell
.venv\Scripts\python.exe -m pip install torch --index-url https://download.pytorch.org/whl/cu126
```

**Test:**
```powershell
.venv\Scripts\python.exe -c "import torch; print(torch.__version__, torch.cuda.is_available())"
```
Expected: something like `2.x.x+cu126 True`.
If it ends with `False`, PyTorch can't see the graphics card. Update the Nvidia driver and try
again. Everything still works on the CPU, just slower.

## Step 3: Install Whisper (and Kokoro)

```powershell
cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
.venv\Scripts\python.exe -m pip install -r requirements.txt
```
This also installs Kokoro, Happy's voice, because the server loads both. The voice is covered in
the [text-to-speech guide](../text-to-speech/README.md).

**Test:**
```powershell
.venv\Scripts\python.exe -c "import whisper, kokoro, soundfile; print('all good')"
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
.venv\Scripts\python.exe stt_server.py
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

### No Wi-Fi router? Happy makes its own network

If Happy can't reach the router within 20 seconds, it starts its own Wi-Fi network:
```
Wi-Fi: not connected (status 1: network not found - check the name, and that it is 2.4 GHz).
Wi-Fi: started Happy's own network "Happy-Robot" (password "happy1234"), Happy's IP 192.168.4.1
```
1. On the PC, join the **Happy-Robot** Wi-Fi network (password `happy1234`). Windows warns
   "No internet". That's fine: Whisper, LM Studio and Kokoro all run on the PC.
2. Make sure `stt_server.py` is running (it already listens on all networks).
3. The Serial Monitor shows `Wi-Fi: a computer joined Happy's network, IP 192.168.4.2`. From then
   on Happy sends to that address, using the port and path from `STT_URL`. You don't need to edit it.

To change the network name or password, add `AP_SSID` and `AP_PASS` to `secrets.h` (see
`secrets.example.h`). The password needs at least 8 characters. Happy stays on its own network
until it restarts. Press **RST** to try the router again.

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

### Happy's face

If the 1.5" OLED is wired as in the main [README](../README.md) (and `oled_face` passes), the
sketch shows Happy's face while you talk:

| Face | When |
|------|------|
| Eyes closed | Connecting to Wi-Fi |
| Smile, blinking | Waiting for you to talk |
| Big eyes, lips slightly parted | Listening (recording) |
| Eyes looking up, "..." | Thinking: the PC is running Whisper, Gemma and Kokoro |
| Two lips that part with the sound | Speaking |
| Frown | The PC didn't answer (`HTTP error ...`), shown for 1.5 s |

The face runs in its own task, so it doesn't slow down listening or speaking. Without a display,
set `SHOW_FACE = false` at the top of the face section.

### Looking toward a sound

With the second INMP441 wired as in the main [README](../README.md) (L/R to 3V3, on the same
SCK/WS/SD wires as the first mic), Happy's eyes turn toward the sound: a sound from Happy's left
makes it look left, a sound from its right makes it look right. The eyes stay that way while
Happy thinks and answers, and glide back to the middle 4 seconds later.

How it works (based on [esp32_sound_direction](https://github.com/entron/esp32_sound_direction)):
a sound from the side reaches one mic a fraction of a millisecond before the other. The sketch
compares the two mics at every possible delay, picks the delay where they match best, and turns
that delay into an angle. Two mics only tell left from right; they can't tell front from back.

To set it up, mount the **L/R to GND** mic on Happy's **left** and the **L/R to 3V3** mic on its
**right** (seen from the robot), both facing forward, and measure the distance between the two
mic holes. Then set `SHOW_DIRECTION = true`, snap your fingers or talk to one side, and watch the
Serial Monitor: `direction: +40.0 deg (left)`.

| Setting | Default | Change it when... |
|---------|---------|-------------------|
| `LOOK_AT_SOUND` | `true` | You have only one mic (`false`): the eyes stay in the middle |
| `MIC_DISTANCE_M` | `0.14` | Always: set the distance between your two mic holes, in metres. 10–15 cm works well |
| `LOOK_FLIP` | `false` | The eyes look away from the sound: set `true` (or swap the two mics) |
| `MIN_CORR` | `0.5` | It always prints `unsure` (lower it, e.g. `0.3`) or the eyes jump around on noise (raise it) |
| `DEAD_ZONE_DEG` | `15` | The eyes twitch when you sit straight in front of Happy (raise it) |
| `EYE_LOOK_PX` | `18` | The eyes should move further or less far to the side |
| `GAZE_HOLD_MS` | `4000` | Happy should look back to the middle sooner or later |

If it always prints `unsure`, check that the second mic works: in `mic_test` the bar shows the
louder mic, so talking right next to one mic and then the other should switch the label between
`L` and `R`.

Microphone settings you can adjust in the sketch:

| Setting | Default | Change it when... |
|---------|---------|-------------------|
| `MIC_SLOT` | `0` | Whisper always hears nothing and `peak` in the server window is near 0: try `1` |
| `GAIN_SHIFT` | `14` | The recording is too quiet (`13`) or distorted (`15`) |
| `MAX_SECONDS_QUIET` | `20` | The longest recording in a quiet room. You want longer questions (raise it). It's only a safety limit: a recording normally ends at the first pause |
| `MAX_SECONDS_NOISY` | `10` | The longest recording in a noisy room, where the noise could keep a recording going |
| `QUIET_ROOM_NOISE` | `100` | The room counts as quiet while its base noise is below this. Set `SHOW_LEVELS = true` and read `noise` in a room you call quiet and one you call noisy, then pick a value between them. Each recording prints `quiet room, up to 20 s` or `noisy room, up to 10 s` |
| `HANDS_FREE` | `true` | You want the BOOT button only (`false`) |
| `MIN_LEVEL` | `300` | Happy starts recording on background noise (raise it) or misses quiet speech (lower it). Set `SHOW_LEVELS = true` to see the levels in the Serial Monitor |
| `SILENCE_MS` | `800` | Happy cuts you off mid-sentence (raise it) or waits too long after you stop (lower it). After this much quiet the recording goes to the PC straight away |
| `END_FACTOR` | `2.0` | While waiting for "Happy", the sketch measures the room's base noise. A recording ends when your voice sinks back to this many times that level. Raise it (e.g. `3.0`) if the recording runs to its time limit although you stopped; lower it (e.g. `1.5`) if quiet word endings get cut |
| `END_MIN_LEVEL` | `200` | The recording ends below this loudness even if 2× the base noise is lower (quiet rooms). Raise it (e.g. `250`) if quiet-room recordings still run to `MAX_SECONDS_QUIET`; lower it (e.g. `120`) if quiet word endings get cut. Keep it below `MIN_LEVEL` |
| `NOISY_END_FACTOR` | `1.2` | Noisy rooms only. While waiting, the sketch also measures how loud the noise's own loud moments get (the level that `NOISE_PEAK_PERCENTILE`, `0.9`, of the waiting time was quieter than). The recording ends when you sink below this many times that level, but never above half your own loudness (`NOISY_END_MAX`, `0.5`). Raise it (e.g. `1.5`) if noisy-room recordings still run to `MAX_SECONDS_NOISY`; lower it (e.g. `1.0`) if you get cut off there |
| `NOISY_VOICE_RESET_MS` | `150` | Noisy rooms only: the `VOICE_RESET_MS` used there. Short bursts of noise no longer count as you talking again. Raise it (e.g. `250`) if noise still keeps the recording going |
| `FOLLOW_SPEAKER_NOISY` | `true` | Needs both mics. In a noisy room, the direction you said "Happy" from is yours; afterwards only talking from within `SPEAKER_TOLERANCE_DEG` (`20`) of it keeps the recording going, so other people talking elsewhere don't. `false` = go by loudness only. `FOLLOW_SPEAKER_QUIET` does the same in quiet rooms (off by default) |
| `SPEAKER_TOLERANCE_DEG` | `20` | You get cut off while still talking (raise it, e.g. `30`; also check `MIC_DISTANCE_M`) or someone close to your side still keeps the recording going (lower it, e.g. `12`) |
| `UNSURE_IS_SPEAKER` | `false` | Sound with no clear direction (far-away chatter, echoes) doesn't count as you. Set `true` if you get cut off in a room with strong echoes |
| `NOISE_WINDOW_MS` | `3000` | How long a stretch of waiting the base noise is measured over. The noise level is the one that `NOISE_PERCENTILE` (`0.3`) of that time was quieter than |
| `NOISE_CLIP_MS` | `500` | How much pure room noise goes to the PC with every recording, for its noise filter (`0` = none; the PC then uses the quietest moments of the recording) |
| `SILENCE_DROP` | `0` | Off by default. Set it to e.g. `0.1` if the room gets louder while you speak (a fan switches on) and the recording runs on: quiet then also counts as falling below this share of how loud you were speaking |
| `VOICE_RESET_MS` | `50` | Clicks or bumps keep the recording going (raise it) |
| `FOLLOW_UP_MS` | `8000` | Happy keeps answering things not meant for it (`0` = always need the name) |

The name Happy answers to is set on the server: `py stt_server.py --wake-words happy,robot`
(`--wake-words ""` answers everything it hears).

To hear exactly what the microphone picked up, play `last_recording.wav`. It's in the `brain`
folder when you start the server by hand, and in `brain\logs` when it runs from autostart.

**Noise filter.** Before Whisper listens, the server removes the room's steady noise (fans, hum,
hiss) with the noise clip the ESP32 recorded while waiting for "Happy". `last_recording_clean.wav`,
next to `last_recording.wav`, is what Whisper actually heard, and the log shows
`denoise: noise profile from room`. If the words sound dull or watery in the clean file, lower
`--denoise-strength` (default `1.5`, e.g. `1.0`); if noise is still there, raise it (e.g. `2.5`).
`--no-denoise` turns the filter off.

### What Happy remembers

Happy only keeps the last few exchanges in its head (`--history`, default 6), and it forgets them
when the server restarts. Things worth keeping (your friends' names, a corrected pronunciation, a new
word) go into `brain/memory.md`: Happy adds a hidden `[REMEMBER: ...]` note to its answer, the
server removes it before speaking, saves the fact, and prints `memory : ...` in the log.

- See the list: open `brain/memory.md`, or `curl.exe http://localhost:8000/memory`
- Fix or forget something: edit or delete the line in `memory.md`. Changes apply on the next answer, no restart needed
- Teach on purpose: "Happy, remember that Jane is our friend"
- Turn it off: `py stt_server.py --memory ""`

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
| `python` opens the Microsoft Store | Use `.venv\Scripts\python.exe` from the `brain` folder (or `py -3.12` for Step 2) |
| `curl : A parameter cannot be found that matches parameter name 'H'` | Use `curl.exe`, not `curl` |
| `Unable to connect` on port 1234 | LM Studio's server isn't running: Step 4 |
| `"reply": ""` and `llm_error` in the JSON | Same: LM Studio's server isn't running, or the model name is wrong (`--lm-model`) |
| `Port 8000 is busy` | Another copy is running (often the autostart task). Stop it as in Step 10 |
| ESP32: `HTTP error -1` (connection refused) | Wrong PC IP in `STT_URL`, the firewall rule from Step 7 is missing, or the server isn't running |
| ESP32: `HTTP error -11` (read timeout) | The PC is too slow or the model is still loading. Try again, or use `--model base.en` |
| ESP32 stuck on `Wi-Fi: connecting....` | Wrong Wi-Fi name or password, or a 5 GHz-only network |
| ESP32 starts `Happy-Robot` although the router is on | Same as above: it couldn't join the router within 20 s. Fix `secrets.h` and press RST |
| `No computer has joined "Happy-Robot" yet` | Join the Happy-Robot Wi-Fi from the PC first |
| On Happy-Robot: `HTTP error -1` | The firewall rule from Step 7 is missing, or the server isn't running |
| `No PSRAM!` | Tools → PSRAM → **OPI PSRAM**, then upload again |
| `You:` is empty / `peak` near 0 | Mic wiring (SD on GPIO 1, L/R to GND) or set `MIC_SLOT = 1` |
| Whisper gets the words wrong | Play `last_recording.wav` and tune `GAIN_SHIFT`; speak 20–30 cm from the mic; try `--model turbo` |
| Answers are slow | Gemma and Whisper share the GPU. Use `--model base.en` or a smaller Gemma |
| Happy calls itself by the wrong name | Edit `../examples/personality.md`, then restart the server |
| Text is right but there's no sound, or the sound is bad | See the [text-to-speech guide](../text-to-speech/README.md) |
