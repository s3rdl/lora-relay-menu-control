# lora-relay-menu-control

LoRa-based 4-channel relay controller using **LILYGO TTGO LoRa32**  
(ESP32 + SX1276 + SSD1306 OLED).

This repository contains two Arduino sketches:

- `sender/` – handheld / local controller with buttons and OLED UI  
- `receiver/` – remote relay board with 4 outputs and OLED UI  

Both devices communicate via **LoRa** and stay fully synchronized.

---

## Quick Start

1. Flash `receiver/receiver.ino` to a **TTGO LoRa32** board  
2. Flash `sender/sender.ino` to a second **TTGO LoRa32**
3. Power both devices
4. If the receiver has no WiFi configured:
    - Hold **GPIO14** while powering on
    - Connect to the Captive Portal and configure WiFi
5. Open the Web UI in a browser:
   - `http://<receiver-ip>/`
   - or `http://lora-receiver.local/` *(if mDNS is available on your system)*
6. Control relays via:
    - sender buttons
    - receiver buttons
    - Web UI

The system synchronizes relay states automatically.

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

### Sequence Control (Receiver)

The receiver supports a **built-in timed sequence**:

1. **SW0 ON** for 10 seconds  
2. **Wait 15 seconds**  
3. **SW1 ON** for 10 seconds  
4. Sequence ends automatically

#### Sequence start

- The sequence is started remotely:
  - via the **sender**
  - or via the **Web UI**

#### Sequence shortening / skipping (GPIO14)

While a sequence is running, **GPIO14** on the receiver has a special function:

- If pressed during **SW0 phase**  
  → SW0 is turned OFF immediately and the sequence continues with the 15 s wait
- If pressed during **SW1 phase**  
  → SW1 is turned OFF immediately and the sequence finishes

This allows the sequence to be **shortened without aborting it completely**.

> GPIO14 does **not cancel** the sequence.  
> It only skips the **current active step** and keeps timing logic intact.

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

## Web UI (Receiver)

The receiver provides a built-in **web interface** that allows full control
and monitoring of the system via a browser.

The Web UI runs **directly on the receiver** and is available whenever the
device is connected to WiFi.

### Forced WiFi Captive Portal (GPIO14 at boot)

The receiver uses **WiFiManager** to handle WiFi configuration.

If the device is already configured but the network is unavailable,
the Captive Portal can be **forced manually**:

#### How to force the portal

1. Power OFF the receiver
2. **Press and hold GPIO14**
3. Power ON the receiver
4. Keep GPIO14 pressed for ~1 second
5. Release the button

The receiver will:

- erase stored WiFi credentials
- start the **WiFiManager Captive Portal**
- wait until new WiFi credentials are saved
- reboot automatically

This works **even if the device was previously connected to a different network**.

> GPIO14 therefore has **two roles** on the receiver:
> - **During boot:** force WiFi Captive Portal  
> - **During runtime:** shorten an active sequence step

### Features

- Toggle relay outputs **SW0–SW3**
- Select the **auto-off relay**
- Start and control the **sequence**
- View live system status:
  - Relay states
  - Auto-off selection
  - Sequence state
  - Last received command
  - LoRa RSSI
  - WiFi SSID and IP address
- Trigger **WiFi reset** (Captive Portal) remotely

### Access

After the receiver is connected to WiFi, open in your browser:

http://your-IP-address/

If mDNS is supported by your system, you can also use:

http://lora-receiver.local/

### Authentication

The Web UI is protected using **HTTP Basic Authentication**.

Default credentials (can be changed in the receiver sketch):

- **Username:** `admin`
- **Password:** `admin123`

### Endpoints (overview)

| URL | Description |
|----|-------------|
| `/` | Landing page (status & links) |
| `/ui` | Full Web UI (authenticated) |
| `/ping` | Health check (returns `OK`) |
| `/api/state` | JSON system state |
| `/api/toggle?ch=x` | Toggle relay `x` |
| `/api/aof?ch=x` | Set auto-off relay |
| `/api/seq/start` | Start sequence |
| `/api/seq/skip` | Skip current sequence step |
| `/api/wifi/reset` | Reset WiFi & reboot |

> **Note:**  
> The Web UI is intended for **local network use**.  
> For remote access from outside the LAN, use a **VPN** (recommended).

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

## Sequence Timing

The timed relay sequence is fully configurable in the receiver sketch.

The default sequence consists of the following steps:

1. **SW0 ON** for 10 seconds  
2. **Wait** for 15 seconds  
3. **SW1 ON** for 10 seconds  

These timings are defined via constants in the receiver sketch:


const unsigned long SEQ_SW0_ON_MS   = 10000UL;  // SW0 active time
const unsigned long SEQ_WAIT_MS    = 15000UL;  // delay between steps
const unsigned long SEQ_SW1_ON_MS  = 10000UL;  // SW1 active time

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

- Menu / control button: `14`
  - Runtime: sequence step skip
  - Boot: force WiFi Captive Portal

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

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, modify, and redistribute this project under the terms of
the GPL-3.0 license.

### Dual Licensing / Commercial Use

Commercial use of this project **requires a separate commercial license**.

If you intend to use this software in a commercial product, service, or
closed-source environment, please contact the author to obtain a commercial
license.

This dual-licensing model ensures that the project remains open and free
for the community, while allowing fair commercial use under separate terms.

## Credits and Attribution

This project is inspired by the work of **VolosR** and the project  
[loraRealySwitch](https://github.com/VolosR/loraRealySwitch).

The original project provided the initial idea, visual concepts, and general
approach for a LoRa-based relay controller with OLED UI.

Parts of this project were **independently reimplemented and significantly
extended**, including:

- bi-directional state synchronization
- auto-off logic
- timed relay sequences
- Web UI with WiFi configuration and Captive Portal
- system status LEDs and monitoring
- improved UI logic and robustness

## Thanks
This project was developed with extensive experimentation, iteration, and the help of open-source tools and discussions. Special thanks to everyone who contributes to an open and collaborative ecosystem.

### Fonts (`myFonts.h`)

The file `myFonts.h` originates from the original **VolosR** project and is used
in this repository with the **explicit permission of the original author**.

Full credit for the font definitions and original inclusion goes to **VolosR**.

If future licensing or attribution requirements arise, this repository will be
updated accordingly.
