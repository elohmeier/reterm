// reTerminal E1001 board support: the 7.5-inch 800x480 monochrome panel and
// the front buttons. The upload session, UART protocol, provisioning, and
// persistence all live in the shared reterm lib; images travel as 1bpp packed
// rows (bit set = white, MSB is the leftmost pixel), 48,000 bytes per screen.
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
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
// Three active-low top buttons with board pullups, left to right: white
// (GPIO5), green (GPIO3), white (GPIO4). The green button stays the EXT0
// photo-session wake like the stock firmware; the two white buttons are an
// EXT1 wake into the rock-paper-scissors game.
constexpr gpio_num_t kGreenButton = GPIO_NUM_3;
constexpr int kLeftButton = 5;
constexpr int kRightButton = 4;
constexpr int kButtonPins[] = {3, 4, 5};
constexpr uint64_t kGameButtonMask =
    (1ULL << kLeftButton) | (1ULL << kRightButton);

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

// Same, but centered on an arbitrary x instead of the panel midline.
void cellText(const char *text, int16_t cx, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor(cx - int16_t(strlen(text)) * 3 * size, y);
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

// --- Rock, paper, scissors -------------------------------------------------
// A white-button wake plays against the hardware RNG. While a screen is up,
// every top button picks a move in physical order: left white = rock, green =
// paper, right white = scissors. The lifetime score persists in its own NVS
// namespace; the panel keeps whatever screen was up last, and the next photo
// session or MQTT image replaces it as usual.

constexpr uint32_t kGameIdleMs = 60 * 1000;
constexpr const char *kMoveNames[] = {"ROCK", "PAPER", "SCISSORS"};
constexpr const char *kMoveButtons[] = {"LEFT BUTTON", "GREEN BUTTON",
                                        "RIGHT BUTTON"};
constexpr const char *kWinReasons[] = {"Rock blunts scissors",
                                       "Paper wraps rock",
                                       "Scissors cut paper"};

struct RpsScore {
  uint32_t wins = 0;
  uint32_t losses = 0;
  uint32_t draws = 0;
};

RpsScore loadRpsScore() {
  RpsScore score;
  Preferences preferences;
  if (preferences.begin("reterm-rps", true)) {
    score.wins = preferences.getUInt("wins", 0);
    score.losses = preferences.getUInt("losses", 0);
    score.draws = preferences.getUInt("draws", 0);
    preferences.end();
  }
  return score;
}

void saveRpsScore(const RpsScore &score) {
  Preferences preferences;
  if (!preferences.begin("reterm-rps", false)) return;
  preferences.putUInt("wins", score.wins);
  preferences.putUInt("losses", score.losses);
  preferences.putUInt("draws", score.draws);
  preferences.end();
}

// The icons draw with GFX primitives on a +/-50 unit grid scaled to a size s
// box centered on (cx, cy), so all three read at any size without bitmaps.

void thickLine(float x0, float y0, float x1, float y1, float thickness,
               uint16_t color) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  const float ox = -dy * thickness / (2 * len);
  const float oy = dx * thickness / (2 * len);
  const int16_t ax = lroundf(x0 + ox), ay = lroundf(y0 + oy);
  const int16_t bx = lroundf(x0 - ox), by = lroundf(y0 - oy);
  const int16_t cx = lroundf(x1 + ox), cy = lroundf(y1 + oy);
  const int16_t ex = lroundf(x1 - ox), ey = lroundf(y1 - oy);
  display.fillTriangle(ax, ay, bx, by, cx, cy, color);
  display.fillTriangle(bx, by, ex, ey, cx, cy, color);
}

