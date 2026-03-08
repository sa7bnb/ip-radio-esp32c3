/**
 * ============================================================
 *  IP-Radio  -  ESP32-C3 Xmini  v1.0
 * ============================================================
 *  v1.0: NODE_NAME in HELLO payload, RegState machine,
 *        TYPE_REJECT, TYPE_ROOM_INFO, OLED room/status display,
 *        Roger beep on PTT release, OLED sleep after 5 s idle,
 *        WS2812 RGB LED indicator
 *          RED       = transmitting (PTT active)
 *          GREEN     = receiving audio
 *          YELLOW blink = waiting for approval
 *          RED blink = rejected / blocked
 *          OFF       = standby
 *
 *  BOOT button (GPIO9) = PTT
 *
 * ============================================================
 *  ARDUINO IDE SETTINGS
 * ============================================================
 *  Board:            ESP32C3 Dev Module
 *  USB CDC On Boot:  Disabled
 *  CPU Frequency:    160 MHz
 *  Flash Mode:       DIO
 *  Partition Scheme: Default 4MB with spiffs
 *  Upload Speed:     921600
 *
 * ============================================================
 *  LIBRARIES
 * ============================================================
 *  arduino-audio-driver  (github.com/pschatzmann/arduino-audio-driver)
 *  Adafruit SSD1306 + Adafruit GFX
 *  Adafruit NeoPixel
 *
 *  Author: SA7BNB
 * ============================================================
 */

#include "AudioBoard.h"
#include "DriverPins.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <math.h>

// ============================================================
//  CONFIGURATION  -  edit these
// ============================================================
#define WIFI_SSID        "SSID"
#define WIFI_PASSWORD    "PASSWORD"
#define SERVER_HOST      "192.168.1.10"
#define SERVER_PORT      12345

// Node name (max 16 chars) - unique identifier shown in server UI
// Use your callsign or any unique name.
#define NODE_NAME        "SA7BNB-1"

// ES8311 MIC PGA (register 0x14, bits[3:0])
// Formula: value = 0x10 | (dB/3)
//   0x10=0dB  0x12=6dB  0x14=12dB  0x16=18dB  0x18=24dB  0x1A=30dB
#define ES8311_REG14_PGA   0x12   // 6 dB PGA
#define ES8311_REG17_ADC   0xBF   // full digital ADC gain

// Software gain
#define TX_GAIN     0.85f   // applied to mic signal before sending
#define RX_GAIN     1.0f    // applied to incoming PCM on playback
#define SPK_VOLUME  82      // DAC speaker volume (0-100), keep < 85 to avoid clipping

// Noise gate - RMS threshold on raw mic signal (0 = disabled, PTT controls TX)
#define NOISE_GATE_RMS   0

// Hard clip ceiling (0.0-1.0)
#define CLIP_CEILING  0.93f

// OLED display sleep after this many ms of inactivity
#define OLED_SLEEP_MS  5000UL

// WS2812 RGB LED - change PIN_WS2812 if your board differs (Xmini-C3 = GPIO2)
#define PIN_WS2812         2
#define WS2812_BRIGHTNESS  40   // 0-255, keep low to avoid glare

// Time to wait before retrying after a REJECT (ms)
#define REJECT_RETRY_MS  60000UL

// ============================================================
//  PINS
// ============================================================
#define PIN_SDA     3
#define PIN_SCL     4
#define PIN_MCLK    10
#define PIN_BCLK    8
#define PIN_WS      6
#define PIN_DOUT    5
#define PIN_DIN     7
#define PIN_PA      11    // NS4150B amplifier enable, active HIGH
#define PIN_PTT     9     // BOOT button, active LOW
#define OLED_ADDR   0x3C
#define ES8311_ADDR 0x18

// ============================================================
//  AUDIO  -  16 kHz mono PCM, 20 ms frames
// ============================================================
#define SAMPLE_RATE      16000
#define FRAME_MS         20
#define FRAME_SAMPLES    (SAMPLE_RATE * FRAME_MS / 1000)   // 320 samples
#define FRAME_BYTES      (FRAME_SAMPLES * 2)               // 640 bytes

