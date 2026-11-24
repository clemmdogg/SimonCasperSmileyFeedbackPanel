#include <Arduino.h>
#include <ezButton.h>

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