// A faceted boulder: a filled polygon fan with white crack lines meeting the
// silhouette at its corners so they read as facet edges.
void drawRockIcon(int16_t cx, int16_t cy, int16_t s) {
  const auto px = [&](float u) { return int16_t(lroundf(cx + u * s / 100)); };
  const auto py = [&](float v) { return int16_t(lroundf(cy + v * s / 100)); };
  static constexpr int8_t outline[][2] = {{-44, 34}, {-50, 4},  {-34, -24},
                                          {-10, -42}, {22, -38}, {44, -16},
                                          {48, 16},  {32, 40}};
  constexpr size_t n = sizeof(outline) / sizeof(outline[0]);
  for (size_t i = 1; i + 1 < n; ++i) {
    display.fillTriangle(px(outline[0][0]), py(outline[0][1]),
                         px(outline[i][0]), py(outline[i][1]),
                         px(outline[i + 1][0]), py(outline[i + 1][1]),
                         GxEPD_BLACK);
  }
  const auto crack = [&](float x0, float y0, float x1, float y1) {
    display.drawLine(px(x0), py(y0), px(x1), py(y1), GxEPD_WHITE);
    display.drawLine(px(x0) + 1, py(y0), px(x1) + 1, py(y1), GxEPD_WHITE);
    display.drawLine(px(x0), py(y0) + 1, px(x1), py(y1) + 1, GxEPD_WHITE);
  };
  crack(-10, -42, -4, -2);
  crack(-4, -2, -44, 34);
  crack(-4, -2, 48, 16);
}

// A ruled sheet with a dog-eared top-right corner.
void drawPaperIcon(int16_t cx, int16_t cy, int16_t s) {
  const int16_t w = s * 68 / 100;
  const int16_t h = s * 88 / 100;
  const int16_t x0 = cx - w / 2;
  const int16_t y0 = cy - h / 2;
  const int16_t fold = s * 26 / 100;
  int16_t border = s / 45;
  if (border < 3) border = 3;
  display.fillRect(x0, y0, w, h, GxEPD_BLACK);
  display.fillRect(x0 + border, y0 + border, w - 2 * border, h - 2 * border,
                   GxEPD_WHITE);
  // Cut the corner outside the fold, then draw the turned-down flap; its
  // hypotenuse doubles as the cut edge.
  display.fillTriangle(x0 + w - fold, y0, x0 + w - 1, y0, x0 + w - 1,
                       y0 + fold, GxEPD_WHITE);
  display.fillTriangle(x0 + w - fold, y0, x0 + w - 1, y0 + fold, x0 + w - fold,
                       y0 + fold, GxEPD_BLACK);
  int16_t rule = s / 55;
  if (rule < 2) rule = 2;
  for (int i = 0; i < 4; ++i) {
    const int16_t y = y0 + fold + s * 8 / 100 + i * (s * 12 / 100);
    display.fillRect(x0 + s * 12 / 100, y,
                     w - 2 * (s * 12 / 100) - (i == 3 ? w / 4 : 0), rule,
                     GxEPD_BLACK);
  }
}

// Open scissors: two blade-to-handle strips crossing at a riveted pivot,
// pointed blades up, ringed handles down.
void drawScissorsIcon(int16_t cx, int16_t cy, int16_t s) {
  const auto px = [&](float u) { return cx + u * s / 100; };
  const auto py = [&](float v) { return cy + v * s / 100; };
  const float pivotX = px(0), pivotY = py(2);
  for (int side = -1; side <= 1; side += 2) {
    const float tipX = px(34.0f * side), tipY = py(-44);
    const float ringX = px(-20.0f * side), ringY = py(24) + s * 0.07f;
    const float bladeDx = pivotX - tipX, bladeDy = pivotY - tipY;
    const float bladeLen = sqrtf(bladeDx * bladeDx + bladeDy * bladeDy);
    const float ox = -bladeDy / bladeLen * s * 0.055f;
    const float oy = bladeDx / bladeLen * s * 0.055f;
    display.fillTriangle(lroundf(tipX), lroundf(tipY), lroundf(pivotX + ox),
                         lroundf(pivotY + oy), lroundf(pivotX - ox),
                         lroundf(pivotY - oy), GxEPD_BLACK);
    thickLine(pivotX, pivotY, ringX, ringY, s * 0.07f, GxEPD_BLACK);
    display.fillCircle(lroundf(ringX), lroundf(ringY), lroundf(s * 0.16f),
                       GxEPD_BLACK);
    display.fillCircle(lroundf(ringX), lroundf(ringY), lroundf(s * 0.09f),
                       GxEPD_WHITE);
  }
  display.fillCircle(lroundf(pivotX), lroundf(pivotY), lroundf(s * 0.05f),
                     GxEPD_BLACK);
  display.fillCircle(lroundf(pivotX), lroundf(pivotY), lroundf(s * 0.02f),
                     GxEPD_WHITE);
}