#define JITTER_FRAMES         5
#define JITTER_BUF_FRAMES_MAX 8

// ============================================================
//  PROTOCOL
// ============================================================
#define MAGIC_0        0xA5
#define MAGIC_1        0x7B
#define TYPE_HELLO     0x01
#define TYPE_AUDIO     0x02
#define TYPE_BYE       0x03
#define TYPE_PING      0x04
#define TYPE_PONG      0x05
#define TYPE_REJECT    0x06   // server -> client: denied / kicked / blocked
#define TYPE_ROOM_INFO 0x07   // server -> client: room assignment
#define HEADER_SIZE    6

#define REJECT_NOT_APPROVED 0x01
#define REJECT_BLOCKED      0x02
#define REJECT_KICKED       0x03
#define REJECT_FULL         0x04

#define HELLO_INTERVAL_MS   5000
#define UDP_RX_BUF_SIZE     (HEADER_SIZE + FRAME_BYTES + 32)

// ============================================================
//  REGISTRATION STATE
// ============================================================
enum RegState : uint8_t {
    REG_NONE     = 0,   // not yet attempted / WiFi not ready
    REG_PENDING  = 1,   // HELLO sent, waiting for admin approval
    REG_ACTIVE   = 2,   // registered and ready to communicate
    REG_REJECTED = 3    // denied / blocked by server
};

// ============================================================
//  GLOBALS
// ============================================================
static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;

WiFiUDP           udp;
Adafruit_SSD1306  oled(128, 64, &Wire, -1);
Adafruit_NeoPixel led(1, PIN_WS2812, NEO_GRB + NEO_KHZ800);

volatile RegState regState   = REG_NONE;
volatile uint8_t  myId       = 0;
volatile bool     pttActive  = false;
volatile bool     remoteTx   = false;
volatile uint8_t  remoteTxId = 0;

// Room info (assigned by server)
volatile uint8_t myRoomId      = 0;
char             myRoomName[16] = "General";

// Jitter buffer
static uint8_t      jbuf[JITTER_BUF_FRAMES_MAX * FRAME_BYTES];
static volatile int jbuf_write = 0;
static volatile int jbuf_read  = 0;
static volatile int jbuf_count = 0;
static portMUX_TYPE jbuf_mux   = portMUX_INITIALIZER_UNLOCKED;

static uint16_t txSeq        = 0;
unsigned long   lastHello    = 0;
unsigned long   lastOled     = 0;
unsigned long   lastRxAudio  = 0;
unsigned long   rejectedAt   = 0;   // timestamp when REG_REJECTED was set
unsigned long   lastActivity = 0;   // last TX or RX - controls OLED sleep
bool            oledSleeping = false;

#define REMOTE_TX_TIMEOUT_MS  600

// ============================================================
//  JITTER BUFFER
// ============================================================
void jbuf_push(const uint8_t* data, int len) {
    if (len != FRAME_BYTES) return;
    portENTER_CRITICAL(&jbuf_mux);
    if (jbuf_count >= JITTER_BUF_FRAMES_MAX) {
        jbuf_read = (jbuf_read + 1) % JITTER_BUF_FRAMES_MAX;
        jbuf_count--;
    }
    memcpy(&jbuf[jbuf_write * FRAME_BYTES], data, FRAME_BYTES);
    jbuf_write = (jbuf_write + 1) % JITTER_BUF_FRAMES_MAX;
    jbuf_count++;
    portEXIT_CRITICAL(&jbuf_mux);
}

bool jbuf_pop(uint8_t* dst) {
    portENTER_CRITICAL(&jbuf_mux);
    if (jbuf_count == 0) { portEXIT_CRITICAL(&jbuf_mux); return false; }
    memcpy(dst, &jbuf[jbuf_read * FRAME_BYTES], FRAME_BYTES);
    jbuf_read = (jbuf_read + 1) % JITTER_BUF_FRAMES_MAX;
    jbuf_count--;
    portEXIT_CRITICAL(&jbuf_mux);
    return true;
}

