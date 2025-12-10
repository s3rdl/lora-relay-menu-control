#include <Wire.h>
#include <SPI.h>
#include <LoRa.h> 
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include "myFonts.h"

#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels
#define OLED_RESET    -1  // Reset pin (-1 if using default reset pin)
#define I2C_SDA       21  // Change if you're using different pins
#define I2C_SCL       22  // Change if you're using different pins

// LoRa pins
#define CONFIG_MOSI 27
#define CONFIG_MISO 19
#define CONFIG_CLK  5
#define CONFIG_NSS  18
#define CONFIG_RST  23
#define CONFIG_DIO0 26

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

String status = "WAITING...";
String loraMsg = "";
int rssi = 0;
int lastOne = 5;

int pinsSw[4] = {2, 15, 13, 12};
int xpos[4]   = {90, 110, 90, 110};
int ypos[4]   = {36, 36, 52, 52};
int deb = 0;

// Local “relay” states corresponding to 4 buttons
bool states[4] = {0, 0, 0, 0};

// Link RSSI from last received packet (ACK)
int linkRssi = 0;
bool haveLinkRssi = false;

// ---------- AUTO-OFF MENU CONFIG ----------
int autoOffRelayIndex = 3;           // default: SW3 has auto-off

// Extra button to change auto-off relay (menu)
#define MENU_BTN_PIN 34              // change to your wiring
bool menuBtnLastLevel = HIGH;
unsigned long menuBtnDebounceTime = 0;
const unsigned long menuBtnDebounceDelay = 50;
// -----------------------------------------

void setup() {
  pinMode(25, OUTPUT);
  pinMode(2, INPUT_PULLUP);
  pinMode(15, INPUT_PULLUP);
  pinMode(13, INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);

  pinMode(MENU_BTN_PIN, INPUT_PULLUP);   // menu button

  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.display();
  setBrightness(70);

  // LoRa init
  SPI.begin(CONFIG_CLK, CONFIG_MISO, CONFIG_MOSI, CONFIG_NSS);
  LoRa.setPins(CONFIG_NSS, CONFIG_RST, CONFIG_DIO0);

  if (!LoRa.begin(868E6)) {
    Serial.println("Starting LoRa failed!");
    while (1);
  }

  LoRa.setSpreadingFactor(8);      // SF8
  LoRa.setSignalBandwidth(125E3);  // 125 kHz
  LoRa.setCodingRate4(5);          // 4/5
  LoRa.setSyncWord(120); 
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  Serial.println("LoRa Sender initialized successfully.");

  drawInit();
}

void drawInit()
{
  display.clearDisplay();
  display.setCursor(0, 4);
  display.setTextColor(1);

  display.setFont(&DejaVu_Sans_Mono_Bold_12);
  display.printf("SENDER");
  display.setFont(NULL);

  // AUTO-OFF info
  display.setCursor(0, 16);
  display.print("AUTO: SW");
  display.print(autoOffRelayIndex);

  // Status + last LoRa message
  display.setCursor(0, 46);
  display.print(status);
  display.setCursor(0, 60);
  display.print(loraMsg);

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

  // Button rectangles with highlight on lastOne
  for (int i = 0; i < 4; i++)
    if (i == lastOne)
      display.fillRect(xpos[i], ypos[i], 16, 12, 1);
    else
      display.drawRect(xpos[i], ypos[i], 16, 12, 1);

  // Show states as 0/1 near the rectangles
  display.setTextColor(1);
  display.setCursor(88, 34);
  display.print(states[0]);
  display.setCursor(108, 34);
  display.print(states[1]);
  display.setCursor(88, 50);
  display.print(states[2]);
  display.setCursor(108, 50);
  display.print(states[3]);

  // decorative bottom line
  for (int i = 0; i < 86; i++)
    if (i % 2 == 0)
      display.drawPixel(i, 62, 1);
    else
      display.drawPixel(i, 63, 1);

  display.setTextColor(1);
  display.setCursor(72, 8);
  display.printf("sWORD 120");
  display.setCursor(72, 21);
  display.printf("125kHz");

  display.fillRect(0, 14, 62, 18, 1);
  display.setTextColor(0);
  display.setCursor(3, 21);
  display.print("SF8 pLen8");

  // Link RSSI
  display.setTextColor(1);
  display.setCursor(0, 32);
  display.print("RSSI: ");
  if (haveLinkRssi) {
    display.print(linkRssi);
  } else {
    display.print("N/A");
  }

  display.display();
}

void loop()
{
  handleMenuButton(); // change auto-off relay if needed

  // --- button logic (plus states[] toggle and ACK check) ---
  for (int i = 0; i < 4; i++) {
    if (digitalRead(pinsSw[i]) == 0) {
      if (deb == 0) {
        deb = 1;
        digitalWrite(25, 1);

        // toggle local state
        states[i] = !states[i];

        LoRa.beginPacket();
        lastOne = i;
        loraMsg = "CNG " + String(lastOne);  // same protocol as original
        LoRa.print(loraMsg);
        LoRa.endPacket();

        status = "SENDING...";
        drawInit();
        Serial.println("Message sent!");

        // wait for ACK (any packet) and record RSSI
        unsigned long startWait = millis();
        haveLinkRssi = false;
        while (millis() - startWait < 500) {
          int packetSize = LoRa.parsePacket();
          if (packetSize) {
            String incoming = "";
            while (LoRa.available()) {
              incoming += (char)LoRa.read();
            }
            incoming.trim();
            linkRssi = LoRa.packetRssi();
            haveLinkRssi = true;

            Serial.print("Incoming: ");
            Serial.print(incoming);
            Serial.print("  RSSI=");
            Serial.println(linkRssi);

            // If receiver sends back its auto-off config, parse it
            if (incoming.startsWith("AOF ")) {
              int idx = incoming.charAt(4) - '0';
              if (idx >= 0 && idx < 4) {
                autoOffRelayIndex = idx;
                Serial.print("Auto-off relay updated from RX: ");
                Serial.println(autoOffRelayIndex);
              }
            }
            break;
          }
        }

        delay(150);
        digitalWrite(25, 0);
        status = "WAITING...";
        loraMsg = "";
        drawInit();
      }
    } else {
      deb = 0;
    }
  }
}

void handleMenuButton() {
  int reading = digitalRead(MENU_BTN_PIN);

  if (reading != menuBtnLastLevel) {
    menuBtnDebounceTime = millis();
    menuBtnLastLevel = reading;
  }

  if ((millis() - menuBtnDebounceTime) > menuBtnDebounceDelay) {
    // button press = transition to LOW
    if (reading == LOW) {
      // cycle auto-off relay 0..3
      autoOffRelayIndex = (autoOffRelayIndex + 1) % 4;
      Serial.print("Menu changed auto-off relay to SW");
      Serial.println(autoOffRelayIndex);

      // send config to receiver: "AOF x"
      LoRa.beginPacket();
      String cfg = "AOF " + String(autoOffRelayIndex);
      LoRa.print(cfg);
      LoRa.endPacket();

      // indicate on display
      status = "SET AUTO SW" + String(autoOffRelayIndex);
      drawInit();

      // wait until button released
      while (digitalRead(MENU_BTN_PIN) == LOW) {
        delay(10);
      }
      status = "WAITING...";
      drawInit();
    }
  }
}

// CNG 1
void setBrightness(uint8_t contrast) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
}
