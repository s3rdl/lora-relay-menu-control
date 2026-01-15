#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include "myFonts.h"

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define I2C_SDA       21
#define I2C_SCL       22

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- LoRa (TTGO LoRa32) ----------------
#define CONFIG_MOSI 27
#define CONFIG_MISO 19
#define CONFIG_CLK  5
#define CONFIG_NSS  18
#define CONFIG_RST  23
#define CONFIG_DIO0 26

// ---------------- Buttons ----------------
int pinsSw[4] = {2, 15, 13, 12};
#define MENU_BTN_PIN 14   // GPIO14 -> GND

// ---------------- LED ----------------
#define LED_PIN 25
const unsigned long LED_PULSE_MS = 120;

// ---------------- Debounce ----------------
const unsigned long DEBOUNCE_MS = 30;
const unsigned long MENU_DEBOUNCE_MS = 50;

// ---------------- Status timing ----------------
const unsigned long SYNC_SHOW_MS = 800;
const unsigned long ERR_SHOW_MS  = 1200;

// ---------------- Battery (TTGO LoRa32) ----------------
// Most TTGO LoRa32 revisions expose VBAT via a divider to GPIO35 (sometimes GPIO34).
#define BAT_ADC_PIN 35
const unsigned long BAT_SAMPLE_MS = 2000UL;

// Li-ion thresholds (adjust if needed)
const float VBAT_EMPTY = 3.20f;
const float VBAT_FULL  = 4.20f;
const float VBAT_LOW   = 3.45f;   // warning threshold

// Low-bat blink
const unsigned long LOWBAT_BLINK_MS = 600UL;

float vbat = 0.0f;           // filtered
float vbatRaw = 0.0f;        // last raw reading
unsigned long nextBatSampleAt = 0;
bool lowBat = false;
bool lowBatBlink = false;
unsigned long nextLowBatBlinkAt = 0;

// ---------------- UI / State ----------------
String status = "WAITING...";
String lastTx = "";
int lastOne = 5;

int xpos[4] = {90, 110, 90, 110};
int ypos[4] = {36, 36, 52, 52};

bool states[4] = {0, 0, 0, 0};
int autoOffRelayIndex = 3;

int linkRssi = 0;
bool haveLinkRssi = false;

unsigned long statusUntil = 0;

// ---------------- Button struct ----------------
struct Button {
  uint8_t pin;
  bool raw;
  bool stable;
  bool lastStable;
  unsigned long changedAt;
};

Button btn[4];
Button menuBtn;

bool ledOn = false;
unsigned long ledUntil = 0;

// ============================================================================
// Helpers
// ============================================================================
void setBrightness(uint8_t contrast) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
}

void ledPulse() {
  digitalWrite(LED_PIN, HIGH);
  ledOn = true;
  ledUntil = millis() + LED_PULSE_MS;
}

void ledService() {
  if (ledOn && (long)(millis() - ledUntil) >= 0) {
    digitalWrite(LED_PIN, LOW);
    ledOn = false;
  }
}

bool debounceUpdate(Button &b, unsigned long debounceMs) {
  bool r = digitalRead(b.pin);
  if (r != b.raw) {
    b.raw = r;
    b.changedAt = millis();
  }
  if ((millis() - b.changedAt) >= debounceMs) {
    if (b.stable != b.raw) {
      b.lastStable = b.stable;
      b.stable = b.raw;
      return true;
    }
  }
  return false;
}

void setStatusTemp(const String &s, unsigned long ms) {
  status = s;
  statusUntil = millis() + ms;
  drawUI();
}

void statusService() {
  if (statusUntil && (long)(millis() - statusUntil) >= 0) {
    statusUntil = 0;
    status = "WAITING...";
    drawUI();
  }
}

