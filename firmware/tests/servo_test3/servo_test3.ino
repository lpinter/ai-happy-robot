// Servo test 3: the third servo. Slowly turns the micro servo one way,
// back to the middle, the other way, and back. No library: a 50 Hz PWM signal
// where the pulse width (0.5-2.5 ms) sets the angle.
// Wiring (see README): red -> 5V, brown -> GND, orange -> GPIO 0.
// GPIO 0 is the BOOT button pin. The servo's signal input doesn't pull it low,
// so the board still boots normally. Pressing BOOT while this runs will make
// the servo jump; that's harmless.
// Prints why the board started, so a brown-out reset from the servo shows up.

#include <esp_system.h>

const int PIN_SERVO = 0;

void writeAngle(int deg) {
  int us = map(deg, 0, 180, 500, 2500);
  ledcWrite(PIN_SERVO, (uint32_t)us * 16384 / 20000);  // 14-bit duty, 20 ms period
}

void sweep(int from, int to) {
  int step = from < to ? 1 : -1;
  for (int a = from; a != to; a += step) {
    writeAngle(a);
    delay(12);  // slow and smooth
  }
  writeAngle(to);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  esp_reset_reason_t why = esp_reset_reason();
  Serial.printf("\n=== Servo test 3 (GPIO %d) === (start reason %d%s)\n", PIN_SERVO, why,
                why == ESP_RST_BROWNOUT ? ": BROWN-OUT, the servo pulled the power down!" : "");
  ledcAttach(PIN_SERVO, 50, 14);
  writeAngle(90);
  delay(800);
}

void loop() {
  Serial.println("servo 3: one way");
  sweep(90, 30);
  delay(600);
  sweep(30, 90);
  delay(400);
  Serial.println("servo 3: other way");
  sweep(90, 150);
  delay(600);
  sweep(150, 90);
  delay(1200);
}
