# 📻 IP-Radio v1.0

> Push-to-talk voice radio over UDP/IP — ESP32-C3 client + Python server with a web-based admin interface.
https://www.youtube.com/shorts/i9r1kPGAcrw

![IP-Radio Server UI](server.png)

---

## Overview

IP-Radio is a custom-built PTT (Push-To-Talk) system that relays 16 kHz mono PCM audio in real time over a local network or internet. The system consists of two parts:

- **Client** — firmware for the ESP32-C3 Xmini with ES8311 codec, OLED display, WS2812 RGB LED, and BOOT button as PTT.
- **Server** — a Python application acting as an audio relay with a web-based administration panel.

Intended use cases: industrial intercom systems, amateur radio experiments, or any scenario where walkie-talkie-style communication is needed without traditional radio infrastructure.

---

## Hardware (Client)

![ESP32-C3 AI Voice WiFi Development Board](ESP32-C3_AI_Voice_WiFi_Development_Board.png)

| Component | Detail |
|---|---|
| Microcontroller | ESP32-C3 Xmini |
| Codec | ES8311 (I²S, I²C) |
| Display | SSD1306 OLED 128×64 px |
| LED indicator | WS2812 RGB (GPIO 2) |
| Amplifier | NS4150B (GPIO 11, active HIGH) |
| PTT button | BOOT button (GPIO 9, active LOW) |
| Battery | LiPo 300 mAh 3.7 V (optional) |

---

## File Structure

```
├── esp32_code.ino      # Firmware for the ESP32-C3 client
├── server.py           # Python server (relay + web UI)
└── ipradio_state.json  # Auto-generated — stores known nodes, rooms and blocked IPs
```

---

## Client — `esp32_code.ino`

### Function

Firmware for the ESP32-C3 that connects to WiFi and communicates with the server over UDP. The user presses the BOOT button to transmit, just like a regular walkie-talkie.

### Audio Pipeline

**Transmit (TX):**
1. Reads stereo I²S from the ES8311 codec, extracts the L channel (microphone).
2. DC-blocking high-pass filter (IIR, coefficient 0.995).
3. RMS calculation for noise gate (configurable threshold).
4. Applies TX_GAIN and hard clip ceiling (CLIP_CEILING).
5. Sends 640-byte UDP packets (320 samples × 16-bit) at 20 ms intervals.

**Receive (RX):**
1. Incoming UDP packets are buffered in a jitter buffer (5–8 frames).
2. Played back via I²S to the NS4150B amplifier with RX_GAIN applied.

**Roger beep:**  
When PTT is released, a 1760 Hz sine tone (150 ms) is automatically sent with an ADSR envelope (attack 10 ms, sustain 80 ms, decay 60 ms) — classic walkie-talkie feel.

### LED Indicator (WS2812)

| Color | Meaning |
|---|---|
| 🔴 Red, solid | PTT active — transmitting |
| 🟢 Green, solid | Receiving audio |
| 🟡 Yellow, blinking 1 Hz | Waiting for admin approval |
| 🔴 Red, blinking 2 Hz | Rejected / blocked by server |
| Off | Standby / offline |

### OLED Display

The display shows the node name, WiFi icon, current room and communication status (`** TX **`, `RECEIVING`, `STANDBY`, `WAITING APPROVAL`, `ACCESS DENIED`). It automatically turns off after 5 seconds of inactivity and wakes again on PTT press or incoming audio.

### Registration State Machine

```
REG_NONE → REG_PENDING → REG_ACTIVE
                       ↘ REG_REJECTED (retry after 60 s)
```

### Protocol

Binary UDP protocol with a 6-byte header:

```
Byte 0-1: Magic (0xA5 0x7B)
Byte 2:   Type  (HELLO / AUDIO / BYE / PING / PONG / REJECT / ROOM_INFO)
Byte 3:   ClientID
Byte 4-5: SeqNum (uint16, big-endian)
```

### Arduino IDE Settings

| Setting | Value |
|---|---|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | Disabled |
| CPU Frequency | 160 MHz |
| Flash Mode | DIO |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 921600 |

### Dependencies

- [arduino-audio-driver](https://github.com/pschatzmann/arduino-audio-driver)
- Adafruit SSD1306 + Adafruit GFX
- Adafruit NeoPixel

### Configuration

Edit these lines at the top of `esp32_code.ino`:

```cpp
#define WIFI_SSID       "YourNetwork"
#define WIFI_PASSWORD   "YourPassword"
#define SERVER_HOST     "192.168.1.10"
#define SERVER_PORT     12345
#define NODE_NAME       "SA7BNB-1"   // Unique name, max 16 characters
```

---

## Server — `server.py`

### Function

Asynchronous Python server (asyncio + aiohttp) that listens for UDP on port 12345 and serves a password-protected web interface on port 8080. The server acts as a relay — audio from one client is forwarded to all other clients in the same room.

### Web Interface

![Server Web UI](server.png)

The web interface updates in real time via Server-Sent Events (SSE) and provides:

- **Pending Approval** — New unknown nodes appear here. The admin manually approves or rejects them.
- **Connected Clients** — Shows active clients with IP, room and last-seen time. Options to kick or block.
- **Known Nodes** — Previously approved nodes are auto-approved on reconnect. Approval can be revoked.
- **Rooms** — Create and delete rooms. Clients in different rooms cannot hear each other.
- **Blocked IPs** — List of blocked IP addresses with the option to unblock.

### Node Management

A node's `NODE_NAME` is its unique identifier. If a known node reconnects from a new IP address, the old session is automatically replaced. Approved nodes, blocked IPs and rooms are persisted in `ipradio_state.json` and loaded on server restart — no reconfiguration needed.

### Security

- Password-protected admin interface (SHA-256 + salt, stored in `ipradio_state.json`).
- Session-based authentication with an HttpOnly cookie, 8-hour sliding window.
- On first launch, navigating to `/` redirects to a setup page to choose a password.

### Installation & Start

```bash
pip install aiohttp
python3 server.py
```

Then open `http://<server-ip>:8080/` in a browser.

### Configuration

Edit the variables at the top of `server.py`:

```python
UDP_HOST       = "0.0.0.0"
UDP_PORT       = 12345
WEB_HOST       = "0.0.0.0"
WEB_PORT       = 8080
CLIENT_TIMEOUT = 30    # seconds without a packet before client is removed
MAX_CLIENTS    = 20
```

---

## System Diagram

```
┌──────────────────┐        UDP :12345         ┌─────────────────────┐
│  ESP32-C3 Xmini  │ ─────────────────────────▶│                     │
│  NODE: SA7BNB-1  │ ◀──────────── relay ───── │   ipradio_server    │
└──────────────────┘                           │   (Python/asyncio)  │
                                               │                     │
┌──────────────────┐        UDP :12345         │                     │
│  ESP32-C3 Xmini  │ ─────────────────────────▶│                     │
│  NODE: SA7BNB-2  │ ◀──────────── relay ───── │                     │
└──────────────────┘                           └──────────┬──────────┘
                                                          │ HTTP :8080
                                               ┌──────────▼──────────┐
                                               │   Web UI (admin)    │
                                               │   SSE real-time     │
                                               └─────────────────────┘
```

---

## License

Free to use and modify for personal and experimental purposes.

---

*Project by SA7BNB*