void drawMoveIcon(int move, int16_t cx, int16_t cy, int16_t s) {
  if (move == 0) drawRockIcon(cx, cy, s);
  else if (move == 1) drawPaperIcon(cx, cy, s);
  else drawScissorsIcon(cx, cy, s);
}

String scoreLine(const RpsScore &score) {
  return "You " + String(score.wins) + " : " + String(score.losses) +
         " reTerminal   (" + String(score.draws) + " draws)";
}

void drawChoiceScreen(const RpsScore &score) {
  static constexpr int16_t cellX[] = {150, 400, 650};
  const String scores = scoreLine(score);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText("ROCK  PAPER  SCISSORS", 16, 4);
    centeredText("Beat the reTerminal: pick a move with a top button", 66, 2);
    for (int move = 0; move < 3; ++move) {
      drawMoveIcon(move, cellX[move], 205, 170);
      cellText(kMoveNames[move], cellX[move], 308, 3);
      cellText(kMoveButtons[move], cellX[move], 342, 2);
    }
    centeredText(scores.c_str(), 398, 2);
    centeredText("Idle for a minute puts the display back to sleep", 446, 2);
  } while (display.nextPage());
}

// diff is (player - machine + 3) % 3: 1 = player won, 2 = machine won.
void drawResultScreen(int player, int machine, int diff,
                      const RpsScore &score) {
  const char *verdict =
      diff == 1 ? "YOU WIN!" : diff == 2 ? "RETERMINAL WINS" : "IT'S A DRAW";
  const char *reason = diff == 0 ? "Same move - nobody scores"
                                 : kWinReasons[diff == 1 ? player : machine];
  const int16_t winnerX = diff == 1 ? 210 : 590;
  const String scores = scoreLine(score);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText(verdict, 18, 5);
    centeredText(reason, 68, 2);
    cellText("YOU", 210, 104, 3);
    cellText("RETERMINAL", 590, 104, 3);
    drawMoveIcon(player, 210, 240, 190);
    drawMoveIcon(machine, 590, 240, 190);
    cellText(kMoveNames[player], 210, 364, 2);
    cellText(kMoveNames[machine], 590, 364, 2);
    display.setTextSize(4);
    display.setCursor(376, 222);
    display.print("VS");
    if (diff != 0) {
      for (int16_t inset = 0; inset < 3; ++inset) {
        display.drawRoundRect(winnerX - 122 + inset, 136 + inset,
                              244 - 2 * inset, 216 - 2 * inset, 14,
                              GxEPD_BLACK);
      }
    }
    centeredText(scores.c_str(), 402, 2);
    centeredText("Rematch with a top button:", 430, 2);
    centeredText("LEFT rock    GREEN paper    RIGHT scissors", 456, 2);
  } while (display.nextPage());
}

// Button indices in physical order; the RPS move mapping (rock/paper/
// scissors) is the same order, so games can use these values directly.
constexpr int kBtnLeft = 0;
constexpr int kBtnGreen = 1;
constexpr int kBtnRight = 2;

int readButton() {
  if (digitalRead(kLeftButton) == LOW) return kBtnLeft;
  if (digitalRead(kGreenButton) == LOW) return kBtnGreen;
  if (digitalRead(kRightButton) == LOW) return kBtnRight;
  return -1;
}

// Waits until every button has read released for 60 ms so the wake press (or
// a rematch press) cannot leak into the next prompt. The deadline guards
// against a stuck button pinning the game loop forever.
void waitForButtonsReleased() {
  const uint32_t deadline = millis() + 10000;
  uint32_t stableSince = millis();
  while (int32_t(deadline - millis()) > 0) {
    if (readButton() >= 0) stableSince = millis();
    else if (millis() - stableSince >= 60) return;
    delay(10);
  }
}

int waitForButton(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (int32_t(deadline - millis()) > 0) {
    const int button = readButton();
    if (button >= 0) {
      delay(30);  // debounce: accept only if the same press is still down
      if (readButton() == button) return button;
    }
    delay(10);
  }
  return -1;
}

