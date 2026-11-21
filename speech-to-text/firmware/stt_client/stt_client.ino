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
// No router? After ROUTER_WAIT_MS Happy starts its own Wi-Fi network (AP_SSID). Join it from
// the PC and Happy talks to that PC directly; everything runs on the PC, so no internet is needed.
// The 1.5" SSD1351 OLED (same pins and driver code as firmware/tests/oled_face) shows Happy's
// face: it blinks while waiting, opens its eyes wide while listening, looks up while thinking,
// and moves its mouth with the sound while speaking. The face runs in its own task on core 0,
// so drawing never interrupts the microphone or the speaker.
// The three micro servos (same pins and PWM code as firmware/tests/servo_test, servo_test2,
// servo_test3) go to their home position at start-up. Then a neck task (also on core 0) keeps
// the head gently alive: slow drift and breathing, the odd glance around, a curious head tilt
// while listening, looking up while thinking, small nods while speaking, and drooping when sad.
//
// Board: ESP32S3 Dev Module, Flash 16MB, PSRAM "OPI PSRAM" (needed for the buffers).
// Core:  esp32 by Espressif 3.x (uses ESP_I2S.h, same as mic_test and amp_test).

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <SPI.h>
#include <esp_system.h>

// Wi-Fi name/password and server URL live in secrets.h (gitignored).
// First time: copy secrets.example.h to secrets.h and fill in your values.
#include "secrets.h"

// Happy's own Wi-Fi network, used when the router can't be reached (see startAccessPoint).
// Set these in secrets.h to change them. The password needs at least 8 characters.
#ifndef AP_SSID
#define AP_SSID "Happy-Robot"
#endif
#ifndef AP_PASS
#define AP_PASS "happy1234"
#endif
const unsigned long ROUTER_WAIT_MS = 20000;   // how long to try the router before making Happy's own network

// INMP441 wiring (from the README / mic_test)
const int PIN_SCK = 42;
const int PIN_WS  = 41;
const int PIN_SD  = 1;
const int PIN_TALK = 0;          // BOOT button, LOW when pressed (shared with servo 3, see below)

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
    case FACE_THINK:                               // looking up and to the side, small smile, "..." below
      ellipse(48, 44, 12, blink ? 2 : 14, EYES);
      ellipse(96, 44, 12, blink ? 2 : 14, EYES);
      mouthCurve(92, 14, 5);
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

// ------------------------------------------------------------------ servos
// Wiring (from the README / servo_test, servo_test2, servo_test3): red -> 5V, brown -> GND,
// orange -> GPIO 45 (neck), GPIO 46 (servo 2), GPIO 0 (servo 3).
// No library: a 50 Hz PWM signal where the pulse width (0.5-2.5 ms) sets the angle.
// Servo 3 shares GPIO 0 with the BOOT button, so while the servos are on, push-to-talk is off
// (the pin is an output then); hands-free "Happy, ..." still works.
const bool USE_SERVOS = true;    // false = no servos connected, BOOT push-to-talk works again
const int  PIN_SERVOS[] = {45, 46, 0};
const int  NUM_SERVOS   = sizeof(PIN_SERVOS) / sizeof(PIN_SERVOS[0]);
// Start-up ("home") angle of each servo, same order as PIN_SERVOS. 90 = middle.
const int  SERVO_HOME[] = {105, 140, 80};
// true = keep sending the pulse so the servo actively holds its angle (it can buzz/jitter
// under load); false = stop the pulse once it has arrived, so it goes quiet and is held
// only by its gears' friction.
const bool SERVO_HOLD[] = {false, false, false};
// All directions are from the viewpoint of the robot
// Servo 1 - right (0) to left (180) 
// Servo 2 - down (0) and up (180)
// Servo 3 - bank left (0) to right (180)

// Neck animation (see neckTask). While it runs, the servos keep getting pulses (SERVO_HOLD is ignored).
const bool  ANIMATE_NECK = true;     // false = the servos go home at start-up and stay there
const float NECK_RANGE[] = {30, 20, 15};  // how far (degrees) each servo may move away from home, either way
const float DRIFT_SCALE  = 1.0f;     // idle sway / breathing: 0 = none, 1 = normal, 2 = twice as much
const bool  IDLE_GLANCES = false;     // look around now and then (gear noise can wake the mic; false = calmer)
const float SPEAK_NOD    = 8.0f;     // how strongly the head nods with the speech (0 = no nodding)
const float MAX_SPEED    = 60.0f;    // degrees per second, never faster (gentler on the power supply too)

