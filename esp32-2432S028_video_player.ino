// Tutorial : https://youtu.be/jYcxUgxz9ks
// Use board "ESP32 Dev Module" (last tested on v3.2.0)

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>  // Install "XPT2046_Touchscreen" from Library Manager
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

// --- Menu style (Bruce-like) ---
#define MENU_BG         0x0000  // Black background
#define MENU_HEADER_BG  0x0318  // Dark green header (Bruce style)
#define MENU_ITEM_BG    0x1082  // Dark gray item
#define MENU_SEL_BG     0x02B0  // Green highlight (selected)
#define MENU_TEXT       0xFFFF  // White text
#define MENU_SEL_TEXT   0xFFFF  // White text selected
#define MENU_DIM_TEXT   0x8410  // Gray text (dim)
#define ITEM_H          36      // Height of each menu item
#define HEADER_H        40      // Header height
#define MENU_PADDING    6       // Padding inside item

// File list
#define MAX_FILES 20
String   mjpegFileList[MAX_FILES];
uint32_t mjpegFileSizes[MAX_FILES] = {0};
int      mjpegCount       = 0;
int      currentMjpegIndex = 0;

// MJPEG globals
MjpegClass     mjpeg;
int            total_frames;
unsigned long  total_read_video, total_decode_video, total_show_video;
unsigned long  start_ms, curr_ms;
long           output_buf_size, estimateBufferSize;
uint8_t       *mjpeg_buf;
uint16_t      *output_buf;

// Display
Arduino_DataBus *bus = new Arduino_HWSPI(2, 15, 14, 13, 12);
Arduino_GFX     *gfx = new Arduino_ILI9341(bus);

// Touch (CYD 2USB uses separate SPI for touch)
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// SD on VSPI
SPIClass sd_spi(VSPI);

// Skip button
volatile bool skipRequested = false;
volatile uint32_t isrTick   = 0;
uint32_t lastPress           = 0;

void IRAM_ATTR onButtonPress() {
    skipRequested = true;
    isrTick = xTaskGetTickCountFromISR();
}

// -------------------------------------------------------
// MENU
// -------------------------------------------------------
int  menuScrollOffset = 0;   // first visible item index
int  menuSelected     = 0;   // currently highlighted item

int visibleItems() {
    return (gfx->height() - HEADER_H) / ITEM_H;
}

void drawMenu() {
    int W = gfx->width();
    int H = gfx->height();
    int vis = visibleItems();

    // Header
    gfx->fillRect(0, 0, W, HEADER_H, MENU_HEADER_BG);
    gfx->setTextColor(MENU_TEXT);
    gfx->setTextSize(2);
    gfx->setCursor(10, 12);
    gfx->print(">> Video Player");

    // Item count top-right
    gfx->setTextSize(1);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d files", mjpegCount);
    gfx->setCursor(W - 60, 16);
    gfx->print(buf);

    // Items
    gfx->fillRect(0, HEADER_H, W, H - HEADER_H, MENU_BG);
    for (int i = 0; i < vis; i++) {
        int idx = menuScrollOffset + i;
        if (idx >= mjpegCount) break;

        int y = HEADER_H + i * ITEM_H;
        bool sel = (idx == menuSelected);

        // Item background
        gfx->fillRect(4, y + 2, W - 8, ITEM_H - 4,
                      sel ? MENU_SEL_BG : MENU_ITEM_BG);

        // Draw left accent bar if selected
        if (sel) {
            gfx->fillRect(4, y + 2, 4, ITEM_H - 4, 0x07E0); // bright green bar
        }

        // File name (truncate if too long)
        String name = mjpegFileList[idx];
        if (name.endsWith(".mjpeg")) name = name.substring(0, name.length() - 6);
        if (name.length() > 22) name = name.substring(0, 21) + "~";

        gfx->setTextColor(sel ? MENU_SEL_TEXT : MENU_TEXT);
        gfx->setTextSize(1);
        gfx->setCursor(16, y + MENU_PADDING + 4);
        gfx->print(name);

        // File size (right aligned, dim color)
        gfx->setTextColor(MENU_DIM_TEXT);
        gfx->setCursor(16, y + MENU_PADDING + 16);
        gfx->print(formatBytes(mjpegFileSizes[idx]));
    }

    // Scrollbar
    if (mjpegCount > vis) {
        int sbH    = H - HEADER_H;
        int barH   = max(10, sbH * vis / mjpegCount);
        int barY   = HEADER_H + sbH * menuScrollOffset / mjpegCount;
        gfx->fillRect(W - 4, HEADER_H, 4, sbH, MENU_ITEM_BG);
        gfx->fillRect(W - 4, barY, 4, barH, 0x07E0);
    }
}

