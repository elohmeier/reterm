#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSerifBoldItalic24pt7b.h>

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

    // A loose double border makes the card feel hand drawn.
    display.drawRoundRect(16, 16, display.width() - 32,
                          display.height() - 32, 32, GxEPD_BLACK);
    display.drawRoundRect(23, 22, display.width() - 46,
                          display.height() - 44, 28, GxEPD_BLACK);

    // Big, curly-ish italic serif lettering.
    display.setFont(&FreeSerifBoldItalic24pt7b);
    display.setTextSize(2);
    centered("CLEO", 145);
    display.setTextSize(1);

    // Curls, confetti, and little bubbles around her name.
    display.drawCircle(105, 112, 24, GxEPD_BLACK);
    display.drawCircle(105, 112, 13, GxEPD_BLACK);
    display.drawCircle(694, 112, 24, GxEPD_BLACK);
    display.drawCircle(694, 112, 13, GxEPD_BLACK);
    display.fillCircle(70, 72, 7, GxEPD_BLACK);
    display.fillCircle(730, 72, 7, GxEPD_BLACK);
    display.fillCircle(150, 174, 5, GxEPD_BLACK);
    display.fillCircle(650, 174, 5, GxEPD_BLACK);

    // One enormous heart: two round lobes flowing into a pointed base.
    display.fillCircle(340, 285, 72, GxEPD_BLACK);
    display.fillCircle(460, 285, 72, GxEPD_BLACK);
    display.fillTriangle(278, 302, 522, 302, 400, 442, GxEPD_BLACK);

    // White highlights give the solid heart a friendly illustrated look.
    display.fillCircle(319, 265, 13, GxEPD_WHITE);
    display.fillCircle(337, 249, 7, GxEPD_WHITE);
  } while (display.nextPage());

  display.hibernate();
  Serial.println("reterm custom bring-up: display hibernated");
}

void loop() { delay(1000); }
