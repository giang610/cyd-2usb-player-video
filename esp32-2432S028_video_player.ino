// ESP32 CYD 2USB - MJAV Video Player (Video + Audio sync)
// Board: ESP32 Dev Module

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include "MjpegClass.h"
#include "SD.h"
#include "driver/i2s.h"

// ── Display pins ─────────────────────────────────────────
#define BL_PIN    21
#define SD_CS     5
#define SD_MISO   19
#define SD_MOSI   23
#define SD_SCK    18

// ── Touch pins ───────────────────────────────────────────
#define TOUCH_CS  33
#define TOUCH_IRQ 36

// ── Audio (GPIO26 = DAC built-in, 8002A amp) ─────────────
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_PIN     -1      // DAC mode: không dùng BCK
#define I2S_WS_PIN      -1      // DAC mode: không dùng WS
#define I2S_DATA_PIN    26      // DAC output → 8002A amp → loa
#define AUDIO_BUF_SIZE  512     // bytes per audio write

// ── Misc ─────────────────────────────────────────────────
#define BOOT_PIN              0
#define BOOT_DEBOUNCE_MS      400
#define DISPLAY_SPI_SPEED     80000000L
#define SD_SPI_SPEED          80000000L

const char *MJAV_FOLDER = "/mjpeg";

// ── MJAV file format ─────────────────────────────────────
// Header: magic(4) + total_frames(4) + fps(4) + sample_rate(4) + channels(4) = 20 bytes
// Per frame: jpeg_size(4) + jpeg_data(N) + audio_size(4) + audio_data(N)
#define MJAV_MAGIC "MJAV"

// ── File list ────────────────────────────────────────────
#define MAX_FILES 20
String   fileList[MAX_FILES];
uint32_t fileSizes[MAX_FILES] = {0};
int      fileCount        = 0;
int      currentFileIndex = 0;

// ── MJPEG ────────────────────────────────────────────────
MjpegClass    mjpeg;
int           total_frames;
unsigned long total_read, total_decode, total_show;
unsigned long start_ms, curr_ms;
uint8_t      *mjpeg_buf;
uint16_t     *output_buf;

// ── Display ──────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(2, 15, 14, 13, 12);
Arduino_GFX     *gfx = new Arduino_ILI9341(bus);

// ── Touch ────────────────────────────────────────────────
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// ── SD ───────────────────────────────────────────────────
SPIClass sd_spi(VSPI);

// ── Skip flag ────────────────────────────────────────────
volatile bool     skipRequested = false;
volatile uint32_t isrTick       = 0;
uint32_t          lastPress     = 0;

void IRAM_ATTR onButtonPress() {
    skipRequested = true;
    isrTick = xTaskGetTickCountFromISR();
}

// ── Colors & Layout ──────────────────────────────────────
#define C_BG      0x0000
#define C_HEADER  0x0318
#define C_UP      0x1082
#define C_MID     0x02B0
#define C_DN      0x1082
#define C_TEXT    0xFFFF
#define C_DIM     0x8410
#define C_ACCENT  0x07E0

#define SCR_W      240
#define SCR_H      320
#define HEADER_H   40
#define ZONE_UP_Y  HEADER_H
#define ZONE_UP_H  80
#define ZONE_MID_Y (HEADER_H + ZONE_UP_H)
#define ZONE_MID_H 120
#define ZONE_DN_Y  (ZONE_MID_Y + ZONE_MID_H)
#define ZONE_DN_H  (SCR_H - ZONE_DN_Y)

// ═══════════════════════════════════════════════════════════
// AUDIO SETUP (I2S DAC mode)
// ═══════════════════════════════════════════════════════════
void audioInit(uint32_t sample_rate, uint32_t channels) {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate          = sample_rate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = (channels == 2) ? I2S_CHANNEL_FMT_RIGHT_LEFT
                                                 : I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };
    i2s_driver_install(I2S_NUM, &cfg, 0, NULL);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN); // GPIO26
    i2s_zero_dma_buffer(I2S_NUM);
}

void audioStop() {
    i2s_zero_dma_buffer(I2S_NUM);
    i2s_driver_uninstall(I2S_NUM);
}

