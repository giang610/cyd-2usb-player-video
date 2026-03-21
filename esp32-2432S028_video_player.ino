// Tutorial : https://youtu.be/jYcxUgxz9ks
// Use board "ESP32 Dev Module" (last tested on v3.2.0)

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include "MjpegClass.h"
#include "SD.h"

// Display pins
#define BL_PIN    21
#define SD_CS     5
#define SD_MISO   19
#define SD_MOSI   23
#define SD_SCK    18

// Touch pins (CYD 2USB)
#define TOUCH_CS  33
#define TOUCH_IRQ 36

#define BOOT_PIN 0
#define BOOT_BUTTON_DEBOUCE_TIME 400

#define DISPLAY_SPI_SPEED 80000000L
#define SD_SPI_SPEED      80000000L

#define PLAYBACK_SPEED    1.4542f
#define TARGET_FPS        24.0f
#define FRAME_INTERVAL_MS ((uint32_t)(1000.0f / (TARGET_FPS * PLAYBACK_SPEED)))

const char *MJPEG_FOLDER = "/mjpeg";

// Colors (Bruce-like)
#define C_BG        0x0000  // Black
#define C_HEADER    0x0318  // Dark green
#define C_UP_ZONE   0x1082  // Dark gray
#define C_MID_ZONE  0x02B0  // Green (play)
#define C_DN_ZONE   0x1082  // Dark gray
#define C_ARROW     0xFFFF  // White
#define C_TEXT      0xFFFF  // White
#define C_DIM       0x8410  // Gray
#define C_ACCENT    0x07E0  // Bright green

// Screen layout (240 wide, 320 tall — portrait)
// Header: 40px
// UP zone: 80px
// MID zone: 120px  ← tên file + PLAY
// DN zone: 80px
#define SCR_W       240
#define SCR_H       320
#define HEADER_H    40
#define ZONE_UP_Y   HEADER_H
#define ZONE_UP_H   80
#define ZONE_MID_Y  (HEADER_H + ZONE_UP_H)
#define ZONE_MID_H  120
#define ZONE_DN_Y   (ZONE_MID_Y + ZONE_MID_H)
#define ZONE_DN_H   (SCR_H - ZONE_DN_Y)

// File list
#define MAX_FILES 20
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int      mjpegCount        = 0;
int      currentMjpegIndex = 0;

// MJPEG globals
MjpegClass    mjpeg;
int           total_frames;
unsigned long total_read_video, total_decode_video, total_show_video;
unsigned long start_ms, curr_ms;
long          output_buf_size, estimateBufferSize;
uint8_t      *mjpeg_buf;
uint16_t     *output_buf;

// Display
Arduino_DataBus *bus = new Arduino_HWSPI(2, 15, 14, 13, 12);
Arduino_GFX     *gfx = new Arduino_ILI9341(bus);

// Touch
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// SD
SPIClass sd_spi(VSPI);

// Skip
volatile bool    skipRequested = false;
volatile uint32_t isrTick      = 0;
uint32_t          lastPress    = 0;

void IRAM_ATTR onButtonPress() {
    skipRequested = true;
    isrTick = xTaskGetTickCountFromISR();
}

// -------------------------------------------------------
// TOUCH HELPER
// -------------------------------------------------------
void getTouchPoint(int &tx, int &ty) {
    TS_Point p = touch.getPoint();
    tx = map(p.x, 3800, 200, 0, SCR_W);
    ty = map(p.y, 3800, 200, 0, SCR_H);
    tx = constrain(tx, 0, SCR_W - 1);
    ty = constrain(ty, 0, SCR_H - 1);
}

// -------------------------------------------------------
// DRAW MENU
// -------------------------------------------------------
void drawHeader() {
    gfx->fillRect(0, 0, SCR_W, HEADER_H, C_HEADER);
    gfx->setTextColor(C_TEXT);
    gfx->setTextSize(2);
    gfx->setCursor(10, 12);
    gfx->print(">> Video Player");
}

