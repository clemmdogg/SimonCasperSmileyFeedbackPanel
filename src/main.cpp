#include <Arduino.h>
#include <ezButton.h>
#include "esp_sleep.h"

// Buttons and LEDs
const int GREENBUTTON_PIN = 33;
const int GREENLED_PIN = 18;
ezButton greenButton(GREENBUTTON_PIN);
const int YELLOWBUTTON_PIN = 32;
const int YELLOWLED_PIN = 19;
ezButton yellowButton(YELLOWBUTTON_PIN);
const int BLUEBUTTON_PIN = 35;
const int BLUELED_PIN = 21;
ezButton blueButton(BLUEBUTTON_PIN);
const int REDBUTTON_PIN = 34;
const int REDLED_PIN = 23;
ezButton redButton(REDBUTTON_PIN);
const unsigned long DEBOUNCE_MS = 50;

// Sleep mode configuration
const uint64_t SLEEP_SECONDS = 10;
const uint64_t US_PR_SECOND = 1000000ULL;
RTC_DATA_ATTR int startUps = 0;

void writeWakeupCause() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Wake up by: timer"); break;
    case ESP_SLEEP_WAKEUP_EXT0:  Serial.println("Wakeup by: button (ext0)"); break;
    case ESP_SLEEP_WAKEUP_EXT1:  Serial.println("Wakeup by: button (ext1)"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup by: touch"); break;
    default:
      Serial.printf("Not woken up from deep sleep (%d)\n", cause);
      break;
  }
}


void setup() {
  Serial.begin(115200);
  greenButton.setDebounceTime(DEBOUNCE_MS);
  pinMode(GREENLED_PIN, OUTPUT);
  yellowButton.setDebounceTime(DEBOUNCE_MS);
  pinMode(YELLOWLED_PIN, OUTPUT);
  blueButton.setDebounceTime(DEBOUNCE_MS);
  pinMode(BLUELED_PIN, OUTPUT);
  redButton.setDebounceTime(DEBOUNCE_MS);
  pinMode(REDLED_PIN, OUTPUT);
  delay(500);

  startUps++;
  Serial.printf("Opstart nr. %d\n", startUps);
  writeWakeupCause();



  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * US_PR_SECOND);
  Serial.printf("Sover i %llu s\n", SLEEP_SECONDS);
  Serial.flush(); 
  esp_deep_sleep_start();
}

void loop() {
  greenButton.loop();
  yellowButton.loop();
  blueButton.loop();
  redButton.loop();

  if (greenButton.isPressed()) {
    digitalWrite(GREENLED_PIN, HIGH);
    delay(500);
    digitalWrite(GREENLED_PIN, LOW);
  }
  if (yellowButton.isPressed()) {
    digitalWrite(YELLOWLED_PIN, HIGH);
    delay(500);
    digitalWrite(YELLOWLED_PIN, LOW);
  }
  if (blueButton.isPressed()) {
    digitalWrite(BLUELED_PIN, HIGH);
    delay(500);
    digitalWrite(BLUELED_PIN, LOW);
  }
  if (redButton.isPressed()) {
    digitalWrite(REDLED_PIN, HIGH);
    delay(500);
    digitalWrite(REDLED_PIN, LOW);
  }
}