// Head moves the AI asks for ("Happy, turn your head left and right", "look up and down", "tilt
// your head"): the server sends them in X-Happy-Head, e.g. "left,up,tilt left,center", and neckTask
// plays them on servo 1 (left/right), servo 2 (up/down) and servo 3 (tilt) while Happy speaks.
const float HEAD_TURN = 25.0f;       // degrees for "left" / "right"           (stays within NECK_RANGE[0])
const float HEAD_LIFT = 18.0f;       // degrees for "up" / "down"              (stays within NECK_RANGE[1])
const float HEAD_TILT = 14.0f;       // degrees for "tilt left" / "tilt right" (stays within NECK_RANGE[2])
struct HeadStep { float pan, nod, roll; uint16_t ms; float k; };   // where to look, for how long, how quickly
const int MAX_HEAD_STEPS = 32;
HeadStep headSteps[MAX_HEAD_STEPS];  // filled by the main loop, taken by neckTask
volatile int  headCount = 0;
volatile bool headNew = false;
portMUX_TYPE headMux = portMUX_INITIALIZER_UNLOCKED;

// Turns "left,up,tilt left,center" into steps for neckTask. Each move changes only its own
// direction, so "up,left" ends up looking up and to the left; "center" brings everything back.
// Unknown words are skipped.
void queueHeadMoves(const String& moves) {
  HeadStep s[MAX_HEAD_STEPS];
  int n = 0;
  float pan = 0, nod = 0, roll = 0;      // where the head is after the previous step
  auto add = [&](float p, float d, float r, uint16_t ms, float k) {
    if (n < MAX_HEAD_STEPS) s[n++] = {p, d, r, ms, k};
  };
  for (int start = 0; start < (int)moves.length();) {
    int comma = moves.indexOf(',', start);
    if (comma < 0) comma = moves.length();
    String m = moves.substring(start, comma);
    m.trim();
    start = comma + 1;
    if      (m == "left")       { pan  =  HEAD_TURN; add(pan, nod, roll, 1300, 0.07f); }
    else if (m == "right")      { pan  = -HEAD_TURN; add(pan, nod, roll, 1300, 0.07f); }
    else if (m == "up")         { nod  =  HEAD_LIFT; add(pan, nod, roll, 1300, 0.07f); }
    else if (m == "down")       { nod  = -HEAD_LIFT; add(pan, nod, roll, 1300, 0.07f); }
    else if (m == "tilt left")  { roll = -HEAD_TILT; add(pan, nod, roll, 1300, 0.07f); }  // servo 3: 0 = bank left
    else if (m == "tilt right") { roll =  HEAD_TILT; add(pan, nod, roll, 1300, 0.07f); }
    else if (m == "center")     { pan = nod = roll = 0; add(pan, nod, roll, 1000, 0.07f); }
    else if (m == "shake") {             // a quick "no-no"
      for (int i = 0; i < 2; i++) { add(pan + 10, nod, roll, 350, 0.15f); add(pan - 10, nod, roll, 350, 0.15f); }
      add(pan, nod, roll, 500, 0.1f);
    } else if (m == "nod") {             // a quick "yes-yes"
      for (int i = 0; i < 2; i++) { add(pan, nod - 10, roll, 350, 0.15f); add(pan, nod + 3, roll, 350, 0.15f); }
      add(pan, nod, roll, 500, 0.1f);
    }
  }
  if (n == 0) return;
  const HeadStep& end = s[n - 1];
  if (end.pan != 0 || end.nod != 0 || end.roll != 0) s[n - 1].ms += 1500;   // left looking somewhere: stay a bit longer
  portENTER_CRITICAL(&headMux);
  memcpy(headSteps, s, n * sizeof(HeadStep));
  headCount = n;
  headNew = true;
  portEXIT_CRITICAL(&headMux);
}

void servoWrite(int pin, int deg) {
  int us = map(deg, 0, 180, 500, 2500);
  ledcWrite(pin, (uint32_t)us * 16384 / 20000);  // 14-bit duty, 20 ms period
}

