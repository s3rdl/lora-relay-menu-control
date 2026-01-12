// receiver/receiver.ino (FULL) — FIXED Web-UI Start (no more “connected but no UI”)
// Key fixes:
// 1) startWebServer() EXISTS and is called as soon as WiFi.isConnected() becomes true (in loop).
// 2) server.handleClient() runs continuously when webRunning==true.
// 3) WiFi.setSleep(false) to avoid flaky connectivity on some routers.
// 4) Force-Portal at boot (GPIO14 hold ~1.2s) uses BLOCKING portal (guaranteed).
// 5) 60s connect-try, then non-blocking portal.
// 6) Web endpoint: POST /api/wifi/reset (BasicAuth) resets creds + reboot.
// 7) Display sleep only turns OLED off (does NOT touch WiFi).
// LED meaning (GREEN LED, GPIO25)
// 1× short blink every 5 s  -> Link OK, idle
// 1× short blink every 1 s  -> Active (TX/RX, Web, buttons, sequence)
// Double blink              -> LoRa link lost / no packets

#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>   // tzapu/WiFiManager

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include "myFonts.h"
#include <esp_system.h>

// ---------------- OLED ----------------
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

// ---------------- IO ----------------
int pinsSw[4] = {2, 15, 13, 12};   // relay outputs (active LOW)
#define MENU_BTN_PIN 14            // shared: skip/menu button to GND

// ---------------- WiFi Recovery (A) ----------------
#define WIFI_FORCE_PORTAL_PIN 14
const unsigned long WIFI_FORCE_HOLD_MS = 1200UL;

// ---------------- LED STATUS ----------------
#define LED_PIN 25

const unsigned long LINK_TIMEOUT_MS   = 8000UL;
const unsigned long ACTIVE_HOLD_MS    = 3000UL;

const unsigned long IDLE_PERIOD_MS    = 5000UL;
const unsigned long ACTIVE_PERIOD_MS  = 1000UL;
const unsigned long ERROR_PERIOD_MS   = 2000UL;
const unsigned long BLINK_ON_MS       = 50UL;
const unsigned long DOUBLE_GAP_MS     = 150UL;

enum LedMode { LED_IDLE, LED_ACTIVE, LED_ERROR };

unsigned long lastLinkPacketAt = 0;
unsigned long lastActivityAtLed = 0;

unsigned long ledNextAt = 0;
uint8_t ledPhase = 0;
bool ledIsOn = false;

void printResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  Serial.print("RESET REASON: ");
  Serial.println((int)r);
}

void ledMarkActivity() { lastActivityAtLed = millis(); }

void ledMarkLinkPacket() {
  lastLinkPacketAt = millis();
  ledNextAt = millis();
  ledPhase = 0;
}

LedMode ledComputeModeReceiver(bool seqActive) {
  unsigned long now = millis();
  if ((now - lastLinkPacketAt) > LINK_TIMEOUT_MS) return LED_ERROR;
  if (seqActive) return LED_ACTIVE;
  if ((long)(now - lastActivityAtLed) < (long)ACTIVE_HOLD_MS) return LED_ACTIVE;
  return LED_IDLE;
}

