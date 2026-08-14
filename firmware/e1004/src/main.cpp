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
constexpr uint32_t kImageBaud = 921600;
constexpr size_t kPackedRowBytes = 1200 / 2;

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

bool receiveHostImage() {
  static constexpr char magic[] = "E1IMG001";
  uint8_t header[sizeof(magic) - 1];
  uint8_t row[kPackedRowBytes];

  Serial.println("READY E1004IMG 1200 1600 4BPP");
  Serial.setTimeout(5000);
  const uint32_t deadline = millis() + 30000;
  while (!Serial.available() && int32_t(deadline - millis()) > 0) delay(10);
  if (!Serial.available()) return false;
  if (Serial.readBytes(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, magic, sizeof(header)) != 0) {
    Serial.println("IMAGE ERROR bad header");
    return false;
  }

  for (int16_t y = 0; y < 1600; ++y) {
    if (Serial.readBytes(row, sizeof(row)) != sizeof(row)) {
      Serial.print("IMAGE ERROR short row ");
      Serial.println(y);
      return false;
    }
    display.epd2.writeNative(row, nullptr, 0, y, 1200, 1);
    if ((y % 200) == 199) {
      Serial.print("IMAGE ROWS ");
      Serial.println(y + 1);
    }
  }
  Serial.println("IMAGE RECEIVED");
  display.epd2.refresh(false);
  display.hibernate();
  Serial.println("IMAGE DISPLAYED");
  return true;
}

uint16_t dither(uint16_t first, uint16_t second, int16_t x, int16_t y,
                uint8_t amount) {
  static constexpr uint8_t bayer4[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  return bayer4[y & 3][x & 3] < amount ? second : first;
}

void drawRainbow() {
  constexpr int16_t centerX = 600;
  constexpr int16_t centerY = 960;
  constexpr int16_t radii[] = {580, 525, 470, 415, 360, 305, 250};
  constexpr uint16_t first[] = {GxEPD_RED,    GxEPD_RED,   GxEPD_YELLOW,
                                GxEPD_GREEN,  GxEPD_BLUE,  GxEPD_BLUE};
  constexpr uint16_t second[] = {GxEPD_RED,   GxEPD_YELLOW, GxEPD_YELLOW,
                                 GxEPD_GREEN, GxEPD_BLUE,   GxEPD_RED};
  constexpr uint8_t blend[] = {0, 8, 0, 0, 0, 7};

  // Render one huge upper-half arch. Ordered pigment dithering makes orange
  // and violet from the panel's six-color native palette.
  for (int16_t y = centerY - radii[0]; y <= centerY; ++y) {
    const int32_t dy = y - centerY;
    for (int16_t x = 20; x < 1180; ++x) {
      const int32_t dx = x - centerX;
      const int32_t distance2 = dx * dx + dy * dy;
      for (uint8_t band = 0; band < 6; ++band) {
        if (distance2 <= int32_t(radii[band]) * radii[band] &&
            distance2 > int32_t(radii[band + 1]) * radii[band + 1]) {
          display.drawPixel(x, y,
                            dither(first[band], second[band], x, y,
                                   blend[band]));
          break;
        }
      }
    }
  }

  // Puffy cloud caps make the rainbow feel like an illustration rather than
  // a color chart.
  display.fillCircle(135, 935, 92, GxEPD_WHITE);
  display.fillCircle(225, 920, 115, GxEPD_WHITE);
  display.fillCircle(315, 945, 82, GxEPD_WHITE);
  display.fillCircle(885, 945, 82, GxEPD_WHITE);
  display.fillCircle(975, 920, 115, GxEPD_WHITE);
  display.fillCircle(1065, 935, 92, GxEPD_WHITE);
}

void drawSparkle(int16_t x, int16_t y, int16_t size, uint16_t color) {
  display.fillTriangle(x, y - size, x + size / 4, y, x, y + size, color);
  display.fillTriangle(x, y - size, x - size / 4, y, x, y + size, color);
  display.fillTriangle(x - size, y, x, y - size / 4, x + size, y, color);
  display.fillTriangle(x - size, y, x, y + size / 4, x + size, y, color);
}

void drawUnicorn() {
  // Ears and a tall golden horn sit behind the face.
  display.fillTriangle(420, 810, 470, 610, 535, 820, GxEPD_WHITE);
  display.fillTriangle(780, 810, 730, 610, 665, 820, GxEPD_WHITE);
  display.fillTriangle(600, 500, 530, 790, 670, 790, GxEPD_YELLOW);
  display.drawLine(552, 700, 648, 700, GxEPD_RED);
  display.drawLine(564, 650, 636, 650, GxEPD_GREEN);
  display.drawLine(576, 600, 624, 600, GxEPD_BLUE);

  // Puffy rainbow mane.
  display.fillCircle(410, 790, 72, GxEPD_RED);
  display.fillCircle(385, 875, 72, GxEPD_YELLOW);
  display.fillCircle(390, 960, 72, GxEPD_GREEN);
  display.fillCircle(420, 1040, 72, GxEPD_BLUE);

  // Rounded face and muzzle.
  display.fillCircle(600, 890, 210, GxEPD_WHITE);
  display.fillRoundRect(430, 850, 340, 300, 140, GxEPD_WHITE);
  display.fillCircle(600, 1050, 150, GxEPD_WHITE);

  // Big bright eyes, lashes, cheeks, and a tiny smile.
  display.fillCircle(525, 900, 30, GxEPD_BLACK);
  display.fillCircle(675, 900, 30, GxEPD_BLACK);
  display.fillCircle(516, 890, 10, GxEPD_BLUE);
  display.fillCircle(666, 890, 10, GxEPD_BLUE);
  display.drawLine(495, 870, 475, 850, GxEPD_BLACK);
  display.drawLine(705, 870, 725, 850, GxEPD_BLACK);
  display.fillCircle(475, 995, 30, GxEPD_RED);
  display.fillCircle(725, 995, 30, GxEPD_RED);
  display.fillCircle(565, 1035, 8, GxEPD_BLACK);
  display.fillCircle(635, 1035, 8, GxEPD_BLACK);
  display.drawLine(565, 1080, 600, 1098, GxEPD_RED);
  display.drawLine(600, 1098, 635, 1080, GxEPD_RED);

  drawSparkle(300, 650, 48, GxEPD_YELLOW);
  drawSparkle(890, 680, 42, GxEPD_GREEN);
  drawSparkle(930, 1060, 52, GxEPD_BLUE);
  drawSparkle(280, 1130, 35, GxEPD_WHITE);
}
}  // namespace