int jbuf_avail() {
    portENTER_CRITICAL(&jbuf_mux);
    int n = jbuf_count;
    portEXIT_CRITICAL(&jbuf_mux);
    return n;
}

// ============================================================
//  UDP HELPERS
// ============================================================
static uint8_t pktBuf[HEADER_SIZE + FRAME_BYTES + 4];

void udpSend(uint8_t type, const uint8_t* payload, int paylen) {
    pktBuf[0] = MAGIC_0; pktBuf[1] = MAGIC_1;
    pktBuf[2] = type;    pktBuf[3] = myId;
    pktBuf[4] = (txSeq >> 8) & 0xFF;
    pktBuf[5] =  txSeq       & 0xFF;
    txSeq++;
    int total = HEADER_SIZE;
    if (payload && paylen > 0) {
        memcpy(pktBuf + HEADER_SIZE, payload, paylen);
        total += paylen;
    }
    udp.beginPacket(SERVER_HOST, SERVER_PORT);
    udp.write(pktBuf, total);
    udp.endPacket();
}

// HELLO sends the node name as payload
void sendHello() {
    static uint8_t helloBuf[HEADER_SIZE + 16];
    const char* name    = NODE_NAME;
    int         nameLen = strlen(name);
    if (nameLen > 16) nameLen = 16;

    helloBuf[0] = MAGIC_0;   helloBuf[1] = MAGIC_1;
    helloBuf[2] = TYPE_HELLO;
    helloBuf[3] = myId;
    helloBuf[4] = (txSeq >> 8) & 0xFF;
    helloBuf[5] =  txSeq       & 0xFF;
    txSeq++;
    memcpy(helloBuf + HEADER_SIZE, name, nameLen);

    udp.beginPacket(SERVER_HOST, SERVER_PORT);
    udp.write(helloBuf, HEADER_SIZE + nameLen);
    udp.endPacket();
    Serial.printf("[UDP] HELLO '%s' (id=%d)\n", name, myId);
}

// ============================================================
//  OLED SLEEP / WAKE
// ============================================================
void oledSleep() {
    if (oledSleeping) return;
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
    oledSleeping = true;
    Serial.println("[OLED] Sleep");
}

void oledWake() {
    if (!oledSleeping) return;
    oled.ssd1306_command(SSD1306_DISPLAYON);
    oledSleeping = false;
    lastOled     = 0;   // force immediate redraw
    Serial.println("[OLED] Wake");
}

// ============================================================
//  WS2812 LED INDICATOR
//  RED solid    = transmitting (PTT active)
//  GREEN solid  = receiving audio
//  YELLOW blink = waiting for approval (1 Hz)
//  RED blink    = rejected / blocked (2 Hz)
//  OFF          = standby / offline
// ============================================================
void ledUpdate() {
    static unsigned long lastBlink = 0;
    static bool blinkState = false;
    unsigned long now = millis();

    if (regState == REG_ACTIVE && pttActive) {
        led.setPixelColor(0, led.Color(255, 0, 0));       // solid red

    } else if (regState == REG_ACTIVE && remoteTx) {
        led.setPixelColor(0, led.Color(0, 255, 0));       // solid green

    } else if (regState == REG_PENDING) {
        if (now - lastBlink > 500) { lastBlink = now; blinkState = !blinkState; }
        led.setPixelColor(0, blinkState ? led.Color(200, 120, 0) : led.Color(0, 0, 0));

    } else if (regState == REG_REJECTED) {
        if (now - lastBlink > 250) { lastBlink = now; blinkState = !blinkState; }
        led.setPixelColor(0, blinkState ? led.Color(255, 0, 0) : led.Color(0, 0, 0));

    } else {
        led.setPixelColor(0, led.Color(0, 0, 0));         // off
    }
    led.show();
}

