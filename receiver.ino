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

int pinsSw[4] = {2, 15, 13, 12};   // relay outputs (active LOW)
#define MENU_BTN_PIN 14            // receiver menu button to GND

const unsigned long AUTO_OFF_DURATION_MS = 10000UL;
const unsigned long MENU_DEBOUNCE_MS = 50;

// --- NEW: alternating display timing ---
const unsigned long INFO_TOGGLE_MS = 2000UL;
bool showRssiInfo = true;
unsigned long nextInfoToggleAt = 0;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String LoraMsg = "";
int rssi = 0;
bool states[4] = {0, 0, 0, 0};

int autoOffRelayIndex = 3;
unsigned long autoOffOnMillis = 0;

// menu debounce
bool menuRaw = HIGH, menuStable = HIGH, menuLastStable = HIGH;
unsigned long menuChangedAt = 0;

void setBrightness(uint8_t contrast) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
}

bool menuDebounceUpdate() {
  bool r = digitalRead(MENU_BTN_PIN);
  if (r != menuRaw) { menuRaw = r; menuChangedAt = millis(); }
  if ((millis() - menuChangedAt) >= MENU_DEBOUNCE_MS) {
    if (menuStable != menuRaw) {
      menuLastStable = menuStable;
      menuStable = menuRaw;
      return true;
    }
  }
  return false;
}

void applyRelayOutput(int idx) {
  digitalWrite(pinsSw[idx], !states[idx]); // active LOW
}

String makeSta() {
  String s = "STA ";
  for (int i = 0; i < 4; i++) s += (states[i] ? '1' : '0');
  return s;
}

void sendPacket(const String &msg) {
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
}

void publishState(bool alsoConfirmAof) {
  if (alsoConfirmAof) sendPacket("AOF " + String(autoOffRelayIndex));
  sendPacket(makeSta());
}

void infoToggleService() {
  unsigned long now = millis();
  if ((long)(now - nextInfoToggleAt) >= 0) {
    showRssiInfo = !showRssiInfo;
    nextInfoToggleAt = now + INFO_TOGGLE_MS;
    drawUI(); // redraw so the new info becomes visible
  }
}

void drawUI() {
  display.clearDisplay();
  display.setCursor(0, 4);
  display.setTextColor(1);

  display.setFont(&DejaVu_Sans_Mono_Bold_12);
  display.printf("RECEIVER");
  display.setFont(NULL);

  display.fillRect(68, 0, 60, 2, 1);

  for (int i = 68; i < 128; i++)
    if (i % 2 == 0) { display.drawPixel(i, 4, 1); display.drawPixel(i, 32, 1); }

  for (int i = 4; i < 32; i++)
    if (i % 2 == 0) { display.drawPixel(68, i, 1); display.drawPixel(127, i, 1); }

  display.setTextColor(1);
  display.setCursor(72, 8);
  display.printf("LAST MSG ");

  display.setCursor(72, 20);
  display.print(LoraMsg);

  // --- INFO BOX (same place as original RSSI box) ---
  display.fillRect(0, 14, 62, 18, 1);
  display.setTextColor(0);
  display.setCursor(3, 21);

  if (showRssiInfo) {
    display.print("RSSI:");
    display.fillRect(32, 19, 28, 11, 0);
    display.setTextColor(1);
    display.setCursor(34, 21);
    display.print(String(rssi));
  } else {
    // show AUTO in the same box, no overlap elsewhere
    display.print("AUTO:");
    display.fillRect(32, 19, 28, 11, 0);
    display.setTextColor(1);
    display.setCursor(34, 21);
    display.print("SW");
    display.print(autoOffRelayIndex);
  }

  // Relay state boxes
  for (int i = 0; i < 4; i++) {
    if (states[i]) display.fillRoundRect(i * 33, 36, 28, 28, 4, 1);
    else display.drawRoundRect(i * 33, 36, 28, 28, 4, 1);

    display.setFont(NULL);
    display.setTextColor(!states[i]);
    display.setCursor(4 + (i * 33), 40);
    display.print("SW" + String(i));

    display.setFont(&DejaVu_Sans_Mono_Bold_12);
    display.setCursor(12 + (i * 33), 59);
    display.print(states[i]);

    display.drawRoundRect(i * 33, 36, 28, 28, 4, 1);
  }

  display.setFont(NULL);
  display.display();
}

void setAutoOffRelay(int idx, bool publish) {
  autoOffRelayIndex = idx;
  autoOffOnMillis = 0;
  LoraMsg = "AOF " + String(idx);
  drawUI();
  if (publish) publishState(true);
}

void handleCng(int n) {
  states[n] = !states[n];
  applyRelayOutput(n);

  if (n == autoOffRelayIndex) {
    if (states[n]) autoOffOnMillis = millis();
    else autoOffOnMillis = 0;
  }

  drawUI();
  sendPacket("ACK " + LoraMsg);
  publishState(false);
}

void setup() {
  Serial.begin(115200);
  pinMode(25, OUTPUT);

  for (int i = 0; i < 4; i++) {
    pinMode(pinsSw[i], OUTPUT);
    digitalWrite(pinsSw[i], HIGH); // OFF
  }

  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  menuRaw = digitalRead(MENU_BTN_PIN);
  menuStable = menuRaw;
  menuLastStable = menuStable;
  menuChangedAt = millis();

  // init alternating display timer
  nextInfoToggleAt = millis() + INFO_TOGGLE_MS;

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

  drawUI();
  publishState(true); // initial AOF + STA
}

void loop() {
  infoToggleService();

  // menu button cycles auto-off relay and publishes
  if (menuDebounceUpdate()) {
    if (menuLastStable == HIGH && menuStable == LOW) {
      int next = (autoOffRelayIndex + 1) % 4;
      setAutoOffRelay(next, true);
    }
  }

  // LoRa receive
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    LoraMsg = "";
    while (LoRa.available()) LoraMsg += (char)LoRa.read();
    LoraMsg.trim();

    rssi = LoRa.packetRssi();

    if (LoraMsg.startsWith("CNG ") && LoraMsg.length() >= 5) {
      int n = LoraMsg.charAt(4) - '0';
      if (n >= 0 && n < 4) handleCng(n);
      else drawUI();
    }
    else if (LoraMsg.startsWith("AOF ") && LoraMsg.length() >= 5) {
      int idx = LoraMsg.charAt(4) - '0';
      if (idx >= 0 && idx < 4) setAutoOffRelay(idx, true);
      else drawUI();
    }
    else if (LoraMsg.startsWith("GET")) {
      publishState(true);
      drawUI();
    }
    else {
      drawUI();
    }
  }

  // Auto-off timer
  if (states[autoOffRelayIndex] && autoOffOnMillis > 0) {
    if (millis() - autoOffOnMillis >= AUTO_OFF_DURATION_MS) {
      states[autoOffRelayIndex] = 0;
      applyRelayOutput(autoOffRelayIndex);
      autoOffOnMillis = 0;

      LoraMsg = "AUTO OFF " + String(autoOffRelayIndex);
      drawUI();

      sendPacket("ACK " + LoraMsg);
      publishState(false);
    }
  }
}
