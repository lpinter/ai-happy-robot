"""
Happy's "ears + brain + voice" on the PC.

  audio --> Whisper (speech-to-text) --> Gemma in LM Studio --> Kokoro (text-to-speech) --> audio

Endpoints:
  POST /talk  audio in -> Happy's spoken answer out (what the ESP32 uses)
                response body: raw PCM, 16 kHz, 16-bit LE, mono
                headers: X-Happy-Text (what you said), X-Happy-Reply (the answer), both URL-encoded
                request header X-Happy-Wake: 1 -> only answer if the text contains a wake word
                (hands-free mode); otherwise the response is empty with X-Happy-Ignored: 1
  POST /stt   audio in -> JSON {"text": ..., "reply": ...}  (no speech, handy for debugging)
  POST /chat  {"text": "..."} -> JSON reply                  (tests LM Studio without a mic)
  POST /say   {"text": "..."} -> WAV file                    (tests the voice; add "chat": true to
                                                              have Happy answer the text first)
  GET  /health

Audio in (/talk and /stt):
  * Content-Type: application/octet-stream -> raw PCM, 16 kHz, 16-bit LE, mono (ESP32)
  * Content-Type: audio/wav                -> any 16-bit PCM WAV (testing with curl)

No ffmpeg needed. LM Studio's server must be running (`lms server start`).

Run:   py stt_server.py --model small --language en
"""

import argparse
import io
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
import warnings
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock

import numpy as np
import torch
import whisper

# Kokoro's model code triggers harmless PyTorch deprecation warnings; keep the log readable
warnings.filterwarnings("ignore", category=UserWarning)
warnings.filterwarnings("ignore", category=FutureWarning)

SAMPLE_RATE = 16000  # Whisper input rate, and the rate we send to the ESP32 speaker
TTS_RATE = 24000     # Kokoro's output rate
DEFAULT_PERSONALITY = Path(__file__).resolve().parents[2] / "examples" / "personality.md"

model = None
model_lock = Lock()  # one transcription at a time on the GPU
tts = None
tts_lock = Lock()
args = None
personality = ""
history = []         # recent turns so Happy remembers the conversation
history_lock = Lock()


# ---------------------------------------------------------------- audio helpers

def pcm16_to_float(pcm: bytes) -> np.ndarray:
    return np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0


