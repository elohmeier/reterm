// reTerminal E1004 board support: the 13.3-inch six-color panel, the IQS323
// capacitive controls, and the device's QR screens. The upload session, UART
// protocol, provisioning, and persistence all live in the shared reterm lib.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_7C.h>
#include <esp_sleep.h>
#include <qrcode.h>
#include <reterm.h>

#include "GxEPD2_T133A01_1200x1600.h"

namespace {
constexpr int kSck = 7;
constexpr int kMiso = 8;
constexpr int kMosi = 9;
constexpr int kCs = 10;
constexpr int kDc = 11;
constexpr int kCs1 = 2;
constexpr int kReset = 38;
constexpr int kBusy = 13;
constexpr int kEnable = 12;
// The three E1004 capacitive controls are channels of an IQS323. Its shared
// active-low RDY/interrupt output is wired to ESP32-S3 GPIO 3.
constexpr gpio_num_t kButton = GPIO_NUM_3;
constexpr int kTouchSda = 39;
constexpr int kTouchScl = 40;
constexpr uint8_t kTouchAddress = 0x44;

SPIClass epaperSpi(HSPI);
GxEPD2_7C<GxEPD2_T133A01_1200x1600, 40> display(
    GxEPD2_T133A01_1200x1600(kCs, kDc, kReset, kBusy, kCs1, kEnable));

bool openTouchWindow() {
  if (digitalRead(kButton) == LOW) return true;
  Wire.beginTransmission(kTouchAddress);
  Wire.write(0xff);
  if (Wire.endTransmission() != 0) return false;
  const uint32_t deadline = millis() + 100;
  while (digitalRead(kButton) == HIGH && int32_t(deadline - millis()) > 0) delay(1);
  return digitalRead(kButton) == LOW;
}

bool writeTouchRegister(uint8_t address, uint8_t value) {
  if (!openTouchWindow()) return false;
  Wire.beginTransmission(kTouchAddress);
  Wire.write(address);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readTouchRegister(uint8_t address, uint8_t *data, size_t length) {
  if (!openTouchWindow()) return false;
  Wire.beginTransmission(kTouchAddress);
  Wire.write(address);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(kTouchAddress, length) != length) return false;
  for (size_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

void prepareTouchWake() {
  // Tap mode reports changes from any of the three channels. Event mode then
  // keeps RDY high during sleep except for a real touch/release event.
  writeTouchRegister(0xa0, 0x09);
  writeTouchRegister(0xd3, 0x02);
  uint8_t control[2] = {};
  if (readTouchRegister(0xc0, control, sizeof(control))) {
    writeTouchRegister(0xc0, control[0] | 0x80);
  }

  // Reading status acknowledges any event that led us here. If a control is
  // still held, wait briefly for release and acknowledge that transition too.
  uint8_t status[18];
  readTouchRegister(0x10, status, sizeof(status));
  const uint32_t releaseDeadline = millis() + 1500;
  while (digitalRead(kButton) == LOW &&
         int32_t(releaseDeadline - millis()) > 0) delay(10);
  if (digitalRead(kButton) == LOW) readTouchRegister(0x10, status, sizeof(status));
}

void drawProvisionQr(const String &ssid, const String &password) {
  constexpr uint8_t version = 5;
  const String wifiQr = "WIFI:T:WPA;S:" + ssid + ";P:" + password + ";;";
  uint8_t qrData[qrcode_getBufferSize(version)];
  QRCode qr;
  qrcode_initText(&qr, qrData, version, ECC_LOW, wifiQr.c_str());
  const int16_t scale = 18;
  const int16_t qrPixels = qr.size * scale;
  const int16_t left = (1200 - qrPixels) / 2;
  const int16_t top = 285;

  display.setFullWindow();
  display.firstPage();
  uint16_t page = 0;
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLUE);
    display.setTextSize(4);
    display.setCursor(185, 115);
    display.print("SET UP WI-FI");
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(215, 210);
    display.print("Scan to join the temporary network");
    reterm::drawQrModules(display, qr, left, top, scale,
                          page * display.pageHeight(),
                          (page + 1) * display.pageHeight(), GxEPD_BLACK);
    ++page;
    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    display.setCursor(170, 1120);
    display.print("A setup page should open automatically");
    display.setTextColor(GxEPD_RED);
    display.setCursor(155, 1210);
    display.print("Otherwise visit http://192.168.4.1");
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);
    display.setCursor(140, 1320);
    display.print("Network: ");
    display.print(ssid);
    display.setCursor(140, 1380);
    display.print("Password: ");
    display.print(password);
  } while (display.nextPage());
}

void drawUploadQr(const String &url) {
  constexpr uint8_t version = 10;
  uint8_t qrData[qrcode_getBufferSize(version)];
  QRCode qr;
  qrcode_initText(&qr, qrData, version, ECC_LOW, url.c_str());
  const int16_t scale = 14;
  const int16_t qrPixels = qr.size * scale;
  const int16_t left = (1200 - qrPixels) / 2;
  const int16_t top = 300;

  display.setFullWindow();
  display.firstPage();
  uint16_t page = 0;
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLUE);
    display.setTextSize(4);
    display.setCursor(215, 120);
    display.print("SEND A PHOTO");
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(265, 220);
    display.print("Scan within five minutes");
    reterm::drawQrModules(display, qr, left, top, scale,
                          page * display.pageHeight(),
                          (page + 1) * display.pageHeight(), GxEPD_BLACK);
    ++page;
    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    display.setCursor(190, 1220);
    display.print("Choose, crop, preview, upload!");
  } while (display.nextPage());
}

class E1004Board final : public reterm::Board {
 public:
  const char *model() const override { return "reterminal-e1004"; }
  const char *hostname() const override { return "reterm-e1004"; }
  const char *uartName() const override { return "E1004"; }
  reterm::Geometry geometry() const override { return {1200, 1600, 1200 / 2}; }
  const char *statusJson() const override {
    return "{\"model\":\"reterminal-e1004\",\"width\":1200,"
           "\"height\":1600,\"bytes\":960000,"
           "\"format\":\"gxepd2-4bpp\","
           "\"palette\":[\"black\",\"white\",\"green\","
           "\"blue\",\"red\",\"yellow\"]}";
  }
  void writeRow(const uint8_t *row, int16_t y) override {
    display.epd2.writeNative(row, nullptr, 0, y, 1200, 1);
  }
  void refresh() override { display.epd2.refresh(false); }
  void hibernate() override { display.hibernate(); }
  void drawProvisionScreen(const String &ssid, const String &password) override {
    drawProvisionQr(ssid, password);
  }
  void drawUploadScreen(const String &url) override { drawUploadQr(url); }
  bool wokeByButton() const override {
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ||
           digitalRead(kButton) == LOW;
  }
  void prepareSleep() override {
    pinMode(kButton, INPUT_PULLUP);
    prepareTouchWake();
    esp_sleep_enable_ext0_wakeup(kButton, 0);
  }
};

E1004Board board;
}  // namespace

void setup() {
  Serial.begin(reterm::kImageBaud);
  delay(200);
  Serial.println("reterm E1004 custom color card: boot");

  epaperSpi.begin(kSck, kMiso, kMosi, -1);
  Wire.begin(kTouchSda, kTouchScl);
  Wire.setClock(100000);
  display.epd2.selectSPI(epaperSpi,
                         SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);

  pinMode(kButton, INPUT_PULLUP);
  reterm::run(board);
}

void loop() { delay(1000); }