void playRockPaperScissors() {
  Serial.println("Arcade: rock paper scissors");
  RpsScore score = loadRpsScore();
  waitForButtonsReleased();
  drawChoiceScreen(score);
  for (int player = waitForButton(kGameIdleMs); player >= 0;
       player = waitForButton(kGameIdleMs)) {
    const int machine = int(esp_random() % 3);
    const int diff = (player - machine + 3) % 3;
    if (diff == 1) ++score.wins;
    else if (diff == 2) ++score.losses;
    else ++score.draws;
    saveRpsScore(score);
    Serial.print("RPS round: player=");
    Serial.print(kMoveNames[player]);
    Serial.print(" reterminal=");
    Serial.print(kMoveNames[machine]);
    Serial.print(" outcome=");
    Serial.println(diff == 1 ? "win" : diff == 2 ? "loss" : "draw");
    drawResultScreen(player, machine, diff, score);
    waitForButtonsReleased();
  }
  // The last screen stays on the panel; run() hibernates the controller and
  // re-arms every wake source through its no-command path.
  Serial.println("Game idle; handing back to the standard flow");
  waitForButtonsReleased();
}

// --- Arcade sprites --------------------------------------------------------
// 1-bit string art ('X' black cell, 'o' white cell, '.' transparent),
// converted from CC0 pixel art: horse, unicorn (horn extended two cells), and
// tornado from Clint Bellanger's "Tiny Creatures"
// (https://opengameart.org/content/tiny-creatures, CC0); heart, apple,
// sparkle, and frame from Kenney's "1-Bit Pack" (https://kenney.nl, CC0).
// The egg is drawn to match. Regenerate with a light/dark threshold over the
// creature mask plus a dilated single-cell outline.

constexpr const char *kSpriteHorse[] = {
    "........XXXXXX",
    ".......XXoXXoX",
    "......XXXooooX",
    ".XXXXXXXXXooXX",
    "XXXoooXXXooooX",
    "XXoooooooXooXX",
    "XXoooooooXooXX",
    "XXooooooooXXXX",
    ".XXooXXXooXXoX",
    ".XooXXXXoXXooX",
    ".XooXXXooXXXoX",
    ".XXooXXooXXXXX",
    "..XXXXXXXXXXXX",
};

constexpr const char *kSpriteUnicorn[] = {
    ".............XX",
    "............XX.",
    "..........XXX..",
    "........XXXoXX.",
    ".......XXoXooX.",
    "......XXXooooX.",
    ".XXXXXXXXXooXX.",
    "XXXoooXXXooooX.",
    "XXooooooooooXX.",
    "XXooooooooooXX.",
    "XXooooooooXXoX.",
    ".XoooooooooooX.",
    ".XooooXXooXooX.",
    ".XooooXooXXXoX.",
    ".XXooXXooXXXXX.",
    "..XXXXXXXXXXXX.",
};

constexpr const char *kSpriteTornado[] = {
    "..XXXXXXXXXX..",
    ".XXooooooooXX.",
    "XXooooooooooXX",
    "XooooooooooooX",
    "XooooooooooooX",
    "XooooooooooooX",
    "XXoooXXoooXoXX",
    ".XoooXXoooXoX.",
    ".XXoooooooooX.",
    "..XoooooooooX.",
    "..XXoooooooXX.",
    "..XoooooooooX.",
    "..XoooooooooX.",
    "..XXXoooooXXX.",
    "....XooooXX...",
};

constexpr const char *kSpriteHeart[] = {
    ".XX....XX.",
    "XXXX..XXXX",
    "XXXXXXXXXX",
    "XXXXXXXXXX",
    ".XXXXXXXX.",
    "..XXXXXX..",
    "...XXXX...",
    "....XX....",
};

constexpr const char *kSpriteApple[] = {
    "......XX..",
    ".....XXX..",
    ".....X....",
    ".XXX..XXX.",
    "XXXXXXXXXX",
    "XXXXXX..XX",
    "XXXXXX..XX",
    "XXXXXXXXXX",
    "XXXXXXXXXX",
    ".XXXXXXXX.",
    ".XXXXXXXX.",
    "..XX..XX..",
};

constexpr const char *kSpriteSparkle[] = {
    ".......X..X..X",
    "........X.X.X.",
    "...X.X...XXX..",
    "....X..XXX.XXX",
    "...X.X...XXX..",
    "........X.X.X.",
    ".......X..X..X",
    "X.X.X.........",
    "..X......X.X..",
    "XX.XX.....X...",
    "..X......X.X..",
    "X.X.X.........",
};

