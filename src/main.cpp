#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ezButton.h>
#include <time.h>
#include "esp_sleep.h"
#include "secrets.h"

// TLS (encrypted MQTT) is used on port 8883, unless secrets.h says otherwise with MQTT_USE_TLS
#ifndef MQTT_USE_TLS
#define MQTT_USE_TLS (MQTT_PORT == 8883)
#endif
#if MQTT_USE_TLS
#include <WiFiClientSecure.h>
#include "mqtt_ca.h"
#endif

// ---------- Buttons and LEDs ----------
// What each colour means, sent along with the LED message
struct Feedback {
  int buttonPin;
  int ledPin;
  const char *colour;
  const char *smiley;
  int value;
};
const Feedback FEEDBACK[] = {
  {33, 18, "green",  ":-D", 1},
  {32, 19, "yellow", ":-)", 2},
  {35, 21, "blue",   ":-/", 3},
  {34, 23, "red",    ":-(", 4},
};
const int BUTTON_COUNT = sizeof(FEEDBACK) / sizeof(FEEDBACK[0]);
const int NO_LED = -1;

ezButton buttons[] = {
  ezButton(FEEDBACK[0].buttonPin, EXTERNAL_PULLDOWN),
  ezButton(FEEDBACK[1].buttonPin, EXTERNAL_PULLDOWN),
  ezButton(FEEDBACK[2].buttonPin, EXTERNAL_PULLDOWN),
  ezButton(FEEDBACK[3].buttonPin, EXTERNAL_PULLDOWN),
};
const unsigned long DEBOUNCE_MS = 50;
const unsigned long LED_ON_MS = 500;

// ---------- Sleep ----------
const unsigned long IDLE_TIMEOUT_MS = 30000;  // Go to deep sleep after 30 seconds of inactivity
const time_t HEARTBEAT_INTERVAL_S = 3600;     // Log "alive" every hour
const uint64_t US_PR_SECOND = 1000000ULL;

// ---------- MQTT ----------
// button: {"colour":"green","smiley":":-D","value":1,"ledAlreadyOn":true,"ledOnColour":"blue",
//          "timestamp":"2026-11-25T10:15:30+01:00"}
// status: {"status":"awake","timestamp":"2026-11-25T10:15:30+01:00"}  (started / awake / sleep / alive)
const char *TOPIC_BUTTON = "elever/casper/smileypanel/button";
const char *TOPIC_STATUS = "elever/casper/smileypanel/status";
const unsigned long WIFI_TIMEOUT_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;

// ---------- Clock (NTP), Danish time with automatic summer time ----------
const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "dk.pool.ntp.org";
const unsigned long NTP_TIMEOUT_MS = 5000;
const time_t VALID_TIME = 1700000000;  // Any time before this means the clock is not set yet

// ---------- Message queue ----------
// Every message is put in a queue first and sent when there is a connection.
// The queue is kept in RTC memory, so messages that could not be sent survive deep sleep.
enum Status : uint8_t { STATUS_STARTED, STATUS_AWAKE, STATUS_SLEEP, STATUS_ALIVE };
const char *STATUS_NAMES[] = {"started", "awake", "sleep", "alive"};

enum EventType : uint8_t { EVENT_STATUS, EVENT_BUTTON };
struct Event {
  time_t time;
  EventType type;
  uint8_t code;     // Status, or index in FEEDBACK for a button event
  int8_t ledOn;     // Index in FEEDBACK of the LED that was already on when the button was pressed, or NO_LED
};
const int QUEUE_SIZE = 64;  // When full, the oldest message is dropped

// Kept in RTC memory, so they survive deep sleep.
// time() keeps counting during deep sleep, so it is used to time the heartbeat.
RTC_DATA_ATTR int startUps = 0;
RTC_DATA_ATTR time_t lastHeartbeat = 0;
RTC_DATA_ATTR Event queue[QUEUE_SIZE];
RTC_DATA_ATTR int queueStart = 0;
RTC_DATA_ATTR int queueCount = 0;

int litLed = NO_LED;                    // Index in FEEDBACK of the LED that is on, or NO_LED
unsigned long litSinceMs = 0;
unsigned long lastActivityMs = 0;
unsigned long lastMqttAttemptMs = 0;
bool pressedSinceBoot = false;
#if MQTT_USE_TLS
WiFiClientSecure netClient;  // Encrypted; checks the broker's certificate against MQTT_CA_CERT
#else
WiFiClient netClient;        // Not encrypted
#endif
PubSubClient mqtt(netClient);