// ============================================================
//  ROGER BEEP
//  Classic walkie-talkie tone: 1760 Hz, 150 ms.
//  ADSR envelope:
//    Attack   10 ms  - linear ramp up
//    Sustain  80 ms  - full amplitude
//    Decay    60 ms  - quadratic fade out
//  Sent as UDP audio frames (relayed to room) and pushed
//  to the local jitter buffer for immediate local playback.
// ============================================================
void sendRogerBeep() {
    static uint8_t beepBuf[FRAME_BYTES];
    int16_t* pcm = (int16_t*)beepBuf;

    const float FREQ        = 1760.0f;
    const float AMP         = 0.45f;
    const int   ATTACK_SAMP = SAMPLE_RATE * 10  / 1000;   //  10 ms
    const int   TOTAL_SAMP  = SAMPLE_RATE * 150 / 1000;   // 150 ms
    const int   DECAY_SAMP  = SAMPLE_RATE * 60  / 1000;   //  60 ms
    const int   SUSTAIN_END = TOTAL_SAMP - DECAY_SAMP;

    const float STEP = 2.0f * 3.14159265f * FREQ / (float)SAMPLE_RATE;
    float phase = 0.0f;
    int   sent  = 0;

    while (sent < TOTAL_SAMP) {
        int chunk = min(FRAME_SAMPLES, TOTAL_SAMP - sent);
        for (int i = 0; i < chunk; i++) {
            int s = sent + i;
            float env;
            if (s < ATTACK_SAMP) {
                env = (float)s / ATTACK_SAMP;
            } else if (s < SUSTAIN_END) {
                env = 1.0f;
            } else {
                float t = (float)(s - SUSTAIN_END) / DECAY_SAMP;
                env = (1.0f - t) * (1.0f - t);
            }
            pcm[i] = (int16_t)(sinf(phase) * AMP * env * 32767.0f);
            phase += STEP;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        for (int i = chunk; i < FRAME_SAMPLES; i++) pcm[i] = 0;

        udpSend(TYPE_AUDIO, beepBuf, FRAME_BYTES);
        jbuf_push(beepBuf, FRAME_BYTES);
        sent += chunk;
    }

    remoteTx     = true;
    lastRxAudio  = millis();
    lastActivity = millis();
    Serial.println("[PTT] Roger beep");
}

// ============================================================
//  CODEC PINS
// ============================================================
class XminiPins : public DriverPins {
public:
    XminiPins() {
        addI2C(PinFunction::CODEC, PIN_SCL, PIN_SDA);
        addI2S(PinFunction::CODEC, PIN_MCLK, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);
    }
} xmini_pins;

AudioBoard xmini_board(AudioDriverES8311, xmini_pins);

// ============================================================
//  ES8311 direct register write via I2C
// ============================================================
static void es8311_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    Serial.printf("[ES8311] reg 0x%02X = 0x%02X  err=%d\n", reg, val, err);
}

// ============================================================
//  I2S DUPLEX INIT
//  Confirmed working format: 16-bit stereo Philips I2S, MCLK*256
//  ES8311 sends/receives stereo interleaved 16-bit:
//    [L0, R0, L1, R1, ...]
//  Microphone data = L channel (index i*2)
//  Playback: same sample on L and R
// ============================================================
bool i2sInit() {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear    = true;
    cc.dma_desc_num  = 8;
    cc.dma_frame_num = FRAME_SAMPLES;
    if (i2s_new_channel(&cc, &tx_chan, &rx_chan) != ESP_OK) return false;

    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
    cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
    cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                       I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = (gpio_num_t)PIN_MCLK;
    cfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
    cfg.gpio_cfg.ws   = (gpio_num_t)PIN_WS;
    cfg.gpio_cfg.dout = (gpio_num_t)PIN_DOUT;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx_chan, &cfg) != ESP_OK) return false;

    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din  = (gpio_num_t)PIN_DIN;
    if (i2s_channel_init_std_mode(rx_chan, &cfg) != ESP_OK) return false;

    i2s_channel_enable(tx_chan);
    i2s_channel_enable(rx_chan);

    Serial.printf("[I2S] OK %d Hz 16-bit stereo  DMA*8*%d\n", SAMPLE_RATE, FRAME_SAMPLES);
    return true;
}

