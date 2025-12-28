#include <WiFiNINA.h>
#include <WiFiClient.h>
#include <Firebase_Arduino_WiFiNINA.h>
#include "DHT.h"

// -------- WiFi Login --------
char ssid[] = "Rafid";
char pass[] = "rafidhasan";

// -------- Firebase --------
#define FIREBASE_HOST "iottest1-4dd35-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "fYcuqbpJG1G6JkucJeUPzsjIdsKUP7djAqvZ3GMN"

WiFiClient client;
FirebaseData fb;

// -------- DHT22 --------
#define DHTPIN 2
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// -------- Sensors --------
int soilPin = A1;
int ldrPin  = A3;

// -------- Ultrasonic (tank) --------
#define TRIG_PIN 6
#define ECHO_PIN 7

// 3 cm = FULL, 11 cm = EMPTY
float FULL_DIST = 3.0;
float EMPTY_DIST = 11.0;

// -------- Actuators --------
int fanPin  = 5;
int pumpPin = 4;
int ledPin  = 8;

int fanState  = 0;
int pumpState = 0;
int ledState  = 0;
int autoMode  = 0;

// Pump safety timer
unsigned long pumpStartTime = 0;
unsigned long PUMP_MAX_RUNTIME = 3000; // 3 sec

// -------- Helpers --------
float fconstrain(float x, float a, float b) {
  return (x < a ? a : (x > b ? b : x));
}

int computeHealthScore(float t, float h, int soil, int light, float waterPct) {
  float s = 100;
  if (t < 18 || t > 32) s -= 20;
  else if (t < 20 || t > 30) s -= 10;

  if (h < 30 || h > 80) s -= 15;
  else if (h < 35 || h > 70) s -= 7;

  if (soil < 35 || soil > 85) s -= 25;
  else if (soil < 45 || soil > 75) s -= 12;

  if (light < 30 || light > 95) s -= 20;
  else if (light < 50 || light > 90) s -= 10;

  if (waterPct < 10) s -= 15;
  else if (waterPct < 20) s -= 5;

  if (s < 0) s = 0;
  if (s > 100) s = 100;

  return (int)s;
}

void setup() {
  Serial.begin(9600);
  while (!Serial);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(fanPin, OUTPUT);
  pinMode(pumpPin, OUTPUT);
  pinMode(ledPin, OUTPUT);

  digitalWrite(fanPin, LOW);
  digitalWrite(pumpPin, LOW);
  digitalWrite(ledPin, LOW);

  dht.begin();

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.println("\nWiFi Connected!");

  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH, ssid, pass);
  Firebase.reconnectWiFi(true);

  Serial.println("\nCOMMANDS:");
  Serial.println("fan on/off");
  Serial.println("pump on/off");
  Serial.println("light on/off");
  Serial.println("auto on/off");
}