// ---------- Queue ----------

void enqueue(EventType type, uint8_t code, int8_t ledOn) {
  if (queueCount == QUEUE_SIZE) {  // Full: drop the oldest
    queueStart = (queueStart + 1) % QUEUE_SIZE;
    queueCount--;
    Serial.println("Queue full - oldest message dropped");
  }
  queue[(queueStart + queueCount) % QUEUE_SIZE] = {time(nullptr), type, code, ledOn};
  queueCount++;
}

void logStatus(Status status) {
  enqueue(EVENT_STATUS, status, NO_LED);
  if (status == STATUS_ALIVE) lastHeartbeat = time(nullptr);
}

// ---------- JSON ----------

// Local time as ISO 8601, e.g. 2026-11-25T10:15:30+01:00
void formatTimestamp(time_t time, char *buffer, size_t size) {
  struct tm local;
  localtime_r(&time, &local);
  char offset[6];  // "+0100"
  strftime(offset, sizeof(offset), "%z", &local);
  size_t len = strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &local);
  snprintf(buffer + len, size - len, "%.3s:%.2s", offset, offset + 3);
}

const char *buildJson(const Event &event, char *json, size_t size) {
  char timestamp[32];
  formatTimestamp(event.time, timestamp, sizeof(timestamp));

  if (event.type == EVENT_STATUS) {
    snprintf(json, size, "{\"status\":\"%s\",\"timestamp\":\"%s\"}",
             STATUS_NAMES[event.code], timestamp);
    return TOPIC_STATUS;
  }

  const Feedback &pressed = FEEDBACK[event.code];
  // Colour of the LED that was already on, as a JSON string, or null
  char ledOnColour[16] = "null";
  if (event.ledOn != NO_LED) snprintf(ledOnColour, sizeof(ledOnColour), "\"%s\"", FEEDBACK[event.ledOn].colour);

  snprintf(json, size,
           "{\"colour\":\"%s\",\"smiley\":\"%s\",\"value\":%d,"
           "\"ledAlreadyOn\":%s,\"ledOnColour\":%s,\"timestamp\":\"%s\"}",
           pressed.colour, pressed.smiley, pressed.value,
           event.ledOn != NO_LED ? "true" : "false", ledOnColour, timestamp);
  return TOPIC_BUTTON;
}

// ---------- WiFi and MQTT ----------

bool connectMqtt() {
  if (mqtt.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = "smileypanel-" + WiFi.macAddress();
  bool ok = (strlen(MQTT_USER) > 0)
                ? mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)
                : mqtt.connect(clientId.c_str());
  if (ok) {
    Serial.println("MQTT connect: OK");
    return true;
  }

  // state: -2 = no connection to the broker (for TLS also: certificate rejected), 4 = wrong user/password, 5 = not allowed
  Serial.printf("MQTT connect: failed (state %d)\n", mqtt.state());
#if MQTT_USE_TLS
  char tlsError[100];
  if (netClient.lastError(tlsError, sizeof(tlsError)) != 0) Serial.printf("  TLS error: %s\n", tlsError);
#endif
  return false;
}

// Send queued messages, oldest first. Stops at the first one that fails.
void sendQueue() {
  char json[192];
  while (queueCount > 0 && mqtt.connected()) {
    const char *topic = buildJson(queue[queueStart], json, sizeof(json));
    if (!mqtt.publish(topic, json)) break;
    Serial.printf("[%s] %s\n", topic, json);
    queueStart = (queueStart + 1) % QUEUE_SIZE;
    queueCount--;
  }
  if (queueCount > 0) Serial.printf("%d message(s) waiting for a connection\n", queueCount);
}

// ---------- Buttons and LEDs ----------

void turnOffLed() {
  if (litLed == NO_LED) return;
  digitalWrite(FEEDBACK[litLed].ledPin, LOW);
  litLed = NO_LED;
}

// Registers every press. Only one LED is on at a time, and it always gets its full LED_ON_MS:
// a press while an LED is on is registered (with the colour that is on), but lights nothing.
void registerPress(int index) {
  enqueue(EVENT_BUTTON, index, litLed);
  if (litLed == NO_LED) {
    digitalWrite(FEEDBACK[index].ledPin, HIGH);
    litLed = index;
    litSinceMs = millis();
  }
  lastActivityMs = millis();
  pressedSinceBoot = true;
}

