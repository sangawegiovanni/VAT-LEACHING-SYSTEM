# VAT Leaching System – Industrial IoT Monitoring

![Dashboard Preview]
<img width="1280" height="960" alt="a5d97303-aaa4-47d0-844a-18f8920a445e" src="https://github.com/user-attachments/assets/03622a9e-1cf2-4d15-9f27-939f95674f3b" />

## Overview

The **VAT Leaching System** is a real‑time industrial monitoring solution for metallurgical processes. It measures tank levels, pH, temperature, ORP, calculates lime dosage, and controls a circulation pump. Data is sent to a cloud MQTT broker and displayed on a live dashboard accessible from any device. Critical alarms are sent via SMS to six predefined numbers using an HTTP gateway.

![System Architecture]

---


## Features

- **Two‑tank monitoring**  
  - Upper tank (Leaching VAT): A0221AU UART ultrasonic sensor (max 22 cm water level).  
  - Lower tank (Solution Tank): HC‑SR04 ultrasonic sensor (empty at 22 cm, full at 0 cm).
- **pH measurement** – Analog pH probe (A0) with software offset and median filtering.
- **Automatic pump control** – Relay on/off based on upper tank distance (shutdown at ≤5 cm, start at ≥8 cm).
- **Lime dosage calculation** – Based on pH and solution volume.
- **TFT colour display** – Real‑time tank levels, pH gauge, pump status, notifications.
- **MQTT cloud publishing** – Public HiveMQ broker, JSON or CSV format.
- **SMS alerts** – HTTP POST to `zoostudios.us.to/sms/send` when pump turns ON (sends to 6 numbers).
- **GitHub Pages dashboard** – Live dashboard accessible worldwide.

---

## Hardware Components

| Component | Model / Spec | Quantity |
|-----------|--------------|----------|
| Arduino Mega 2560 | – | 1 |
| ESP32 Dev Board | – | 1 |
| Ultrasonic sensor (upper tank) | A0221AU (UART) | 1 |
| Ultrasonic sensor (lower tank) | HC‑SR04 | 1 |
| pH sensor kit | Analog (BNC) | 1 |
| Relay module | 5V | 1 |
| TFT display | ILI9341 (SPI) | 1 |
| Power supply | 5V / 2A (for ESP32 + Mega) | 2 |
| Jumper wires | – | many |

![Hardware Setup](./images/hardware.jpg)

---

## Wiring Diagram

### Arduino Mega Connections

| Mega Pin | Connected To |
|----------|--------------|
| 5V, GND | Power supply, sensors, relay |
| 53 (CS), 9 (DC), 8 (RST) | TFT display (SPI) |
| 18 (TX1) | A0221AU (RX – yellow wire) |
| 19 (RX1) | A0221AU (TX – white wire) |
| 28 | HC‑SR04 TRIG |
| 30 | HC‑SR04 ECHO |
| A0 | pH sensor (A0) |
| 6 | Relay IN |
| 16 (TX2) | ESP32 RX (GPIO26) |
| 17 (RX2) | ESP32 TX (GPIO27) – optional |
| GND | ESP32 GND (critical!) |

### ESP32 Connections

| ESP32 Pin | Connected To |
|-----------|--------------|
| GPIO26 (RX1) | Mega TX2 (pin 16) |
| GPIO27 (TX1) | Mega RX2 (pin 17) – optional |
| GND | Mega GND (critical) |

![Wiring Diagram](./images/wiring.jpg)

> **⚠️ Important:** A common ground wire between Mega and ESP32 is mandatory for clean UART communication.

---

## Software

### 1. Arduino Mega Code

Upload the provided `mega_vat_leaching.ino` to the Mega. It:
- Reads sensors (A0221AU, HC‑SR04, pH).
- Controls pump relay.
- Sends 8‑field CSV packets via Serial2 every 2 seconds:  
  `DATA, d1, d2, ph, wl1, wl2, vol, lime, pump`

### 2. ESP32 Code

Upload `esp32_vat_leaching.ino` to the ESP32. It:
- Listens to Serial1 (GPIO26/27) for Mega data.
- Parses the CSV lines (robust parser prints raw and parsed values).
- Publishes JSON to MQTT public broker `broker.hivemq.com:1883`.
- Sends HTTP POST SMS when pump turns ON.

### 3. Web Dashboard

The dashboard (`index.html`) is hosted on **GitHub Pages**. It:
- Connects to the same MQTT topic via WebSockets (`wss://broker.hivemq.com:8884/mqtt`).
- Displays pH, tank levels, volume, lime, pump status.
- Auto‑refreshes every 5 seconds.

**Live URL:** `https://sangawegiovanni.github.io/VAT-LEACHING-SYSTEM`

![Dashboard Screenshot](./images/dashboard_screenshot.jpg)

---

## Installation & Setup

### Prerequisites

- Arduino IDE with libraries:
  - `Adafruit_GFX`, `Adafruit_ILI9341`
  - `SPI`, `EEPROM`, `PubSubClient`, `ArduinoJson`
- GitHub account for Pages hosting.

### Steps

1. **Upload Mega code** – Select board “Arduino Mega 2560”, COM port, 115200 baud.
2. **Upload ESP32 code** – Select board “ESP32 Dev Module”, COM port, 115200 baud.
3. **Wire hardware** according to the diagram above.
4. **Connect common ground** between Mega and ESP32.
5. **Open Serial Monitor** for ESP32 – you should see clean `[RAW] DATA,...` lines.
6. **Adjust pH offset** – In Mega code, `PH_OFFSET` is currently `-4.73`. Calibrate by measuring pH in neutral water and adjusting until reading shows 7.0.
7. **Deploy dashboard** – Push `index.html` to GitHub repository, enable Pages.
8. **Access dashboard** – Open `https://yourusername.github.io/repo-name`.

---

## SMS Gateway

The ESP32 sends an HTTP POST to `https://zoostudios.us.to/sms/send` with JSON body:

```json
{
  "source_addr": "androidSMSGateway",
  "message": "VAT Leaching System\nPump: ON\npH: 8.22\nVolume: 3.90 L",
  "recipients": [
    { "recipient_id": "1", "dest_addr": "255637341780" },
    { "recipient_id": "2", "dest_addr": "255692007363" },
    ...
  ]
}