// ============================================================================
// Battery
// ============================================================================
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float readBatteryVoltage() {
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  delay(2);

  uint32_t sum = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    sum += analogRead(BAT_ADC_PIN);
    delay(2);
  }

  float raw = (float)sum / (float)N;

  // ADC pin voltage (approx)
  float vAdc = (raw / 4095.0f) * 3.3f;

  // Divider compensation (TTGO typical)
  float vBat = vAdc * 2.0f;

  return vBat;
}

uint8_t batteryBars(float v) {
  // map VBAT_EMPTY..VBAT_FULL -> 0..4
  float vv = clampf(v, VBAT_EMPTY, VBAT_FULL);
  float p = (vv - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY); // 0..1
  int bars = (int)(p * 4.0f + 0.5f);
  if (bars < 0) bars = 0;
  if (bars > 4) bars = 4;
  return (uint8_t)bars;
}

void batteryService() {
  unsigned long now = millis();

  // sample
  if ((long)(now - nextBatSampleAt) >= 0) {
    nextBatSampleAt = now + BAT_SAMPLE_MS;

    vbatRaw = readBatteryVoltage();

    // simple low-pass filter (80% old, 20% new)
    if (vbat < 0.1f) vbat = vbatRaw;
    else vbat = (vbat * 0.80f) + (vbatRaw * 0.20f);

    lowBat = (vbat > 0.1f && vbat <= VBAT_LOW);

    drawUI();
  }

  // blink state for low battery indicator
  if (lowBat && (long)(now - nextLowBatBlinkAt) >= 0) {
    nextLowBatBlinkAt = now + LOWBAT_BLINK_MS;
    lowBatBlink = !lowBatBlink;
    drawUI();
  }
  if (!lowBat) {
    lowBatBlink = false;
  }
}

// ============================================================================
// UI (Battery Icon + LowBat)
// ============================================================================
void drawBatteryIcon(int x, int y, uint8_t bars, bool warnBlink) {
  // battery outline: 18x8 + tip
  // Outline
  display.drawRect(x, y, 18, 8, 1);
  display.drawRect(x + 18, y + 2, 2, 4, 1);

  // Fill bars (each 3px wide, 1px gap)
  int bx = x + 2;
  int by = y + 2;
  for (int i = 0; i < 4; i++) {
    if (i < bars) display.fillRect(bx + i * 4, by, 3, 4, 1);
  }

  // Low-bat warning: blink a small "!" near icon
  if (warnBlink) {
    display.setCursor(x - 6, y + 1);
    display.print("!");
  }
}

void drawUI() {
  display.clearDisplay();
  display.setCursor(0, 4);
  display.setTextColor(1);

  display.setFont(&DejaVu_Sans_Mono_Bold_12);
  display.print("SENDER");
  display.setFont(NULL);

  // Battery icon top-right area (does not collide with right box)
  // We place it under the top label line on the left side to avoid the panel
  // But your right panel starts at x=68, so we can place battery at x=48 safely.
  uint8_t bars = batteryBars(vbat);
  drawBatteryIcon(60, 35, bars, lowBat && lowBatBlink);

  display.setCursor(0, 16);
  display.print("AUTO:SW");
  display.print(autoOffRelayIndex);

  display.setCursor(0, 25);
  display.print("RSSI:");
  if (haveLinkRssi) display.print(linkRssi);
  else display.print("N/A");

  // Battery voltage line below RSSI
  display.setCursor(0, 35);
  display.print("BAT:");
  if (vbat > 0.1f) {
    display.print(vbat, 2);
    display.print("V");
  } else {
    display.print("N/A");
  }

  // Low battery text (only when low)
  if (lowBat) {
    display.setCursor(0, 44);
    display.print("LOW BAT!");
  }

  // Status line (move slightly if low bat is shown)
  display.setCursor(0, lowBat ? 54 : 46);
  display.print(status);

  // Right panel box
  display.fillRect(68, 0, 60, 2, 1);

  for (int i = 68; i < 128; i++)
    if (i % 2 == 0) {
      display.drawPixel(i, 4, 1);
      display.drawPixel(i, 32, 1);
    }

  for (int i = 4; i < 32; i++)
    if (i % 2 == 0) {
      display.drawPixel(68, i, 1);
      display.drawPixel(127, i, 1);
    }

  display.setCursor(72, 8);
  display.print("LAST TX");

  display.setCursor(72, 20);
  display.print(lastTx);

  // Button rectangles
  for (int i = 0; i < 4; i++) {
    if (i == lastOne) display.fillRect(xpos[i], ypos[i], 16, 12, 1);
    else display.drawRect(xpos[i], ypos[i], 16, 12, 1);
  }

  // Real relay states
  display.setCursor(88, 34);  display.print(states[0]);
  display.setCursor(108, 34); display.print(states[1]);
  display.setCursor(88, 50);  display.print(states[2]);
  display.setCursor(108, 50); display.print(states[3]);

  display.display();
}