constexpr const char *kSpriteFrame[] = {
    ".XXXXXXXXXXXXXX.",
    "X..............X",
    "X.XXXXXXXXXXXX.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.X..........X.X",
    "X.XXXXXXXXXXXX.X",
    "X..............X",
    ".XXXXXXXXXXXXXX.",
};

constexpr const char *kSpriteEgg[] = {
    "...XXXXXX...",
    "..XXooooXX..",
    ".XXooooooXX.",
    ".XoooXooooX.",
    "XoooXXXooooX",
    "XooXXXXXoooX",
    "XoooXXXooooX",
    "XooooXoooooX",
    "XooooooooooX",
    ".XooooooooX.",
    ".XXooooooXX.",
    "..XXXXXXXX..",
};

void drawSpriteImpl(const char *const rows[], int count, int16_t x, int16_t y,
                    int16_t scale) {
  for (int j = 0; j < count; ++j) {
    for (int i = 0; rows[j][i]; ++i) {
      const char cell = rows[j][i];
      if (cell == '.') continue;
      display.fillRect(x + i * scale, y + j * scale, scale, scale,
                       cell == 'X' ? GxEPD_BLACK : GxEPD_WHITE);
    }
  }
}

template <size_t N>
void drawSprite(const char *const (&rows)[N], int16_t x, int16_t y,
                int16_t scale) {
  drawSpriteImpl(rows, int(N), x, y, scale);
}

// Centers the sprite on cx with its feet on the ground line.
template <size_t N>
void drawSpriteOnGround(const char *const (&rows)[N], int16_t cx,
                        int16_t ground, int16_t scale) {
  const int16_t width = int16_t(strlen(rows[0])) * scale;
  drawSpriteImpl(rows, int(N), cx - width / 2, ground - int16_t(N) * scale,
                 scale);
}

// --- Luna the unicorn ------------------------------------------------------
// A slow tamagotchi living on the deep-sleep duty cycle. There is no wall
// clock, so elapsed time is counted in Home Assistant timer wakes: each one
// ages the pet by the configured interval. Care happens through the arcade
// menu; leaving the pet on screen puts it "on the wall", where every timer
// wake repaints it until a new photo replaces the saved SPIFFS image
// (detected by checksum, so photos always win). Without a configured HA
// broker there are no timer wakes and the pet simply pauses between visits.

constexpr uint32_t kPetFedMaxMin = 2 * 24 * 60;    // a full belly lasts two days
constexpr uint32_t kPetJoyMaxMin = 36 * 60;        // entertained for a day and a half
constexpr uint32_t kPetFeedMin = 12 * 60;
constexpr uint32_t kPetPlayMin = 9 * 60;
constexpr uint32_t kPetLostAfterMin = 2 * 24 * 60; // starved this long: runs off
constexpr uint32_t kPetHatchMin = 8 * 60;
constexpr uint32_t kPetFoalUntilMin = 3 * 24 * 60;
constexpr uint32_t kPetElderMin = 10 * 24 * 60;

struct PetState {
  bool adopted = false;
  bool lost = false;
  bool onWall = false;
  uint32_t ageMin = 0;
  uint32_t fedMin = kPetFedMaxMin;
  uint32_t joyMin = kPetJoyMaxMin;
  uint32_t starvedMin = 0;
  uint32_t wallSum = 0;  // checksum of the saved image when wall mode began
};

enum class PetStage { Egg, Foal, Unicorn, Celestial, Gone };

PetStage petStage(const PetState &pet) {
  if (pet.lost) return PetStage::Gone;
  if (pet.ageMin < kPetHatchMin) return PetStage::Egg;
  if (pet.ageMin < kPetFoalUntilMin) return PetStage::Foal;
  if (pet.ageMin < kPetElderMin) return PetStage::Unicorn;
  return PetStage::Celestial;
}

PetState loadPet() {
  PetState pet;
  Preferences preferences;
  if (preferences.begin("reterm-pet", true)) {
    pet.adopted = preferences.getBool("adopted", false);
    pet.lost = preferences.getBool("lost", false);
    pet.onWall = preferences.getBool("wall", false);
    pet.ageMin = preferences.getUInt("age", 0);
    pet.fedMin = preferences.getUInt("fed", kPetFedMaxMin);
    pet.joyMin = preferences.getUInt("joy", kPetJoyMaxMin);
    pet.starvedMin = preferences.getUInt("starved", 0);
    pet.wallSum = preferences.getUInt("wallsum", 0);
    preferences.end();
  }
  return pet;
}