// Same, but with fractions of a degree (1 duty step = ~1.2 us = ~0.1 degree), so slow moves glide
// instead of stepping a whole degree at a time.
void servoWriteF(int pin, float deg) {
  float us = 500.0f + constrain(deg, 0.0f, 180.0f) * (2000.0f / 180.0f);
  ledcWrite(pin, (uint32_t)(us * 16384.0f / 20000.0f + 0.5f));
}

// Moves every servo to its home angle, one at a time, so they don't all pull current at once
// (that can brown out the board).
void servosToMiddle() {
  for (int i = 0; i < NUM_SERVOS; i++) {
    ledcAttach(PIN_SERVOS[i], 50, 14);
    servoWrite(PIN_SERVOS[i], SERVO_HOME[i]);
    delay(400);
  }
  delay(400);                    // let the last one arrive
  for (int i = 0; i < NUM_SERVOS; i++) {
    if (SERVO_HOLD[i] || ANIMATE_NECK) continue;
    ledcDetach(PIN_SERVOS[i]);   // no more pulses: the servo stops correcting (and buzzing)
    pinMode(PIN_SERVOS[i], OUTPUT);
    digitalWrite(PIN_SERVOS[i], LOW);  // keep the signal line quiet, not floating
  }
  Serial.printf("Servos: home position %d / %d / %d degrees.\n", SERVO_HOME[0], SERVO_HOME[1], SERVO_HOME[2]);
}

// Slow, never-repeating sway for one axis (0 = turn, 1 = nod, 2 = tilt), in degrees.
// Sine waves whose periods don't divide into each other, so the pattern never visibly loops.
// The nod has a ~4 s wave in it: that reads as breathing.
float neckDrift(int axis, float t) {
  switch (axis) {
    case 0:  return 2.5f * sinf(t * 0.83f)        + 1.2f * sinf(t * 2.03f + 1.3f);
    case 1:  return 1.0f * sinf(t * 1.57f)        + 1.0f * sinf(t * 0.61f + 0.7f);
    default: return 1.2f * sinf(t * 0.47f + 2.1f) + 0.5f * sinf(t * 1.31f);
  }
}