void setup() {
  Serial.begin(kImageBaud);
  delay(200);
  Serial.println("reterm E1004 custom color card: boot");

  epaperSpi.begin(kSck, kMiso, kMosi, -1);
  display.epd2.selectSPI(epaperSpi,
                         SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);

  if (receiveHostImage()) return;

  Serial.println("No host image; drawing built-in card");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    drawRainbow();

    display.drawRoundRect(25, 25, display.width() - 50,
                          display.height() - 50, 55, GxEPD_BLUE);
    display.drawRoundRect(38, 38, display.width() - 76,
                          display.height() - 76, 48, GxEPD_RED);

    display.setFont(&FreeSerifBoldItalic24pt7b);
    display.setTextSize(4);
    display.setTextColor(GxEPD_BLUE);
    centered("CLEO", 310);
    display.setTextSize(1);

    // Bright confetti around her name, including every native pigment.
    display.fillCircle(135, 180, 30, GxEPD_YELLOW);
    display.fillCircle(1060, 180, 30, GxEPD_GREEN);
    display.fillCircle(210, 360, 22, GxEPD_RED);
    display.fillCircle(990, 360, 22, GxEPD_BLUE);
    display.fillCircle(105, 475, 16, GxEPD_GREEN);
    display.fillCircle(1095, 475, 16, GxEPD_YELLOW);
    display.fillCircle(155, 560, 13, GxEPD_BLACK);
    display.fillCircle(1045, 560, 13, GxEPD_RED);

    // A huge red heart with a warm yellow shine.
    display.fillCircle(440, 760, 235, GxEPD_RED);
    display.fillCircle(760, 760, 235, GxEPD_RED);
    display.fillTriangle(225, 820, 975, 820, 600, 1310, GxEPD_RED);
    display.fillCircle(380, 685, 48, GxEPD_YELLOW);
    display.fillCircle(435, 625, 25, GxEPD_YELLOW);

    drawUnicorn();

    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    centered("YOU ARE LOVED!", 1450);
    display.setTextSize(1);
  } while (display.nextPage());

  display.hibernate();
  Serial.println("reterm E1004 custom color card: display hibernated");
}

void loop() { delay(1000); }
