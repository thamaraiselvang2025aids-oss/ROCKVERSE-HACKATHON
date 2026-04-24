// HealthGuard ESP32 - Wokwi Simulation
// Fixed LEDs + ThingSpeak cloud + Telegram alerts

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ---------------- WIFI ----------------
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ---------------- CLOUD (ThingSpeak) ----------------
String apiKey = "GXWQCVCZKNZQ9YDB";

// ---------------- TELEGRAM ----------------
String botToken = "8610542234:AAFn2whrAKlx8Gk6FlLXgsQ5vyeNT6x9kOE";
String chatID   = "7053488830";

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- PINS ----------------
#define BUZZER     13
#define LED_GREEN  25
#define LED_YELLOW 26
#define LED_RED    27

// ---------------- VARIABLES ----------------
float hr, spo2, sysBP, diaBP, stress;
float bpm = 80;

bool arrhythmia = false;
bool strokeRisk = false;

// ECG
float ecgX = 0;
int   lastY = 32;
int   xPos  = 0;

// BPM
unsigned long lastBeat     = 0;
bool          peakDetected = false;

// Demo cycle: 0=SAFE, 1=ARRHYTHMIA(yellow), 2=STROKE(red)
#define DEMO_DURATION 5000UL
int           demoState      = 0;
unsigned long demoStateStart = 0;

// Buzzer (non-blocking pulsing)
unsigned long lastBeep = 0;
bool          buzzerOn = false;
#define BEEP_ON    200UL
#define BEEP_OFF_A 800UL
#define BEEP_OFF_S 300UL

// Upload / alert timing
unsigned long lastUpload = 0;
unsigned long lastAlert  = 0;
#define UPLOAD_INTERVAL 15000UL   // ThingSpeak minimum 15s
#define ALERT_INTERVAL  30000UL   // Telegram cooldown 30s

// ---------------- ECG ----------------
float ecgFunc(float x) {
  return  0.12f * exp(-pow((x - 0.20f) / 0.040f, 2))
        - 0.25f * exp(-pow((x - 0.37f) / 0.015f, 2))
        + 1.20f * exp(-pow((x - 0.40f) / 0.010f, 2))
        - 0.35f * exp(-pow((x - 0.43f) / 0.012f, 2))
        + 0.35f * exp(-pow((x - 0.70f) / 0.040f, 2));
}

void drawECG(float val) {
  int y = 42 - (int)(val * 25);
  display.drawLine(xPos, lastY, xPos + 1, y, WHITE);
  lastY = y;
  xPos++;
  if (xPos >= 128) {
    xPos = 0;
    display.fillRect(0, 16, 128, 40, BLACK);
  }
}

// ---------------- BPM ----------------
void detectBPM(float val) {
  if (val > 0.9f && !peakDetected) {
    peakDetected = true;
    unsigned long now = millis();
    if (now - lastBeat > 300) {
      bpm      = 60.0f / ((now - lastBeat) / 1000.0f);
      lastBeat = now;
    }
  }
  if (val < 0.4f) peakDetected = false;
  if (bpm < 40 || bpm > 200) bpm = 80;
}

// ---------------- DEMO STATE MACHINE ----------------
void applyDemoState() {
  unsigned long now = millis();
  if (now - demoStateStart >= DEMO_DURATION) {
    demoState = (demoState + 1) % 3;
    demoStateStart = now;
  }

  switch (demoState) {

    case 0: // SAFE - green  (riskScore ~0)
      hr    = 82  + random(-3, 3);
      spo2  = 98  + random(-1, 1);
      sysBP = 118 + random(-4, 4);
      diaBP = 78;
      bpm   = 80;
      break;

    case 1: // ARRHYTHMIA - yellow  (riskScore 4, safely below 6)
      // HR=97->+1, BP=133->+1, BPM=103->+1, stress=(37+3)=40->+1 = total 4
      hr    = 97  + random(-1, 1);
      spo2  = 97  + random(-1, 0);  // keep SpO2 high so stress stays <=40
      sysBP = 133 + random(-2, 2);
      diaBP = 84;
      bpm   = 103;
      break;

    case 2: // STROKE RISK - red  (riskScore >= 6)
      hr    = 118 + random(-2, 2);
      spo2  = 91  + random(-1, 0);
      sysBP = 152 + random(-3, 3);
      diaBP = 95;
      bpm   = 125;
      break;
  }
}

// ---------------- AI LOGIC ----------------
void aiPrediction() {
  stress = (hr - 60) + (100 - spo2);

  int riskScore = 0;

  if      (hr > 110) riskScore += 2;
  else if (hr >  95) riskScore += 1;

  if      (spo2 < 93) riskScore += 2;
  else if (spo2 < 96) riskScore += 1;

  if      (sysBP > 145) riskScore += 2;
  else if (sysBP > 130) riskScore += 1;

  if      (bpm > 120 || bpm <  55) riskScore += 2;
  else if (bpm > 100 || bpm <  65) riskScore += 1;

  if      (stress > 40) riskScore += 2;
  else if (stress > 25) riskScore += 1;

  strokeRisk = (riskScore >= 6);
  arrhythmia = (!strokeRisk && riskScore >= 3);
}

