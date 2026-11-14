// Happy: ears + mouth
//
// Hold the BOOT button (GPIO 0), speak, release. The ESP32-S3 records from the
// INMP441 (same pins as mic_test) and POSTs 16 kHz / 16-bit / mono raw PCM to
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

I2SClass i2s;                    // microphone
I2SClass amp;                    // speaker
int16_t* pcm = nullptr;          // recording buffer in PSRAM
uint8_t* reply = nullptr;        // Happy's answer (16 kHz, 16-bit mono) in PSRAM
const size_t MAX_SAMPLES = (size_t)SAMPLE_RATE * MAX_SECONDS;
const size_t MAX_REPLY_BYTES = (size_t)SAMPLE_RATE * 2 * MAX_REPLY_SECONDS;

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Wi-Fi: connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\nWi-Fi: connected, IP %s\n", WiFi.localIP().toString().c_str());
}

size_t recordWhileHeld() {
  int32_t frame[2 * 256];        // 256 stereo frames of 32-bit samples
  size_t n = 0;

  // throw away ~100 ms so the mic settles
  for (int i = 0; i < 6; i++) i2s.readBytes((char*)frame, sizeof(frame));

  while (digitalRead(PIN_TALK) == LOW && n < MAX_SAMPLES) {
    size_t got = i2s.readBytes((char*)frame, sizeof(frame)) / sizeof(int32_t);
    for (size_t i = MIC_SLOT; i < got && n < MAX_SAMPLES; i += 2) {
      int32_t s = frame[i] >> GAIN_SHIFT;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      pcm[n++] = (int16_t)s;
    }
  }
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

void talkToHappy(size_t samples) {
  connectWifi();
  HTTPClient http;
  http.setTimeout(60000);        // Whisper + Gemma + Kokoro; the first request after a restart is slower
  http.begin(STT_URL);
  http.addHeader("Content-Type", "application/octet-stream");
  const char* keys[] = {"X-Happy-Text", "X-Happy-Reply"};
  http.collectHeaders(keys, 2);

  int code = http.POST((uint8_t*)pcm, samples * sizeof(int16_t));
  if (code != 200) {
    Serial.printf("HTTP error %d: %s\n", code, http.errorToString(code).c_str());
    http.end();
    return;
  }
  Serial.printf("You:   %s\n", urlDecode(http.header("X-Happy-Text")).c_str());
  Serial.printf("Happy: %s\n", urlDecode(http.header("X-Happy-Reply")).c_str());

  // download the whole answer first, then play it (no gaps if Wi-Fi stutters)
  int len = http.getSize();
  if (len <= 0) { Serial.println("(nothing to say)"); http.end(); return; }
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
  Serial.println("Ready. Hold BOOT, talk, release.");
}

void loop() {
  if (digitalRead(PIN_TALK) == LOW) {
    Serial.println("Listening...");
    size_t n = recordWhileHeld();
    Serial.printf("Recorded %.1f s, sending...\n", n / (float)SAMPLE_RATE);
    if (n > SAMPLE_RATE / 4) talkToHappy(n);
    else Serial.println("Too short, ignored.");
    while (digitalRead(PIN_TALK) == LOW) delay(10);   // wait for release
  }
  delay(10);
}
