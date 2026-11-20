#include <SD.h>
#include <M5Unified.h>
#include <soc/gpio_struct.h>
#include "epaper_portraits.h"
#include "sd_test_assets_embedded.h"

// M5Stack Basic v2.7 terminals -> Waveshare RPi Zero PhotoPainter
// EPD_RST uses the bottom SCL terminal / GPIO22 to avoid GPIO25.
constexpr int EPD_MOSI = 2;   // G2
constexpr int EPD_SCLK = 5;   // G5
constexpr int EPD_CS   = 16;  // R2
constexpr int EPD_DC   = 17;  // T2
constexpr int EPD_RST  = 22;  // bottom terminal labeled SCL
constexpr int EPD_BUSY = 35;
constexpr int EPD_PWR  = 26;  // second DA (DAC2)

constexpr int EPD_WIDTH  = 800;
constexpr int EPD_HEIGHT = 480;

// Spectra 6 / EPD_7in3e color indices
constexpr uint8_t EPD_BLACK  = 0x0;
constexpr uint8_t EPD_WHITE  = 0x1;
constexpr uint8_t EPD_YELLOW = 0x2;
constexpr uint8_t EPD_RED    = 0x3;
constexpr uint8_t EPD_BLUE   = 0x5;
constexpr uint8_t EPD_GREEN  = 0x6;

enum class Picture : uint8_t {
  ClosedEyes,
  OpenEyes,
};

Picture currentPicture = Picture::ClosedEyes;
bool panelReady = false;
bool hasDisplayedPicture = false;

constexpr char SD_CLOSED_PATH[] = "/assets/me_close.jpg";
constexpr char SD_OPEN_PATH[] = "/assets/me_open.jpg";
constexpr char SD_SETTING_PATH[] = "/setting.txt";
constexpr char SD_TEST_ASSET_MARKER_PATH[] =
  "/assets/.distinct_test_images_v1";

struct ScheduleConfig {
  uint8_t initialHour = 12;
  uint8_t initialMinute = 0;
  uint8_t startHour = 18;
  uint8_t startMinute = 0;
  uint8_t endHour = 9;
  uint8_t endMinute = 0;
};

ScheduleConfig scheduleConfig;
bool sdReady = false;
bool sdImageReady = false;
M5Canvas imageCanvas(&M5.Display);

bool writeEmbeddedSdAsset(const char* path,
                          const uint8_t* data,
                          size_t length) {
  if (SD.exists(path) && !SD.remove(path)) {
    Serial.printf("Could not replace %s\n", path);
    return false;
  }

  File output = SD.open(path, FILE_WRITE);
  if (!output) {
    Serial.printf("Could not create %s\n", path);
    return false;
  }
  const size_t written = output.write(data, length);
  output.close();
  Serial.printf("Installed %s: %u/%u bytes\n", path,
                static_cast<unsigned>(written),
                static_cast<unsigned>(length));
  return written == length;
}

void installDistinctSdTestAssets() {
  if (!sdReady || SD.exists(SD_TEST_ASSET_MARKER_PATH)) return;

  if (!SD.exists("/assets") && !SD.mkdir("/assets")) {
    Serial.println("Could not create /assets on SD card");
    return;
  }

  const bool closeOk = writeEmbeddedSdAsset(
    SD_CLOSED_PATH, SD_TEST_CLOSE_JPG, SD_TEST_CLOSE_JPG_len);
  const bool openOk = writeEmbeddedSdAsset(
    SD_OPEN_PATH, SD_TEST_OPEN_JPG, SD_TEST_OPEN_JPG_len);
  if (!closeOk || !openOk) {
    Serial.println("SD test artwork install incomplete; retry next boot");
    return;
  }

  File marker = SD.open(SD_TEST_ASSET_MARKER_PATH, FILE_WRITE);
  if (marker) {
    marker.println("distinct-test-images-v1");
    marker.close();
    Serial.println("Distinct SD test artwork installed");
  }
}

