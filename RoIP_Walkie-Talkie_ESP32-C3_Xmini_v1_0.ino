/**
 * ============================================================
 *  RoIP Walkie-Talkie  –  ESP32-C3 Xmini  v1.0
 * ============================================================
 *  Simple UDP-only walkie-talkie, optimized for low latency.
 *  No TCP, no JSON, no passwords.
 *
 *  Protocol (6-byte header + PCM):
 *    [0-1]  Magic    0xA5 0x7B
 *    [2]    Type     0x01=HELLO 0x02=AUDIO 0x03=BYE 0x04=PING 0x05=PONG
 *    [3]    ClientID (assigned by server)
 *    [4-5]  SeqNum   uint16 BE
 *    [6+]   PCM 16-bit mono 16 kHz
 *
 *  BOOT button (GPIO9) = PTT
 *
 * ============================================================
 *  ARDUINO IDE SETTINGS
 * ============================================================
 *  Board:            ESP32C3 Dev Module
 *  USB CDC On Boot:  Disabled
 *  CPU Frequency:    160MHz
 *  Flash Mode:       DIO
 *  Partition Scheme: Default 4MB with spiffs
 *  Upload Speed:     921600
 *
 * ============================================================
 *  LIBRARIES
 * ============================================================
 *  arduino-audio-driver  (github.com/pschatzmann/arduino-audio-driver)
 *  Adafruit SSD1306 + Adafruit GFX
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
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <math.h>

// ============================================================
//  CONFIGURATION  –  edit these
// ============================================================
#define WIFI_SSID        "SSID"
#define WIFI_PASSWORD    "PASSWORD"
#define SERVER_HOST      "192.168.1.24"
#define SERVER_PORT      12345

// ── ES8311 MIC PGA (register 0x14, bits[3:0]) ───────────────
// Based on ES8311 User Guide: PGAGAIN = bits[3:0], step 3dB
// Formula: REG14_VALUE = 0x10 | (dB/3)
//   0x10 =  0 dB   (default, very weak)
//   0x12 =  6 dB
//   0x14 = 12 dB
//   0x16 = 18 dB   ← good starting point for built-in mic
//   0x18 = 24 dB
//   0x1A = 30 dB   (max analog PGA)
// NOTE: setInputVolume() in arduino-audio-driver is buggy for ES8311.
//       Writing register 0x14 directly via I2C instead.
#define ES8311_REG14_PGA   0x12   // 6 dB PGA – lower analog gain = cleaner signal, TX_GAIN compensates

// ── ES8311 ADC digital volume (register 0x17) ─────────────────
// 0xBF = 0 dB (full, no attenuation)
// 0x7F ≈ -6 dB
// 0x3F ≈ -12 dB
#define ES8311_REG17_ADC   0xBF   // full digital gain, controlled via TX_GAIN

// ── Software TX volume (0.0–1.0) ───────────────────────────────
// Applied to the mic signal BEFORE transmission. 1.0 = no attenuation.
// Lower if signal still clips despite reasonable PGA.
#define TX_GAIN   0.85f   // slightly below full to give headroom at the ceiling

// ── Software RX volume (0.0–1.0) ───────────────────────────────
// Scales incoming PCM during playback.
// Reduces the risk of NS4150B amplifier clipping.
#define RX_GAIN   1.0f

// ── Playback volume (0–100 via codec DAC) ──────────────────
// NS4150B is a 3W amp. Keep below 70 to avoid clipping.
#define SPK_VOLUME  82

// ── Noise gate – RMS threshold (0–32767, measured on RAW signal) ───
// 100 = blocks quiet noise but passes speech.
// Raise to 200–300 if background noise is being transmitted. Lower if voice is cut off.
#define NOISE_GATE_RMS   0     // Disabled – PTT controls gating, no VOX effect

// ── Hard ceiling (safety clip) ───────────────────────────────
// No compression – just a ceiling to protect against extreme peaks.
// Lower (e.g. 0.80) if clipping still occurs.
#define CLIP_CEILING  0.93f

// ============================================================
//  PINS  (verified against Xmini-C3 original firmware)
// ============================================================
#define PIN_SDA    3
#define PIN_SCL    4
#define PIN_MCLK   10
#define PIN_BCLK   8
#define PIN_WS     6
#define PIN_DOUT   5
#define PIN_DIN    7
#define PIN_PA     11     // NS4150B enable, active HIGH
#define PIN_PTT    9      // BOOT button, active LOW
#define OLED_ADDR  0x3C
#define ES8311_ADDR 0x18  // ES8311 I2C address

// ============================================================
//  AUDIO  – 16 kHz mono PCM, 20 ms frames
//  ES8311 confirmed stable with 16-bit STEREO Philips I2S.
//  16 kHz = good voice quality, reasonable bandwidth (32 KB/s raw PCM).
//  FRAME_BYTES = network payload (mono 16-bit).
//  I2S handled as stereo interleaved → see i2sInit().
// ============================================================
#define SAMPLE_RATE      16000
#define FRAME_MS         20
#define FRAME_SAMPLES    (SAMPLE_RATE * FRAME_MS / 1000)   // 320 samples
#define FRAME_BYTES      (FRAME_SAMPLES * 2)               // 640 bytes PCM mono

// Jitter buffer
#define JITTER_FRAMES    5   // 5×20ms = 100ms buffer – reduces WiFi jitter artifacts
#define JITTER_BUF_FRAMES_MAX  8

// ============================================================
//  PROTOCOL
// ============================================================
#define MAGIC_0       0xA5
#define MAGIC_1       0x7B
#define TYPE_HELLO    0x01
#define TYPE_AUDIO    0x02
#define TYPE_BYE      0x03
#define TYPE_PING     0x04
#define TYPE_PONG     0x05
#define HEADER_SIZE   6

#define HELLO_INTERVAL_MS   5000
#define UDP_RX_BUF_SIZE     (HEADER_SIZE + FRAME_BYTES + 16)

// ============================================================
//  GLOBALS
// ============================================================
static i2s_chan_handle_t tx_chan = NULL;
static i2s_chan_handle_t rx_chan = NULL;

WiFiUDP          udp;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

volatile uint8_t  myId       = 0;
volatile bool     registered = false;
volatile bool     pttActive  = false;
volatile bool     remoteTx   = false;
volatile uint8_t  remoteTxId = 0;

// Jitter buffer
static uint8_t  jbuf[JITTER_BUF_FRAMES_MAX * FRAME_BYTES];
static volatile int jbuf_write = 0;
static volatile int jbuf_read  = 0;
static volatile int jbuf_count = 0;
static portMUX_TYPE jbuf_mux   = portMUX_INITIALIZER_UNLOCKED;

static uint16_t txSeq      = 0;
unsigned long   lastHello  = 0;
unsigned long   lastOled   = 0;
unsigned long   lastRxAudio = 0;   // timeout for remoteTx flag
#define REMOTE_TX_TIMEOUT_MS  600   // clear remoteTx if no audio for 600 ms

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
//  ES8311 direct write via I2C
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
//
//  CONFIRMED WORKING FORMAT (from audio_test15):
//    16-bit STEREO Philips I2S, MCLK×256
//
//  ES8311 sends/receives stereo interleaved 16-bit:
//    [L0, R0, L1, R1, ...]  each value is int16_t
//  Microphone data = L channel (index i*2)
//  Playback: same value on L and R
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

    // TX
    cfg.gpio_cfg.mclk = (gpio_num_t)PIN_MCLK;
    cfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
    cfg.gpio_cfg.ws   = (gpio_num_t)PIN_WS;
    cfg.gpio_cfg.dout = (gpio_num_t)PIN_DOUT;
    cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx_chan, &cfg) != ESP_OK) return false;

    // RX
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din  = (gpio_num_t)PIN_DIN;
    if (i2s_channel_init_std_mode(rx_chan, &cfg) != ESP_OK) return false;

    i2s_channel_enable(tx_chan);
    i2s_channel_enable(rx_chan);

    Serial.printf("[I2S] OK %d Hz 16bit stereo  DMA×8×%d\n", SAMPLE_RATE, FRAME_SAMPLES);
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
//  OLED
// ============================================================
void oledUpdate() {
    oled.clearDisplay();
    oled.fillRect(0, 0, 128, 10, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(2, 1);
    oled.printf("RoIP v1.0 ID:%d", myId);
    if (WiFi.isConnected()) { oled.setCursor(110, 1); oled.print("W"); }
    oled.setTextColor(SSD1306_WHITE);

    if (pttActive) {
        oled.fillRect(0, 14, 128, 28, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setTextSize(2);
        oled.setCursor(6, 18);
        oled.print("** TX **");
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
    } else if (remoteTx) {
        oled.fillRect(0, 14, 128, 28, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setCursor(4, 17); oled.printf("RX  <-- ID:%d", remoteTxId);
        oled.setCursor(4, 28); oled.print("RECEIVING AUDIO");
        oled.setTextColor(SSD1306_WHITE);
    } else {
        oled.setCursor(0, 20);
        if (registered) {
            oled.print("STANDBY");
            oled.setCursor(0, 32); oled.print("BOOT = Talk");
        } else {
            oled.print("Connecting...");
        }
    }

    oled.drawLine(0, 54, 127, 54, SSD1306_WHITE);
    oled.setCursor(0, 56);
    oled.printf("jbuf:%d pga:%ddB", jbuf_avail(), (ES8311_REG14_PGA & 0x0F) * 3);
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

            if (type == TYPE_PONG) {
                if (!registered && n >= HEADER_SIZE + 1) {
                    myId       = rxBuf[HEADER_SIZE];
                    registered = true;
                    Serial.printf("[UDP] Registered, ID=%d\n", myId);
                }
            } else if (type == TYPE_AUDIO) {
                int audioLen = n - HEADER_SIZE;
                if (audioLen == FRAME_BYTES) {
                    remoteTxId  = cid;
                    remoteTx    = true;
                    lastRxAudio = millis();   // update timeout
                    jbuf_push(rxBuf + HEADER_SIZE, audioLen);
                }
            } else if (type == TYPE_PING) {
                udpSend(TYPE_PONG, NULL, 0);
            }
        }
        next:
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ============================================================
//  TASK: AUDIO PLAYBACK  jitter buffer → I2S
//
//  Plays ONE frame per iteration, synchronized with I2S DMA.
//  i2s_channel_write() blocks until DMA can accept data (~20ms)
//  which provides natural 20ms/frame timing without vTaskDelay.
//
//  Previous bug: while(jbuf_pop()) drained the entire buffer in a burst
//  in <1ms → DMA underrun → silence → burst → choppy/stuttering audio.
// ============================================================
void taskAudioPlay(void*) {
    static uint8_t  frame16[FRAME_BYTES];
    static int16_t  stereo[FRAME_SAMPLES * 2];
    static int16_t  silence[FRAME_SAMPLES * 2] = {};
    const  size_t   WRITE_BYTES = FRAME_SAMPLES * 4;
    size_t bw;
    bool playing = false;   // true = we are in an active QSO

    for (;;) {
        // ── Timeout: no packet for 600ms → end playback ──
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

        // ── Wait until buffer has JITTER_FRAMES before starting playback ──
        // Gives the network time to deliver a few frames and reduces
        // the risk of underrun at the start of a QSO.
        if (!playing && jbuf_avail() < JITTER_FRAMES) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        playing = true;

        // ── Play ONE frame – if buffer is empty: play silence ──
        // Silence keeps DMA running and prevents underrun clicks.
        // The next frame should arrive within <20ms (one packet interval).
        if (jbuf_pop(frame16)) {
            int16_t* src = (int16_t*)frame16;
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(src[i] * RX_GAIN);
                stereo[i * 2] = stereo[i * 2 + 1] = s;
            }
            i2s_channel_write(tx_chan, stereo, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
        } else {
            // Buffer underrun – play one frame of silence
            i2s_channel_write(tx_chan, silence, WRITE_BYTES, &bw, pdMS_TO_TICKS(50));
        }
    }
}

// ============================================================
//  TASK: AUDIO TX  mic → server (when PTT active)
//
//  ES8311 provides 16-bit stereo interleaved:
//    stereo[i*2]   = L channel (microphone)
//    stereo[i*2+1] = R channel (ignored)
//
//  Pipeline:
//  1. Read stereo, extract L channel
//  2. DC filter (IIR high-pass)
//  3. RMS calculation for noise gate (on RAW signal, BEFORE TX_GAIN)
//  4. Noise gate
//  5. Apply TX_GAIN + soft limiter
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
        if (!registered || !pttActive) {
            dcX = dcY = 0.0f;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        esp_err_t err = i2s_channel_read(rx_chan, stereo, READ_BYTES,
                                          &bytesRead, pdMS_TO_TICKS(30));
        if (err != ESP_OK || bytesRead != READ_BYTES) continue;

        // ── 1+2. Extract L channel + DC filter ───────────────────
        static float dcFiltered[FRAME_SAMPLES];
        int64_t sumSq = 0;

        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float x = (float)stereo[i * 2];   // L channel = microphone
            float y = x - dcX + DC_A * dcY;
            dcX = x;  dcY = y;
            dcFiltered[i] = y;
            // RMS calculated on raw (pre-gain) signal
            int32_t vi = (int32_t)y;
            sumSq += (int64_t)vi * vi;
        }

        // ── 3+4. Noise gate (on raw signal, BEFORE TX_GAIN) ────────
        // Important: noise gate MUST run on raw signal.
        // Running after TX_GAIN (e.g. 0.6) reduces RMS by 0.6
        // and normal speech may fall below the threshold → silence.
        uint32_t rms = (uint32_t)sqrtf((float)(sumSq / FRAME_SAMPLES));
        if (rms < NOISE_GATE_RMS) continue;

        // ── 5. TX_GAIN + soft limiter ─────────────────────────────
        // No compression – just a safety ceiling for extreme peaks.
        // No soft limiter = no compression artifacts, natural voice.
        int16_t* outPcm = (int16_t*)outBuf;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float n = (dcFiltered[i] * TX_GAIN) / 32768.0f;
            if (n >  CLIP_CEILING) n =  CLIP_CEILING;
            if (n < -CLIP_CEILING) n = -CLIP_CEILING;
            outPcm[i] = (int16_t)(n * 32767.0f);
        }

        // ── 6. Transmit ─────────────────────────────────────────────
        udpSend(TYPE_AUDIO, outBuf, FRAME_BYTES);
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== RoIP Walkie-Talkie v1.0 ===");

    pinMode(PIN_PTT, INPUT_PULLUP);

    // PA enable
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
    oled.setCursor(0, 0); oled.println("RoIP v1.0 init...");
    oled.display();

    // ── Codec init ───────────────────────────────────────────
    if (!xmini_board.begin()) {
        Serial.println("[CODEC] FAIL – board.begin()");
    } else {
        // DAC playback volume. NS4150B clips at high values.
        xmini_board.setVolume(SPK_VOLUME);

        // ── ES8311 PGA (mic gain) via direct I2C register write ──
        // setInputVolume() in arduino-audio-driver is known buggy for ES8311
        // (issue #22 on GitHub). Writing registers directly instead.
        //
        // Register 0x14 (SYSTEM_REG14), bits[3:0] = PGAGAIN
        // bits[5:4] = LINSESL (input select, keep 01 = MIC1P/N)
        // Formula: value = 0x10 | (dB/3)
        //   0x10=0dB  0x12=6dB  0x14=12dB  0x16=18dB  0x18=24dB  0x1A=30dB
        es8311_write(0x14, ES8311_REG14_PGA);

        // Register 0x17 = ADC digital volume
        // 0xBF=0dB (full), 0x7F≈-6dB, 0x3F≈-12dB
        es8311_write(0x17, ES8311_REG17_ADC);

        Serial.printf("[CODEC] OK  PGA=%ddB (0x%02X)  ADCvol=0x%02X  spk=%d\n",
                      (ES8311_REG14_PGA & 0x0F) * 3,
                      ES8311_REG14_PGA, ES8311_REG17_ADC, SPK_VOLUME);
    }

    // ── I2S ──────────────────────────────────────────────────
    delay(50);
    if (!i2sInit()) {
        Serial.println("[I2S] FAIL – halting");
        oled.setCursor(0, 20); oled.println("I2S FAIL"); oled.display();
        while (1) delay(1000);
    }

    // Startup sound (confirms I2S + DAC are working)
    playTone(880, 80); delay(40);
    playTone(1320, 120);

    // ── WiFi ─────────────────────────────────────────────────
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

    // ── Start tasks ─────────────────────────────────────────
    xTaskCreatePinnedToCore(taskUdpRx,    "udpRx",   3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(taskAudioPlay,"audPlay",  3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(taskAudioTx,  "audTx",   3072, NULL, 4, NULL, 0);

    oledUpdate();
    Serial.println("[SYS] Ready! BOOT = PTT");
}

// ============================================================
//  MAIN LOOP – keepalive + PTT + OLED
// ============================================================
void loop() {
    unsigned long now = millis();

    if (WiFi.isConnected() && (now - lastHello > HELLO_INTERVAL_MS)) {
        lastHello = now;
        udpSend(TYPE_HELLO, NULL, 0);
    }

    bool btn = (digitalRead(PIN_PTT) == LOW);
    if (btn && !pttActive && registered) {
        pttActive = true;
        Serial.println("[PTT] ON");
    } else if (!btn && pttActive) {
        pttActive = false;
        Serial.println("[PTT] OFF");
    }

    if (now - lastOled > 150) {
        lastOled = now;
        oledUpdate();
    }

    delay(10);
}
