# lora-relay-menu-control

LoRa-based 4-channel relay controller using **LILYGO TTGO LoRa32 (ESP32 + SX1276 + OLED)** boards.

This repo contains two Arduino sketches:

- `sender/` – handheld / local controller with 4 buttons and an OLED UI
- `receiver/` – remote relay board with 4 outputs and an OLED UI

Both sides use LoRa to communicate and support:

- 4 logical relay channels (SW0–SW3)
- Configurable **auto-off relay** (which channel turns off automatically after a timeout)
- **Menu button** on *both* sender and receiver to choose the auto-off relay
- RSSI display for signal quality

## Features

### Sender

- 4 buttons (GPIO 2, 15, 13, 12) with `INPUT_PULLUP`
- Each button toggles a channel and sends:  
  `CNG x` (e.g. `CNG 0`, `CNG 1`, …)
- Extra **menu button** (GPIO 34 by default) cycles which relay (0–3) is the **auto-off** relay
- Sends `AOF x` (e.g. `AOF 2`) to update the receiver's auto-off channel
- Shows on OLED:
  - Which relay is auto-off: `AUTO: SWx`
  - Logical states of SW0–SW3
  - Last status string
  - Link RSSI based on ACKs from the receiver

### Receiver

- 4 relay outputs (GPIO 2, 15, 13, 12) as `OUTPUT`, active-LOW, matching the original project
- Receives:
  - `CNG x` → toggles SWx and drives the corresponding relay pin
  - `AOF x` → changes which relay has auto-off
- Auto-off behaviour:
  - When the configured relay is turned ON, a timer starts
  - After **10 seconds** the relay is turned OFF automatically
- Extra **menu button** (GPIO 34 by default) to locally cycle auto-off relay
  - Also sends `AOF x` back to the sender, keeping both in sync
- Shows on OLED:
  - Last received LoRa message
  - RSSI of last packet
  - Which relay is auto-off: `AUTO: SWx`
  - States of all 4 relays

## Hardware

- Board: **LILYGO TTGO LoRa32 868/915 MHz** (ESP32 + SX1276 + SSD1306 OLED)
- LoRa wiring: uses the board's default pins
- OLED: uses default I2C on GPIO 21 (SDA) and GPIO 22 (SCL)
- Make sure to select the correct board in Arduino IDE (e.g. TTGO LoRa32 or a compatible ESP32 LoRa board definition).

### Pins

Sender:

- Buttons: `2, 15, 13, 12` → to GND, with `INPUT_PULLUP`
- Menu button: `34` → to GND, with `INPUT_PULLUP`

Receiver:

- Relays: `2, 15, 13, 12` → `OUTPUT`, active-LOW (as in the original repo)
- Menu button: `34` → to GND, with `INPUT_PULLUP`

You can change pins in the arrays / defines at the top of each sketch.

## Protocol

- Toggle commands:
  - `CNG x` (ASCII string with length 5), where `x` is `0..3`
- Auto-off configuration:
  - `AOF x` (ASCII string with length 5), where `x` is `0..3`
- ACKs:
  - Receiver replies to `CNG x` with: `ACK CNG x`
  - Receiver may also echo `AOF x` back to the sender

## Auto-off behaviour

- Controlled by `autoOffRelayIndex` on both sides.
- Auto-off delay is set to 10 s via:

```cpp
const unsigned long AUTO_OFF_DURATION = 10000UL;
```

on the receiver. Change this constant to adjust the timing.

## Building

1. Install **Arduino IDE** (or PlatformIO).
2. Install **ESP32 board support**.
3. Install libraries:
   - `LoRa` by Sandeep Mistry
   - `Adafruit_GFX`
   - `Adafruit_SSD1306`
4. Open `sender/sender.ino` and `receiver/receiver.ino` as separate sketches or as two projects.
5. Select the correct board (TTGO LoRa32 / ESP32 Dev Module with matching pinout).
6. Flash each sketch to its board.

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

For the full license text, see the official GNU GPL v3.0 page:

https://www.gnu.org/licenses/gpl-3.0.en.html
