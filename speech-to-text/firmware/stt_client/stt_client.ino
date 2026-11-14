// Happy: ears + mouth
//
// Hands-free: just say "Happy, ..." - the ESP32 starts recording when it hears speech
// and stops after a short silence. The PC only answers if Whisper heard the name "Happy"
// (or if you reply within FOLLOW_UP_MS of Happy's last answer). Holding the BOOT button
// (GPIO 0) still works as push-to-talk, and then no name is needed.
// The ESP32-S3 records from the INMP441 (same pins as mic_test) and POSTs 16 kHz / 16-bit / mono raw PCM to
// the PC's stt_server.py /talk. The PC runs Whisper -> Gemma -> Kokoro and sends
// back Happy's spoken answer, which plays on the MAX98357A (same pins as amp_test).
// What you said and Happy's answer are also printed on the Serial Monitor (115200).
//
// Board: ESP32S3 Dev Module, Flash 16MB, PSRAM "OPI PSRAM" (needed for the buffers).
// Core:  esp32 by Espressif 3.x (uses ESP_I2S.h, same as mic_test and amp_test).

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>

// Wi-Fi name/password and server URL live in secrets.h (gitignored).
// First time: copy secrets.example.h to secrets.h and fill in your values.
#include "secrets.h"

// INMP441 wiring (from the README / mic_test)
const int PIN_SCK = 42;
const int PIN_WS  = 41;
const int PIN_SD  = 1;
const int PIN_TALK = 0;          // BOOT button, LOW when pressed

const int SAMPLE_RATE = 16000;
const int MAX_SECONDS = 10;
const int MIC_SLOT    = 0;       // L/R tied to GND -> left slot (0). If text is always empty and peak ~0, try 1.
const int GAIN_SHIFT  = 14;      // 32-bit sample >> 16 = plain 16-bit; >> 14 adds ~12 dB (INMP441 is quiet)

// MAX98357A wiring (from the README / amp_test)
const int PIN_AMP_BCLK = 40;
const int PIN_AMP_LRC  = 39;
const int PIN_AMP_DIN  = 38;
const int VOLUME       = 70;     // 0-100 %. Lower it if the speaker distorts or the board resets.
const int MAX_REPLY_SECONDS = 30;

// Hands-free listening. Set SHOW_LEVELS = true to see the mic level and pick MIN_LEVEL.
const bool  HANDS_FREE    = true;   // false = BOOT button only, like before
const bool  SHOW_LEVELS   = true;  // print the mic level twice a second (for tuning)
const int   MIN_LEVEL     = 1000;    // never start recording below this loudness...
const float NOISE_FACTOR  = 3.0f;   // ...or below 3x the room's background noise
const int   START_MS      = 80;     // this much loud sound in a row starts a recording
const int   SILENCE_MS    = 800;    // this much quiet ends it
const int   PREROLL_MS    = 300;    // sound kept from just before the start, so the first word isn't cut
const int   MIN_SPEECH_MS = 300;    // shorter sounds (a clap, a door) are ignored
const int   FOLLOW_UP_MS  = 8000;   // after Happy answers, you can reply without saying "Happy" (0 = off)
const int   ECHO_FLUSH_MS = 300;    // after Happy speaks, throw away this much mic input (its own echo)

const size_t BLOCK    = 256;                          // mono samples per mic read
const int    BLOCK_MS = BLOCK * 1000 / SAMPLE_RATE;   // = 16 ms
const size_t PREROLL  = (size_t)SAMPLE_RATE * PREROLL_MS / 1000;

I2SClass i2s;                    // microphone
I2SClass amp;                    // speaker
int16_t* pcm = nullptr;          // recording buffer in PSRAM
uint8_t* reply = nullptr;        // Happy's answer (16 kHz, 16-bit mono) in PSRAM
const size_t MAX_SAMPLES = (size_t)SAMPLE_RATE * MAX_SECONDS;
const size_t MAX_REPLY_BYTES = (size_t)SAMPLE_RATE * 2 * MAX_REPLY_SECONDS;
int16_t block[BLOCK];            // one mic read
int16_t preroll[PREROLL];        // ring buffer: the last PREROLL_MS while waiting for speech
float noise = 0;                 // background noise level, learned while it's quiet
unsigned long lastAnswerAt = 0;  // when Happy last finished speaking (0 = never)
unsigned long speechStartAt = 0; // when the current recording started

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Wi-Fi: connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300); Serial.print(".");
    if (millis() - t0 > 15000) {   // stuck: say why, then try again
      wl_status_t st = WiFi.status();
      Serial.printf("\nWi-Fi: not connected (status %d: %s). Retrying...\n", st,
        st == WL_NO_SSID_AVAIL  ? "network not found - check the name, and that it is 2.4 GHz" :
        st == WL_CONNECT_FAILED ? "connect failed - check the password" :
                                  "no answer from the router - wrong password or WPA3-only network?");
      WiFi.disconnect();
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      t0 = millis();
    }
  }
  Serial.printf("\nWi-Fi: connected, IP %s\n", WiFi.localIP().toString().c_str());
}

