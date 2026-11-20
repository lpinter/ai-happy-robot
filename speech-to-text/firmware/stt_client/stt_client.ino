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
// The 1.5" SSD1351 OLED (same pins and driver code as firmware/tests/oled_face) shows Happy's
// face: it blinks while waiting, opens its eyes wide while listening, looks up while thinking,
// and moves its mouth with the sound while speaking. The face runs in its own task on core 0,
// so drawing never interrupts the microphone or the speaker.
//
// Board: ESP32S3 Dev Module, Flash 16MB, PSRAM "OPI PSRAM" (needed for the buffers).
// Core:  esp32 by Espressif 3.x (uses ESP_I2S.h, same as mic_test and amp_test).

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <SPI.h>

// Wi-Fi name/password and server URL live in secrets.h (gitignored).
// First time: copy secrets.example.h to secrets.h and fill in your values.
#include "secrets.h"

// INMP441 wiring (from the README / mic_test)
const int PIN_SCK = 42;
const int PIN_WS  = 41;
const int PIN_SD  = 1;
const int PIN_TALK = 0;          // BOOT button, LOW when pressed

const int SAMPLE_RATE = 16000;
const int MAX_SECONDS = 20;
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
const bool  SHOW_LEVELS   = false;  // print the mic level twice a second (for tuning)
const int   MIN_LEVEL     = 300;    // never start recording below this loudness...
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

// ------------------------------------------------------------------ face (1.5" SSD1351 OLED)
// Wiring (from the README / oled_face): CLK -> GPIO 21, DIN -> GPIO 47, CS -> GPIO 14,
// DC -> GPIO 2, RST -> EN, VCC -> 3V3, GND -> GND.
const bool SHOW_FACE   = true;   // false = no display connected
const int PIN_OLED_SCK  = 21;
const int PIN_OLED_MOSI = 47;
const int PIN_OLED_CS   = 14;
const int PIN_OLED_DC   = 2;

const uint16_t EYES  = 0x07FF;   // cyan (RGB565)
const uint16_t SMILE = 0xFD20;   // orange

enum Face { FACE_SLEEP, FACE_IDLE, FACE_LISTEN, FACE_THINK, FACE_SPEAK, FACE_SAD };
volatile Face face = FACE_SLEEP; // set by the main loop, drawn by faceTask
volatile float mouthOpen = 0;    // 0..1, how loud Happy is speaking right now

uint16_t fb[128 * 128];          // RGB565, stored big-endian for the panel

void oledSend(bool data, uint8_t b) {
  digitalWrite(PIN_OLED_DC, data ? HIGH : LOW);
  digitalWrite(PIN_OLED_CS, LOW);
  SPI.transfer(b);
  digitalWrite(PIN_OLED_CS, HIGH);
}

// SSD1351 takes command parameters with DC high: {command, param count, params...}.
void oledCmds(const uint8_t* seq, size_t len) {
  for (size_t i = 0; i < len;) {
    oledSend(false, seq[i]);
    uint8_t n = seq[i + 1];
    for (uint8_t k = 0; k < n; k++) oledSend(true, seq[i + 2 + k]);
    i += 2 + n;
  }
}

void oledInit() {
  const uint8_t seq[] = {
    0xFD, 1, 0x12, 0xFD, 1, 0xB1,  // unlock
    0xAE, 0,                       // display off
    0xB3, 1, 0xF1,                 // clock
    0xCA, 1, 0x7F,                 // 128 rows
    0xA0, 1, 0x74,                 // remap: 65k colour, COM split
    0xA1, 1, 0x00, 0xA2, 1, 0x00,  // start line, offset
    0xB5, 1, 0x00, 0xAB, 1, 0x01,  // GPIO off, internal regulator on
    0xB1, 1, 0x32, 0xBE, 1, 0x05,  // precharge, VCOMH
    0xC1, 3, 0xC8, 0x80, 0xC8,     // contrast A/B/C
    0xC7, 1, 0x0F,                 // master contrast
    0xB4, 3, 0xA0, 0xB5, 0x55,     // segment low voltage
    0xB6, 1, 0x01,                 // second precharge
    0xA6, 0,                       // normal display
    0xAF, 0,                       // display on
  };
  oledCmds(seq, sizeof(seq));
}