// Chuyển PCM signed 16-bit → DAC unsigned 8-bit (I2S DAC dùng upper 8 bits)
void audioWrite(const uint8_t *pcm_buf, size_t len) {
    // Tạo buffer 16-bit với giá trị DAC (unsigned, offset +32768)
    static uint16_t dac_buf[AUDIO_BUF_SIZE / 2];
    size_t samples = len / 2;
    const int16_t *src = (const int16_t *)pcm_buf;
    for (size_t i = 0; i < samples && i < AUDIO_BUF_SIZE / 2; i++) {
        // DAC: 0-255 mapped to 16-bit I2S upper byte
        uint16_t val = (uint16_t)((int32_t)src[i] + 32768) >> 8; // 0-255
        dac_buf[i] = val << 8; // upper byte for DAC
    }
    size_t written;
    i2s_write(I2S_NUM, dac_buf, samples * 2, &written, portMAX_DELAY);
}

// ═══════════════════════════════════════════════════════════
// TOUCH HELPER
// ═══════════════════════════════════════════════════════════
void getTouchPoint(int &tx, int &ty) {
    TS_Point p = touch.getPoint();
    tx = map(p.x, 3800, 200, 0, SCR_W);
    ty = map(p.y, 3800, 200, 0, SCR_H);
    tx = constrain(tx, 0, SCR_W - 1);
    ty = constrain(ty, 0, SCR_H - 1);
}

// ═══════════════════════════════════════════════════════════
// MENU DRAW
// ═══════════════════════════════════════════════════════════
void drawHeader() {
    gfx->fillRect(0, 0, SCR_W, HEADER_H, C_HEADER);
    gfx->setTextColor(C_TEXT);
    gfx->setTextSize(2);
    gfx->setCursor(10, 12);
    gfx->print(">> Video Player");
}

void drawUpZone(bool pressed = false) {
    gfx->fillRect(0, ZONE_UP_Y, SCR_W, ZONE_UP_H, pressed ? C_ACCENT : C_UP);
    gfx->fillRect(0, ZONE_UP_Y, 4, ZONE_UP_H, C_ACCENT);
    int cx = SCR_W / 2, cy = ZONE_UP_Y + ZONE_UP_H / 2;
    for (int i = 0; i < 20; i++)
        gfx->drawFastHLine(cx - i, cy + i - 10, i * 2 + 1, C_TEXT);
    gfx->setTextColor(C_DIM); gfx->setTextSize(1);
    gfx->setCursor(cx - 10, cy + 16); gfx->print("PREV");
}

void drawMidZone(bool pressed = false) {
    gfx->fillRect(0, ZONE_MID_Y, SCR_W, ZONE_MID_H, pressed ? 0x0460 : C_MID);
    gfx->fillRect(0, ZONE_MID_Y, 6, ZONE_MID_H, C_ACCENT);

    char idx[16];
    snprintf(idx, sizeof(idx), "%d / %d", currentFileIndex + 1, fileCount);
    gfx->setTextColor(C_DIM); gfx->setTextSize(1);
    gfx->setCursor(SCR_W - 50, ZONE_MID_Y + 8); gfx->print(idx);

    String name = fileList[currentFileIndex];
    if (name.endsWith(".mjav")) name = name.substring(0, name.length() - 5);
    if (name.length() > 18) name = name.substring(0, 17) + "~";
    gfx->setTextColor(C_TEXT); gfx->setTextSize(2);
    int nameX = max(10, (SCR_W - (int)name.length() * 12) / 2);
    gfx->setCursor(nameX, ZONE_MID_Y + 20); gfx->print(name);

    gfx->setTextColor(C_DIM); gfx->setTextSize(1);
    gfx->setCursor(16, ZONE_MID_Y + 46); gfx->print(formatBytes(fileSizes[currentFileIndex]));

    // PLAY button
    int btnX = SCR_W / 2 - 36, btnY = ZONE_MID_Y + 68;
    gfx->fillRoundRect(btnX, btnY, 72, 34, 8, pressed ? 0x0460 : C_ACCENT);
    gfx->setTextColor(C_BG); gfx->setTextSize(2);
    gfx->setCursor(btnX + 16, btnY + 9); gfx->print("PLAY");
}