// Runs on core 0 at 50 Hz and moves the head to fit the face:
//   goal = home + pose for the current face + glance + speech nod + drift (limited to NECK_RANGE)
// The position follows the goal through two smoothing stages, so every move starts and stops
// gently (an S-curve) instead of jumping. Offsets below: turn + = left, nod + = up, tilt + = right.
void neckTask(void*) {
  float pos[3], mid[3];                    // smoothed position, and the stage before it
  for (int i = 0; i < NUM_SERVOS; i++) pos[i] = mid[i] = SERVO_HOME[i];
  float glance[3] = {0, 0, 0};
  unsigned long nextGlance = millis() + random(3000, 8000), glanceEnd = 0;
  float talk = 0, talkSlow = 0;            // speech loudness: quick and slow follower
  int side = 1;                            // which way to tilt this time (-1 / +1)
  int last = -1;
  HeadStep cmd[MAX_HEAD_STEPS];            // head moves the AI asked for (all three servos)
  int cmdN = 0, cmdI = 0;
  unsigned long cmdEnd = 0;
  const float maxStep = MAX_SPEED * 0.02f;
  TickType_t wake = xTaskGetTickCount();

  while (true) {
    Face f = face;
    unsigned long now = millis();
    float t = now / 1000.0f;

    if (f != last) {                       // a new face: pick a side, forget the old glance
      side = random(2) ? 1 : -1;
      for (int i = 0; i < 3; i++) glance[i] = 0;
      glanceEnd = 0;
      nextGlance = now + random(2000, 6000);
      last = f;
    }

    float pose[3] = {0, 0, 0};
    float k = 0.06f;                       // smoothing per stage: bigger = quicker moves
    float drift = 1.0f;                    // how much idle sway in this state
    switch (f) {
      case FACE_SLEEP:  pose[1] = -12;                               k = 0.025f; drift = 0.3f; break;
      case FACE_IDLE:                                                k = 0.07f;                break;
      case FACE_LISTEN: pose[1] = -3;  pose[2] = 8 * side;           k = 0.05f;  drift = 0.3f; break;  // curious tilt, keep quiet for the mic
      case FACE_THINK:  pose[0] = 8;   pose[1] = 10; pose[2] = -3;   k = 0.05f;  drift = 0.6f; break;  // up and to the side, like the eyes
      case FACE_SPEAK:                                                           drift = 0.8f; break;
      case FACE_SAD:    pose[1] = -14; pose[2] = -6 * side;          k = 0.04f;  drift = 0.4f; break;
    }

    // speaking: nod on the stressed syllables (loudness above its own average) and sway a little
    talk += (mouthOpen - talk) * 0.08f;
    talkSlow += (talk - talkSlow) * 0.02f;
    float speech[3] = {0, 0, 0};
    if (f == FACE_SPEAK) {
      speech[1] = SPEAK_NOD * (talk - talkSlow);
      speech[0] = 3.0f * sinf(t * 0.9f);
    }

    // glances: now and then look somewhere else for a moment, sometimes chaining a few
    if (IDLE_GLANCES && (f == FACE_IDLE || f == FACE_SPEAK)) {
      float s = f == FACE_SPEAK ? 0.5f : 1.0f;     // smaller while talking
      if (glanceEnd && now >= glanceEnd) {
        glanceEnd = 0;
        if (random(100) < 35) nextGlance = now;    // look somewhere else straight away
        else { for (int i = 0; i < 3; i++) glance[i] = 0; nextGlance = now + random(3000, 9000); }
      } else if (!glanceEnd && now >= nextGlance) {
        glance[0] = random(-15, 16) * s;
        glance[1] = random(-4, 6) * s;
        glance[2] = random(3) == 0 ? random(-6, 7) * s : 0;   // sometimes a curious tilt too
        glanceEnd = now + random(800, 2500);
      }
    }

    // asked to move the head: these moves take over the servos until they're done
    if (headNew) {
      portENTER_CRITICAL(&headMux);
      cmdN = headCount;
      memcpy(cmd, headSteps, cmdN * sizeof(HeadStep));
      headNew = false;
      portEXIT_CRITICAL(&headMux);
      cmdI = -1;
      cmdEnd = 0;
    }
    if (cmdN && now >= cmdEnd) {
      if (++cmdI < cmdN) cmdEnd = now + cmd[cmdI].ms;
      else cmdN = 0;                       // done: back to the normal behaviour
    }

    for (int i = 0; i < NUM_SERVOS; i++) {
      float off = pose[i] + glance[i] + speech[i] + neckDrift(i, t) * drift * DRIFT_SCALE;
      float ki = k;
      if (cmdN) {                          // servo 1 = turn, servo 2 = nod, servo 3 = tilt
        float want = i == 0 ? cmd[cmdI].pan : i == 1 ? cmd[cmdI].nod : cmd[cmdI].roll;
        off = want + neckDrift(i, t) * 0.3f;
        ki = cmd[cmdI].k;
      }
      float goal = SERVO_HOME[i] + constrain(off, -NECK_RANGE[i], NECK_RANGE[i]);
      mid[i] += (goal - mid[i]) * ki;
      pos[i] += constrain((mid[i] - pos[i]) * ki * 1.5f, -maxStep, maxStep);
      servoWriteF(PIN_SERVOS[i], pos[i]);
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));     // one servo frame
  }
}

// BOOT button held? Always false while servo 3 owns GPIO 0.
bool talkPressed() {
  return !USE_SERVOS && digitalRead(PIN_TALK) == LOW;
}

// Access point mode: when the router can't be reached, Happy makes its own Wi-Fi network.
// Join it from the PC; the PC gets an address from Happy (e.g. 192.168.4.2), and Happy sends
// its recordings to that address, using the port and path from STT_URL.
// Restart Happy to go back to the router.
volatile bool apMode = false;
volatile uint32_t apClientIp = 0;   // the last computer that joined Happy's network (0 = none yet)

void onApEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
    apClientIp = info.wifi_ap_staipassigned.ip.addr;
    Serial.printf("Wi-Fi: a computer joined Happy's network, IP %s\n", IPAddress(apClientIp).toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED && WiFi.softAPgetStationNum() == 0) {
    apClientIp = 0;
    Serial.println("Wi-Fi: the computer left Happy's network");
  }
}