// Reads one block from the mic as 16-bit mono into `out`. Returns the sample count;
// `level` gets the loudness (RMS with the DC offset removed).
size_t readBlock(int16_t* out, float& level) {
  int32_t frame[2 * BLOCK];      // stereo frames of 32-bit samples
  size_t got = i2s.readBytes((char*)frame, sizeof(frame)) / sizeof(int32_t);
  size_t n = 0;
  int64_t sum = 0, sumSq = 0;
  for (size_t i = MIC_SLOT; i < got; i += 2) {
    int32_t s = frame[i] >> GAIN_SHIFT;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[n++] = (int16_t)s;
    sum += s;
    sumSq += (int64_t)s * s;
  }
  if (n == 0) { level = 0; return 0; }
  float mean = (float)sum / n;
  float var = (float)sumSq / n - mean * mean;
  level = var > 0 ? sqrtf(var) : 0;
  return n;
}

// Throw away `ms` of mic input (stale audio and Happy's own echo).
void flushMic(int ms) {
  float level;
  for (int t = 0; t < ms; t += BLOCK_MS) readBlock(block, level);
}

size_t recordWhileHeld() {
  size_t n = 0;
  float level;
  flushMic(100);                 // let the mic settle
  while (digitalRead(PIN_TALK) == LOW && n < MAX_SAMPLES) {
    size_t got = readBlock(block, level);
    for (size_t i = 0; i < got && n < MAX_SAMPLES; i++) pcm[n++] = block[i];
  }
  return n;
}

// Waits until someone speaks, then records until they stop.
// Returns the number of samples, or 0 if it was only a short noise or the BOOT button was pressed.
size_t listenForSpeech() {
  const int startBlocks = START_MS / BLOCK_MS;
  const int silenceBlocks = SILENCE_MS / BLOCK_MS;
  size_t head = 0, filled = 0;   // preroll ring buffer
  int loud = 0;
  float level, threshold = MIN_LEVEL;
  unsigned long lastShow = 0;
  if (noise == 0) noise = MIN_LEVEL / NOISE_FACTOR;

  // 1. wait for a few loud blocks in a row
  while (true) {
    if (digitalRead(PIN_TALK) == LOW) return 0;
    size_t got = readBlock(block, level);
    for (size_t i = 0; i < got; i++) {
      preroll[head] = block[i];
      head = (head + 1) % PREROLL;
      if (filled < PREROLL) filled++;
    }
    threshold = max((float)MIN_LEVEL, noise * NOISE_FACTOR);
    if (SHOW_LEVELS && millis() - lastShow > 500) {
      Serial.printf("level %5.0f   noise %5.0f   threshold %5.0f\n", level, noise, threshold);
      lastShow = millis();
    }
    if (level > threshold) {
      if (++loud >= startBlocks) break;
    } else {
      loud = 0;
      noise += (level - noise) * (level < noise ? 0.05f : 0.002f);  // follow quiet fast, noise slowly
    }
  }

  // 2. record: first the preroll (it holds the start of the word), then until a silence
  speechStartAt = millis();
  Serial.println("Listening...");
  size_t n = 0;
  size_t oldest = (head + PREROLL - filled) % PREROLL;
  for (size_t i = 0; i < filled; i++) pcm[n++] = preroll[(oldest + i) % PREROLL];

  const float endLevel = threshold * 0.7f;   // a bit lower, so quiet word endings still count
  int quiet = 0, voiced = loud;
  while (n < MAX_SAMPLES && quiet < silenceBlocks) {
    size_t got = readBlock(block, level);
    for (size_t i = 0; i < got && n < MAX_SAMPLES; i++) pcm[n++] = block[i];
    if (level > endLevel) { quiet = 0; voiced++; }
    else quiet++;
  }
  if (voiced * BLOCK_MS < MIN_SPEECH_MS) {
    Serial.println("Just a noise, ignored.");
    return 0;
  }
  // cut the trailing silence down to ~300 ms (less for Whisper to imagine words in)
  size_t extra = quiet > 20 ? (size_t)(quiet - 20) * BLOCK : 0;
  if (extra < n) n -= extra;
  return n;
}