void ledStatusServiceReceiver(bool seqActive) {
  unsigned long now = millis();
  LedMode mode = ledComputeModeReceiver(seqActive);

  if ((long)(now - ledNextAt) < 0) return;

  if (mode == LED_IDLE) {
    if (!ledIsOn) { digitalWrite(LED_PIN, HIGH); ledIsOn = true; ledNextAt = now + BLINK_ON_MS; }
    else { digitalWrite(LED_PIN, LOW); ledIsOn = false; ledNextAt = now + (IDLE_PERIOD_MS - BLINK_ON_MS); }
    return;
  }

  if (mode == LED_ACTIVE) {
    if (!ledIsOn) { digitalWrite(LED_PIN, HIGH); ledIsOn = true; ledNextAt = now + BLINK_ON_MS; }
    else { digitalWrite(LED_PIN, LOW); ledIsOn = false; ledNextAt = now + (ACTIVE_PERIOD_MS - BLINK_ON_MS); }
    return;
  }

  switch (ledPhase) {
    case 0:
      digitalWrite(LED_PIN, HIGH); ledIsOn = true;
      ledNextAt = now + BLINK_ON_MS;
      ledPhase = 1;
      break;
    case 1:
      digitalWrite(LED_PIN, LOW); ledIsOn = false;
      ledNextAt = now + DOUBLE_GAP_MS;
      ledPhase = 2;
      break;
    case 2:
      digitalWrite(LED_PIN, HIGH); ledIsOn = true;
      ledNextAt = now + BLINK_ON_MS;
      ledPhase = 3;
      break;
    default:
      digitalWrite(LED_PIN, LOW); ledIsOn = false;
      {
        unsigned long used = (2 * BLINK_ON_MS) + DOUBLE_GAP_MS;
        unsigned long rest = (ERROR_PERIOD_MS > used) ? (ERROR_PERIOD_MS - used) : 200;
        ledNextAt = now + rest;
      }
      ledPhase = 0;
      break;
  }
}

// ---------------- Timing / UI ----------------
const unsigned long AUTO_OFF_DURATION_MS = 10000UL;
const unsigned long MENU_DEBOUNCE_MS = 50;

const unsigned long INFO_TOGGLE_MS = 2000UL;
bool showRssiInfo = true;
unsigned long nextInfoToggleAt = 0;

// Display idle timeout (OLED only)
const unsigned long DISPLAY_IDLE_MS = 120000UL;
bool displayOn = true;
unsigned long lastActivityAt = 0;

// ---------------- State ----------------
String LoraMsg = "";
int rssi = 0;
bool states[4] = {0, 0, 0, 0};
int autoOffRelayIndex = 3;
unsigned long autoOffOnMillis = 0;

// ---------------- Sequence ----------------
bool seqActive = false;
uint8_t seqPhase = 0;
unsigned long seqNextAt = 0;
bool skipSw0 = false;
bool skipSw1 = false;

// Restart gating (modified behavior)
bool seqCompleted = true;
bool seqRestartArmed = true;    // NOW: will be re-armed immediately on finish (phase 2 end)

bool lastMenuLevel = HIGH;
unsigned long lastSkipAt = 0;
const unsigned long SKIP_DEBOUNCE_MS = 180;

// Normal menu debounce (only when no sequence)
bool menuRaw = HIGH, menuStable = HIGH, menuLastStable = HIGH;
unsigned long menuChangedAt = 0;

// ---------------- WiFi / Web ----------------
WiFiManager wm;
WebServer server(80);
bool webRunning = false;

// Basic Auth
const char* WEB_USER = "admin";
const char* WEB_PASS = "admin123";

// Portal AP name
const char* AP_NAME = "LoRa-Relay-Setup";

// WiFi state
enum WifiState { WIFI_TRY_CONNECT, WIFI_PORTAL, WIFI_CONNECTED };
WifiState wifiState = WIFI_TRY_CONNECT;

unsigned long wifiTryStartedAt = 0;
const unsigned long WIFI_TRY_MS = 60000UL; // 60s
bool portalRunning = false;

// ---------------- Helpers ----------------
void noteActivity() {
  lastActivityAt = millis();
  if (!displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayOn = true;
  }
}

void displayPowerService() {
  if (!displayOn) return;
  if (seqActive) return;
  if ((long)(millis() - lastActivityAt) >= (long)DISPLAY_IDLE_MS) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayOn = false;
  }
}

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

void sendPacket(const String &msg) {
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
}

void sendInfoToSender(const String &text) {
  sendPacket("INF " + text);
}