void drawUpZone(bool pressed = false) {
    uint16_t bg = pressed ? C_ACCENT : C_UP_ZONE;
    gfx->fillRect(0, ZONE_UP_Y, SCR_W, ZONE_UP_H, bg);
    // Left accent bar
    gfx->fillRect(0, ZONE_UP_Y, 4, ZONE_UP_H, C_ACCENT);
    // UP arrow (triangle)
    int cx = SCR_W / 2;
    int cy = ZONE_UP_Y + ZONE_UP_H / 2;
    for (int i = 0; i < 20; i++) {
        gfx->drawFastHLine(cx - i, cy + i - 10, i * 2 + 1, C_ARROW);
    }
    gfx->setTextColor(C_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(cx - 10, cy + 16);
    gfx->print("PREV");
}

void drawMidZone(bool pressed = false) {
    uint16_t bg = pressed ? 0x0460 : C_MID_ZONE;
    gfx->fillRect(0, ZONE_MID_Y, SCR_W, ZONE_MID_H, bg);
    // Left accent bar
    gfx->fillRect(0, ZONE_MID_Y, 6, ZONE_MID_H, C_ACCENT);

    // File index indicator
    char idx[16];
    snprintf(idx, sizeof(idx), "%d / %d", currentMjpegIndex + 1, mjpegCount);
    gfx->setTextColor(C_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(SCR_W - 50, ZONE_MID_Y + 8);
    gfx->print(idx);

    // File name (truncate, centered)
    String name = mjpegFileList[currentMjpegIndex];
    if (name.endsWith(".mjpeg")) name = name.substring(0, name.length() - 6);
    if (name.length() > 18) name = name.substring(0, 17) + "~";
    gfx->setTextColor(C_TEXT);
    gfx->setTextSize(2);
    int nameX = max(10, (SCR_W - (int)name.length() * 12) / 2);
    gfx->setCursor(nameX, ZONE_MID_Y + 20);
    gfx->print(name);

    // File size
    gfx->setTextColor(C_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(16, ZONE_MID_Y + 46);
    gfx->print(formatBytes(mjpegFileSizes[currentMjpegIndex]));

    // PLAY button
    int btnX = SCR_W / 2 - 36;
    int btnY = ZONE_MID_Y + 68;
    gfx->fillRoundRect(btnX, btnY, 72, 34, 8, pressed ? 0x0460 : C_ACCENT);
    gfx->setTextColor(C_BG);
    gfx->setTextSize(2);
    gfx->setCursor(btnX + 16, btnY + 9);
    gfx->print("PLAY");
}

void drawDnZone(bool pressed = false) {
    uint16_t bg = pressed ? C_ACCENT : C_DN_ZONE;
    gfx->fillRect(0, ZONE_DN_Y, SCR_W, ZONE_DN_H, bg);
    // Left accent bar
    gfx->fillRect(0, ZONE_DN_Y, 4, ZONE_DN_H, C_ACCENT);
    // DOWN arrow (triangle)
    int cx = SCR_W / 2;
    int cy = ZONE_DN_Y + ZONE_DN_H / 2;
    for (int i = 0; i < 20; i++) {
        gfx->drawFastHLine(cx - i, cy - i + 10, i * 2 + 1, C_ARROW);
    }
    gfx->setTextColor(C_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(cx - 10, cy - 26);
    gfx->print("NEXT");
}

void drawMenu() {
    gfx->fillScreen(C_BG);
    drawHeader();
    drawUpZone();
    drawMidZone();
    drawDnZone();
}

// -------------------------------------------------------
// MENU LOOP
// -------------------------------------------------------
void showMenu() {
    drawMenu();
    uint32_t lastTouchTime = 0;

    while (true) {
        if (touch.tirqTouched() && touch.touched()) {
            uint32_t now = millis();
            if (now - lastTouchTime < 300) continue; // debounce
            lastTouchTime = now;

            int tx, ty;
            getTouchPoint(tx, ty);

            if (ty >= ZONE_UP_Y && ty < ZONE_MID_Y) {
                // UP zone — previous
                drawUpZone(true);
                delay(120);
                currentMjpegIndex--;
                if (currentMjpegIndex < 0) currentMjpegIndex = mjpegCount - 1;
                drawUpZone(false);
                drawMidZone();

            } else if (ty >= ZONE_MID_Y && ty < ZONE_DN_Y) {
                // MID zone — play
                drawMidZone(true);
                delay(150);
                return; // exit menu → play

            } else if (ty >= ZONE_DN_Y) {
                // DOWN zone — next
                drawDnZone(true);
                delay(120);
                currentMjpegIndex++;
                if (currentMjpegIndex >= mjpegCount) currentMjpegIndex = 0;
                drawDnZone(false);
                drawMidZone();
            }
        }

        // Boot button also plays
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

// -------------------------------------------------------
// SETUP & LOOP
// -------------------------------------------------------
void setup() {
    Serial.begin(115200);

    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);

    if (!gfx->begin(DISPLAY_SPI_SPEED)) {
        Serial.println("Display init failed!");
        while (true) {}
    }
    gfx->setRotation(0);
    gfx->fillScreen(C_BG);
    gfx->invertDisplay(true);

    touchSPI.begin(25, 39, 32, TOUCH_CS);
    touch.begin(touchSPI);
    touch.setRotation(2);

    if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd")) {
        Serial.println("SD mount failed!");
        while (true) {}
    }

    output_buf_size = gfx->width() * 4 * 2;
    output_buf = (uint16_t *)heap_caps_aligned_alloc(16, output_buf_size * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!output_buf) { Serial.println("output_buf alloc failed!"); while (true) {} }

    estimateBufferSize = gfx->width() * gfx->height() * 2 / 5;
    mjpeg_buf = (uint8_t *)heap_caps_malloc(estimateBufferSize, MALLOC_CAP_8BIT);
    if (!mjpeg_buf) { Serial.println("mjpeg_buf alloc failed!"); while (true) {} }

    loadMjpegFilesList();

    pinMode(BOOT_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(BOOT_PIN), onButtonPress, FALLING);
}

void loop() {
    showMenu();
    playSelectedMjpeg(currentMjpegIndex);
    // Sau khi video kết thúc hoặc bị dừng → hiện menu lại
}

// -------------------------------------------------------
// PLAYBACK
// -------------------------------------------------------
void playSelectedMjpeg(int mjpegIndex) {
    String fullPath = String(MJPEG_FOLDER) + "/" + mjpegFileList[mjpegIndex];
    char mjpegFilename[128];
    fullPath.toCharArray(mjpegFilename, sizeof(mjpegFilename));
    Serial.printf("Playing %s\n", mjpegFilename);
    mjpegPlayFromSDCard(mjpegFilename);
}

int jpegDrawCallback(JPEGDRAW *pDraw) {
    unsigned long s = millis();
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    total_show_video += millis() - s;
    return 1;
}

void mjpegPlayFromSDCard(char *mjpegFilename) {
    File mjpegFile = SD.open(mjpegFilename, "r");
    if (!mjpegFile || mjpegFile.isDirectory()) {
        Serial.printf("ERROR: Failed to open %s\n", mjpegFilename);
        return;
    }

    gfx->fillScreen(RGB565_BLACK);
    start_ms = curr_ms = millis();
    total_frames = total_read_video = total_decode_video = total_show_video = 0;

    mjpeg.setup(&mjpegFile, mjpeg_buf, jpegDrawCallback, true,
                0, 0, gfx->width(), gfx->height());

    while (!skipRequested && mjpegFile.available() && mjpeg.readMjpegBuf()) {
        uint32_t frame_start = millis();

        total_read_video += millis() - curr_ms;
        curr_ms = millis();

        mjpeg.drawJpg();
        total_decode_video += millis() - curr_ms;

        // Chạm màn hình khi đang phát → quay về menu
        if (touch.tirqTouched() && touch.touched()) {
            skipRequested = true;
        }

        uint32_t elapsed = millis() - frame_start;
        if (elapsed < FRAME_INTERVAL_MS) delay(FRAME_INTERVAL_MS - elapsed);

        curr_ms = millis();
        total_frames++;
    }

    if (skipRequested) {
        uint32_t now = millis();
        if (now - lastPress >= BOOT_BUTTON_DEBOUCE_TIME) lastPress = now;
    }
    skipRequested = false;

    int time_used = millis() - start_ms;
    mjpegFile.close();
    skipRequested = false;

    float fps = 1000.0 * total_frames / time_used;
    total_decode_video -= total_show_video;
    Serial.printf("Total frames: %d | FPS: %0.1f\n", total_frames, fps);
}

// -------------------------------------------------------
// SD FILE LIST
// -------------------------------------------------------
void loadMjpegFilesList() {
    File mjpegDir = SD.open(MJPEG_FOLDER);
    if (!mjpegDir) {
        Serial.printf("Failed to open %s\n", MJPEG_FOLDER);
        while (true) {}
    }
    mjpegCount = 0;
    while (true) {
        File file = mjpegDir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".mjpeg")) {
                mjpegFileList[mjpegCount]  = name;
                mjpegFileSizes[mjpegCount] = file.size();
                mjpegCount++;
                if (mjpegCount >= MAX_FILES) break;
            }
        }
        file.close();
    }
    mjpegDir.close();
    Serial.printf("%d mjpeg files found\n", mjpegCount);
}

String formatBytes(size_t bytes) {
    if (bytes < 1024)             return String(bytes) + " B";
    else if (bytes < 1024 * 1024) return String(bytes / 1024.0, 1) + " KB";
    else                          return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}