void savePet(const PetState &pet) {
  Preferences preferences;
  if (!preferences.begin("reterm-pet", false)) return;
  preferences.putBool("adopted", pet.adopted);
  preferences.putBool("lost", pet.lost);
  preferences.putBool("wall", pet.onWall);
  preferences.putUInt("age", pet.ageMin);
  preferences.putUInt("fed", pet.fedMin);
  preferences.putUInt("joy", pet.joyMin);
  preferences.putUInt("starved", pet.starvedMin);
  preferences.putUInt("wallsum", pet.wallSum);
  preferences.end();
}

void agePet(PetState &pet, uint32_t minutes) {
  if (!pet.adopted || pet.lost) return;
  pet.ageMin += minutes;
  if (pet.ageMin < kPetHatchMin) return;  // eggs need no care
  pet.fedMin -= min(pet.fedMin, minutes);
  pet.joyMin -= min(pet.joyMin, minutes);
  if (pet.fedMin == 0) {
    pet.starvedMin += minutes;
    if (pet.starvedMin >= kPetLostAfterMin) {
      pet.lost = true;
      Serial.println("The pet starved too long and wandered off");
    }
  } else {
    pet.starvedMin = 0;
  }
}

// FNV-1a over the saved image; the lib owns these paths, but reading them is
// safe and lets wall mode notice when a new photo arrived.
uint32_t savedImageChecksum() {
  if (!SPIFFS.begin(true)) return 0;
  const char *path = SPIFFS.exists("/current-image.bin") ? "/current-image.bin"
                                                         : "/previous-image.bin";
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return 0;
  uint32_t hash = 2166136261u;
  uint8_t buffer[256];
  for (;;) {
    const size_t got = file.read(buffer, sizeof(buffer));
    if (got == 0) break;
    for (size_t i = 0; i < got; ++i) hash = (hash ^ buffer[i]) * 16777619u;
  }
  file.close();
  return hash;
}

// Board-side variant of the lib's restore: repaints the saved photo without
// hibernating (run()'s no-command path parks the panel afterwards).
bool restoreSavedPhoto() {
  if (!SPIFFS.begin(true)) return false;
  const char *path = SPIFFS.exists("/current-image.bin") ? "/current-image.bin"
                                                         : "/previous-image.bin";
  File file = SPIFFS.open(path, FILE_READ);
  if (!file || file.size() != size_t(48000)) {
    if (file) file.close();
    return false;
  }
  uint8_t row[100];
  for (int16_t y = 0; y < 480; ++y) {
    if (file.read(row, sizeof(row)) != sizeof(row)) {
      file.close();
      return false;
    }
    display.epd2.writeNative(row, nullptr, 0, y, 800, 1);
  }
  file.close();
  display.epd2.refresh(false);
  Serial.println("Saved photo restored");
  return true;
}