String makeSta() {
  String s = "STA ";
  for (int i = 0; i < 4; i++) s += (states[i] ? '1' : '0');
  return s;
}

void publishState(bool alsoConfirmAof) {
  if (alsoConfirmAof) sendPacket("AOF " + String(autoOffRelayIndex));
  sendPacket(makeSta());
}

String jsonState() {
  String j;
  j.reserve(256);
  j = "{";
  j += "\"sta\":\"";
  for (int i=0;i<4;i++) j += (states[i] ? '1' : '0');
  j += "\",";
  j += "\"autoOff\":" + String(autoOffRelayIndex) + ",";
  j += "\"seqActive\":" + String(seqActive ? "true":"false") + ",";
  j += "\"seqPhase\":" + String(seqPhase) + ",";
  j += "\"rssi\":" + String(rssi) + ",";
  j += "\"lastMsg\":\"";
  String m = LoraMsg; m.replace("\"","'");
  j += m;
  j += "\",";
  j += "\"wifi\":\"";
  j += (WiFi.isConnected() ? WiFi.SSID() : String("DISCONNECTED"));
  j += "\",";
  j += "\"ip\":\"";
  j += (WiFi.isConnected() ? WiFi.localIP().toString() : String("0.0.0.0"));
  j += "\"";
  j += "}";
  return j;
}

// ---------------- UI ----------------
void drawUI() {
  if (!displayOn) return;

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
    display.print("AUTO:");
    display.fillRect(32, 19, 28, 11, 0);
    display.setTextColor(1);
    display.setCursor(34, 21);
    display.print("SW");
    display.print(autoOffRelayIndex);
  }

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

void infoToggleService() {
  unsigned long now = millis();
  if ((long)(now - nextInfoToggleAt) >= 0) {
    showRssiInfo = !showRssiInfo;
    nextInfoToggleAt = now + INFO_TOGGLE_MS;
    drawUI();
  }
}

// ---------------- Logic ----------------
void setAutoOffRelay(int idx, bool publish) {
  autoOffRelayIndex = idx;
  autoOffOnMillis = 0;
  LoraMsg = "AOF " + String(idx);
  noteActivity(); ledMarkActivity();
  drawUI();
  if (publish) publishState(true);
}

void handleCng(int n) {
  // Block SW0/SW1 during sequence
  if (seqActive && (n == 0 || n == 1)) {
    LoraMsg = "BLOCK CNG " + String(n) + " (SEQ)";
    noteActivity(); ledMarkActivity();
    drawUI();

    // Optional: tell sender & ack
    sendInfoToSender(LoraMsg);
    sendPacket("ERR CNG " + String(n) + " SEQ");

    // Re-publish current state so UI stays in sync
    publishState(false);
    return;
  }

  states[n] = !states[n];
  applyRelayOutput(n);
  if (n == autoOffRelayIndex) autoOffOnMillis = states[n] ? millis() : 0;

  noteActivity(); ledMarkActivity();
  drawUI();
  sendPacket("ACK " + LoraMsg);
  publishState(false);
}

// ---------------- Sequence ----------------
void startSequence() {
  if (seqActive) {
    LoraMsg = "SEQ BLOCK RUN";
    drawUI();
    sendPacket("ERR SEQ RUNNING");
    return;
  }

  if (states[1]) {
    LoraMsg = "SEQ BLOCK SW1";
    drawUI();
    sendPacket("ERR SEQ SW1ON");
    return;
  }

  if (!seqRestartArmed) {
    LoraMsg = "SEQ BLOCK BTN";
    drawUI();
    sendPacket("ERR SEQ BTN");
    return;
  }

  seqCompleted = false;
  seqRestartArmed = false;

  seqActive = true;
  seqPhase = 0;
  skipSw0 = false;
  skipSw1 = false;

  states[0] = 1;
  applyRelayOutput(0);
  publishState(false);

  seqNextAt = millis() + 10000UL;

  LoraMsg = "SEQ SW0 ON";
  drawUI();
}

