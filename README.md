# 📡 RoIP Walkie-Talkie — ESP32-C3 Xmini

> Push-to-talk voice communication over WiFi using raw UDP. No accounts, no passwords, no cloud.

![Platform](https://img.shields.io/badge/platform-ESP32--C3-blue?logo=espressif)
![Language](https://img.shields.io/badge/firmware-Arduino%20C%2B%2B-orange)
![Server](https://img.shields.io/badge/server-Python%203.10%2B-3776AB?logo=python&logoColor=white)
![Protocol](https://img.shields.io/badge/protocol-UDP-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

---

## Overview

A minimal, low-latency **Radio over IP (RoIP)** system built around the ESP32-C3 Xmini board. Hold the BOOT button to transmit — every other connected device hears you immediately through its speaker.

```
[ESP32 #1]  ──── UDP ────►  [roip_server.py]  ──── UDP ────►  [ESP32 #2]
 BOOT held                     relays audio                  plays on speaker
 mic captured                  to all others                 OLED shows RX
```

The project is split into two components:

| Component | Description |
|-----------|-------------|
| `roip_server.py` | Lightweight Python relay server. Receives audio from one client and forwards it to all others. Runs on any Linux machine, Raspberry Pi, or VPS. |
| `RoIP_Walkie-Talkie_ESP32-C3_Xmini_v1_0.ino` | ESP32-C3 firmware. Handles WiFi, audio capture/playback, jitter buffering, OLED display, and PTT logic. |

---

## Features

- 🔇 **PTT (Push-to-Talk)** via the onboard BOOT button
- 🎙️ **Full-duplex capable** — any client can transmit, all others receive simultaneously
- 📦 **Jitter buffer** — smooths out WiFi packet timing for clean audio playback
- 🔊 **ES8311 codec** with direct I2C register control for precise mic gain
- 📺 **OLED status display** — shows TX/RX state, client ID, jitter buffer level, and PGA gain
- 🔁 **Auto-reconnect** — periodic HELLO keepalives re-register the device after server restart
- ⚡ **Low overhead** — binary UDP protocol, no JSON, no TLS, no broker

---

## Hardware

| Component | Part |
|-----------|------|
| MCU | ESP32-C3 Xmini |
| Audio codec | ES8311 |
| Speaker amplifier | NS4150B (3W, GPIO 11 enable) |
| Display | SSD1306 0.96" OLED 128×64 |
| PTT | BOOT button (GPIO 9, active LOW) |

### Pin mapping

| Signal | GPIO |
|--------|------|
| I2C SDA | 3 |
| I2C SCL | 4 |
| I2S MCLK | 10 |
| I2S BCLK | 8 |
| I2S WS | 6 |
| I2S DOUT | 5 |
| I2S DIN | 7 |
| PA enable | 11 |
| PTT | 9 |

---

## Protocol

All packets use a fixed 6-byte binary header over UDP:

```
 0     1     2       3          4–5
[0xA5][0x7B][Type][ClientID][SeqNum BE]  +  Payload
```

| Type | Value | Payload |
|------|-------|---------|
| HELLO | `0x01` | *(none)* — register with server |
| AUDIO | `0x02` | PCM 16-bit mono 16 kHz, 20 ms frames (640 bytes) |
| BYE | `0x03` | *(none)* — disconnect |
| PING | `0x04` | *(none)* — keepalive |
| PONG | `0x05` | `1 byte` — assigned ClientID |

Raw audio bandwidth: **~32 KB/s** per active transmitter.

---

## Getting started

### Server

**Requirements:** Python 3.10+, no external dependencies.

```bash
python3 roip_server.py
```

The server binds to UDP port `12345` on all interfaces. To run persistently:

```bash
# Simple background process
nohup python3 roip_server.py > roip.log 2>&1 &

# Or as a systemd service (recommended for Raspberry Pi)
```

Configuration is at the top of `roip_server.py`:

```python
UDP_PORT       = 12345
CLIENT_TIMEOUT = 30    # seconds before idle client is dropped
MAX_CLIENTS    = 20
LOG_AUDIO      = False # set True to log every audio packet
```

---

### Firmware

**1. Configure credentials**

Edit the `CONFIGURATION` section at the top of the `.ino` file:

```cpp
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"
#define SERVER_HOST   "192.168.1.x"  // IP of the machine running roip_server.py
#define SERVER_PORT   12345
```

**2. Arduino IDE board settings**

| Setting | Value |
|---------|-------|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | **Disabled** |
| CPU Frequency | 160 MHz |
| Flash Mode | DIO |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 921600 |

**3. Required libraries**

Install via Arduino Library Manager or manually:

- [`arduino-audio-driver`](https://github.com/pschatzmann/arduino-audio-driver)
- `Adafruit SSD1306`
- `Adafruit GFX Library`

**4. Flash and verify**

Open Serial Monitor at **115200 baud**. On successful boot you will hear a two-tone beep and the OLED will show `STANDBY`.

---

## Usage

| Action | Result |
|--------|--------|
| Hold BOOT button | Transmits microphone audio — OLED shows `** TX **` |
| Release BOOT | Stops transmitting |
| Incoming audio | OLED shows `RX <-- ID:x`, audio plays through speaker |

Up to **20 clients** can be connected simultaneously. Each device is auto-assigned a numeric ID by the server on first contact.

---

## Audio tuning

The following defines in the firmware can be adjusted for your environment:

| Define | Default | Description |
|--------|---------|-------------|
| `ES8311_REG14_PGA` | `0x12` | Mic analog gain. `0x10`=0dB · `0x12`=6dB · `0x14`=12dB · `0x16`=18dB · `0x1A`=30dB |
| `TX_GAIN` | `0.85` | Software attenuation applied before transmitting (0.0–1.0) |
| `RX_GAIN` | `1.0` | Software scaling applied during playback (0.0–1.0) |
| `SPK_VOLUME` | `82` | DAC output volume (0–100). Keep below ~85 to avoid NS4150B clipping |
| `NOISE_GATE_RMS` | `0` | Noise gate threshold on raw RMS. `0` = disabled (PTT-only mode) |
| `JITTER_FRAMES` | `5` | Frames buffered before playback begins (5 × 20ms = 100ms pre-roll) |
| `CLIP_CEILING` | `0.93` | Hard clip ceiling for TX signal (0.0–1.0) |

> **Tip:** If transmitted audio is too quiet, increase `ES8311_REG14_PGA` one step at a time (e.g. `0x12` → `0x14`). If it clips or distorts, lower `TX_GAIN` first.


*Author: SA7BNB*