void oledShow() {
  const uint8_t win[] = {0x15, 2, 0x00, 0x7F, 0x75, 2, 0x00, 0x7F};
  oledCmds(win, sizeof(win));
  oledSend(false, 0x5C);         // write RAM
  digitalWrite(PIN_OLED_DC, HIGH);
  digitalWrite(PIN_OLED_CS, LOW);
  SPI.writeBytes((uint8_t*)fb, sizeof(fb));
  digitalWrite(PIN_OLED_CS, HIGH);
}

void px(int x, int y, uint16_t c) {
  if (x < 0 || x > 127 || y < 0 || y > 127) return;
  fb[y * 128 + x] = (c >> 8) | (c << 8);  // swap to big-endian
}

void ellipse(int cx, int cy, int rx, int ry, uint16_t c) {
  for (int y = -ry; y <= ry; y++)
    for (int x = -rx; x <= rx; x++)
      if ((float)x * x / (rx * rx) + (float)y * y / (ry * ry) <= 1.0f) px(cx + x, cy + y, c);
}

// A 4-pixel-thick curve: bend > 0 = smile (ends up), bend < 0 = frown, 0 = flat line.
void mouthCurve(int cy, int halfWidth, int bend) {
  for (int x = -halfWidth; x <= halfWidth; x++) {
    int y = cy - bend * (x * x) / (halfWidth * halfWidth);
    for (int t = 0; t < 4; t++) px(64 + x, y + t, SMILE);
  }
}

// Two lips that meet at the corners and part in the middle by `open` pixels, on a slight smile.
void lips(int cy, int halfWidth, int open) {
  for (int x = -halfWidth; x <= halfWidth; x++) {
    float t = (float)(x * x) / (halfWidth * halfWidth);  // 0 in the middle, 1 at the corners
    int base = cy - (int)(8 * t);                        // corners curve up
    int gap = (int)(open / 2.0f * (1 - t));
    for (int k = 0; k < 3; k++) {
      px(64 + x, base - gap + k - 1, SMILE);             // upper lip
      px(64 + x, base + gap + k - 1, SMILE);             // lower lip
    }
  }
}

// mouth = 0..18 (how open while speaking), dots = 0..3 (thinking animation)
void drawFace(Face f, bool blink, int mouth, int dots) {
  memset(fb, 0, sizeof(fb));
  int eyeH = blink ? 2 : 18;
  switch (f) {
    case FACE_SLEEP:                               // eyes closed, waiting for Wi-Fi
      ellipse(40, 54, 14, 2, EYES);
      ellipse(88, 54, 14, 2, EYES);
      mouthCurve(94, 12, 0);
      break;
    case FACE_IDLE:                                // the oled_face test face
      ellipse(40, 52, 14, eyeH, EYES);
      ellipse(88, 52, 14, eyeH, EYES);
      mouthCurve(92, 24, 14);
      break;
    case FACE_LISTEN:                              // big eyes, lips slightly parted
      ellipse(40, 50, 16, blink ? 2 : 22, EYES);
      ellipse(88, 50, 16, blink ? 2 : 22, EYES);
      lips(96, 16, 4);
      break;
    case FACE_THINK:                               // looking up and to the side, "..." below
      ellipse(48, 44, 12, blink ? 2 : 14, EYES);
      ellipse(96, 44, 12, blink ? 2 : 14, EYES);
      mouthCurve(90, 12, 0);
      for (int i = 0; i < dots; i++) ellipse(52 + i * 12, 112, 3, 3, SMILE);
      break;
    case FACE_SPEAK:                               // lips part with the sound
      ellipse(40, 52, 14, 18, EYES);
      ellipse(88, 52, 14, 18, EYES);
      lips(94, 24, mouth);
      break;
    case FACE_SAD:                                 // something went wrong
      ellipse(40, 56, 14, 10, EYES);
      ellipse(88, 56, 14, 10, EYES);
      mouthCurve(92, 20, -10);
      break;
  }
  oledShow();
}

