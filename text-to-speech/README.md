# Happy: talking (text-to-speech)

How to install and test Happy's voice, one step at a time. Happy speaks with
[Kokoro](https://huggingface.co/hexgrad/Kokoro-82M), a small text-to-speech model that runs
locally on your PC.

```
 ESP32-S3 (body)                              Windows PC (brain)
 ───────────────                              ──────────────────
 INMP441 mic ──── 16 kHz audio over Wi-Fi ──► stt_server.py  POST /talk
                                                 1. Whisper     speech -> text   (speech-to-text guide)
                                                 2. Gemma       text -> answer   (speech-to-text guide)
                                                 3. Kokoro      answer -> speech  ◄── this guide
 MAX98357A speaker ◄── 16 kHz spoken answer ─────┘
```

**The code lives in the [`speech-to-text`](../speech-to-text) folder.** One server
(`stt_server.py`) and one ESP32 sketch (`stt_client.ino`) do the hearing, the thinking and the
talking. This guide covers only the talking part.

Each step ends with a **Test**. Don't move on until it passes.
Run all commands in **PowerShell**, and always type `curl.exe`, not `curl`.

## What you need first

- **Steps 1–5 of the [speech-to-text guide](../speech-to-text/README.md) are done**:
  Python 3.12, PyTorch, the packages from `requirements.txt` (which include Kokoro), LM Studio's
  server, and `stt_server.py` running and answering `/health`.
- The MAX98357A amp and speaker are wired as in the main [README](../README.md):
  BCLK → GPIO 40, LRC → GPIO 39, DIN → GPIO 38, VIN → 5V, GND → GND.

---

## Step 1: Check that Kokoro is installed

```powershell
py -3.12 -c "import kokoro, soundfile; print('kokoro ok')"
```
Expected: `kokoro ok`

If you get `ModuleNotFoundError`, install it:
```powershell
py -3.12 -m pip install kokoro soundfile
```

## Step 2: Check that the server loaded the voice

With `stt_server.py` running (speech-to-text guide, Step 5):
```powershell
curl.exe http://localhost:8000/health
```
Expected: the JSON ends with `"voice": "af_heart"`.

If it says `"voice": null`, the server was started with `--no-tts`. Start it without that option.

When the server starts you'll see `Loading Kokoro voice 'af_heart' ...`. The **first start
downloads the Kokoro model** (~330 MB) and an English language pack, which takes a few minutes.
You can ignore any `Warning: You are sending unauthenticated requests to the HF Hub` line.

## Step 3: Hear Happy's voice on your PC

`/say` reads out any text you send it and returns a WAV file.

Run this from a folder where you can save files, e.g. `cd $env:TEMP`:
```powershell
curl.exe -X POST http://localhost:8000/say -H "Content-Type: application/json" -d '{\"text\":\"Hello, I am Happy! Nice to meet you.\"}' -o say.wav
start say.wav
```
Expected: your PC plays Happy's voice. The server window shows `say    : Hello, I am Happy! ...`.

## Step 4: Pick a voice and speed

Stop the server (**Ctrl+C**) and start it again with a different voice:
```powershell
cd D:\_Laci\_Current\Projects\Git\AI\ai-happy-robot\speech-to-text\brain
py -3.12 stt_server.py --voice am_michael
```
Then repeat Step 3 to hear it.

Some voices to try:

| Voice | Sounds like |
|-------|-------------|
| `af_heart` (default) | American, female, warm |
| `af_bella` | American, female |
| `af_nicole` | American, female, soft |
| `am_michael` | American, male |
| `am_puck` | American, male, playful |
| `bf_emma` | British, female |
| `bm_george` | British, male |

The first letter picks the accent (`a` = American, `b` = British) and the second the gender.
Add `--speed 1.1` to talk faster or `--speed 0.9` to talk slower.

## Step 5: Have Happy answer out loud on your PC

Add `\"chat\": true` and Happy answers the text, through Gemma, instead of just reading it:
```powershell
curl.exe -X POST http://localhost:8000/say -H "Content-Type: application/json" -d '{\"text\":\"What is your favorite color?\", \"chat\": true}' -o answer.wav
start answer.wav
```
Expected: you hear something like "I really love the bright color of shiny copper…".
If you hear "Sorry, my brain is not answering right now", LM Studio's server isn't running
(speech-to-text guide, Step 4).

## Step 6: Test the whole loop the robot uses (`/talk`)

`/talk` takes a recording and sends back Happy's spoken answer as raw sound for the robot's
speaker. Make `test.wav` as in the speech-to-text guide, Step 6b, then:
```powershell
curl.exe -X POST -H "Content-Type: audio/wav" --data-binary "@test.wav" -D - -o reply.pcm http://localhost:8000/talk
```
Expected, among the headers it prints:
```
Content-Type: audio/L16; rate=16000; channels=1
X-Happy-Text: Hello Happy, how are you today?
X-Happy-Reply: I am doing great ...
```
`reply.pcm` should be well over 0 bytes: one second of speech is 32,000 bytes. A normal player
can't open this file; that's expected, and Step 3 is the test for how it sounds.

The server window shows how long each part took, about 4–5 seconds in total:
```
  you    : Hello Happy, how are you today?
  happy  : I am doing great because my gears are spinning smoothly today. How are you feeling?
  voice  : 5.0s of speech in 0.59s
```

## Step 7: Check the robot's speaker

Before using the full sketch, make sure the amp and speaker work on their own:

1. Upload `firmware/tests/amp_test/amp_test.ino` from the main repo.
2. You should hear a short rising four-note chirp every 3 seconds.

If you hear nothing, check the amp wiring (BCLK 40, LRC 39, DIN 38, VIN to **5V**, not 3.3 V)
and the speaker on the screw terminal.

## Step 8: Hear Happy on the robot

1. Upload `speech-to-text/firmware/stt_client/stt_client.ino`, following the speech-to-text
   guide, Steps 7 and 8 (firewall, Wi-Fi settings, `STT_URL` ending in **`/talk`**, OPI PSRAM).
2. Open the Serial Monitor at **115200**, hold **BOOT**, ask a question, and let go.

Expected in the Serial Monitor:
```
Listening...
Recorded 2.8 s, sending...
You:   Hello Happy, what's your favorite color?
Happy: I really love the bright color of shiny copper. Do you have a favorite color too?
Speaking 5.1 s...
```
…and you hear the answer from the speaker.

Sound settings at the top of the sketch:

| Setting | Default | Change it when... |
|---------|---------|-------------------|
| `VOLUME` | `70` | It distorts, or the board resets while talking (lower it); it's too quiet (raise it, max 100) |
| `MAX_REPLY_SECONDS` | `30` | Answers get cut off (raise it; each second uses 32 KB of PSRAM) |

## Step 9: Keep your voice settings in autostart

If you use the autostart task (speech-to-text guide, Step 10), it doesn't use the options you
typed by hand. To keep your voice, open `speech-to-text/brain/start_happy.ps1` and add the
options to the line starting with `cmd /c "py -3.12 -u stt_server.py ...`, for example:
```
... stt_server.py --voice am_michael --speed 1.1 --model ...
```
Then restart the task:
```powershell
Stop-ScheduledTask -TaskName "Happy Brain"
Get-NetTCPConnection -LocalPort 8000 -State Listen | ForEach-Object { Stop-Process -Id $_.OwningProcess }
Start-ScheduledTask -TaskName "Happy Brain"
```

**Test:**
```powershell
curl.exe http://localhost:8000/health
```
`"voice"` should show your new voice.

---

## Troubleshooting

| What you see | Likely cause and fix |
|--------------|----------------------|
| `ModuleNotFoundError: No module named 'kokoro'` | Step 1 |
| `"voice": null` in `/health` | The server was started with `--no-tts` |
| `/say` returns `text-to-speech is off` | Same as above |
| `say.wav` is 0 bytes or won't play | Check the server window for an error. Make sure you used `curl.exe` and `-o say.wav` |
| You hear "Sorry, my brain is not answering right now" | LM Studio's server isn't running: speech-to-text guide, Step 4 |
| Serial Monitor shows `(nothing to say)` | Whisper heard nothing, so there was no answer. See the speech-to-text guide's troubleshooting |
| Serial Monitor shows `Happy: ...` but the speaker is silent | Amp wiring; check with `amp_test` (Step 7) |
| Clicks or a hum between answers | Normal for the MAX98357A when idle; keep the speaker wires short and away from the mic |
| Sound is distorted, or the board resets while talking | Lower `VOLUME`; power the amp from a good 5 V supply |
| The answer stops halfway | Raise `MAX_REPLY_SECONDS` in the sketch |
| Voice sounds too fast or too slow | `--speed` (Step 4) |
| Happy pronounces a word wrong | Rephrase it in `examples/personality.md`, or spell it as it sounds |