// The server URL-encodes the text headers; turn "%C3%A9" etc. back into characters.
String urlDecode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length()) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

void playReply(size_t bytes) {
  int16_t frame[2 * 256];        // stereo out: same sample to left and right
  const int16_t* in = (const int16_t*)reply;
  size_t samples = bytes / 2;
  for (size_t done = 0; done < samples;) {
    size_t n = min((size_t)256, samples - done);
    for (size_t i = 0; i < n; i++) {
      int16_t s = (int32_t)in[done + i] * VOLUME / 100;
      frame[2 * i] = frame[2 * i + 1] = s;
    }
    amp.write((uint8_t*)frame, n * 4);
    done += n;
  }
  memset(frame, 0, sizeof(frame));                  // a little silence so the amp doesn't pop
  for (int i = 0; i < 8; i++) amp.write((uint8_t*)frame, sizeof(frame));
}

// Sends the recording. needName = the server should only answer if it hears "Happy".
// Returns true if Happy answered.
bool talkToHappy(size_t samples, bool needName) {
  connectWifi();
  HTTPClient http;
  http.setTimeout(60000);        // Whisper + Gemma + Kokoro; the first request after a restart is slower
  http.begin(STT_URL);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Happy-Wake", needName ? "1" : "0");
  const char* keys[] = {"X-Happy-Text", "X-Happy-Reply", "X-Happy-Ignored"};
  http.collectHeaders(keys, 3);

  int code = http.POST((uint8_t*)pcm, samples * sizeof(int16_t));
  if (code != 200) {
    Serial.printf("HTTP error %d: %s\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }
  Serial.printf("You:   %s\n", urlDecode(http.header("X-Happy-Text")).c_str());
  if (http.header("X-Happy-Ignored") == "1") {
    Serial.println("(not talking to Happy - say \"Happy\" to wake it)");
    http.end();
    return false;
  }
  Serial.printf("Happy: %s\n", urlDecode(http.header("X-Happy-Reply")).c_str());

  // download the whole answer first, then play it (no gaps if Wi-Fi stutters)
  int len = http.getSize();
  if (len <= 0) { Serial.println("(nothing to say)"); http.end(); return false; }
  size_t want = min((size_t)len, MAX_REPLY_BYTES) & ~(size_t)1;
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long t0 = millis();
  while (got < want && millis() - t0 < 20000) {
    size_t avail = stream->available();
    if (avail) got += stream->readBytes(reply + got, min(avail, want - got));
    else delay(1);
  }
  http.end();
  Serial.printf("Speaking %.1f s...\n", got / 2.0f / SAMPLE_RATE);
  playReply(got);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_TALK, INPUT_PULLUP);

  pcm = (int16_t*)ps_malloc(MAX_SAMPLES * sizeof(int16_t));
  reply = (uint8_t*)ps_malloc(MAX_REPLY_BYTES);
  if (!pcm || !reply) { Serial.println("No PSRAM! Set Tools > PSRAM > OPI PSRAM."); while (true) delay(1000); }

  i2s.setPins(PIN_SCK, PIN_WS, -1, PIN_SD);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Mic I2S init failed"); while (true) delay(1000);
  }
  amp.setPins(PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN);
  if (!amp.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Amp I2S init failed"); while (true) delay(1000);
  }

  connectWifi();
  if (HANDS_FREE) Serial.println("Ready. Say \"Happy, ...\" (or hold BOOT, talk, release).");
  else            Serial.println("Ready. Hold BOOT, talk, release.");
}

void loop() {
  size_t n = 0;
  bool needName = false;

  if (digitalRead(PIN_TALK) == LOW) {                 // push-to-talk: no name needed
    Serial.println("Listening...");
    n = recordWhileHeld();
    while (digitalRead(PIN_TALK) == LOW) delay(10);   // wait for release
    if (n < SAMPLE_RATE / 4) { Serial.println("Too short, ignored."); return; }
  } else if (HANDS_FREE) {
    n = listenForSpeech();
    if (n == 0) return;
    bool followUp = lastAnswerAt != 0 && speechStartAt - lastAnswerAt < (unsigned long)FOLLOW_UP_MS;
    needName = !followUp;
  } else {
    delay(10);
    return;
  }

  Serial.printf("Recorded %.1f s, sending...\n", n / (float)SAMPLE_RATE);
  if (talkToHappy(n, needName)) lastAnswerAt = millis();
  flushMic(ECHO_FLUSH_MS);       // drop audio from while we were busy, and Happy's own echo
}
