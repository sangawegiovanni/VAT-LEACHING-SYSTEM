# VAT Leaching System – Industrial IoT Monitoring

![Dashboard Preview](./images/dashboard.png)

## Overview

The **VAT Leaching System** is a real‑time industrial monitoring solution for metallurgical processes. It measures tank levels, pH, temperature, ORP, calculates lime dosage, and controls a circulation pump. Data is sent to a cloud MQTT broker and displayed on a live dashboard accessible from any device. Critical alarms are sent via **GSM (SIM800C)** to six predefined numbers.

![System Architecture](./images/architecture.png)

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
- **GSM SMS alerts** – SIM800C module sends SMS to six numbers when pump turns ON (direct AT commands, no external gateway).
- **GitHub Pages dashboard** – Live dashboard accessible worldwide.

---

## Hardware Components

| Component | Model / Spec | Quantity |
|-----------|--------------|----------|
| Arduino Mega 2560 | – | 1 |
| ESP32 Dev Board | – | 1 |
| GSM module | SIM800C (UART) | 1 |
| Ultrasonic sensor (upper tank) | A0221AU (UART) | 1 |
| Ultrasonic sensor (lower tank) | HC‑SR04 | 1 |
| pH sensor kit | Analog (BNC) | 1 |
| Relay module | 5V | 1 |
| TFT display | ILI9341 (SPI) | 1 |
| Power supply | 5V / 2A (for ESP32 + Mega) | 2 |
| External 5V/2A supply | for SIM800C | 1 |
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
| GPIO16 (RX2) | SIM800C TXD |
| GPIO17 (TX2) | SIM800C RXD |
| 5V | External 5V/2A supply (shared ground) |

![Wiring Diagram](./images/wiring.jpg)

> **⚠️ Important:**  
> - Common ground between Mega and ESP32 is mandatory for clean UART communication.  
> - SIM800C must be powered from an **external 5V/2A supply** – the ESP32’s 5V pin cannot provide enough current.

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
- **Controls SIM800C via Serial2 (GPIO16/17)**:
  - Initialises the module at 115200 baud.
  - Sends SMS to six predefined numbers when pump turns ON.
  - Handles incoming calls from an authorised number (sends status SMS and auto‑answers/hangs up).

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
5. **Power SIM800C externally** (5V/2A). Connect its TXD/RXD to ESP32 GPIO16/17.
6. **Open Serial Monitor** for ESP32 – you should see `[GSM] SIM800C ready` and clean `[RAW] DATA,...` lines.
7. **Adjust pH offset** – In Mega code, `PH_OFFSET` is currently `-4.73`. Calibrate by measuring pH in neutral water and adjusting until reading shows 7.0.
8. **Deploy dashboard** – Push `index.html` to GitHub repository, enable Pages.
9. **Access dashboard** – Open `https://yourusername.github.io/repo-name`.

---

## GSM SMS Configuration

The ESP32 uses a SIM800C module on Serial2 (GPIO16/17) at 115200 baud. It sends SMS to the following numbers (without ‘+’ prefix) when the pump turns ON:
255xxxxxxx

text

You can change these numbers in the ESP32 code (`SMS_NUMBERS` array). The authorised caller number (for remote status request) is also configurable.

**Example SMS content:**
VAT LEACHING SYSTEM
====================
STATUS: PUMP ON

UPPER TANK
Dist: 21.7 cm
Level: 0.3 cm

LOWER TANK
Dist: 19.5 cm
Level: 2.5 cm
Vol: 3.90 L
Lime: 1.5 g

pH: 8.22

text

The module also detects incoming calls: if the call comes from the authorised number, it sends a status SMS and then hangs up; other callers are rejected.

---

## Troubleshooting

| Symptom | Likely cause | Solution |
|---------|--------------|----------|
| Garbage on ESP32 Serial Monitor | Missing ground between Mega and ESP32 | Connect a GND wire |
| No `[RAW]` lines | Wrong baud rate or swapped TX/RX | Set both to 9600, check wiring |
| GSM not responding | Insufficient power or wrong baud | Use external 5V/2A supply, set Serial2 to 115200 |
| pH reads 14.00 or 0.00 | Probe not connected or wrong offset | Calibrate using `calib` command |
| Lower tank always 0% | HC‑SR04 not triggered or out of range | Check pins 28/30, distance should be < 450cm |
| Dashboard shows “OFFLINE” | MQTT broker not reachable | Check ESP32 Wi‑Fi, broker URL |

---

## Future Improvements

- Add temperature and ORP sensors.
- Use TLS/SSL for MQTT with certificate.
- Implement database logging (InfluxDB + Grafana).
- Add OTA updates for ESP32.

---

## License

This project is open‑source. Feel free to use and modify for your own industrial monitoring needs.

---

## Contact

For questions or contributions, please open an issue on the GitHub repository.