void drawDnZone(bool pressed = false) {
    gfx->fillRect(0, ZONE_DN_Y, SCR_W, ZONE_DN_H, pressed ? C_ACCENT : C_DN);
    gfx->fillRect(0, ZONE_DN_Y, 4, ZONE_DN_H, C_ACCENT);
    int cx = SCR_W / 2, cy = ZONE_DN_Y + ZONE_DN_H / 2;
    for (int i = 0; i < 20; i++)
        gfx->drawFastHLine(cx - i, cy - i + 10, i * 2 + 1, C_TEXT);
    gfx->setTextColor(C_DIM); gfx->setTextSize(1);
    gfx->setCursor(cx - 10, cy - 26); gfx->print("NEXT");
}

void drawMenu() {
    gfx->fillScreen(C_BG);
    drawHeader(); drawUpZone(); drawMidZone(); drawDnZone();
}

// ═══════════════════════════════════════════════════════════
// MENU LOOP
// ═══════════════════════════════════════════════════════════
void showMenu() {
    drawMenu();
    uint32_t lastTouchTime = 0;
    while (true) {
        if (touch.tirqTouched() && touch.touched()) {
            uint32_t now = millis();
            if (now - lastTouchTime < 300) { delay(10); continue; }
            lastTouchTime = now;
            int tx, ty; getTouchPoint(tx, ty);

            if (ty >= ZONE_UP_Y && ty < ZONE_MID_Y) {
                drawUpZone(true); delay(120);
                currentFileIndex = (currentFileIndex - 1 + fileCount) % fileCount;
                drawUpZone(false); drawMidZone();
            } else if (ty >= ZONE_MID_Y && ty < ZONE_DN_Y) {
                drawMidZone(true); delay(150);
                return;
            } else if (ty >= ZONE_DN_Y) {
                drawDnZone(true); delay(120);
                currentFileIndex = (currentFileIndex + 1) % fileCount;
                drawDnZone(false); drawMidZone();
            }
        }
        if (digitalRead(BOOT_PIN) == LOW) {
            delay(50);
            if (digitalRead(BOOT_PIN) == LOW) {
                while (digitalRead(BOOT_PIN) == LOW) delay(10);
                return;
            }
        }
        delay(10);
    }
}

// ═══════════════════════════════════════════════════════════
// JPEG DRAW CALLBACK
// ═══════════════════════════════════════════════════════════
int jpegDrawCallback(JPEGDRAW *pDraw) {
    unsigned long s = millis();
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    total_show += millis() - s;
    return 1;
}