// Turns the LED off after LED_ON_MS and reads the buttons. Never waits, so no press is missed.
void pollButtons() {
  if (litLed != NO_LED && millis() - litSinceMs >= LED_ON_MS) turnOffLed();
  for (int i = 0; i < BUTTON_COUNT; i++) {
    buttons[i].loop();
    if (buttons[i].isPressed()) registerPress(i);
  }
}

// ---------- Startup steps that take time (the buttons are read while waiting) ----------

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    pollButtons();
    delay(10);
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " failed");
}

// The clock keeps running in deep sleep, so it only has to be set via NTP after power on.
// After a wakeup NTP still runs in the background and corrects any drift.
void syncClock() {
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
  if (time(nullptr) >= VALID_TIME || WiFi.status() != WL_CONNECTED) return;

  Serial.print("Setting clock via NTP");
  unsigned long start = millis();
  while (time(nullptr) < VALID_TIME && millis() - start < NTP_TIMEOUT_MS) {
    pollButtons();
    delay(10);
  }
  Serial.println(time(nullptr) >= VALID_TIME ? " OK" : " failed");
}

// ---------- Sleep ----------

void goToDeepSleep(bool announce) {
  if (announce) logStatus(STATUS_SLEEP);
  turnOffLed();

  // Send what we can. Anything not sent stays in the queue in RTC memory.
  if (connectMqtt()) {
    sendQueue();
    mqtt.loop();
    delay(100);  // Give the last messages time to leave
  }

  // Wake up when a button is pressed, or when the next heartbeat is due.
  // Clamped in case the clock jumped when NTP set it.
  time_t secondsLeft = HEARTBEAT_INTERVAL_S - (time(nullptr) - lastHeartbeat);
  if (secondsLeft < 1) secondsLeft = 1;
  if (secondsLeft > HEARTBEAT_INTERVAL_S) secondsLeft = HEARTBEAT_INTERVAL_S;

  uint64_t wakeupMask = 0;
  for (int i = 0; i < BUTTON_COUNT; i++) wakeupMask |= 1ULL << FEEDBACK[i].buttonPin;
  esp_sleep_enable_ext1_wakeup(wakeupMask, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)secondsLeft * US_PR_SECOND);

  Serial.printf("Deep sleep (next heartbeat in %ld s)\n", (long)secondsLeft);
  Serial.flush();
  mqtt.disconnect();
  WiFi.disconnect(true);
  esp_deep_sleep_start();
}

// ---------- Main program ----------

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < BUTTON_COUNT; i++) {
    buttons[i].setDebounceTime(DEBOUNCE_MS);
    pinMode(FEEDBACK[i].ledPin, OUTPUT);
  }

  startUps++;
  Serial.printf("Startup no. %d\n", startUps);
  Serial.printf("MQTT: %s on port %d\n", MQTT_USE_TLS ? "TLS (encrypted, certificate checked)" : "NOT encrypted", MQTT_PORT);

  // Register the press that woke us straight away, before WiFi, so the LED lights at once
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    logStatus(STATUS_AWAKE);
    uint64_t wakePins = esp_sleep_get_ext1_wakeup_status();
    for (int i = 0; i < BUTTON_COUNT; i++) {
      if (wakePins & (1ULL << FEEDBACK[i].buttonPin)) registerPress(i);
    }
  }

#if MQTT_USE_TLS
  netClient.setCACert(MQTT_CA_CERT);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectWifi();
  syncClock();

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Woken only to report that the panel is still running
    logStatus(STATUS_ALIVE);
    if (!pressedSinceBoot) goToDeepSleep(false);
    logStatus(STATUS_AWAKE);  // Someone pressed a button meanwhile, so stay awake
  } else if (cause != ESP_SLEEP_WAKEUP_EXT1) {
    // Power on or reset
    logStatus(STATUS_STARTED);
    logStatus(STATUS_ALIVE);
  }

  connectMqtt();
  sendQueue();
  lastActivityMs = millis();
}

void loop() {
  pollButtons();

  // Keep the MQTT connection alive, retry now and then if it dropped, and send the queue
  if (mqtt.connected()) {
    mqtt.loop();
    sendQueue();
  } else if (millis() - lastMqttAttemptMs >= MQTT_RETRY_MS) {
    lastMqttAttemptMs = millis();
    if (connectMqtt()) sendQueue();
  }

  if (time(nullptr) - lastHeartbeat >= HEARTBEAT_INTERVAL_S) {
    logStatus(STATUS_ALIVE);
  }

  if (millis() - lastActivityMs >= IDLE_TIMEOUT_MS) {
    goToDeepSleep(true);
  }
}