void drawPetScreen(const PetState &pet) {
  const PetStage stage = petStage(pet);
  const char *title = "LUNA THE UNICORN";
  const char *stageName = "UNICORN";
  const char *hints = "LEFT feed    GREEN menu    RIGHT play";
  if (stage == PetStage::Egg) {
    title = "A MYSTERIOUS EGG";
    stageName = "EGG";
    hints = "GREEN menu";
  } else if (stage == PetStage::Foal) {
    stageName = "FOAL";
  } else if (stage == PetStage::Celestial) {
    stageName = "CELESTIAL";
  } else if (stage == PetStage::Gone) {
    title = "LUNA IS GONE";
    stageName = "GONE";
    hints = "LEFT adopt a new egg    GREEN menu";
  }
  const int fedPct = int(pet.fedMin * 100 / kPetFedMaxMin);
  const int joyPct = int(pet.joyMin * 100 / kPetJoyMaxMin);
  const char *mood = "Luna is content";
  if (pet.fedMin == 0) mood = "Luna is starving!";
  else if (fedPct <= 25) mood = "Luna is hungry";
  else if (joyPct <= 25) mood = "Luna is bored";
  else if (fedPct >= 70 && joyPct >= 70) mood = "Luna sparkles with joy!";
  const String age = String(pet.ageMin / 1440) + "d " +
                     String((pet.ageMin % 1440) / 60) + "h";
  const String hatch =
      "Hatches in about " +
      String((kPetHatchMin - min(kPetHatchMin, pet.ageMin) + 59) / 60) + " h";
  const String fedLabel = String(fedPct) + "%";
  const String joyLabel = String(joyPct) + "%";

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText(title, 14, 4);
    display.fillRect(40, 386, 340, 3, GxEPD_BLACK);
    switch (stage) {
      case PetStage::Egg:
        drawSpriteOnGround(kSpriteEgg, 210, 384, 16);
        break;
      case PetStage::Foal:
        drawSpriteOnGround(kSpriteHorse, 210, 384, 17);
        break;
      case PetStage::Celestial:
        drawSpriteOnGround(kSpriteUnicorn, 210, 384, 18);
        drawSprite(kSpriteSparkle, 30, 110, 4);
        drawSprite(kSpriteSparkle, 344, 140, 3);
        drawSprite(kSpriteSparkle, 344, 300, 3);
        break;
      case PetStage::Gone:
        drawSpriteOnGround(kSpriteTornado, 210, 384, 17);
        break;
      default:
        drawSpriteOnGround(kSpriteUnicorn, 210, 384, 18);
        break;
    }
    display.setTextSize(3);
    display.setCursor(430, 100);
    display.print("STAGE: ");
    display.print(stageName);
    display.setTextSize(2);
    display.setCursor(430, 140);
    display.print("AGE ");
    display.print(age);
    if (stage == PetStage::Egg) {
      display.setCursor(430, 200);
      display.print(hatch);
      display.setCursor(430, 232);
      display.print("Keep it cozy...");
    } else if (stage == PetStage::Gone) {
      display.setCursor(430, 200);
      display.print("A whirlwind took Luna");
      display.setCursor(430, 232);
      display.print("away to the clouds.");
    } else {
      drawSprite(kSpriteApple, 430, 182, 4);
      drawSprite(kSpriteHeart, 430, 254, 4);
      for (int inset = 0; inset < 3; ++inset) {
        display.drawRect(490 + inset, 186 + inset, 270 - 2 * inset,
                         30 - 2 * inset, GxEPD_BLACK);
        display.drawRect(490 + inset, 250 + inset, 270 - 2 * inset,
                         30 - 2 * inset, GxEPD_BLACK);
      }
      display.fillRect(496, 192, (270 - 12) * fedPct / 100, 18, GxEPD_BLACK);
      display.fillRect(496, 256, (270 - 12) * joyPct / 100, 18, GxEPD_BLACK);
      display.setCursor(700, 224);
      display.print(fedLabel);
      display.setCursor(700, 288);
      display.print(joyLabel);
      display.setCursor(430, 330);
      display.print(mood);
    }
    centeredText(hints, 446, 2);
  } while (display.nextPage());
}

// Runs the pet screen. Returns true to go back to the menu; false when the
// idle timeout fired, which leaves the pet on the wall.
bool runPetScreen() {
  PetState pet = loadPet();
  if (!pet.adopted) {
    pet.adopted = true;
    savePet(pet);
    Serial.println("Arcade: a mysterious egg was adopted");
  }
  for (;;) {
    drawPetScreen(pet);
    for (;;) {
      const int button = waitForButton(kGameIdleMs);
      if (button < 0) {
        pet.onWall = true;
        pet.wallSum = savedImageChecksum();
        savePet(pet);
        Serial.println("Pet idle; it stays on the wall");
        return false;
      }
      waitForButtonsReleased();
      if (button == kBtnGreen) return true;
      const PetStage stage = petStage(pet);
      if (stage == PetStage::Gone) {
        if (button == kBtnLeft) {
          pet = PetState();
          pet.adopted = true;
          break;
        }
      } else if (stage != PetStage::Egg) {
        if (button == kBtnLeft) {
          pet.fedMin = min(kPetFedMaxMin, pet.fedMin + kPetFeedMin);
          pet.starvedMin = 0;
          break;
        }
        if (button == kBtnRight) {
          pet.joyMin = min(kPetJoyMaxMin, pet.joyMin + kPetPlayMin);
          break;
        }
      }
      // Ignored press (e.g. feeding the egg): keep waiting without a redraw.
    }
    savePet(pet);
  }
}