// ============================================================================
// LoRa
// ============================================================================
void sendPacket(const String &out) {
  lastTx = out;
  setStatusTemp("SENDING...", 300);
  ledPulse();

  LoRa.beginPacket();
  LoRa.print(out);
  LoRa.endPacket();
}

void handleIncoming() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  String incoming = "";
  while (LoRa.available()) incoming += (char)LoRa.read();
  incoming.trim();

  linkRssi = LoRa.packetRssi();
  haveLinkRssi = true;

  if (incoming.startsWith("ERR SEQ")) {
    setStatusTemp("SEQ BLOCKED", ERR_SHOW_MS);
    return;
  }

  if (incoming.startsWith("STA ") && incoming.length() >= 8) {
    for (int i = 0; i < 4; i++) {
      char c = incoming.charAt(4 + i);
      if (c == '0' || c == '1') states[i] = (c == '1');
    }
    setStatusTemp("SYNC", SYNC_SHOW_MS);
    return;
  }

  if (incoming.startsWith("AOF ") && incoming.length() >= 5) {
    int idx = incoming.charAt(4) - '0';
    if (idx >= 0 && idx < 4) {
      autoOffRelayIndex = idx;
      drawUI();
    }
    return;
  }
}

// ============================================================================
// Setup / Loop
// ============================================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (int i = 0; i < 4; i++) {
    pinMode(pinsSw[i], INPUT_PULLUP);
    btn[i] = { (uint8_t)pinsSw[i], digitalRead(pinsSw[i]),
               digitalRead(pinsSw[i]), digitalRead(pinsSw[i]), millis() };
  }

  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  menuBtn = { MENU_BTN_PIN, digitalRead(MENU_BTN_PIN),
              digitalRead(MENU_BTN_PIN), digitalRead(MENU_BTN_PIN), millis() };

  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.display();
  setBrightness(70);

  analogReadResolution(12);
  nextBatSampleAt = millis() + 300;
  nextLowBatBlinkAt = millis() + LOWBAT_BLINK_MS;

  SPI.begin(CONFIG_CLK, CONFIG_MISO, CONFIG_MOSI, CONFIG_NSS);
  LoRa.setPins(CONFIG_NSS, CONFIG_RST, CONFIG_DIO0);
  if (!LoRa.begin(868E6)) while (1);

  LoRa.setSpreadingFactor(8);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(120);
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);

  sendPacket("GET 0");
  drawUI();
}

void loop() {
  ledService();
  statusService();
  handleIncoming();
  batteryService();

  // Relay buttons
  for (int i = 0; i < 4; i++) {
    if (debounceUpdate(btn[i], DEBOUNCE_MS)) {
      if (btn[i].lastStable == HIGH && btn[i].stable == LOW) {
        lastOne = i;
        sendPacket("CNG " + String(i));
      }
    }
  }

  // Menu button
  if (debounceUpdate(menuBtn, MENU_DEBOUNCE_MS)) {
    if (menuBtn.lastStable == HIGH && menuBtn.stable == LOW) {
      autoOffRelayIndex = (autoOffRelayIndex + 1) % 4;
      sendPacket("AOF " + String(autoOffRelayIndex));
    }
  }
}