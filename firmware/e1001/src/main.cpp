// reTerminal E1001 board support: the 7.5-inch 800x480 monochrome panel and
// the front buttons. The upload session, UART protocol, provisioning, and
// persistence all live in the shared reterm lib; images travel as 1bpp packed
// rows (bit set = white, MSB is the leftmost pixel), 48,000 bytes per screen.
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <esp_sleep.h>
#include <qrcode.h>
#include <reterm.h>

namespace {
constexpr int kSck = 7;
constexpr int kMosi = 9;
constexpr int kCs = 10;
constexpr int kDc = 11;
constexpr int kReset = 12;
constexpr int kBusy = 13;
// Three active-low front buttons with board pullups. Only the green button
// (GPIO3) is an RTC wake source in the stock firmware; mirror that here.
constexpr gpio_num_t kGreenButton = GPIO_NUM_3;
constexpr int kButtonPins[] = {3, 4, 5};

SPIClass epaperSpi(HSPI);
GxEPD2_BW<GxEPD2_750_GDEY075T7, 160>
    display(GxEPD2_750_GDEY075T7(kCs, kDc, kReset, kBusy));

// Center text drawn with the default 6x8 GFX font at the given size.
void centeredText(const char *text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  const int16_t width = int16_t(strlen(text)) * 6 * size;
  display.setCursor((800 - width) / 2, y);
  display.print(text);
}

void drawProvisionQr(const String &ssid, const String &password) {
  constexpr uint8_t version = 5;
  const String wifiQr = "WIFI:T:WPA;S:" + ssid + ";P:" + password + ";;";
  uint8_t qrData[qrcode_getBufferSize(version)];
  QRCode qr;
  qrcode_initText(&qr, qrData, version, ECC_LOW, wifiQr.c_str());
  const int16_t scale = 8;
  const int16_t qrPixels = qr.size * scale;
  const int16_t left = (800 - qrPixels) / 2;
  const int16_t top = 64;

  display.setFullWindow();
  display.firstPage();
  uint16_t page = 0;
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText("SET UP WI-FI", 12, 3);
    centeredText("Scan to join the temporary network", 42, 2);
    reterm::drawQrModules(display, qr, left, top, scale,
                          page * display.pageHeight(),
                          (page + 1) * display.pageHeight(), GxEPD_BLACK);
    ++page;
    centeredText("A setup page should open automatically", 376, 2);
    centeredText("Otherwise visit http://192.168.4.1", 400, 2);
    display.setTextSize(1);
    display.setCursor(left, 432);
    display.print("Network: ");
    display.print(ssid);
    display.setCursor(left, 446);
    display.print("Password: ");
    display.print(password);
  } while (display.nextPage());
}

void drawUploadQr(const String &url) {
  constexpr uint8_t version = 10;
  uint8_t qrData[qrcode_getBufferSize(version)];
  QRCode qr;
  qrcode_initText(&qr, qrData, version, ECC_LOW, url.c_str());
  const int16_t scale = 6;
  const int16_t qrPixels = qr.size * scale;
  const int16_t left = (800 - qrPixels) / 2;
  const int16_t top = 56;

  display.setFullWindow();
  display.firstPage();
  uint16_t page = 0;
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText("SEND A PHOTO", 12, 3);
    reterm::drawQrModules(display, qr, left, top, scale,
                          page * display.pageHeight(),
                          (page + 1) * display.pageHeight(), GxEPD_BLACK);
    ++page;
    centeredText("Scan within five minutes", 414, 2);
    centeredText("Choose, crop, preview, upload!", 446, 2);
  } while (display.nextPage());
}

class E1001Board final : public reterm::Board {
 public:
  const char *model() const override { return "reterminal-e1001"; }
  const char *hostname() const override { return "reterm-e1001"; }
  const char *uartName() const override { return "E1001"; }
  reterm::Geometry geometry() const override { return {800, 480, 800 / 8}; }
  const char *statusJson() const override {
    return "{\"model\":\"reterminal-e1001\",\"width\":800,"
           "\"height\":480,\"bytes\":48000,"
           "\"format\":\"gxepd2-1bpp\","
           "\"palette\":[\"black\",\"white\"]}";
  }
  void writeRow(const uint8_t *row, int16_t y) override {
    display.epd2.writeNative(row, nullptr, 0, y, 800, 1);
  }
  void refresh() override { display.epd2.refresh(false); }
  void hibernate() override { display.hibernate(); }
  void drawProvisionScreen(const String &ssid, const String &password) override {
    drawProvisionQr(ssid, password);
  }
  void drawUploadScreen(const String &url) override { drawUploadQr(url); }
  bool wokeByButton() const override {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) return true;
    for (int pin : kButtonPins) {
      if (digitalRead(pin) == LOW) return true;
    }
    return false;
  }
  bool wakeHoldRequestsWifiReset() override {
    // Keep holding the green button for five seconds through the wake to
    // forget the saved network. A normal tap releases within the first poll.
    const uint32_t deadline = millis() + 5000;
    while (digitalRead(kGreenButton) == LOW) {
      if (int32_t(deadline - millis()) <= 0) return true;
      delay(20);
    }
    return false;
  }
  void prepareSleep() override {
    pinMode(kGreenButton, INPUT_PULLUP);
    esp_sleep_enable_ext0_wakeup(kGreenButton, 0);
  }
};

E1001Board board;
}  // namespace

void setup() {
  Serial.begin(reterm::kImageBaud);
  delay(200);
  Serial.println("reterm E1001 custom photo card: boot");

  epaperSpi.begin(kSck, -1, kMosi, -1);
  display.epd2.selectSPI(epaperSpi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);

  for (int pin : kButtonPins) pinMode(pin, INPUT_PULLUP);
  reterm::run(board);
}

void loop() { delay(1000); }