// Runs on core 0 and redraws only when the picture changes, so the mic loop on core 1
// never waits for the display (a full frame takes ~33 ms at 8 MHz).
void faceTask(void*) {
  pinMode(PIN_OLED_CS, OUTPUT);
  pinMode(PIN_OLED_DC, OUTPUT);
  digitalWrite(PIN_OLED_CS, HIGH);
  SPI.begin(PIN_OLED_SCK, -1, PIN_OLED_MOSI, PIN_OLED_CS);
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  vTaskDelay(pdMS_TO_TICKS(200)); // RST is tied to EN, so the panel resets with the board
  oledInit();

  unsigned long nextBlink = millis() + random(2000, 4500);
  int lastKey = -1;
  while (true) {
    Face f = face;
    unsigned long now = millis();
    bool blink = false;
    if (f == FACE_IDLE || f == FACE_LISTEN || f == FACE_THINK) {
      if (now >= nextBlink + 140) nextBlink = now + random(2000, 4500);
      else if (now >= nextBlink) blink = true;
    }
    int mouth = f == FACE_SPEAK ? (int)(mouthOpen * 18) : 0;
    int dots = f == FACE_THINK ? (now / 300) % 4 : 0;
    int key = f * 10000 + blink * 1000 + mouth * 10 + dots;
    if (key != lastKey) {
      drawFace(f, blink, mouth, dots);
      lastKey = key;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  face = FACE_SLEEP;
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
  face = FACE_IDLE;
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
  face = FACE_LISTEN;
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
  face = FACE_LISTEN;
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
    face = FACE_IDLE;
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
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
      int16_t s = (int32_t)in[done + i] * VOLUME / 100;
      frame[2 * i] = frame[2 * i + 1] = s;
      peak = max(peak, abs((int)in[done + i]));
    }
    // the server normalises speech to ~90% of full scale, so ~20000 is a fully open mouth
    float target = min(1.0f, peak / 20000.0f);
    mouthOpen = mouthOpen + (target - mouthOpen) * (target > mouthOpen ? 0.6f : 0.25f);
    amp.write((uint8_t*)frame, n * 4);
    done += n;
  }
  mouthOpen = 0;
  memset(frame, 0, sizeof(frame));                  // a little silence so the amp doesn't pop
  for (int i = 0; i < 8; i++) amp.write((uint8_t*)frame, sizeof(frame));
}

// Sends the recording. needName = the server should only answer if it hears "Happy".
// Returns true if Happy answered.
bool talkToHappy(size_t samples, bool needName) {
  connectWifi();
  face = FACE_THINK;
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
    face = FACE_SAD;
    delay(1500);                 // show the sad face for a moment
    face = FACE_IDLE;
    return false;
  }
  Serial.printf("You:   %s\n", urlDecode(http.header("X-Happy-Text")).c_str());
  if (http.header("X-Happy-Ignored") == "1") {
    Serial.println("(not talking to Happy - say \"Happy\" to wake it)");
    http.end();
    face = FACE_IDLE;
    return false;
  }
  Serial.printf("Happy: %s\n", urlDecode(http.header("X-Happy-Reply")).c_str());

  // download the whole answer first, then play it (no gaps if Wi-Fi stutters)
  int len = http.getSize();
  if (len <= 0) { Serial.println("(nothing to say)"); http.end(); face = FACE_IDLE; return false; }
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
  face = FACE_SPEAK;
  playReply(got);
  face = FACE_IDLE;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_TALK, INPUT_PULLUP);
  if (SHOW_FACE) xTaskCreatePinnedToCore(faceTask, "face", 4096, nullptr, 1, nullptr, 0);

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
    if (n < SAMPLE_RATE / 4) { Serial.println("Too short, ignored."); face = FACE_IDLE; return; }
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