void startAccessPoint() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.println("Wi-Fi: could not start Happy's own network (the password must be 8+ characters)");
    return;
  }
  apMode = true;
  Serial.printf("Wi-Fi: started Happy's own network \"%s\" (password \"%s\"), Happy's IP %s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  Serial.println("Wi-Fi: join it from the PC and run stt_server.py there.");
}

// The address to send recordings to: STT_URL as it is, or in access point mode,
// STT_URL with the host swapped for the PC that joined Happy's network.
String serverUrl() {
  if (!apMode) return STT_URL;
  String url = STT_URL;
  int hostStart = url.indexOf("://");
  hostStart = hostStart < 0 ? 0 : hostStart + 3;
  int hostEnd = hostStart;
  while (hostEnd < (int)url.length() && url[hostEnd] != ':' && url[hostEnd] != '/') hostEnd++;
  return url.substring(0, hostStart) + IPAddress(apClientIp).toString() + url.substring(hostEnd);
}

// Joins the router. If that doesn't work within ROUTER_WAIT_MS, Happy starts its own network.
void connectWifi() {
  if (apMode || WiFi.status() == WL_CONNECTED) return;
  face = FACE_SLEEP;
  Serial.printf("Wi-Fi: connecting to %s", WIFI_SSID);
  static bool eventsHooked = false;
  if (!eventsHooked) { WiFi.onEvent(onApEvent); eventsHooked = true; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300); Serial.print(".");
    if (millis() - t0 > ROUTER_WAIT_MS) {
      wl_status_t st = WiFi.status();
      Serial.printf("\nWi-Fi: not connected (status %d: %s).\n", st,
        st == WL_NO_SSID_AVAIL  ? "network not found - check the name, and that it is 2.4 GHz" :
        st == WL_CONNECT_FAILED ? "connect failed - check the password" :
                                  "no answer from the router - wrong password or WPA3-only network?");
      startAccessPoint();
      face = FACE_IDLE;
      return;
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
  while (talkPressed() && n < MAX_SAMPLES) {
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
    if (talkPressed()) return 0;
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
  if (apMode && apClientIp == 0) {
    Serial.printf("No computer has joined \"%s\" yet - nobody to send this to.\n", AP_SSID);
    face = FACE_SAD;
    delay(1500);
    face = FACE_IDLE;
    return false;
  }
  face = FACE_THINK;
  HTTPClient http;
  http.setTimeout(60000);        // Whisper + Gemma + Kokoro; the first request after a restart is slower
  http.begin(serverUrl());
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Happy-Wake", needName ? "1" : "0");
  const char* keys[] = {"X-Happy-Text", "X-Happy-Reply", "X-Happy-Ignored", "X-Happy-Head"};
  http.collectHeaders(keys, 4);

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
  String head = http.header("X-Happy-Head");
  if (head.length()) Serial.printf("Head:  %s\n", head.c_str());

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
  if (USE_SERVOS && ANIMATE_NECK && head.length()) queueHeadMoves(head);   // turn while speaking
  face = FACE_SPEAK;
  playReply(got);
  face = FACE_IDLE;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  esp_reset_reason_t why = esp_reset_reason();
  if (why == ESP_RST_BROWNOUT) Serial.println("Started after a BROWN-OUT: the servos or the speaker pulled the power down!");
  if (USE_SERVOS) servosToMiddle();
  else            pinMode(PIN_TALK, INPUT_PULLUP);
  if (USE_SERVOS && ANIMATE_NECK) xTaskCreatePinnedToCore(neckTask, "neck", 4096, nullptr, 2, nullptr, 0);
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
  if (USE_SERVOS) Serial.println("Ready. Say \"Happy, ...\" (BOOT push-to-talk is off: servo 3 uses GPIO 0).");
  else if (HANDS_FREE) Serial.println("Ready. Say \"Happy, ...\" (or hold BOOT, talk, release).");
  else            Serial.println("Ready. Hold BOOT, talk, release.");
}

void loop() {
  size_t n = 0;
  bool needName = false;

  if (talkPressed()) {                 // push-to-talk: no name needed
    Serial.println("Listening...");
    n = recordWhileHeld();
    while (talkPressed()) delay(10);   // wait for release
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