void requestSkipViaGPIO14() {
  if (!seqActive) return;

  if (seqPhase == 0) skipSw0 = true;
  else if (seqPhase == 2) skipSw1 = true;

  if (seqPhase == 0) LoraMsg = "REQ SKIP SW0";
  else if (seqPhase == 2) LoraMsg = "REQ SKIP SW1";
  else LoraMsg = "SKIP N/A";

  noteActivity(); ledMarkActivity();
  drawUI();
}

void finishSequence() {
  seqActive = false;
  seqPhase = 3;
  seqCompleted = true;

  // ✅ CHANGE: re-arm restart immediately when phase=2 ends
  seqRestartArmed = true;

  LoraMsg = "SEQ DONE";
  drawUI();
  publishState(false);
}

void sequenceService() {
  if (!seqActive) return;

  unsigned long now = millis();

  if (seqPhase == 0 && skipSw0) {
    states[0] = 0; applyRelayOutput(0); publishState(false);
    skipSw0 = false;
    seqPhase = 1; seqNextAt = now + 15000UL;
    LoraMsg = "SEQ SW0 OFF";
    noteActivity(); ledMarkActivity(); drawUI();
    return;
  }

  if (seqPhase == 2 && skipSw1) {
    states[1] = 0; applyRelayOutput(1); publishState(false);
    skipSw1 = false;
    finishSequence();
    return;
  }

  if ((long)(now - seqNextAt) < 0) return;

  if (seqPhase == 0) {
    states[0] = 0; applyRelayOutput(0); publishState(false);
    seqPhase = 1; seqNextAt = now + 15000UL;
    LoraMsg = "SEQ SW0 OFF";
    noteActivity(); ledMarkActivity(); drawUI();
    return;
  }

  if (seqPhase == 1) {
    states[1] = 1; applyRelayOutput(1); publishState(false);
    seqPhase = 2; seqNextAt = now + 10000UL;
    LoraMsg = "SEQ SW1 ON";
    noteActivity(); ledMarkActivity(); drawUI();
    return;
  }

  if (seqPhase == 2) {
    states[1] = 0; applyRelayOutput(1); publishState(false);
    finishSequence();
    return;
  }
}

// ---------------- WiFi Force Portal (A) ----------------
bool isForcePortalHeldAtBoot() {
  pinMode(WIFI_FORCE_PORTAL_PIN, INPUT_PULLUP);
  unsigned long start = millis();
  while (millis() - start < WIFI_FORCE_HOLD_MS) {
    if (digitalRead(WIFI_FORCE_PORTAL_PIN) == HIGH) return false;
    delay(10);
  }
  return true;
}