def float_to_pcm16(audio: np.ndarray) -> bytes:
    return (np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes()


def resample(audio: np.ndarray, src: int, dst: int) -> np.ndarray:
    if src == dst or len(audio) == 0:
        return audio
    n = int(len(audio) * dst / src)
    return np.interp(np.linspace(0, len(audio), n, endpoint=False),
                     np.arange(len(audio)), audio).astype(np.float32)


def wav_to_float(data: bytes) -> np.ndarray:
    with wave.open(io.BytesIO(data), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("WAV must be 16-bit PCM")
        rate, channels = w.getframerate(), w.getnchannels()
        audio = pcm16_to_float(w.readframes(w.getnframes()))
    if channels > 1:
        audio = audio.reshape(-1, channels).mean(axis=1)
    return resample(audio, rate, SAMPLE_RATE)


def to_wav(audio: np.ndarray, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(float_to_pcm16(audio))
    return buf.getvalue()


# ---------------------------------------------------------------- speech-to-text (Whisper)

def transcribe(audio: np.ndarray) -> dict:
    t0 = time.time()
    with model_lock:
        result = model.transcribe(
            audio,
            language=args.language,          # None = auto-detect
            fp16=torch.cuda.is_available(),
            condition_on_previous_text=False,
        )
    return {
        "text": result["text"].strip(),
        "language": result.get("language"),
        "seconds_audio": round(len(audio) / SAMPLE_RATE, 2),
        "seconds_stt": round(time.time() - t0, 2),
    }


def heard_name(text: str) -> bool:
    """True if Whisper's text contains one of the wake words (e.g. "Hey Happy, ...")."""
    return any(w in args.wake_words for w in re.findall(r"[a-z]+", text.lower()))


# ---------------------------------------------------------------- the brain (LM Studio)

def ask_llm(text: str) -> dict:
    """Send what the person said to Gemma in LM Studio (OpenAI-compatible API)."""
    with history_lock:
        messages = [{"role": "system", "content": personality}] + history + \
                   [{"role": "user", "content": text}]
    payload = {
        "model": args.lm_model,
        "messages": messages,
        "max_tokens": 200,
        "temperature": 0.7,
        "reasoning_effort": "none",   # Gemma "thinks" by default; off = faster, short spoken answers
    }
    req = urllib.request.Request(
        args.lm_url, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=300) as r:  # first call loads the model: slow
            data = json.load(r)
    except urllib.error.URLError as e:
        return {"reply": "", "llm_error": f"LM Studio not reachable at {args.lm_url}: {e}. "
                                          "Is the server running? (lms server start)"}
    reply = (data["choices"][0]["message"].get("content") or "").strip()

    with history_lock:
        history.extend([{"role": "user", "content": text},
                        {"role": "assistant", "content": reply}])
        del history[:-2 * args.history]   # keep the last N exchanges
    return {"reply": reply, "seconds_llm": round(time.time() - t0, 2)}


# ---------------------------------------------------------------- text-to-speech (Kokoro)

def speak(text: str, rate: int = SAMPLE_RATE) -> np.ndarray:
    """Turn text into speech. Returns float audio at `rate` Hz, normalised to ~90% volume."""
    if not text or tts is None:
        return np.zeros(0, dtype=np.float32)
    with tts_lock:
        parts = [a.numpy() for _, _, a in tts(text, voice=args.voice, speed=args.speed)]
    if not parts:
        return np.zeros(0, dtype=np.float32)
    audio = np.concatenate(parts).astype(np.float32)
    peak = float(np.max(np.abs(audio)))
    if peak > 0:
        audio *= 0.9 / peak
    pad = np.zeros(int(0.05 * TTS_RATE), dtype=np.float32)   # 50 ms of silence: no click at the start
    return resample(np.concatenate([pad, audio, pad]), TTS_RATE, rate)


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, body: bytes, ctype: str, headers: dict = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, code: int, obj: dict) -> None:
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length > 0 else b""

    def _read_json(self) -> dict:
        try:
            return json.loads(self._read_body() or b"{}")
        except json.JSONDecodeError:
            return {}

    def _read_audio(self):
        """Returns float audio at 16 kHz, or sends an error and returns None."""
        data = self._read_body()
        if not data:
            self._send_json(400, {"error": "empty body"})
            return None
        ctype = (self.headers.get("Content-Type") or "").lower()
        try:
            audio = wav_to_float(data) if ("wav" in ctype or data[:4] == b"RIFF") \
                else pcm16_to_float(data)
        except Exception as e:
            self._send_json(400, {"error": f"bad audio: {e}"})
            return None
        if args.save_dir:  # keep the last recording so you can listen to what the mic heard
            Path(args.save_dir, "last_recording.wav").write_bytes(to_wav(audio, SAMPLE_RATE))
        return audio

    def _hear_and_answer(self, audio: np.ndarray, need_name: bool = False) -> dict:
        """Whisper + LM Studio. Prints the conversation to the console/log.
        need_name: hands-free recording, only answer if the wake word was heard."""
        if len(audio) < SAMPLE_RATE // 4:
            return {"text": "", "reply": "", "note": "audio too short"}
        peak = float(np.max(np.abs(audio)))
        result = transcribe(audio)
        result["peak"] = round(peak, 3)
        print(f"[{self.client_address[0]}] {result['seconds_audio']}s audio, peak {peak:.3f}, "
              f"stt {result['seconds_stt']}s")
        print(f"  you    : {result['text']}")
        if need_name and result["text"] and not heard_name(result["text"]):
            result["ignored"] = True
            print("  (ignored: no wake word)")
            return result
        if result["text"] and not args.no_llm:
            result.update(ask_llm(result["text"]))
            print(f"  happy  : {result.get('reply') or result.get('llm_error')}")
        return result

    # --- routes ---

    def do_GET(self):
        if self.path == "/health":
            self._send_json(200, {"ok": True, "model": args.model,
                                  "cuda": torch.cuda.is_available(),
                                  "llm": None if args.no_llm else args.lm_model,
                                  "voice": None if tts is None else args.voice})
        else:
            self._send_json(404, {"error": "not found"})

    def do_POST(self):
        routes = {"/talk": self._talk, "/stt": self._stt, "/chat": self._chat, "/say": self._say}
        route = routes.get(self.path)
        if route:
            return route()
        self._send_json(404, {"error": "use POST /talk, /stt, /chat or /say"})

    def _stt(self):
        audio = self._read_audio()
        if audio is not None:
            self._send_json(200, self._hear_and_answer(audio))

    def _talk(self):
        audio = self._read_audio()
        if audio is None:
            return
        need_name = self.headers.get("X-Happy-Wake") == "1" and bool(args.wake_words)
        result = self._hear_and_answer(audio, need_name)
        reply = result.get("reply") or ""
        if not reply and result.get("llm_error"):
            reply = "Sorry, my brain is not answering right now."
        t0 = time.time()
        speech = speak(reply)
        if len(speech):
            print(f"  voice  : {len(speech) / SAMPLE_RATE:.1f}s of speech in {time.time() - t0:.2f}s")
        q = lambda s: urllib.parse.quote(s or "", safe=" ,.!?'")
        # 200 with an empty body = nothing to say (e.g. Whisper heard nothing)
        self._send(200, float_to_pcm16(speech), "audio/L16; rate=16000; channels=1",
                   {"X-Happy-Text": q(result.get("text")), "X-Happy-Reply": q(reply),
                    "X-Happy-Ignored": "1" if result.get("ignored") else "0"})

    def _chat(self):
        text = (self._read_json().get("text") or "").strip()
        if not text:
            return self._send_json(400, {"error": 'send JSON like {"text": "hello"}'})
        result = {"text": text, **ask_llm(text)}
        print(f"  you    : {text}\n  happy  : {result.get('reply') or result.get('llm_error')}")
        self._send_json(200, result)

    def _say(self):
        req = self._read_json()
        text = (req.get("text") or "").strip()
        if not text:
            return self._send_json(400, {"error": 'send JSON like {"text": "hello"}'})
        if tts is None:
            return self._send_json(503, {"error": "text-to-speech is off (--no-tts)"})
        if req.get("chat"):
            text = ask_llm(text).get("reply") or "Sorry, my brain is not answering right now."
        print(f"  say    : {text}")
        self._send(200, to_wav(speak(text, TTS_RATE), TTS_RATE), "audio/wav")

    def log_message(self, fmt, *a):  # keep the console quiet; we print our own lines
        pass


class Server(ThreadingHTTPServer):
    # On Windows, address reuse lets a second copy bind the same port and silently
    # steal requests from the first one. Fail loudly instead.
    allow_reuse_address = False


def main():
    global model, tts, args, personality
    p = argparse.ArgumentParser(description="Happy: Whisper + LM Studio + Kokoro server")
    p.add_argument("--model", default="small",
                   help="Whisper model: tiny, base, small, medium, large-v3, turbo (.en = English-only)")
    p.add_argument("--language", default="en", help="e.g. en, hu, de; 'auto' to detect")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--save-dir", default=".", help="where to write last_recording.wav ('' to disable)")
    p.add_argument("--lm-url", default="http://localhost:1234/v1/chat/completions")
    p.add_argument("--lm-model", default="google/gemma-4-26b-a4b-qat",
                   help="model id from: curl.exe http://localhost:1234/v1/models")
    p.add_argument("--personality", default=str(DEFAULT_PERSONALITY))
    p.add_argument("--history", type=int, default=6, help="how many past exchanges Happy remembers")
    p.add_argument("--no-llm", action="store_true", help="speech-to-text only, skip LM Studio")
    p.add_argument("--voice", default="af_heart",
                   help="Kokoro voice, e.g. af_heart, af_bella, am_michael, bm_george, am_puck")
    p.add_argument("--speed", type=float, default=1.0, help="speaking speed, e.g. 0.9 or 1.2")
    p.add_argument("--no-tts", action="store_true", help="don't load Kokoro (no /talk speech, no /say)")
    p.add_argument("--wake-words", default="happy",
                   help="comma-separated names Happy answers to in hands-free mode ('' = answer everything)")
    args = p.parse_args()
    args.wake_words = {w.strip().lower() for w in args.wake_words.split(",") if w.strip()}
    if args.language == "auto":
        args.language = None

    try:
        personality = Path(args.personality).read_text(encoding="utf-8")
        print(f"Personality: {args.personality}")
    except OSError:
        personality = "You are Happy, a small friendly desk robot. Answer in one or two short sentences."
        print(f"Personality file not found ({args.personality}), using a built-in default.")

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Loading Whisper '{args.model}' on {device} ...")
    model = whisper.load_model(args.model, device=device)

    if not args.no_tts:
        from kokoro import KPipeline   # imported here so --no-tts works without Kokoro installed
        print(f"Loading Kokoro voice '{args.voice}' ...")
        tts = KPipeline(lang_code=args.voice[0], repo_id="hexgrad/Kokoro-82M")
        speak("Hello.")                # warm-up, so the first real answer isn't slow

    if not args.no_llm:
        print(f"LLM: {args.lm_model} at {args.lm_url}")
    try:
        server = Server((args.host, args.port), Handler)
    except OSError as e:
        raise SystemExit(f"Port {args.port} is busy ({e}). Is another stt_server.py already running?")
    print(f"Listening on http://{args.host}:{args.port}  (POST /talk /stt /chat /say, GET /health)")
    server.serve_forever()


if __name__ == "__main__":
    main()