// ============================================================
//  STARTUP SOUND
// ============================================================
void playTone(uint16_t hz, uint16_t ms) {
    const uint32_t N = (uint32_t)SAMPLE_RATE * ms / 1000;
    const float    f = 2.0f * 3.14159265f * hz / (float)SAMPLE_RATE;
    int16_t buf[FRAME_SAMPLES * 2];
    size_t bw;
    uint32_t pos = 0;
    while (pos < N) {
        uint32_t chunk = min((uint32_t)FRAME_SAMPLES, N - pos);
        for (uint32_t k = 0; k < chunk; k++) {
            int16_t s = (int16_t)(sinf(f * (pos + k)) * 16000.0f);
            buf[k * 2] = buf[k * 2 + 1] = s;
        }
        i2s_channel_write(tx_chan, buf, chunk * 4, &bw, pdMS_TO_TICKS(50));
        pos += chunk;
    }
}

// ============================================================
//  OLED LAYOUT (128x64 px)
//   Row  0-10  Title bar (inverted):  "SA7BNB-1   [wifi]"
//   Row 13-41  Status box:  TX / RX / STANDBY / PENDING / REJECTED
//   Row 44-55  Room line (when active) or connection status
// ============================================================

// WiFi symbol: 3 arcs + dot, 8x8 px, 1=lit
static const uint8_t PROGMEM wifi_bmp[] = {
    0x7E,   // .111111.   outer arc
    0x81,   // 1......1
    0x00,   // gap
    0x3C,   // ..1111..   middle arc
    0x42,   // .1....1.
    0x00,   // gap
    0x18,   // ...11...   inner arc / dot
    0x18    // ...11...
};