// ---------------- Web Auth ----------------
bool ensureAuth() {
  if (!server.authenticate(WEB_USER, WEB_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// ---------------- Web UI (dark) ----------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LoRa Relay Receiver</title>
<style>
:root{--bg:#000;--card:#0b0b0b;--text:#fff;--muted:#bdbdbd;--border:#222;--btn:#111;--btnb:#444;--btnh:#1a1a1a;}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,sans-serif;background:var(--bg);color:var(--text);max-width:760px;margin:18px auto;padding:0 12px}
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:14px;margin:14px 0}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace;color:var(--muted)}
.row{display:flex;gap:10px;flex-wrap:wrap}
button{background:var(--btn);color:var(--text);border:1px solid var(--btnb);border-radius:12px;padding:12px 14px;margin:6px 6px 6px 0;cursor:pointer}
button:hover{background:var(--btnh)}
small{color:var(--muted)}
</style>
</head>
<body>
<h2>LoRa Relay Receiver</h2>

<div class="card">
  <div><b>WiFi</b>: <span id="wifi" class="mono"></span></div>
  <div><b>IP</b>: <span id="ip" class="mono"></span></div>
  <div><b>RSSI</b>: <span id="rssi" class="mono"></span></div>
  <div><b>Last</b>: <span id="last" class="mono"></span></div>
</div>

<div class="card">
  <div><b>States</b>: <span id="sta" class="mono"></span></div>
  <div class="row">
    <button onclick="toggleCh(0)">Toggle SW0</button>
    <button onclick="toggleCh(1)">Toggle SW1</button>
    <button onclick="toggleCh(2)">Toggle SW2</button>
    <button onclick="toggleCh(3)">Toggle SW3</button>
  </div>
</div>

<div class="card">
  <div><b>Auto-Off</b>: <span id="aof" class="mono"></span></div>
  <div class="row">
    <button onclick="setAof(0)">AOF SW0</button>
    <button onclick="setAof(1)">AOF SW1</button>
    <button onclick="setAof(2)">AOF SW2</button>
    <button onclick="setAof(3)">AOF SW3</button>
  </div>
</div>

<div class="card">
  <div><b>Sequence</b>: <span id="seq" class="mono"></span></div>
  <div class="row">
    <button onclick="seqStart()">Start SEQ 0</button>
    <button onclick="seqSkip()">Skip (GPIO14 action)</button>
  </div>
</div>

<div class="card">
  <div><b>WiFi</b>:</div>
  <div class="row">
    <button onclick="wifiReset()">Reset WiFi + Reboot</button>
  </div>
  <small class="mono">POST /api/wifi/reset</small>
</div>

<script>
async function api(path, method="POST"){
  const r = await fetch(path,{method});
  if(!r.ok){alert("HTTP "+r.status); return;}
  await refresh();
}
async function refresh(){
  const r = await fetch('/api/state');
  if(!r.ok) return;
  const j = await r.json();
  document.getElementById('wifi').textContent = j.wifi;
  document.getElementById('ip').textContent = j.ip;
  document.getElementById('rssi').textContent = j.rssi;
  document.getElementById('last').textContent = j.lastMsg;
  document.getElementById('sta').textContent = j.sta;
  document.getElementById('aof').textContent = "SW"+j.autoOff;
  document.getElementById('seq').textContent = (j.seqActive ? ("ON phase="+j.seqPhase) : "OFF");
}
function toggleCh(ch){ api('/api/toggle?ch='+ch); }
function setAof(ch){ api('/api/aof?ch='+ch); }
function seqStart(){ api('/api/seq/start'); }
function seqSkip(){ api('/api/seq/skip'); }
async function wifiReset(){
  if(!confirm("WiFi löschen und neu starten?")) return;
  await api('/api/wifi/reset');
}
setInterval(refresh, 800);
refresh();
</script>
</body></html>
)HTML";

// ---------------- Web Server ----------------
void startWebServer() {
  if (webRunning) return;

  server.on("/", HTTP_GET, []() {
    if (!ensureAuth()) return;
    noteActivity();
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", HTTP_GET, []() {
    if (!ensureAuth()) return;
    noteActivity();
    server.send(200, "application/json", jsonState());
  });

  server.on("/api/toggle", HTTP_POST, []() {
    if (!ensureAuth()) return;
    noteActivity(); ledMarkActivity();
    int ch = server.hasArg("ch") ? server.arg("ch").toInt() : -1;
    if (ch >= 0 && ch < 4) {
      LoraMsg = "WEB CNG " + String(ch);
      sendInfoToSender(LoraMsg);
      handleCng(ch);
      server.send(200, "text/plain", "OK");
    } else server.send(400, "text/plain", "BAD CH");
  });

  server.on("/api/aof", HTTP_POST, []() {
    if (!ensureAuth()) return;
    noteActivity(); ledMarkActivity();
    int ch = server.hasArg("ch") ? server.arg("ch").toInt() : -1;
    if (ch >= 0 && ch < 4) {
      LoraMsg = "WEB AOF " + String(ch);
      sendInfoToSender(LoraMsg);
      setAutoOffRelay(ch, true);
      server.send(200, "text/plain", "OK");
    } else server.send(400, "text/plain", "BAD CH");
  });

  server.on("/api/seq/start", HTTP_POST, []() {
    if (!ensureAuth()) return;
    noteActivity(); ledMarkActivity();
    LoraMsg = "WEB SEQ 0";
    sendInfoToSender(LoraMsg);
    startSequence();
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/seq/skip", HTTP_POST, []() {
    if (!ensureAuth()) return;
    noteActivity(); ledMarkActivity();
    LoraMsg = "WEB SKIP";
    sendInfoToSender(LoraMsg);
    requestSkipViaGPIO14();
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/wifi/reset", HTTP_POST, []() {
    if (!ensureAuth()) return;

    noteActivity(); ledMarkActivity();
    server.send(200, "text/plain", "RESETTING WIFI...");
    sendInfoToSender("WIFI RESET");
    delay(200);

    wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(200);
    ESP.restart();
  });

  server.begin();
  webRunning = true;

  LoraMsg = "WEB UP " + WiFi.localIP().toString();
  noteActivity(); ledMarkActivity();
  drawUI();
  publishState(true);

  Serial.println("[WEB] server.begin() OK");
  Serial.print("[WEB] Open: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

// ---------------- WiFi Portal functions ----------------
void wifiStartPortalNonBlocking() {
  wm.setCaptivePortalEnable(true);
  wm.setConnectTimeout(10);
  wm.setConfigPortalBlocking(false);

  portalRunning = wm.startConfigPortal(AP_NAME);
  wifiState = WIFI_PORTAL;

  LoraMsg = portalRunning ? "WiFi PORTAL" : "Portal FAIL";
  noteActivity(); drawUI();

  Serial.print("[WIFI] Non-blocking portal: ");
  Serial.println(portalRunning ? "STARTED" : "FAILED");
}

// GUARANTEED portal (blocking), used for forced portal at boot
void wifiStartPortalBlockingForce() {
  wm.setCaptivePortalEnable(true);
  wm.setConnectTimeout(10);
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(0);

  LoraMsg = "PORTAL (FORCE)";
  noteActivity(); drawUI();

  Serial.println("[WIFI] FORCE portal (blocking) start...");
  bool ok = wm.startConfigPortal(AP_NAME);

  Serial.print("[WIFI] FORCE portal result: ");
  Serial.println(ok ? "SAVED" : "EXIT");

  LoraMsg = ok ? "WIFI SAVED" : "PORTAL EXIT";
  noteActivity(); drawUI();
  delay(300);
  ESP.restart();
}

void wifiStartTryConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // stored credentials (NVS)
  wifiTryStartedAt = millis();
  wifiState = WIFI_TRY_CONNECT;

  LoraMsg = "WiFi TRY...";
  noteActivity(); drawUI();

  Serial.println("[WIFI] Trying stored credentials...");
}

// Main WiFi service (never blocks)
void wifiService() {
  if (WiFi.isConnected()) {
    if (wifiState != WIFI_CONNECTED) {
      wifiState = WIFI_CONNECTED;

      WiFi.setSleep(false);
      WiFi.mode(WIFI_STA);

      LoraMsg = "WiFi OK " + WiFi.localIP().toString();
      noteActivity(); ledMarkActivity();
      drawUI();
      publishState(true);

      Serial.print("[WIFI] Connected: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (wifiState == WIFI_TRY_CONNECT) {
    if (millis() - wifiTryStartedAt > WIFI_TRY_MS) {
      Serial.println("[WIFI] Connect timeout -> start portal");
      WiFi.disconnect(true, true);
      delay(100);
      wifiStartPortalNonBlocking();
    }
    delay(1);
    yield();
    return;
  }

  if (wifiState == WIFI_PORTAL) {
    if (portalRunning) wm.process();
    delay(1);
    yield();
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  printResetReason();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  lastLinkPacketAt = millis();
  ledNextAt = millis();
  ledPhase = 0;
  ledIsOn = false;

  for (int i = 0; i < 4; i++) {
    pinMode(pinsSw[i], OUTPUT);
    digitalWrite(pinsSw[i], HIGH); // OFF
  }

  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  menuRaw = digitalRead(MENU_BTN_PIN);
  menuStable = menuRaw;
  menuLastStable = menuStable;
  menuChangedAt = millis();
  lastMenuLevel = digitalRead(MENU_BTN_PIN);

  nextInfoToggleAt = millis() + INFO_TOGGLE_MS;

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) while (1);
  display.display();
  setBrightness(70);
  Wire.setClock(400000);
  Wire.setTimeout(50);

  // LoRa init
  SPI.begin(CONFIG_CLK, CONFIG_MISO, CONFIG_MOSI, CONFIG_NSS);
  LoRa.setPins(CONFIG_NSS, CONFIG_RST, CONFIG_DIO0);
  if (!LoRa.begin(868E6)) while (1);

  LoRa.setSpreadingFactor(8);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(120);
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);

  displayOn = true;
  lastActivityAt = millis();

  WiFi.setSleep(false);

  bool forcePortal = isForcePortalHeldAtBoot();
  if (forcePortal) {
    Serial.println("[WIFI] FORCE portal requested (GPIO14 held). Resetting WiFi...");
    LoraMsg = "WIFI RESET...";
    noteActivity(); drawUI();

    wm.resetSettings();
    WiFi.disconnect(true, true);
    delay(200);

    wifiStartPortalBlockingForce(); // does not return
  } else {
    wifiStartTryConnect();
  }

  publishState(true);
  drawUI();
}

void loop() {
  delay(2);

  wifiService();

  if (WiFi.isConnected() && !webRunning) {
    startWebServer();
  }

  if (webRunning) {
    server.handleClient();
  }

  ledStatusServiceReceiver(seqActive);
  sequenceService();
  infoToggleService();

  // GPIO14 skip during sequence
  bool menuLevel = digitalRead(MENU_BTN_PIN);
  if (seqActive) {
    if (lastMenuLevel == HIGH && menuLevel == LOW) {
      unsigned long now = millis();
      if (now - lastSkipAt > SKIP_DEBOUNCE_MS) {
        lastSkipAt = now;
        requestSkipViaGPIO14();
      }
    }
  }
  lastMenuLevel = menuLevel;

  // Normal menu (only when not in sequence)
  if (!seqActive) {
    if (menuDebounceUpdate()) {
      if (menuLastStable == HIGH && menuStable == LOW) {
        noteActivity();
        ledMarkActivity();
        int next = (autoOffRelayIndex + 1) % 4;
        setAutoOffRelay(next, true);
      }
    }
  }

  // LoRa receive
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    noteActivity();
    ledMarkLinkPacket();

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
  else if (LoraMsg.startsWith("SEQ")) {
    startSequence();
  }
  else if (LoraMsg.startsWith("GET")) {
    publishState(true);
    drawUI();
  }
  else {
    drawUI();
    }
  }

  // Auto-off timer (only when no sequence)
  if (!seqActive) {
    if (states[autoOffRelayIndex] && autoOffOnMillis > 0) {
      if (millis() - autoOffOnMillis >= AUTO_OFF_DURATION_MS) {
        states[autoOffRelayIndex] = 0;
        applyRelayOutput(autoOffRelayIndex);
        autoOffOnMillis = 0;

        LoraMsg = "AUTO OFF " + String(autoOffRelayIndex);
        noteActivity();
        ledMarkActivity();
        drawUI();

        sendPacket("ACK " + LoraMsg);
        publishState(false);
      }
    }
  }

  displayPowerService();
}