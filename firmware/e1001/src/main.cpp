// reTerminal E1001 board support: the 7.5-inch 800x480 monochrome panel and
// the front buttons. The upload session, UART protocol, provisioning, and
// persistence all live in the shared reterm lib; images travel as 1bpp packed
// rows (bit set = white, MSB is the leftmost pixel), 48,000 bytes per screen.
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Preferences.h>
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

int pressedMove() {
  if (digitalRead(kLeftButton) == LOW) return 0;
  if (digitalRead(kGreenButton) == LOW) return 1;
  if (digitalRead(kRightButton) == LOW) return 2;
  return -1;
}

// Waits until every button has read released for 60 ms so the wake press (or
// a rematch press) cannot leak into the next prompt. The deadline guards
// against a stuck button pinning the game loop forever.
void waitForButtonsReleased() {
  const uint32_t deadline = millis() + 10000;
  uint32_t stableSince = millis();
  while (int32_t(deadline - millis()) > 0) {
    if (pressedMove() >= 0) stableSince = millis();
    else if (millis() - stableSince >= 60) return;
    delay(10);
  }
}

int waitForMove(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (int32_t(deadline - millis()) > 0) {
    const int move = pressedMove();
    if (move >= 0) {
      delay(30);  // debounce: accept only if the same press is still down
      if (pressedMove() == move) return move;
    }
    delay(10);
  }
  return -1;
}

void playRockPaperScissors() {
  Serial.println("White-button wake: rock paper scissors");
  RpsScore score = loadRpsScore();
  waitForButtonsReleased();
  drawChoiceScreen(score);
  for (int player = waitForMove(kGameIdleMs); player >= 0;
       player = waitForMove(kGameIdleMs)) {
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
    // Either white button wakes into the game. IDF 4.4 still names mode 0
    // ALL_LOW, but on the S3 the hardware wakes when ANY masked pin goes low
    // (IDF 5 renames it ESP_EXT1_WAKEUP_ANY_LOW). Hold the RTC pullups so the
    // pins cannot float low once the digital domain powers down.
    esp_sleep_enable_ext1_wakeup(kGameButtonMask, ESP_EXT1_WAKEUP_ALL_LOW);
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
  // A white-button wake plays rock paper scissors first, then falls into the
  // shared runtime: buttons are released by then, so run() takes its
  // no-command path, which hibernates the panel and deep sleeps.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1)
    playRockPaperScissors();
  reterm::run(board);
}

void loop() { delay(1000); }