void loop() {

  // -------------------------------------------------------
  // ULTRASONIC SENSOR
  // -------------------------------------------------------
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH);
  float distance = duration * 0.0343 / 2;

  // Clamp between 3cm and 11cm
  distance = fconstrain(distance, FULL_DIST, EMPTY_DIST);

  // Convert to empty percentage:
  // 11cm = 100% empty
  // 3cm = 0% empty
  float emptyPercent = (distance - FULL_DIST) * 100.0 / (EMPTY_DIST - FULL_DIST);

  // Convert to FULL percentage (this is what UI uses)
  float tankFill = 100.0 - emptyPercent;

  tankFill = fconstrain(tankFill, 0, 100);

  // -------------------------------------------------------
  // SOIL MOISTURE
  // -------------------------------------------------------
  int soilRaw = analogRead(soilPin);
  int soilPercent = constrain(map(soilRaw, 1023, 300, 0, 100), 0, 100);

  // -------------------------------------------------------
  // LIGHT SENSOR
  // -------------------------------------------------------
  int ldrRaw = analogRead(ldrPin);
  int lightPercent = constrain(map(ldrRaw, 0, 1023, 0, 100), 0, 100);

  // -------------------------------------------------------
  // DHT SENSOR
  // -------------------------------------------------------
  float humidity = dht.readHumidity();
  float tempC = dht.readTemperature();

  // -------------------------------------------------------
  // READ AUTOMATION TOGGLES
  // -------------------------------------------------------
  if (Firebase.getInt(fb, "/greenhouse/control/auto_mode")) {
    autoMode = fb.intData();
  }

  int desiredFan  = fanState;
  int desiredPump = pumpState;
  int desiredLed  = ledState;

  // -------------------------------------------------------
  // AUTO MODE LOGIC
  // -------------------------------------------------------
  if (autoMode == 1) {
    desiredFan  = (tempC > 30 || humidity > 70) ? 1 : 0;
    desiredPump = (soilPercent < 40 && tankFill > 20) ? 1 : 0;
    desiredLed  = (lightPercent < 40) ? 1 : 0;
  } else {
    if (Firebase.getInt(fb, "/greenhouse/control/fan"))
      desiredFan = fb.intData();

    if (Firebase.getInt(fb, "/greenhouse/control/pump"))
      desiredPump = fb.intData();

    if (Firebase.getInt(fb, "/greenhouse/control/light"))
      desiredLed = fb.intData();
  }

  // -------------------------------------------------------
  // APPLY FAN
  // -------------------------------------------------------
  fanState = desiredFan;
  digitalWrite(fanPin, fanState ? HIGH : LOW);

  // -------------------------------------------------------
  // APPLY LED
  // -------------------------------------------------------
  ledState = desiredLed;
  digitalWrite(ledPin, ledState ? HIGH : LOW);

  // -------------------------------------------------------
  // PUMP WITH SAFETY TIMER
  // -------------------------------------------------------
  if (desiredPump == 1 && pumpState == 0) {
    pumpState = 1;
    pumpStartTime = millis();
    Serial.println("Pump ON (timer started)");
  }

  if (pumpState == 1) {
    if (millis() - pumpStartTime > PUMP_MAX_RUNTIME) {
      pumpState = 0;
      Serial.println("Pump auto-OFF (safety limit reached)");
    }
  }

  digitalWrite(pumpPin, pumpState ? HIGH : LOW);

  // -------------------------------------------------------
  // HEALTH SCORE
  // -------------------------------------------------------
  int healthScore = computeHealthScore(tempC, humidity, soilPercent, lightPercent, tankFill);

  // -------------------------------------------------------
  // FIREBASE UPLOAD
  // -------------------------------------------------------
  Firebase.setFloat(fb, "/greenhouse/water_level_distance", distance);
  Firebase.setFloat(fb, "/greenhouse/water_level_percent", tankFill);
  Firebase.setInt(fb,   "/greenhouse/soil_moisture", soilPercent);
  Firebase.setInt(fb,   "/greenhouse/light", lightPercent);
  Firebase.setFloat(fb, "/greenhouse/temperature", tempC);
  Firebase.setFloat(fb, "/greenhouse/humidity", humidity);
  Firebase.setInt(fb,   "/greenhouse/fan_state", fanState);
  Firebase.setInt(fb,   "/greenhouse/pump_state", pumpState);
  Firebase.setInt(fb,   "/greenhouse/light_state", ledState);
  Firebase.setInt(fb,   "/greenhouse/health_score", healthScore);

  Firebase.pushFloat(fb, "/greenhouse/logs/temperature", tempC);
  Firebase.pushFloat(fb, "/greenhouse/logs/humidity", humidity);
  Firebase.pushInt(fb,   "/greenhouse/logs/soil_moisture", soilPercent);
  Firebase.pushFloat(fb, "/greenhouse/logs/water_level_percent", tankFill);

  // -------------------------------------------------------
  // SERIAL COMMANDS
  // -------------------------------------------------------
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim(); cmd.toLowerCase();

    if (cmd == "fan on") fanState = 1;
    else if (cmd == "fan off") fanState = 0;
    else if (cmd == "light on") ledState = 1;
    else if (cmd == "light off") ledState = 0;
    else if (cmd == "pump on") { pumpState = 1; pumpStartTime = millis(); }
    else if (cmd == "pump off") pumpState = 0;
    else if (cmd == "auto on") autoMode = 1;
    else if (cmd == "auto off") autoMode = 0;
    else Serial.println("Unknown command");
  }

  // -------------------------------------------------------
  // DEBUG PRINT
  // -------------------------------------------------------
  Serial.println("----- GREENHOUSE DATA -----");
  Serial.print("Distance(cm): "); Serial.println(distance);
  Serial.print("Tank Fill(%): "); Serial.println(tankFill);
  Serial.print("Temp: "); Serial.println(tempC);
  Serial.print("Humidity: "); Serial.println(humidity);
  Serial.print("Soil %: "); Serial.println(soilPercent);
  Serial.print("Light %: "); Serial.println(lightPercent);
  Serial.print("Fan: "); Serial.println(fanState);
  Serial.print("Pump: "); Serial.println(pumpState);
  Serial.print("LED: "); Serial.println(ledState);
  Serial.print("Auto: "); Serial.println(autoMode);
  Serial.print("Health Score: "); Serial.println(healthScore);
  Serial.println("---------------------------");

  delay(2000);
}