void epdSpiWrite(uint8_t value) {
  // Software SPI prevents contention with the M5Stack internal LCD SPI bus.
  // Direct GPIO writes are much faster than digitalWrite for the 192 KB frame.
  constexpr uint32_t clockMask = 1UL << EPD_SCLK;
  constexpr uint32_t dataMask = 1UL << EPD_MOSI;
  for (int bit = 7; bit >= 0; --bit) {
    GPIO.out_w1tc = clockMask;
    if ((value >> bit) & 0x01) {
      GPIO.out_w1ts = dataMask;
    } else {
      GPIO.out_w1tc = dataMask;
    }
    __asm__ __volatile__("nop\n nop\n nop\n nop\n");
    GPIO.out_w1ts = clockMask;
    __asm__ __volatile__("nop\n nop\n nop\n nop\n");
  }
  GPIO.out_w1tc = clockMask;
}

void epdCommand(uint8_t value) {
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  epdSpiWrite(value);
  digitalWrite(EPD_CS, HIGH);
}

void epdData(uint8_t value) {
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  epdSpiWrite(value);
  digitalWrite(EPD_CS, HIGH);
}

bool waitForLevel(int level, uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (digitalRead(EPD_BUSY) != level) {
    if (millis() - startedAt >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

// A command is complete only after BUSY actually goes LOW and returns HIGH.
// Merely seeing HIGH immediately after the command can be the pre-command state.
bool waitForBusyPulse(const char* stage,
                      uint32_t lowTimeoutMs,
                      uint32_t highTimeoutMs) {
  Serial.printf("[%s] wait BUSY LOW; current=%d\n", stage,
                digitalRead(EPD_BUSY));
  if (!waitForLevel(LOW, lowTimeoutMs)) {
    Serial.printf("[%s] BUSY never went LOW\n", stage);
    return false;
  }
  if (!waitForLevel(HIGH, highTimeoutMs)) {
    Serial.printf("[%s] BUSY stayed LOW\n", stage);
    return false;
  }
  Serial.printf("[%s] BUSY pulse OK\n", stage);
  return true;
}

void epdReset() {
  digitalWrite(EPD_RST, HIGH);
  delay(20);
  digitalWrite(EPD_RST, LOW);
  delay(2); // PhotoPainterは長いLOWリセットを避ける
  digitalWrite(EPD_RST, HIGH);
  delay(20);
}

bool epdInit() {
  // Give the panel a real power-control cycle before every update. This also
  // recovers from an ESP32 reset or firmware upload during a previous refresh.
  digitalWrite(EPD_CS, HIGH);
  digitalWrite(EPD_RST, LOW);
  digitalWrite(EPD_PWR, LOW);
  delay(700);

  digitalWrite(EPD_PWR, HIGH);
  delay(150);

  epdReset();
  if (!waitForLevel(HIGH, 5000)) {
    Serial.println("RESET: BUSY stayed LOW");
    digitalWrite(EPD_PWR, LOW);
    return false;
  }
  delay(30);

  // Waveshare EPD_7in3e official initialization sequence
  epdCommand(0xAA);
  epdData(0x49); epdData(0x55); epdData(0x20);
  epdData(0x08); epdData(0x09); epdData(0x18);

  epdCommand(0x01); epdData(0x3F);
  epdCommand(0x00); epdData(0x5F); epdData(0x69);
  epdCommand(0x03);
  epdData(0x00); epdData(0x54); epdData(0x00); epdData(0x44);

  epdCommand(0x05);
  epdData(0x40); epdData(0x1F); epdData(0x1F); epdData(0x2C);

  epdCommand(0x06);
  epdData(0x6F); epdData(0x1F); epdData(0x17); epdData(0x49);

  epdCommand(0x08);
  epdData(0x6F); epdData(0x1F); epdData(0x1F); epdData(0x22);

  epdCommand(0x30); epdData(0x03);
  epdCommand(0x50); epdData(0x3F);
  epdCommand(0x60); epdData(0x02); epdData(0x00);
  epdCommand(0x61);
  epdData(0x03); epdData(0x20); epdData(0x01); epdData(0xE0);
  epdCommand(0x84); epdData(0x01);
  epdCommand(0xE3); epdData(0x2F);

  epdCommand(0x04); // power on
  if (!waitForBusyPulse("POWER_ON", 3000, 30000)) {
    digitalWrite(EPD_PWR, LOW);
    return false;
  }
  return true;
}

uint8_t nearestEpdColor(uint16_t rgb565) {
  const int red = ((rgb565 >> 11) & 0x1F) * 255 / 31;
  const int green = ((rgb565 >> 5) & 0x3F) * 255 / 63;
  const int blue = (rgb565 & 0x1F) * 255 / 31;
  const uint8_t codes[] = {EPD_BLACK, EPD_WHITE, EPD_YELLOW,
                           EPD_RED, EPD_BLUE, EPD_GREEN};
  const uint8_t palette[][3] = {
    {0, 0, 0}, {255, 255, 255}, {255, 220, 0},
    {205, 20, 20}, {0, 70, 200}, {0, 145, 70},
  };
  uint32_t bestDistance = UINT32_MAX;
  uint8_t bestCode = EPD_WHITE;
  for (size_t i = 0; i < sizeof(codes); ++i) {
    const int dr = red - palette[i][0];
    const int dg = green - palette[i][1];
    const int db = blue - palette[i][2];
    const uint32_t distance = dr * dr + dg * dg + db * db;
    if (distance < bestDistance) {
      bestDistance = distance;
      bestCode = codes[i];
    }
  }
  return bestCode;
}

bool loadSdPicture(Picture picture) {
  if (!sdReady) return false;
  const char* path = picture == Picture::OpenEyes ? SD_OPEN_PATH : SD_CLOSED_PATH;
  if (!SD.exists(path)) return false;

  imageCanvas.fillScreen(TFT_WHITE);
  // SD artwork is 480x800 portrait. Decode at half scale to a 240x400
  // canvas, then duplicate each pixel while streaming to the 800x480 panel.
  fs::FS& sdFileSystem = SD;
  const bool ok = imageCanvas.drawJpgFile(sdFileSystem, path, 0, 0, 240, 400,
                                           0, 0, 0.5f, 0.5f);
  Serial.printf("SD image %s: %s\n", path, ok ? "OK" : "decode failed");
  return ok;
}

void streamEmbeddedPicture(Picture picture) {
  const uint8_t* image = picture == Picture::OpenEyes
                           ? EPD_IMAGE_OPEN
                           : EPD_IMAGE_CLOSED;
  constexpr size_t packedImageBytes = EPD_WIDTH * EPD_HEIGHT / 2;
  for (size_t index = 0; index < packedImageBytes; ++index) {
    epdSpiWrite(pgm_read_byte(image + index));
  }
}

void streamSdPicture() {
  // Controller-native coordinates are 800x480 landscape. The physical panel
  // is portrait, so this performs the same CCW rotation as the built-in data.
  for (int controllerY = 0; controllerY < EPD_HEIGHT; ++controllerY) {
    const int canvasX = (EPD_HEIGHT - 1 - controllerY) / 2;
    for (int byteX = 0; byteX < EPD_WIDTH / 2; ++byteX) {
      const int canvasY = byteX;
      const uint8_t code = nearestEpdColor(imageCanvas.readPixel(canvasX, canvasY));
      epdSpiWrite((code << 4) | code);
    }
  }
}

bool refreshPanel(Picture picture) {
  if (!epdInit()) {
    return false;
  }

  epdCommand(0x10); // start image data
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  if (sdImageReady) streamSdPicture();
  else streamEmbeddedPicture(picture);
  digitalWrite(EPD_CS, HIGH);

  epdCommand(0x06);
  epdData(0x6F); epdData(0x1F); epdData(0x17); epdData(0x49);
  epdCommand(0x12); // display refresh
  epdData(0x00);
  if (!waitForBusyPulse("REFRESH", 3000, 120000)) {
    digitalWrite(EPD_PWR, LOW);
    return false;
  }

  epdCommand(0x02); // controller power off
  epdData(0x00);
  const bool ok = waitForLevel(HIGH, 30000);
  digitalWrite(EPD_PWR, LOW);
  return ok;
}

enum class UiScreen : uint8_t {
  ClockSet,
  Home,
  Menu,
  ScheduleSet,
};

UiScreen uiScreen = UiScreen::ClockSet;
uint8_t clockHour = 12;
uint8_t clockMinute = 0;
uint8_t editField = 0;
uint8_t menuSelection = 0;
bool bootClockSetup = true;
bool initialTimeFromSd = false;
uint32_t nextScheduleCheck = 0;
int lastHomeSecond = -1;
int lastScheduleMinute = -1;
bool softwareClockRunning = false;
uint32_t softwareClockBaseSeconds = 0;
uint32_t softwareClockSetAtMs = 0;

// Keep the controller unobtrusive in the dark. A button press wakes only the
// M5Basic LCD; the user presses again to perform the intended operation.
constexpr uint8_t LCD_ACTIVE_BRIGHTNESS = 96;
constexpr uint32_t LCD_TIMEOUT_MS = 20000;
bool lcdAwake = true;
uint32_t lastLcdInteractionMs = 0;

const char* imageSourceLabel() {
  return sdImageReady ? "SD CARD" : "BUILT-IN";
}

bool parseTimeValue(const String& value, uint8_t& hour, uint8_t& minute) {
  int h = -1;
  int m = -1;
  if (sscanf(value.c_str(), "%d:%d", &h, &m) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  hour = h;
  minute = m;
  return true;
}

void loadSettings() {
  if (!sdReady || !SD.exists(SD_SETTING_PATH)) return;
  File file = SD.open(SD_SETTING_PATH, FILE_READ);
  while (file && file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) continue;
    const int separator = line.indexOf('=');
    if (separator < 0) continue;
    String key = line.substring(0, separator);
    String value = line.substring(separator + 1);
    key.trim();
    value.trim();
    key.toUpperCase();
    if (key == "START_TIME") {
      parseTimeValue(value, scheduleConfig.startHour,
                     scheduleConfig.startMinute);
    } else if (key == "INITIAL_TIME") {
      initialTimeFromSd = parseTimeValue(value, scheduleConfig.initialHour,
                                         scheduleConfig.initialMinute);
    } else if (key == "END_TIME") {
      parseTimeValue(value, scheduleConfig.endHour,
                     scheduleConfig.endMinute);
    }
  }
  file.close();
  Serial.printf("Initial time: %02u:%02u (%s)\n",
                scheduleConfig.initialHour, scheduleConfig.initialMinute,
                initialTimeFromSd ? "SD" : "default");
}

void saveSettings() {
  if (!sdReady) return;
  File file = SD.open(SD_SETTING_PATH, FILE_WRITE);
  if (!file) return;
  file.println("# e-paper schedule settings");
  file.printf("INITIAL_TIME=%02u:%02u\n", scheduleConfig.initialHour,
              scheduleConfig.initialMinute);
  file.printf("START_TIME=%02u:%02u\n", scheduleConfig.startHour,
              scheduleConfig.startMinute);
  file.printf("END_TIME=%02u:%02u\n", scheduleConfig.endHour,
              scheduleConfig.endMinute);
  file.close();
}

void drawM5Status(const char* title, uint16_t color) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(18, 28);
  M5.Display.println(title);

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(18, 100);
  M5.Display.println("PLEASE WAIT...");
  M5.Display.setCursor(18, 132);
  M5.Display.println("EPD REFRESH");
  M5.Display.setCursor(18, 164);
  M5.Display.println(imageSourceLabel());
}

bool showPicture(Picture picture, bool force = false) {
  const bool open = picture == Picture::OpenEyes;
  if (!force && hasDisplayedPicture && currentPicture == picture) {
    return true;
  }

  // Resolve the source before touching the e-paper. This applies to the
  // mandatory first closed-eye refresh as well as scheduled/test updates.
  // A missing or unreadable SD asset falls back to the embedded image.
  sdImageReady = loadSdPicture(picture);
  drawM5Status(open ? "UPDATING: OPEN" : "UPDATING: CLOSED", TFT_YELLOW);
  panelReady = refreshPanel(picture);

  if (panelReady) {
    currentPicture = picture;
    hasDisplayedPicture = true;
    drawM5Status(open ? "DISPLAY: OPEN" : "DISPLAY: CLOSED", TFT_GREEN);
  } else {
    drawM5Status("EPD ERROR / BUSY", TFT_RED);
  }
  return panelReady;
}

void drawClockSet() {
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(16, 14);
  M5.Display.println("SET CURRENT TIME");
  M5.Display.setTextSize(5);
  M5.Display.setCursor(50, 70);
  M5.Display.printf("%02u:%02u", clockHour, clockMinute);
  M5.Display.drawRect(editField == 0 ? 44 : 173, 62,
                      editField == 0 ? 78 : 82, 54, TFT_YELLOW);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(12, 164);
  M5.Display.println("A: -   B: NEXT/OK   C: +");
  M5.Display.setCursor(12, 202);
  if (bootClockSetup) {
    M5.Display.printf("BOOT VALUE: %s",
                      initialTimeFromSd ? "SD CARD" : "DEFAULT");
  } else {
    M5.Display.println("Clock is saved to the RTC");
  }
}

bool readValidRtcClock(uint8_t& hour, uint8_t& minute, uint8_t& second) {
  // The mandatory boot UI always establishes this monotonic software clock.
  // It keeps advancing even on units whose BM8563 RTC is absent or stopped.
  if (softwareClockRunning) {
    const uint32_t elapsed = (millis() - softwareClockSetAtMs) / 1000;
    const uint32_t total = (softwareClockBaseSeconds + elapsed) % 86400;
    hour = total / 3600;
    minute = (total / 60) % 60;
    second = total % 60;
    return true;
  }
  m5::rtc_datetime_t dt{};
  if (!M5.Rtc.getDateTime(&dt)) return false;
  const int h = static_cast<int>(dt.time.hours);
  const int m = static_cast<int>(dt.time.minutes);
  const int s = static_cast<int>(dt.time.seconds);
  if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return false;
  hour = h;
  minute = m;
  second = s;
  return true;
}

bool readValidRtcTime(uint8_t& hour, uint8_t& minute) {
  uint8_t second = 0;
  return readValidRtcClock(hour, minute, second);
}

void drawHomeClock() {
  uint8_t hour = clockHour;
  uint8_t minute = clockMinute;
  uint8_t second = 0;
  readValidRtcClock(hour, minute, second);
  lastHomeSecond = hour * 3600 + minute * 60 + second;

  // Redraw only the clock rectangle so the screen does not flicker each second.
  M5.Display.fillRect(55, 58, 230, 42, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(78, 66);
  M5.Display.printf("%02u:%02u:%02u", hour, minute, second);
}

void drawHome() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(14, 18);
  M5.Display.println("SCHEDULE ON");
  drawHomeClock();
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(14, 122);
  M5.Display.printf("OPEN  %02u:%02u\n", scheduleConfig.startHour,
                    scheduleConfig.startMinute);
  M5.Display.setCursor(14, 148);
  M5.Display.printf("CLOSE %02u:%02u\n", scheduleConfig.endHour,
                    scheduleConfig.endMinute);
  M5.Display.setCursor(14, 184);
  M5.Display.printf("IMAGE: %s", imageSourceLabel());
  M5.Display.setCursor(14, 218);
  M5.Display.println("A:CLOSE  B:MENU  C:OPEN");
}

void drawMenu() {
  M5.Display.fillScreen(TFT_DARKGREY);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(14, 10);
  M5.Display.println("SETTINGS");
  M5.Display.setTextSize(2);
  const char* items[] = {
    "SET CURRENT TIME",
    "SET SHOW SCHEDULE",
    "BACK",
  };
  for (uint8_t i = 0; i < 3; ++i) {
    const int y = 65 + i * 42;
    if (i == menuSelection) {
      M5.Display.fillRect(10, y - 4, 300, 29, TFT_NAVY);
      M5.Display.setTextColor(TFT_YELLOW, TFT_NAVY);
    } else {
      M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    }
    M5.Display.setCursor(18, y);
    M5.Display.printf("%c %s", i == menuSelection ? '>' : ' ', items[i]);
  }
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setCursor(12, 210);
  M5.Display.println("A:UP  B:SELECT  C:DOWN");
}

void drawScheduleSet() {
  M5.Display.fillScreen(TFT_DARKCYAN);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(16, 14);
  M5.Display.println("SET SHOW SCHEDULE");
  M5.Display.setTextSize(4);
  M5.Display.setCursor(25, 67);
  M5.Display.printf("OPEN  %02u:%02u", scheduleConfig.startHour,
                    scheduleConfig.startMinute);
  M5.Display.setCursor(25, 118);
  M5.Display.printf("CLOSE %02u:%02u", scheduleConfig.endHour,
                    scheduleConfig.endMinute);
  const int boxes[][4] = {
    {156, 60, 55, 43}, {225, 60, 55, 43},
    {156, 111, 55, 43}, {225, 111, 55, 43},
  };
  M5.Display.drawRect(boxes[editField][0], boxes[editField][1],
                      boxes[editField][2], boxes[editField][3], TFT_YELLOW);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(12, 181);
  M5.Display.println("A: -   B: NEXT/SAVE   C: +");
  M5.Display.setCursor(12, 212);
  M5.Display.println("Cross-midnight supported");
}

void applySchedule() {
  if (uiScreen != UiScreen::Home) return;
  uint8_t hour = clockHour;
  uint8_t minute = clockMinute;
  readValidRtcTime(hour, minute);
  const int currentMinute = hour * 60 + minute;
  if (currentMinute == lastScheduleMinute) return;
  lastScheduleMinute = currentMinute;

  if (hour == scheduleConfig.startHour
      && minute == scheduleConfig.startMinute) {
    showPicture(Picture::OpenEyes);
    drawHome();
  } else if (hour == scheduleConfig.endHour
             && minute == scheduleConfig.endMinute) {
    showPicture(Picture::ClosedEyes);
    drawHome();
  }
}

void setRtcTime() {
  softwareClockBaseSeconds = clockHour * 3600UL + clockMinute * 60UL;
  softwareClockSetAtMs = millis();
  softwareClockRunning = true;
  lastScheduleMinute = -1;

  m5::rtc_datetime_t dt{};
  const bool readOk = M5.Rtc.getDateTime(&dt);
  const int year = static_cast<int>(dt.date.year);
  const int month = static_cast<int>(dt.date.month);
  const int date = static_cast<int>(dt.date.date);
  // Keep the RTC calendar and change only the user-requested clock time.
  // A blank RTC reports -1/255 values, so validate every date component.
  if (!readOk || year < 2020 || year > 2099
      || month < 1 || month > 12 || date < 1 || date > 31) {
    dt = {{2026, 11, 20}, {clockHour, clockMinute, 0}};
  } else {
    dt.time.hours = clockHour;
    dt.time.minutes = clockMinute;
    dt.time.seconds = 0;
  }
  M5.Rtc.setDateTime(dt);
}

void changeEditedValue(int delta) {
  uint8_t* value = nullptr;
  int modulus = 60;
  if (uiScreen == UiScreen::ClockSet) {
    value = editField == 0 ? &clockHour : &clockMinute;
    modulus = editField == 0 ? 24 : 60;
  } else {
    if (editField == 0) value = &scheduleConfig.startHour;
    if (editField == 1) value = &scheduleConfig.startMinute;
    if (editField == 2) value = &scheduleConfig.endHour;
    if (editField == 3) value = &scheduleConfig.endMinute;
    modulus = (editField == 0 || editField == 2) ? 24 : 60;
  }
  *value = (*value + modulus + delta) % modulus;
}

// The M5Basic is mounted upside down, so the physical left/right switches are
// reversed. Keep the UI labels intuitive by mapping logical A to hardware C
// and logical C to hardware A. B remains the center switch.
bool uiButtonAWasPressed() { return M5.BtnC.wasPressed(); }
bool uiButtonBWasPressed() { return M5.BtnB.wasPressed(); }
bool uiButtonCWasPressed() { return M5.BtnA.wasPressed(); }

bool updateLcdWakeState() {
  const bool anyButtonPressed = M5.BtnA.wasPressed()
                             || M5.BtnB.wasPressed()
                             || M5.BtnC.wasPressed();
  if (anyButtonPressed) {
    lastLcdInteractionMs = millis();
    if (!lcdAwake) {
      M5.Display.setBrightness(LCD_ACTIVE_BRIGHTNESS);
      lcdAwake = true;
      return true; // Consume the wake-up press; do not change EPD/UI state.
    }
  }

  if (lcdAwake && millis() - lastLcdInteractionMs >= LCD_TIMEOUT_MS) {
    M5.Display.setBrightness(0);
    lcdAwake = false;
  }
  return false;
}

void handleButtons() {
  if (uiScreen == UiScreen::ClockSet) {
    bool redraw = false;
    if (uiButtonAWasPressed()) {
      changeEditedValue(-1);
      redraw = true;
    }
    if (uiButtonCWasPressed()) {
      changeEditedValue(1);
      redraw = true;
    }
    if (uiButtonBWasPressed()) {
      if (editField == 0) {
        ++editField;
        redraw = true;
      }
      else {
        setRtcTime();
        editField = 0;
        bootClockSetup = false;
        uiScreen = UiScreen::Home;
        // Every boot starts from a known closed-eye state. Resolving the
        // picture here also makes the home screen report SD CARD immediately
        // when the SD asset is available.
        showPicture(Picture::ClosedEyes, true);
        drawHome();
        applySchedule();
        return;
      }
    }
    if (redraw) drawClockSet();
    return;
  }

  if (uiScreen == UiScreen::Home) {
    if (uiButtonBWasPressed()) {
      menuSelection = 0;
      uiScreen = UiScreen::Menu;
      drawMenu();
    } else if (uiButtonAWasPressed()) {
      showPicture(Picture::ClosedEyes);
      drawHome();
    } else if (uiButtonCWasPressed()) {
      showPicture(Picture::OpenEyes);
      drawHome();
    }
    return;
  }

  if (uiScreen == UiScreen::Menu) {
    if (uiButtonAWasPressed()) {
      menuSelection = (menuSelection + 2) % 3;
      drawMenu();
    } else if (uiButtonCWasPressed()) {
      menuSelection = (menuSelection + 1) % 3;
      drawMenu();
    } else if (uiButtonBWasPressed()) {
      if (menuSelection == 0) {
        bootClockSetup = false;
        uint8_t currentHour = clockHour;
        uint8_t currentMinute = clockMinute;
        readValidRtcTime(currentHour, currentMinute);
        clockHour = currentHour;
        clockMinute = currentMinute;
        editField = 0;
        uiScreen = UiScreen::ClockSet;
        drawClockSet();
      } else if (menuSelection == 1) {
        editField = 0;
        uiScreen = UiScreen::ScheduleSet;
        drawScheduleSet();
      } else {
        uiScreen = UiScreen::Home;
        drawHome();
        applySchedule();
      }
    }
    return;
  }

  if (uiScreen == UiScreen::ScheduleSet) {
    bool redraw = false;
    if (uiButtonAWasPressed()) {
      changeEditedValue(-1);
      redraw = true;
    }
    if (uiButtonCWasPressed()) {
      changeEditedValue(1);
      redraw = true;
    }
    if (uiButtonBWasPressed()) {
      if (editField < 3) {
        ++editField;
        redraw = true;
      }
      else {
        saveSettings();
        editField = 0;
        uiScreen = UiScreen::Home;
        drawHome();
        applySchedule();
        return;
      }
    }
    if (redraw) drawScheduleSet();
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(3); // M5Basic is mounted upside down on the bridge board.
  M5.Display.setBrightness(LCD_ACTIVE_BRIGHTNESS);
  lastLcdInteractionMs = millis();
  M5.Speaker.end(); // Keep the internal speaker explicitly disabled.
  Serial.begin(115200);

  pinMode(EPD_CS, OUTPUT);
  pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_RST, OUTPUT);
  pinMode(EPD_PWR, OUTPUT);
  pinMode(EPD_BUSY, INPUT);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_SCLK, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  digitalWrite(EPD_PWR, LOW);
  digitalWrite(EPD_MOSI, LOW);
  digitalWrite(EPD_SCLK, LOW);

  imageCanvas.setColorDepth(8);
  if (!imageCanvas.createSprite(240, 400)) {
    Serial.println("Could not allocate SD image canvas");
  }

  sdReady = SD.begin(GPIO_NUM_4, SPI, 25000000);
  Serial.printf("SD card: %s\n", sdReady ? "OK" : "not found");
  installDistinctSdTestAssets();
  loadSettings();

  // The mandatory boot-time clock screen starts from INITIAL_TIME in
  // setting.txt. This makes setup deterministic even when the RTC is invalid.
  clockHour = scheduleConfig.initialHour;
  clockMinute = scheduleConfig.initialMinute;
  editField = 0;
  bootClockSetup = true;
  uiScreen = UiScreen::ClockSet;
  drawClockSet();
}

void loop() {
  M5.update();
  const bool consumedWakePress = updateLcdWakeState();
  if (!consumedWakePress) handleButtons();

  if (uiScreen == UiScreen::Home && millis() >= nextScheduleCheck) {
    nextScheduleCheck = millis() + 15000;
    applySchedule();
  }

  if (uiScreen == UiScreen::Home) {
    uint8_t hour = clockHour;
    uint8_t minute = clockMinute;
    uint8_t second = 0;
    readValidRtcClock(hour, minute, second);
    const int currentSecond = hour * 3600 + minute * 60 + second;
    if (currentSecond != lastHomeSecond) drawHomeClock();
  }
  delay(10);
}

/*
  The EPD initialization sequence is adapted from Waveshare's EPD_7in3e
  reference driver, distributed under the MIT License:
  https://github.com/waveshareteam/e-Paper

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to inclusion of this notice.
*/