void oledUpdate() {
    oled.clearDisplay();

    // -- Title bar (inverted) - shows node name + WiFi icon -------
    oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setTextSize(1);
    oled.setCursor(2, 2);
    oled.print(NODE_NAME);
    if (WiFi.isConnected()) {
        oled.drawBitmap(119, 1, wifi_bmp, 8, 8, SSD1306_BLACK);
    }
    oled.setTextColor(SSD1306_WHITE);

    // -- Status box ------------------------------------------
    if (regState == REG_ACTIVE && pttActive) {
        oled.fillRect(0, 13, 128, 29, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setTextSize(2);
        oled.setCursor(6, 17);
        oled.print("** TX **");
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);

    } else if (regState == REG_ACTIVE && remoteTx) {
        oled.fillRect(0, 13, 128, 29, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setTextSize(1);
        oled.setCursor(4, 16);
        oled.print("** RECEIVING **");
        oled.setCursor(4, 27);
        oled.printf("FROM: %s", myRoomName);
        oled.setTextColor(SSD1306_WHITE);

    } else if (regState == REG_ACTIVE) {
        oled.setCursor(2, 16);
        oled.print("STANDBY");
        oled.setCursor(2, 27);
        oled.printf("Room: %s", myRoomName);

    } else if (regState == REG_PENDING) {
        static uint8_t blinkCnt = 0;
        blinkCnt++;
        if (blinkCnt & 2) oled.fillRect(120, 13, 6, 6, SSD1306_WHITE);
        oled.setCursor(2, 16);
        oled.print("WAITING APPROVAL");
        oled.setCursor(2, 27);
        oled.print("Contact admin");

    } else if (regState == REG_REJECTED) {
        oled.fillRect(0, 13, 128, 29, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setCursor(4, 16);
        oled.print("!! ACCESS DENIED");
        oled.setCursor(4, 27);
        oled.print("   BY SERVER !!");
        oled.setTextColor(SSD1306_WHITE);

    } else {
        // REG_NONE
        oled.setCursor(2, 16);
        oled.print("Connecting...");
        oled.setCursor(2, 27);
        oled.print(WiFi.isConnected() ? "Sending HELLO" : "Searching WiFi");
    }

    // -- IP-Radio label at the bottom ------------------------
    oled.setCursor(2, 56);
    oled.print("IP-Radio v1.0");

    oled.display();
}

// ============================================================
//  TASK: UDP RX
// ============================================================
void taskUdpRx(void*) {
    static uint8_t rxBuf[UDP_RX_BUF_SIZE];
    for (;;) {
        int pkt = udp.parsePacket();
        if (pkt >= HEADER_SIZE) {
            int n = udp.read(rxBuf, sizeof(rxBuf));
            if (n < HEADER_SIZE) goto next;
            if (rxBuf[0] != MAGIC_0 || rxBuf[1] != MAGIC_1) goto next;

            uint8_t type = rxBuf[2];
            uint8_t cid  = rxBuf[3];
            int     pay  = n - HEADER_SIZE;

            // PONG - server approves and assigns ID
            if (type == TYPE_PONG) {
                if (regState != REG_ACTIVE && pay >= 2) {
                    myId     = rxBuf[HEADER_SIZE];        // byte 0: assigned ID
                    myRoomId = rxBuf[HEADER_SIZE + 1];    // byte 1: room ID
                    if (pay >= 3) {
                        int nameLen = pay - 2;
                        if (nameLen > 15) nameLen = 15;
                        memcpy(myRoomName, rxBuf + HEADER_SIZE + 2, nameLen);
                        myRoomName[nameLen] = '\0';
                    }
                    regState = REG_ACTIVE;
                    Serial.printf("[UDP] Registered  id=%d  room=%d '%s'\n",
                                  myId, myRoomId, myRoomName);
                } else if (regState == REG_ACTIVE && pay >= 2) {
                    // Keepalive confirmation - update room if changed
                    if (rxBuf[HEADER_SIZE + 1] != myRoomId) {
                        myRoomId = rxBuf[HEADER_SIZE + 1];
                        if (pay >= 3) {
                            int nameLen = pay - 2;
                            if (nameLen > 15) nameLen = 15;
                            memcpy(myRoomName, rxBuf + HEADER_SIZE + 2, nameLen);
                            myRoomName[nameLen] = '\0';
                        }
                        Serial.printf("[UDP] Room updated: %d '%s'\n",
                                      myRoomId, myRoomName);
                    }
                }

            // REJECT - denied by server
            } else if (type == TYPE_REJECT) {
                uint8_t reason = (pay >= 1) ? rxBuf[HEADER_SIZE] : 0;
                const char* rsn = "";
                switch (reason) {
                    case REJECT_NOT_APPROVED: rsn = "not approved";  break;
                    case REJECT_BLOCKED:      rsn = "blocked";       break;
                    case REJECT_KICKED:       rsn = "kicked";        break;
                    case REJECT_FULL:         rsn = "server full";   break;
                    default:                  rsn = "unknown";       break;
                }
                Serial.printf("[UDP] REJECT: %s (0x%02X)\n", rsn, reason);
                regState   = REG_REJECTED;
                pttActive  = false;
                rejectedAt = millis();

            // ROOM_INFO - server changes room
            } else if (type == TYPE_ROOM_INFO) {
                if (pay >= 1) {
                    myRoomId = rxBuf[HEADER_SIZE];
                    if (pay >= 2) {
                        int nameLen = pay - 1;
                        if (nameLen > 15) nameLen = 15;
                        memcpy(myRoomName, rxBuf + HEADER_SIZE + 1, nameLen);
                        myRoomName[nameLen] = '\0';
                    }
                    Serial.printf("[UDP] Room changed: %d '%s'\n",
                                  myRoomId, myRoomName);
                }

            // AUDIO - incoming audio
            } else if (type == TYPE_AUDIO) {
                if (regState == REG_ACTIVE) {
                    int audioLen = n - HEADER_SIZE;
                    if (audioLen == FRAME_BYTES) {
                        remoteTxId   = cid;
                        remoteTx     = true;
                        lastRxAudio  = millis();
                        lastActivity = millis();
                        jbuf_push(rxBuf + HEADER_SIZE, audioLen);
                    }
                }

            // PING - reply with PONG
            } else if (type == TYPE_PING) {
                udpSend(TYPE_PONG, NULL, 0);
            }
        }
        next:
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ============================================================
//  TASK: AUDIO PLAYBACK  jitter buffer -> I2S
// ============================================================
void taskAudioPlay(void*) {
    static uint8_t  frame16[FRAME_BYTES];
    static int16_t  stereo[FRAME_SAMPLES * 2];
    static int16_t  silence[FRAME_SAMPLES * 2] = {};
    const  size_t   WRITE_BYTES = FRAME_SAMPLES * 4;
    size_t bw;
    bool playing = false;

    for (;;) {
        if (remoteTx && (millis() - lastRxAudio) > REMOTE_TX_TIMEOUT_MS) {
            remoteTx = false;
            playing  = false;
            // Two silent frames to flush DMA without a click
            i2s_channel_write(tx_chan, silence, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
            i2s_channel_write(tx_chan, silence, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
            continue;
        }

        if (!remoteTx) {
            playing = false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Wait until jitter buffer has enough frames before starting playback
        if (!playing && jbuf_avail() < JITTER_FRAMES) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        playing = true;

        if (jbuf_pop(frame16)) {
            int16_t* src = (int16_t*)frame16;
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(src[i] * RX_GAIN);
                stereo[i * 2] = stereo[i * 2 + 1] = s;
            }
            i2s_channel_write(tx_chan, stereo, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
        } else {
            // Buffer underrun - play silence to keep DMA running
            i2s_channel_write(tx_chan, silence, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
        }
    }
}

// ============================================================
//  TASK: AUDIO TX  mic -> server (when PTT and ACTIVE)
//
//  Pipeline:
//  1. Read stereo I2S, extract L channel (microphone)
//  2. DC-blocking high-pass filter (IIR)
//  3. RMS for noise gate (on raw signal, before TX_GAIN)
//  4. Noise gate
//  5. Apply TX_GAIN + hard clip ceiling
//  6. Send mono 16-bit UDP
// ============================================================
void taskAudioTx(void*) {
    static int16_t stereo[FRAME_SAMPLES * 2];
    static uint8_t outBuf[FRAME_BYTES];
    const  size_t  READ_BYTES = FRAME_SAMPLES * 4;
    size_t bytesRead;

    float dcX = 0.0f, dcY = 0.0f;
    const float DC_A = 0.995f;

    for (;;) {
        if (regState != REG_ACTIVE || !pttActive) {
            dcX = dcY = 0.0f;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        esp_err_t err = i2s_channel_read(rx_chan, stereo, READ_BYTES,
                                          &bytesRead, pdMS_TO_TICKS(30));
        if (err != ESP_OK || bytesRead != READ_BYTES) continue;

        static float dcFiltered[FRAME_SAMPLES];
        int64_t sumSq = 0;

        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float x = (float)stereo[i * 2];   // L channel = mic
            float y = x - dcX + DC_A * dcY;
            dcX = x;  dcY = y;
            dcFiltered[i] = y;
            int32_t vi = (int32_t)y;
            sumSq += (int64_t)vi * vi;
        }

        uint32_t rms = (uint32_t)sqrtf((float)(sumSq / FRAME_SAMPLES));
        if (rms < NOISE_GATE_RMS) continue;

        int16_t* outPcm = (int16_t*)outBuf;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float n = (dcFiltered[i] * TX_GAIN) / 32768.0f;
            if (n >  CLIP_CEILING) n =  CLIP_CEILING;
            if (n < -CLIP_CEILING) n = -CLIP_CEILING;
            outPcm[i] = (int16_t)(n * 32767.0f);
        }

        udpSend(TYPE_AUDIO, outBuf, FRAME_BYTES);
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n=== IP-Radio v1.0  [%s] ===\n", NODE_NAME);

    pinMode(PIN_PTT, INPUT_PULLUP);

    // WS2812 LED init - off
    led.begin();
    led.setBrightness(WS2812_BRIGHTNESS);
    led.setPixelColor(0, led.Color(0, 0, 0));
    led.show();

    // Amplifier enable
    gpio_reset_pin((gpio_num_t)PIN_PA);
    gpio_set_direction((gpio_num_t)PIN_PA, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_PA, 1);

    // I2C + OLED
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.clearDisplay();
    oled.setCursor(0, 0); oled.println("IP-Radio v1.0 init...");
    oled.display();

    // Codec init
    if (!xmini_board.begin()) {
        Serial.println("[CODEC] FAIL - board.begin()");
    } else {
        xmini_board.setVolume(SPK_VOLUME);
        es8311_write(0x14, ES8311_REG14_PGA);
        es8311_write(0x17, ES8311_REG17_ADC);
        Serial.printf("[CODEC] OK  PGA=%ddB  spk=%d\n",
                      (ES8311_REG14_PGA & 0x0F) * 3, SPK_VOLUME);
    }

    // I2S
    delay(50);
    if (!i2sInit()) {
        Serial.println("[I2S] FAIL - halting");
        oled.setCursor(0, 20); oled.println("I2S FAIL"); oled.display();
        while (1) delay(1000);
    }

    // Startup tones confirm I2S + DAC working
    playTone(880, 80); delay(40);
    playTone(1320, 120);

    // WiFi
    oled.setCursor(0, 20); oled.println("WiFi..."); oled.display();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print(".");
    }
    Serial.println();
    if (!WiFi.isConnected()) {
        Serial.println("[WiFi] FAIL");
        oled.setCursor(0, 30); oled.println("WiFi FAIL"); oled.display();
        while (1) delay(1000);
    }
    Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    udp.begin(SERVER_PORT);

    // Start tasks
    xTaskCreatePinnedToCore(taskUdpRx,    "udpRx",   3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(taskAudioPlay,"audPlay",  3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(taskAudioTx,  "audTx",   3072, NULL, 4, NULL, 0);

    oledUpdate();
    lastActivity = millis();
    Serial.println("[SYS] Ready! BOOT = PTT");}

// ============================================================
//  MAIN LOOP  -  keepalive + PTT + OLED + LED
// ============================================================
void loop() {
    unsigned long now = millis();

    // Handle REJECTED state - wait, then retry
    if (regState == REG_REJECTED) {
        if ((now - rejectedAt) >= REJECT_RETRY_MS) {
            Serial.println("[UDP] Reject timeout - retrying");
            regState  = REG_NONE;
            myId      = 0;
            lastHello = 0;   // trigger HELLO immediately
        }
        if (now - lastOled > 500) {
            lastOled = now;
            oledUpdate();
        }
        ledUpdate();
        delay(10);
        return;
    }

    // HELLO / keepalive
    if (WiFi.isConnected() && (now - lastHello > HELLO_INTERVAL_MS)) {
        lastHello = now;
        sendHello();
        if (regState == REG_NONE) regState = REG_PENDING;
    }

    // PTT button
    bool btn = (digitalRead(PIN_PTT) == LOW);
    if (btn && !pttActive && regState == REG_ACTIVE) {
        pttActive    = true;
        lastActivity = now;
        if (oledSleeping) oledWake();
        Serial.println("[PTT] ON");
    } else if (!btn && pttActive) {
        pttActive = false;
        Serial.println("[PTT] OFF");
        sendRogerBeep();
    }

    // OLED sleep / wake
    if (!oledSleeping && (now - lastActivity) > OLED_SLEEP_MS
        && regState == REG_ACTIVE) {
        oledSleep();
    }
    if (oledSleeping && (pttActive || remoteTx)) {
        oledWake();
    }

    // OLED update
    if (!oledSleeping) {
        uint16_t interval = (regState == REG_PENDING) ? 500 : 150;
        if (now - lastOled > interval) {
            lastOled = now;
            oledUpdate();
        }
    }

    // WS2812 LED
    ledUpdate();

    delay(10);
}
