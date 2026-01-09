# lora-relay-menu-control

LoRa-based 4-channel relay controller using **LILYGO TTGO LoRa32**  
(ESP32 + SX1276 + SSD1306 OLED).

This repository contains two Arduino sketches:

- `sender/` – handheld / local controller with buttons and OLED UI  
- `receiver/` – remote relay board with 4 outputs and OLED UI  

Both devices communicate via **LoRa** and stay fully synchronized.

---

## Features (Overview)

- 4 logical relay channels (**SW0–SW3**)
- Bi-directional LoRa communication
- Configurable **auto-off relay** (turns OFF automatically after timeout)
- **Menu button on both devices** to select the auto-off relay
- Automatic **state synchronization** from receiver to sender
- OLED UI on both boards
- GPL-3.0 licensed

## LED Status (green LED, GPIO25)

| Pattern | Meaning |
|------|--------|
| 1× short blink / 5 s | Link OK, idle |
| 1× short blink / 1 s | Active (TX/RX, Web, buttons, sequence) |
| Double blink | LoRa link lost / no packets |
---

## Sender

### Hardware

- Board: **LILYGO TTGO LoRa32**
- Buttons:
  - GPIO `2`, `15`, `13`, `12`
  - Wiring: GPIO → button → **GND**
  - Mode: `INPUT_PULLUP`
- Menu button:
  - GPIO **14**
  - Wiring: GPIO → button → **GND**
- Status LED:
  - GPIO `25`

### Function

- Each button toggles a relay channel by sending  
  `CNG x`
- Menu button cycles the auto-off relay by sending  
  `AOF x`

### Sender OLED shows

- `AUTO: SWx` – selected auto-off relay
- `RSSI:` – link RSSI of last received packet
- `LAST TX` – last transmitted command
- Relay states **as reported by the receiver**
- `SYNC` indicator when a state update is received

The sender never guesses relay states.  
All relay states come exclusively from receiver synchronization messages.

---

## Receiver

### Hardware

- Board: **LILYGO TTGO LoRa32**
- Relay outputs:
  - GPIO `2`, `15`, `13`, `12`
  - `OUTPUT`, **active-LOW**
- Menu button:
  - GPIO **14**
  - Wiring: GPIO → button → **GND**

### Function

- Receives commands:
  - `CNG x` – toggle relay `x`
  - `AOF x` – select auto-off relay
- Auto-off logic:
  - When the selected relay turns ON, a timer starts
  - After **10 seconds**, the relay is turned OFF automatically
- Menu button can also locally change the auto-off relay

### Receiver OLED shows

- Last received LoRa message
- Relay states (**SW0–SW3**)
- Alternating info field (every **2 seconds**):
  - `RSSI: -xx`
  - `AUTO: SWx`

---

## State Synchronization

The receiver is the **single source of truth**.

After **any state change** (toggle, auto-off timeout, menu change),  
the receiver sends a full state update:

STA abcd

Example:

STA 1010

Meaning:

- SW0 = ON  
- SW1 = OFF  
- SW2 = ON  
- SW3 = OFF  

The sender updates its display immediately and briefly shows **SYNC**.

This prevents:

- inverted states
- display desynchronization
- incorrect UI after auto-off events

---

## Protocol

| Command | Direction | Description |
|-------|-----------|-------------|
| `CNG x` | Sender → Receiver | Toggle relay `x` |
| `AOF x` | Sender ↔ Receiver | Set auto-off relay |
| `STA abcd` | Receiver → Sender | Relay state synchronization |
| `GET 0` | Sender → Receiver | Request current state |
| `ACK …` | Receiver → Sender | Optional acknowledgement |

All messages are short ASCII strings.

---

## Auto-off Timing

Defined in the receiver sketch:

const unsigned long AUTO_OFF_DURATION_MS = 10000UL;

Change this value to adjust the auto-off delay.

---

## Wiring Summary

### Sender

- Buttons: `2`, `15`, `13`, `12`
- Menu button: `14`
- LED: `25`

### Receiver

- Relays: `2`, `15`, `13`, `12` (active-LOW)
- Menu button: `14`

GPIO `34` / `35` are intentionally **not used**  
(input-only, no internal pull-ups).

---

## Building & Flashing

1. Install **Arduino IDE** or **PlatformIO**
2. Install ESP32 board support
3. Install required libraries:
   - **LoRa** (Sandeep Mistry)
   - **Adafruit_GFX**
   - **Adafruit_SSD1306**
4. Open `sender/sender.ino` and `receiver/receiver.ino`
5. Select board: **TTGO LoRa32** (or compatible ESP32 LoRa board)
6. Flash each sketch to its corresponding board

---

## License

This project is licensed under the  
**GNU General Public License v3.0 (GPL-3.0)**.

https://www.gnu.org/licenses/gpl-3.0.en.html
