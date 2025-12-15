#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include "myFonts.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define I2C_SDA       21
#define I2C_SCL       22

// LoRa pins (TTGO LoRa32)
#define CONFIG_MOSI 27
#define CONFIG_MISO 19
#define CONFIG_CLK  5
#define CONFIG_NSS  18
#define CONFIG_RST  23
#define CONFIG_DIO0 26

// Buttons
int pinsSw[4] = {2, 15, 13, 12};
#define MENU_BTN_PIN 14   // GPIO14 -> GND

// LED
#define LED_PIN 25
const unsigned long LED_PULSE_MS = 120;

// Debounce
const unsigned long DEBOUNCE_MS = 30;
const unsigned long MENU_DEBOUNCE_MS = 50;

// SYNC display timing
const unsigned long SYNC_SHOW_MS = 800;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String status = "WAITING...";
String lastTx = "";
int lastOne = 5;

// UI layout
int xpos[4] = {90, 110, 90, 110};
int ypos[4] = {36, 36, 52, 52};

// Real relay states (from receiver only!)
bool states[4] = {0, 0, 0, 0};

// Auto-off selection
int autoOffRelayIndex = 3;

// RSSI
int linkRssi = 0;
bool haveLinkRssi = false;

// SYNC status timing
unsigned long statusUntil = 0;

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

// ----------------- helpers -----------------

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

// ----------------- UI -----------------

void drawUI() {
  display.clearDisplay();
  display.setCursor(0, 4);
  display.setTextColor(1);

  display.setFont(&DejaVu_Sans_Mono_Bold_12);
  display.print("SENDER");
  display.setFont(NULL);

  display.setCursor(0, 16);
  display.print("AUTO:SW");
  display.print(autoOffRelayIndex);

  display.setCursor(0, 28);
  display.print("RSSI:");
  if (haveLinkRssi) display.print(linkRssi);
  else display.print("N/A");

  display.setCursor(0, 46);
  display.print(status);

  // Right box
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

// ----------------- LoRa -----------------

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

  // --- STATE SYNC ---
  if (incoming.startsWith("STA ") && incoming.length() >= 8) {
    for (int i = 0; i < 4; i++) {
      char c = incoming.charAt(4 + i);
      if (c == '0' || c == '1') states[i] = (c == '1');
    }
    setStatusTemp("SYNC", SYNC_SHOW_MS);
    return;
  }

  // --- Auto-off confirm ---
  if (incoming.startsWith("AOF ") && incoming.length() >= 5) {
    int idx = incoming.charAt(4) - '0';
    if (idx >= 0 && idx < 4) {
      autoOffRelayIndex = idx;
      drawUI();
    }
    return;
  }
}

// ----------------- setup / loop -----------------

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

  SPI.begin(CONFIG_CLK, CONFIG_MISO, CONFIG_MOSI, CONFIG_NSS);
  LoRa.setPins(CONFIG_NSS, CONFIG_RST, CONFIG_DIO0);
  if (!LoRa.begin(868E6)) while (1);

  LoRa.setSpreadingFactor(8);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(120);
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);

  sendPacket("GET 0");   // initial sync
  drawUI();
}

void loop() {
  ledService();
  statusService();
  handleIncoming();

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