// Called on every timer wake before the shared runtime runs the HA check-in:
// ages the pet and repaints its wall screen while it still owns the wall.
void petTimerTick() {
  uint32_t wakeMinutes = 0;
  Preferences ha;  // read-only peek at the shared runtime's interval setting
  if (ha.begin("reterm-ha", true)) {
    wakeMinutes = ha.getUInt("wake_min", 0);
    ha.end();
  }
  if (wakeMinutes == 0) return;
  PetState pet = loadPet();
  if (!pet.adopted) return;
  agePet(pet, wakeMinutes);
  if (pet.onWall) {
    if (savedImageChecksum() != pet.wallSum) {
      pet.onWall = false;  // a new photo claimed the wall; let it win
      Serial.println("Pet wall mode ended: the saved image changed");
    } else {
      Serial.println("Timer wake: repainting the pet on the wall");
      drawPetScreen(pet);
    }
  }
  savePet(pet);
}

// --- Arcade menu -----------------------------------------------------------

void drawMenuScreen(int selected) {
  static constexpr const char *entries[] = {
      "ROCK PAPER SCISSORS", "LUNA THE UNICORN", "BACK TO THE PHOTO"};
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText("RETERM ARCADE", 24, 4);
    for (int entry = 0; entry < 3; ++entry) {
      const int16_t y = 116 + entry * 84;
      if (entry == selected) {
        display.fillRoundRect(150, y, 510, 64, 12, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
      } else {
        display.drawRoundRect(150, y, 510, 64, 12, GxEPD_BLACK);
        display.drawRoundRect(151, y + 1, 508, 62, 12, GxEPD_BLACK);
      }
      cellText(entries[entry], 405, y + 20, 3);
      display.setTextColor(GxEPD_BLACK);
      if (entry == 0) drawScissorsIcon(110, y + 32, 52);
      else if (entry == 1) drawSprite(kSpriteUnicorn, 84, y + 8, 3);
      else drawSprite(kSpriteFrame, 86, y + 8, 3);
    }
    centeredText("LEFT down    GREEN choose    RIGHT up", 436, 2);
  } while (display.nextPage());
}

void runArcade() {
  Serial.println("White-button wake: arcade menu");
  waitForButtonsReleased();
  int selected = 0;
  for (;;) {
    drawMenuScreen(selected);
    const int button = waitForButton(kGameIdleMs);
    waitForButtonsReleased();
    if (button == kBtnLeft) {
      selected = (selected + 1) % 3;
      continue;
    }
    if (button == kBtnRight) {
      selected = (selected + 2) % 3;
      continue;
    }
    if (button == kBtnGreen) {
      if (selected == 0) {
        // RPS leaves its result screen up; a wall-mode pet reclaims the
        // panel on the next timer wake.
        playRockPaperScissors();
        return;
      }
      if (selected == 1) {
        if (runPetScreen()) continue;  // green: back to the menu
        return;                        // idle: pet stays on the wall
      }
      PetState pet = loadPet();
      if (pet.onWall) {
        pet.onWall = false;
        savePet(pet);
      }
      if (!restoreSavedPhoto())
        Serial.println("No saved image available to restore");
      return;
    }
    // Menu idle: never leave the menu as wallpaper.
    const PetState pet = loadPet();
    if (pet.adopted && pet.onWall) drawPetScreen(pet);
    else if (!restoreSavedPhoto())
      Serial.println("No saved image available to restore");
    return;
  }
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
    // Either white button wakes into the arcade. Hold the RTC pullups so the
    // pins cannot float low once the digital domain powers down.
    esp_sleep_enable_ext1_wakeup(kGameButtonMask, ESP_EXT1_WAKEUP_ANY_LOW);
    rtc_gpio_pullup_en(gpio_num_t(kLeftButton));
    rtc_gpio_pulldown_dis(gpio_num_t(kLeftButton));
    rtc_gpio_pullup_en(gpio_num_t(kRightButton));
    rtc_gpio_pulldown_dis(gpio_num_t(kRightButton));
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
  // Board-side hooks run before the shared runtime: a timer wake ages the
  // tamagotchi (and repaints its wall screen), a white-button wake opens the
  // arcade. Both fall through into run() with the buttons released, so it
  // takes its normal path — check-in on timer wakes, the no-command
  // hibernate-and-sleep otherwise.
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) petTimerTick();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) runArcade();
  reterm::run(board);
}

void loop() { delay(1000); }