// ---------------- LED ----------------
void updateLED() {
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED,    LOW);

  if      (strokeRisk) digitalWrite(LED_RED,    HIGH);
  else if (arrhythmia) digitalWrite(LED_YELLOW, HIGH);
  else                 digitalWrite(LED_GREEN,  HIGH);
}

// ---------------- BUZZER (non-blocking) ----------------
void updateBuzzer() {
  if (!strokeRisk && !arrhythmia) {
    noTone(BUZZER);
    buzzerOn = false;
    return;
  }

  unsigned long now     = millis();
  unsigned long offTime = strokeRisk ? BEEP_OFF_S : BEEP_OFF_A;
  int           freq    = strokeRisk ? 2000 : 1000;

  if (!buzzerOn && (now - lastBeep >= offTime)) {
    tone(BUZZER, freq);
    buzzerOn = true;
    lastBeep = now;
  } else if (buzzerOn && (now - lastBeep >= BEEP_ON)) {
    noTone(BUZZER);
    buzzerOn = false;
    lastBeep = now;
  }
}

// ---------------- THINGSPEAK UPLOAD ----------------
void uploadCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://api.thingspeak.com/update?api_key=" + apiKey
             + "&field1=" + String((int)hr)
             + "&field2=" + String((int)spo2)
             + "&field3=" + String((int)sysBP)
             + "&field4=" + String((int)diaBP)
             + "&field5=" + String((int)bpm)
             + "&field6=" + String((int)stress);

  http.begin(url);
  int code = http.GET();
  Serial.print("ThingSpeak HTTP: "); Serial.println(code);
  http.end();
}

// ---------------- TELEGRAM ALERT ----------------
void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + botToken
             + "/sendMessage?chat_id=" + chatID
             + "&text=" + message;

  http.begin(url);
  int code = http.GET();
  Serial.print("Telegram HTTP: "); Serial.println(code);
  http.end();
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(BUZZER,     OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED,    OUTPUT);

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wStart < 8000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " Connected!" : " Timeout");

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed");
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  demoStateStart = millis();

  display.setTextSize(1);
  display.setCursor(10, 24);
  display.print("HealthGuard Ready");
  display.display();
  delay(1000);
  display.clearDisplay();
}

// ---------------- LOOP ----------------
void loop() {

  // ---- Demo vitals ----
  applyDemoState();

  // ---- ECG ----
  float yVal = ecgFunc(fmod(ecgX, 1.0f));
  detectBPM(yVal);
  drawECG(yVal);
  ecgX += 0.01f;

  // ---- AI ----
  aiPrediction();

  // ---- Outputs ----
  updateLED();
  updateBuzzer();

  unsigned long now = millis();

  // ---- ThingSpeak upload every 15s ----
  if (now - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = now;
    uploadCloud();
  }

  // ---- Telegram alert with 30s cooldown ----
  if (now - lastAlert >= ALERT_INTERVAL) {
    if (strokeRisk) {
      lastAlert = now;
      sendTelegram("STROKE ALERT! HR:" + String((int)hr)
                 + " SpO2:" + String((int)spo2)
                 + " BP:" + String((int)sysBP) + "/" + String((int)diaBP)
                 + " BPM:" + String((int)bpm));
    } else if (arrhythmia) {
      lastAlert = now;
      sendTelegram("ARRHYTHMIA DETECTED! HR:" + String((int)hr)
                 + " SpO2:" + String((int)spo2)
                 + " BPM:" + String((int)bpm));
    }
  }

  // ---- OLED top bar ----
  display.fillRect(0, 0, 128, 16, BLACK);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("HR:");   display.print((int)hr);
  display.setCursor(64, 0);
  display.print("SpO2:"); display.print((int)spo2);

  display.setCursor(0, 8);
  display.print("BP:");   display.print((int)sysBP);
  display.print("/");     display.print((int)diaBP);
  display.setCursor(64, 8);
  display.print("BPM:");  display.print((int)bpm);

  // ---- OLED bottom alert ----
  display.fillRect(0, 54, 128, 10, BLACK);
  display.setCursor(0, 54);
  if      (strokeRisk) display.print("** STROKE ALERT **");
  else if (arrhythmia) display.print("! ARRHYTHMIA !");
  else                 display.print("STATUS: SAFE");

  display.display();

  // ---- Serial debug ----
  Serial.print("STATE:");
  Serial.print(demoState == 0 ? "SAFE" : demoState == 1 ? "ARRHYTHMIA" : "STROKE");
  Serial.print(" HR:");   Serial.print((int)hr);
  Serial.print(" SpO2:"); Serial.print((int)spo2);
  Serial.print(" BP:");   Serial.print((int)sysBP);
  Serial.print(" BPM:");  Serial.println((int)bpm);

  delay(100);
}