// Map raw touch coords to screen coords for CYD 2USB (landscape 320x240)
// Raw values roughly: X 200-3800, Y 200-3800
void getTouchPoint(int &tx, int &ty) {
    TS_Point p = touch.getPoint();
    // Map raw to screen — adjust these if touch is off
    tx = map(p.x, 3800, 200, 0, gfx->width());
    ty = map(p.y, 3800, 200, 0, gfx->height());
    tx = constrain(tx, 0, gfx->width() - 1);
    ty = constrain(ty, 0, gfx->height() - 1);
}

void showMenu() {
    menuSelected     = currentMjpegIndex;
    menuScrollOffset = 0;
    int vis = visibleItems();
    // Scroll so selected is visible
    if (menuSelected >= vis) menuScrollOffset = menuSelected - vis + 1;

    gfx->fillScreen(MENU_BG);
    drawMenu();

    uint32_t lastTouch   = 0;
    int      lastTouchY  = -1;
    bool     dragging    = false;
    int      dragStartY  = 0;
    int      dragStartOff = 0;

    while (true) {
        if (touch.tirqTouched() && touch.touched()) {
            int tx, ty;
            getTouchPoint(tx, ty);
            uint32_t now = millis();

            if (now - lastTouch > 80) { // debounce
                lastTouch = now;

                if (!dragging) {
                    dragging    = true;
                    dragStartY  = ty;
                    dragStartOff = menuScrollOffset;
                }

                // Drag scroll
                int delta = (dragStartY - ty) / ITEM_H;
                int newOff = constrain(dragStartOff + delta, 0, max(0, mjpegCount - vis));
                if (newOff != menuScrollOffset) {
                    menuScrollOffset = newOff;
                    drawMenu();
                }
            }
            lastTouchY = ty;
        } else {
            if (dragging) {
                // Finger lifted — was it a tap (little movement)?
                int moved = abs(lastTouchY - dragStartY);
                if (moved < 10 && lastTouchY >= HEADER_H) {
                    int tappedItem = menuScrollOffset + (lastTouchY - HEADER_H) / ITEM_H;
                    if (tappedItem >= 0 && tappedItem < mjpegCount) {
                        if (tappedItem == menuSelected) {
                            // Double tap same item = play
                            currentMjpegIndex = menuSelected;
                            dragging = false;
                            return; // exit menu → play
                        } else {
                            menuSelected = tappedItem;
                            drawMenu();
                        }
                    }
                }
                dragging = false;
            }
        }

        // Boot button = confirm play selected
        if (digitalRead(BOOT_PIN) == LOW) {
            delay(50);
            if (digitalRead(BOOT_PIN) == LOW) {
                currentMjpegIndex = menuSelected;
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

    // Display
    if (!gfx->begin(DISPLAY_SPI_SPEED)) {
        Serial.println("Display init failed!");
        while (true) {}
    }
    gfx->setRotation(0);
    gfx->fillScreen(MENU_BG);
    gfx->invertDisplay(true);

    // Touch (HSPI)
    touchSPI.begin(25, 39, 32, TOUCH_CS); // SCK, MISO, MOSI, CS
    touch.begin(touchSPI);
    touch.setRotation(0);

    // SD (VSPI)
    if (!SD.begin(SD_CS, sd_spi, SD_SPI_SPEED, "/sd")) {
        Serial.println("SD mount failed!");
        while (true) {}
    }

    // Buffers
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
    // Show menu first
    showMenu();
    // Play selected
    playSelectedMjpeg(currentMjpegIndex);
    // After video ends, show menu again
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

        // Touch during playback = return to menu
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