// ═══════════════════════════════════════════════════════════
// PLAY MJAV FILE
// ═══════════════════════════════════════════════════════════
void playMjav(const char *path) {
    File f = SD.open(path, "r");
    if (!f || f.isDirectory()) {
        Serial.printf("ERROR: Cannot open %s\n", path);
        return;
    }

    // Đọc header
    char magic[5] = {0};
    f.read((uint8_t *)magic, 4);
    if (strcmp(magic, MJAV_MAGIC) != 0) {
        Serial.println("ERROR: Not a valid .mjav file");
        f.close(); return;
    }

    uint32_t total_frames_file, fps, sample_rate, channels;
    f.read((uint8_t *)&total_frames_file, 4);
    f.read((uint8_t *)&fps,               4);
    f.read((uint8_t *)&sample_rate,       4);
    f.read((uint8_t *)&channels,          4);

    Serial.printf("MJAV: %d frames @ %d FPS, audio %dHz %dch\n",
                  total_frames_file, fps, sample_rate, channels);

    uint32_t frame_interval_ms = 1000 / fps;

    // Init audio
    audioInit(sample_rate, channels);

    gfx->fillScreen(0x0000);
    start_ms = curr_ms = millis();
    total_frames = total_read = total_decode = total_show = 0;
    skipRequested = false;

    static uint8_t  audio_buf[4096];
    static uint32_t jpeg_size, audio_size;

    while (!skipRequested && f.available()) {
        uint32_t frame_start = millis();

        // Đọc jpeg_size
        if (f.read((uint8_t *)&jpeg_size, 4) != 4) break;
        if (jpeg_size == 0 || jpeg_size > (uint32_t)(gfx->width() * gfx->height() * 2)) break;

        // Đọc JPEG data vào mjpeg_buf
        size_t got = f.read(mjpeg_buf, jpeg_size);
        if (got != jpeg_size) break;
        total_read += millis() - curr_ms;
        curr_ms = millis();

        // Decode và hiển thị JPEG
        JPEGDEC jpeg_dec;
        jpeg_dec.openRAM(mjpeg_buf, jpeg_size, jpegDrawCallback);
        jpeg_dec.setPixelType(RGB565_BIG_ENDIAN);
        jpeg_dec.decode(0, 0, 0);
        jpeg_dec.close();
        total_decode += millis() - curr_ms;
        curr_ms = millis();

        // Đọc audio_size
        if (f.read((uint8_t *)&audio_size, 4) != 4) break;

        // Đọc và phát audio theo chunk
        uint32_t remaining = audio_size;
        while (remaining > 0) {
            uint32_t chunk = min(remaining, (uint32_t)sizeof(audio_buf));
            size_t   n     = f.read(audio_buf, chunk);
            if (n == 0) break;
            audioWrite(audio_buf, n);
            remaining -= n;
        }

        // Chạm màn hình = dừng
        if (touch.tirqTouched() && touch.touched()) {
            skipRequested = true;
        }

        // Timing
        uint32_t elapsed = millis() - frame_start;
        if (elapsed < frame_interval_ms) delay(frame_interval_ms - elapsed);

        total_frames++;
    }

    audioStop();
    f.close();
    skipRequested = false;

    int time_used = millis() - start_ms;
    Serial.printf("Done: %d frames, %.1f FPS\n",
                  total_frames, 1000.0f * total_frames / time_used);
}

// ═══════════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);

    if (!gfx->begin(DISPLAY_SPI_SPEED)) {
        Serial.println("Display init failed!"); while (true) {}
    }
    gfx->setRotation(0);
    gfx->fillScreen(0x0000);
    gfx->invertDisplay(true);

    touchSPI.begin(25, 39, 32, TOUCH_CS);
    touch.begin(touchSPI);
    touch.setRotation(0);

    if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd")) {
        Serial.println("SD mount failed!"); while (true) {}
    }

    // Buffer
    output_buf = (uint16_t *)heap_caps_aligned_alloc(16,
        SCR_W * 4 * 2 * sizeof(uint16_t), MALLOC_CAP_DMA);
    mjpeg_buf = (uint8_t *)heap_caps_malloc(
        SCR_W * SCR_H * 2 / 5, MALLOC_CAP_8BIT);
    if (!output_buf || !mjpeg_buf) {
        Serial.println("Buffer alloc failed!"); while (true) {}
    }

    loadFileList();

    pinMode(BOOT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(BOOT_PIN), onButtonPress, FALLING);
}

void loop() {
    showMenu();

    String fullPath = String(MJAV_FOLDER) + "/" + fileList[currentFileIndex];
    char path[128];
    fullPath.toCharArray(path, sizeof(path));
    playMjav(path);
}

// ═══════════════════════════════════════════════════════════
// FILE LIST
// ═══════════════════════════════════════════════════════════
void loadFileList() {
    File dir = SD.open(MJAV_FOLDER);
    if (!dir) { Serial.printf("Cannot open %s\n", MJAV_FOLDER); while (true) {} }
    fileCount = 0;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".mjav")) {
                fileList[fileCount]  = name;
                fileSizes[fileCount] = file.size();
                fileCount++;
                if (fileCount >= MAX_FILES) break;
            }
        }
        file.close();
    }
    dir.close();
    Serial.printf("%d .mjav files found\n", fileCount);
}

String formatBytes(size_t bytes) {
    if (bytes < 1024)             return String(bytes) + " B";
    else if (bytes < 1024 * 1024) return String(bytes / 1024.0, 1) + " KB";
    else                          return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}
