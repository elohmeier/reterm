#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeSerifBoldItalic24pt7b.h>

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

SPIClass epaperSpi(HSPI);
GxEPD2_7C<GxEPD2_T133A01_1200x1600, 40> display(
    GxEPD2_T133A01_1200x1600(kCs, kDc, kReset, kBusy, kCs1, kEnable));

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
  Serial.println("reterm E1004 custom color card: boot");

  epaperSpi.begin(kSck, kMiso, kMosi, -1);
  display.epd2.selectSPI(epaperSpi,
                         SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.drawRoundRect(25, 25, display.width() - 50,
                          display.height() - 50, 55, GxEPD_BLUE);
    display.drawRoundRect(38, 38, display.width() - 76,
                          display.height() - 76, 48, GxEPD_RED);

    display.setFont(&FreeSerifBoldItalic24pt7b);
    display.setTextSize(4);
    display.setTextColor(GxEPD_BLUE);
    centered("CLEO", 310);
    display.setTextSize(1);

    // Bright confetti around her name.
    display.fillCircle(135, 180, 30, GxEPD_YELLOW);
    display.fillCircle(1060, 180, 30, GxEPD_GREEN);
    display.fillCircle(210, 360, 22, GxEPD_RED);
    display.fillCircle(990, 360, 22, GxEPD_BLUE);
    display.fillCircle(105, 475, 16, GxEPD_GREEN);
    display.fillCircle(1095, 475, 16, GxEPD_YELLOW);

    // A huge red heart with a warm yellow shine.
    display.fillCircle(440, 760, 235, GxEPD_RED);
    display.fillCircle(760, 760, 235, GxEPD_RED);
    display.fillTriangle(225, 820, 975, 820, 600, 1310, GxEPD_RED);
    display.fillCircle(380, 685, 48, GxEPD_YELLOW);
    display.fillCircle(435, 625, 25, GxEPD_YELLOW);

    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    centered("YOU ARE LOVED!", 1450);
    display.setTextSize(1);
  } while (display.nextPage());

  display.hibernate();
  Serial.println("reterm E1004 custom color card: display hibernated");
}

void loop() { delay(1000); }
