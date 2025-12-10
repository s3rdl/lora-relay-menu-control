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

// LoRa pins
#define CONFIG_MOSI 27
#define CONFIG_MISO 19
#define CONFIG_CLK  5
#define CONFIG_NSS  18
#define CONFIG_RST  23
#define CONFIG_DIO0 26

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String LoraMsg = "";
int rssi = 0;
bool states[4] = {0, 0, 0, 0};
int pinsSw[4]  = {2, 15, 13, 12};

// Auto-off configuration (runtime changeable)
int autoOffRelayIndex = 3;                // default SW3
unsigned long autoOffOnMillis = 0;
const unsigned long AUTO_OFF_DURATION = 10000UL;   // 10s

// Menu button on receiver to change which relay has auto-off
#define MENU_BTN_PIN 34      // change to your wiring
bool menuBtnLastLevel = HIGH;
unsigned long menuBtnDebounceTime = 0;
const unsigned long menuBtnDebounceDelay = 50;

void setup() {
  Serial.begin(115200);
  pinMode(25, OUTPUT);
  pinMode(2, OUTPUT);
  pinMode(15, OUTPUT);
  pinMode(13, OUTPUT);
  pinMode(12, OUTPUT);

  digitalWrite(2, 1);
  digitalWrite(13, 1);
  digitalWrite(15, 1);
  digitalWrite(12, 1);

  pinMode(MENU_BTN_PIN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.display();
  setBrightness(70);

  SPI.begin(CONFIG_CLK, CONFIG_MISO, CONFIG_MOSI, CONFIG_NSS);
  LoRa.setPins(CONFIG_NSS, CONFIG_RST, CONFIG_DIO0);

  if (!LoRa.begin(868E6)) {
    Serial.println("Starting LoRa failed!");
    while (1);
  }
  LoRa.setSpreadingFactor(8);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(120);
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);

  Serial.println("LoRa Receiver initialized successfully.");
  drawInit();
}

void drawInit()
{
  display.clearDisplay();
  display.setCursor(0, 4);
  display.setTextColor(1);

  display.setFont(&DejaVu_Sans_Mono_Bold_12);
  display.printf("RECEIVER");
  display.setFont(NULL);

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

  display.setTextColor(1);
  display.setCursor(72, 8);
  display.printf("LAST MSG ");

  display.setTextColor(1);
  display.setCursor(72, 20);
  display.print(LoraMsg);

  display.fillRect(0, 14, 62, 18, 1);
  display.setTextColor(0);
  display.setCursor(3, 21);
  display.print("RSSI:");

  display.fillRect(32, 19, 28, 11, 0);
  display.setTextColor(1);
  display.setCursor(34, 21);
  display.print(String(rssi));

  // AUTO-OFF relay info
  display.setTextColor(1);
  display.setCursor(0, 32);
  display.print("AUTO: SW");
  display.print(autoOffRelayIndex);

  for (int i = 0; i < 4; i++) {
    if (states[i]) 
      display.fillRoundRect(i * 33, 36, 28, 28, 4, 1);
    else
      display.drawRoundRect(i * 33, 36, 28, 28, 4, 1);

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

char c;
void loop()
{
  // 1) LoRa packets
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    Serial.print("Received packet: ");
    LoraMsg = "";

    while (LoRa.available()) {
      LoraMsg = LoRa.readString();
      Serial.print(LoraMsg);
    }
    Serial.println();

    if (LoraMsg.length() == 5) {
      // Command types: "CNG x" or "AOF x"
      if (LoraMsg.startsWith("CNG ")) {
        digitalWrite(25, 1);
        c = LoraMsg.charAt(4);
        int n = c - '0';

        if (n >= 0 && n < 4) {
          states[n] = !states[n];
          digitalWrite(pinsSw[n], !states[n]);

          // If this is the auto-off relay, start/stop timer
          if (n == autoOffRelayIndex) {
            if (states[autoOffRelayIndex]) {
              autoOffOnMillis = millis();
            } else {
              autoOffOnMillis = 0;
            }
          }
        }

        // RSSI / SNR
        rssi = LoRa.packetRssi();
        Serial.print("RSSI: ");
        Serial.println(rssi);
        Serial.print("SNR: ");
        Serial.println(LoRa.packetSnr());

        drawInit();

        // Send ACK back so sender can display link RSSI
        LoRa.beginPacket();
        LoRa.print("ACK ");
        LoRa.print(LoraMsg);
        LoRa.endPacket();

        digitalWrite(25, 0);

      } else if (LoraMsg.startsWith("AOF ")) {
        // Config message: change auto-off relay index
        int idx = LoraMsg.charAt(4) - '0';
        if (idx >= 0 && idx < 4) {
          autoOffRelayIndex = idx;
          autoOffOnMillis = 0; // reset timer
          Serial.print("Auto-off relay set to SW");
          Serial.println(autoOffRelayIndex);
          drawInit();

          // Optionally confirm back with same config
          LoRa.beginPacket();
          LoRa.print("AOF ");
          LoRa.print(autoOffRelayIndex);
          LoRa.endPacket();
        }
      }
    }
  }

  // 2) Auto-OFF logic
  if (states[autoOffRelayIndex] && autoOffOnMillis > 0) {
    unsigned long elapsed = millis() - autoOffOnMillis;
    if (elapsed >= AUTO_OFF_DURATION) {
      states[autoOffRelayIndex] = 0;
      digitalWrite(pinsSw[autoOffRelayIndex], !states[autoOffRelayIndex]);
      autoOffOnMillis = 0;

      Serial.print("SW");
      Serial.print(autoOffRelayIndex);
      Serial.println(" auto-OFF");

      LoraMsg = "AUTO" + String(autoOffRelayIndex);
      drawInit();
    }
  }

  // 3) Local menu button on receiver to change auto-off relay
  handleMenuButton();
}

void handleMenuButton() {
  int reading = digitalRead(MENU_BTN_PIN);

  if (reading != menuBtnLastLevel) {
    menuBtnDebounceTime = millis();
    menuBtnLastLevel = reading;
  }

  if ((millis() - menuBtnDebounceTime) > menuBtnDebounceDelay) {
    if (reading == LOW) {
      // cycle 0..3
      autoOffRelayIndex = (autoOffRelayIndex + 1) % 4;
      autoOffOnMillis = 0; // reset timer
      Serial.print("RX menu set auto-off relay to SW");
      Serial.println(autoOffRelayIndex);

      drawInit();

      // send config to sender too
      LoRa.beginPacket();
      LoRa.print("AOF ");
      LoRa.print(autoOffRelayIndex);
      LoRa.endPacket();

      // wait until released
      while (digitalRead(MENU_BTN_PIN) == LOW) {
        delay(10);
      }
    }
  }
}

// CNG 1
void setBrightness(uint8_t contrast) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
}
