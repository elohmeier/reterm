#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

namespace {
constexpr int kSck = 7;
constexpr int kMosi = 9;
constexpr int kCs = 10;
constexpr int kDc = 11;
constexpr int kReset = 12;
constexpr int kBusy = 13;

SPIClass epaperSpi(HSPI);
GxEPD2_BW<GxEPD2_750_GDEY075T7, 160>
    display(GxEPD2_750_GDEY075T7(kCs, kDc, kReset, kBusy));

void centered(const char *text, int16_t baseline) {
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text, 0, baseline, &x1, &y1, &width, &height);
  display.setCursor((display.width() - width) / 2 - x1, baseline);
  display.print(text);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("reterm custom bring-up: boot");

  epaperSpi.begin(kSck, -1, kMosi, -1);
  display.epd2.selectSPI(epaperSpi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.drawRect(12, 12, display.width() - 24, display.height() - 24,
                     GxEPD_BLACK);
    display.setFont(&FreeSansBold24pt7b);
    centered("reterm custom firmware", 210);
    display.setFont(&FreeSans12pt7b);
    centered("E1001 display bring-up succeeded", 270);
  } while (display.nextPage());

  display.hibernate();
  Serial.println("reterm custom bring-up: display hibernated");
}

void loop() { delay(1000); }
