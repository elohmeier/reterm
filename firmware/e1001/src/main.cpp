// reTerminal E1001 board support: the 7.5-inch 800x480 monochrome panel and
// the front buttons. The upload session, UART protocol, provisioning, and
// persistence all live in the shared reterm lib; images travel as 1bpp packed
// rows (bit set = white, MSB is the leftmost pixel), 48,000 bytes per screen.
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <initializer_list>
#include <GxEPD2_BW.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <qrcode.h>
#include <reterm.h>
#include <soc/gpio_sd_reg.h>
#include <soc/soc.h>

namespace {
constexpr int kSck = 7;
constexpr int kMosi = 9;
constexpr int kCs = 10;
constexpr int kDc = 11;
constexpr int kReset = 12;
constexpr int kBusy = 13;
// Three active-low top buttons with board pullups, left to right: white
// (GPIO5), white (GPIO4), green (GPIO3). The cluster sits right of center
// above the panel (user-measured cap centers over the 16 cm drawable width:
// 8.8 / 9.7 / 10.3 cm, i.e. screen columns 440 / 485 / 515). The green
// button stays the EXT0 photo-session wake like the stock firmware; the two
// white buttons are an EXT1 wake into the arcade.
constexpr gpio_num_t kGreenButton = GPIO_NUM_3;
constexpr int kLeftButton = 5;
constexpr int kRightButton = 4;
constexpr int kButtonPins[] = {3, 4, 5};
constexpr uint64_t kGameButtonMask =
    (1ULL << kLeftButton) | (1ULL << kRightButton);

SPIClass epaperSpi(HSPI);

// Exposes the protected UC8179 command plumbing so the video player can run
// encoder-supplied panel scripts (init, fast-LUT waveform, ghost cleanup)
// and stream full frames without the driver's per-image windowing overhead.
class VideoPanel final : public GxEPD2_750_GDEY075T7 {
 public:
  VideoPanel(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_750_GDEY075T7(cs, dc, rst, busy) {}
  // Script format: {cmd, len, data...}*. Power-on and refresh commands
  // busy-wait automatically so scripts stay declarative.
  void runScript(const uint8_t *script, size_t length) {
    for (size_t i = 0; i + 2 <= length;) {
      const uint8_t cmd = script[i++];
      const uint8_t count = script[i++];
      if (i + count > length) return;
      _writeCommand(cmd);
      for (uint8_t k = 0; k < count; ++k) _writeData(script[i + k]);
      i += count;
      if (cmd == 0x04) _waitWhileBusy("script power on", 300);
      if (cmd == 0x12) _waitWhileBusy("script refresh", 5000);
    }
  }
  void writeVideoFrame(const uint8_t *data, size_t length) {
    _writeCommand(0x13);  // new data RAM; N2OCP copies it to old on refresh
    _startTransfer();
    _pSPIx->writeBytes(data, length);
    _endTransfer();
  }
  // Returns the measured busy time in milliseconds.
  uint32_t refreshVideo(uint16_t timeoutMs) {
    const uint32_t started = millis();
    _writeCommand(0x12);
    _waitWhileBusy(nullptr, timeoutMs);
    return millis() - started;
  }
  // Windowed variants: writing and driving only the dirty rows keeps the
  // transfer cost proportional to the changed region. `command` selects the
  // RAM plane: 0x13 = new data, 0x10 = old data — writing a spoofed old
  // plane forces the next refresh to re-drive chosen pixels even when the
  // new data did not change (the ghost-trail scrubber relies on this).
  void writeVideoWindow(uint8_t command, const uint8_t *rowsData,
                        uint16_t yStart, uint16_t rows) {
    _writeCommand(0x91);  // partial in
    setWindow(0, yStart, WIDTH, rows);
    _writeCommand(command);
    _startTransfer();
    _pSPIx->writeBytes(rowsData, size_t(rows) * (WIDTH / 8));
    _endTransfer();
    _writeCommand(0x92);  // partial out
  }
  uint32_t refreshRows(uint16_t yStart, uint16_t rows, uint16_t timeoutMs) {
    const uint32_t started = millis();
    _writeCommand(0x91);
    setWindow(0, yStart, WIDTH, rows);
    _writeCommand(0x12);
    _waitWhileBusy(nullptr, timeoutMs);
    _writeCommand(0x92);
    return millis() - started;
  }

 private:
  // Mirrors the driver's private _setPartialRamArea.
  void setWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const uint16_t xe = (x + w - 1) | 0x0007;
    const uint16_t ye = y + h - 1;
    x &= 0xFFF8;
    _writeCommand(0x90);  // partial window
    _writeData(x / 256);
    _writeData(x % 256);
    _writeData(xe / 256);
    _writeData(xe % 256);
    _writeData(y / 256);
    _writeData(y % 256);
    _writeData(ye / 256);
    _writeData(ye % 256);
    _writeData(0x01);
  }
};

GxEPD2_BW<VideoPanel, 160> display(VideoPanel(kCs, kDc, kReset, kBusy));

// Center text drawn with the default 6x8 GFX font at the given size. The
// Adafruit_GFX target lets the same helpers draw to the panel or to an
// off-screen GFXcanvas1 (whose 1bpp buffer is the panel wire format).
void centeredText(Adafruit_GFX &g, const char *text, int16_t y, uint8_t size) {
  g.setTextSize(size);
  const int16_t width = int16_t(strlen(text)) * 6 * size;
  g.setCursor((800 - width) / 2, y);
  g.print(text);
}

void centeredText(const char *text, int16_t y, uint8_t size) {
  centeredText(display, text, y, size);
}

// Same, but centered on an arbitrary x instead of the panel midline.
void cellText(Adafruit_GFX &g, const char *text, int16_t cx, int16_t y,
              uint8_t size) {
  g.setTextSize(size);
  g.setCursor(cx - int16_t(strlen(text)) * 3 * size, y);
  g.print(text);
}

void cellText(const char *text, int16_t cx, int16_t y, uint8_t size) {
  cellText(display, text, cx, y, size);
}

void drawMessageScreen(const char *line1, const char *line2) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText(line1, 200, 3);
    centeredText(line2, 248, 2);
  } while (display.nextPage());
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
    centeredText("Scan within five minutes to send a photo", 414, 2);
    centeredText("Hold the GREEN button for the menu instead", 446, 2);
  } while (display.nextPage());
}

// --- Piezo audio -----------------------------------------------------------
// Sampled audio through the piezo: the S3's sigma-delta modulator turns an
// 8-bit duty into a high-frequency 1-bit density stream; updating the duty
// at the PCM sample rate from a hardware-timer ISR plays real audio. The
// ISR is a single MMIO write (no function calls), so it stays safe even
// while flash cache is briefly disabled. Shared by the video player's
// interleaved PCM track and the arcade's synthesized sound effects.

constexpr int kBuzzerPin = 45;
constexpr size_t kAudioRingSize = 32768;  // power of two, ~2 s at 16 kHz

struct AudioState {
  uint8_t *ring;
  volatile uint32_t head;
  volatile uint32_t tail;
  volatile uint32_t underruns;
  uint32_t regBase;  // sigma-delta register with the duty bits cleared
};

AudioState g_pcm = {nullptr, 0, 0, 0, 0};
hw_timer_t *g_pcmTimer = nullptr;

void IRAM_ATTR audioIsr() {
  if (g_pcm.tail != g_pcm.head) {
    const uint8_t sample = g_pcm.ring[g_pcm.tail];
    g_pcm.tail = (g_pcm.tail + 1) & (kAudioRingSize - 1);
    // The hardware wants a signed density; sample^0x80 is (sample-128) in
    // two's complement.
    REG_WRITE(GPIO_SIGMADELTA0_REG, g_pcm.regBase | (sample ^ 0x80));
  } else {
    ++g_pcm.underruns;
    REG_WRITE(GPIO_SIGMADELTA0_REG, g_pcm.regBase);  // midpoint = silence
  }
}

size_t audioRingFree() {
  return kAudioRingSize - 1 - ((g_pcm.head - g_pcm.tail) & (kAudioRingSize - 1));
}

void audioRingPush(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const uint32_t next = (g_pcm.head + 1) & (kAudioRingSize - 1);
    if (next == g_pcm.tail) return;  // full: drop the tail of the chunk
    g_pcm.ring[g_pcm.head] = data[i];
    g_pcm.head = next;
  }
}

// Starts the sample clock; whatever the ring already holds plays first. The
// caller owns g_pcm.ring and the head/tail indices.
bool audioStart(uint32_t sampleRate) {
  if (!g_pcm.ring || sampleRate == 0 || 16000000u % sampleRate != 0)
    return false;
  g_pcm.underruns = 0;
  sigmaDeltaSetup(kBuzzerPin, 0, 312500);
  g_pcm.regBase = REG_READ(GPIO_SIGMADELTA0_REG) & ~0xFFu;
  g_pcmTimer = timerBegin(1, 5, true);  // 16 MHz base
  timerAttachInterrupt(g_pcmTimer, &audioIsr, true);
  timerAlarmWrite(g_pcmTimer, 16000000u / sampleRate, true);
  timerAlarmEnable(g_pcmTimer);
  return true;
}

void audioStop() {
  if (g_pcmTimer) {
    timerAlarmDisable(g_pcmTimer);
    timerEnd(g_pcmTimer);
    g_pcmTimer = nullptr;
  }
  sigmaDeltaDetachPin(kBuzzerPin);
  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);
}

// --- Arcade sound effects --------------------------------------------------
// Chiptune blips synthesized straight into the audio ring; the ISR plays
// them while the code draws and the panel refreshes, so jingles overlap the
// slow e-paper updates for free. Square waves for the bright UI ticks,
// triangles for the softer sad notes; every tone decays linearly so nothing
// clicks or drones on the piezo.

constexpr uint32_t kSfxRate = 16000;

bool sfxBegin() {
  if (g_pcm.ring) return true;
  g_pcm.ring = (uint8_t *)malloc(kAudioRingSize);
  if (!g_pcm.ring) return false;
  g_pcm.head = g_pcm.tail = 0;
  if (!audioStart(kSfxRate)) {
    free(g_pcm.ring);
    g_pcm.ring = nullptr;
    return false;
  }
  return true;
}

void sfxEnd() {
  if (!g_pcm.ring) return;
  const uint32_t start = millis();  // let the last stinger ring out
  while (g_pcm.tail != g_pcm.head && millis() - start < 800) delay(10);
  audioStop();
  free(g_pcm.ring);
  g_pcm.ring = nullptr;
}

void sfxTone(uint16_t freqHz, uint16_t ms, uint8_t peak, bool mellow) {
  if (!g_pcm.ring) return;
  const uint32_t total = kSfxRate * uint32_t(ms) / 1000;
  const uint32_t period = kSfxRate / freqHz;
  uint8_t chunk[128];
  size_t fill = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const int32_t amp = int32_t(peak) * int32_t(total - i) / int32_t(total);
    const uint32_t pos = i % period;
    int32_t v;
    if (mellow) {
      const int32_t x = int32_t(pos * 4 * uint32_t(amp) / period);
      v = x < 2 * amp ? x - amp : 3 * amp - x;
    } else {
      v = pos < period / 2 ? amp : -amp;
    }
    chunk[fill++] = uint8_t(128 + v);
    if (fill == sizeof(chunk)) {
      audioRingPush(chunk, fill);
      fill = 0;
    }
  }
  if (fill) audioRingPush(chunk, fill);
}

void sfxRest(uint16_t ms) {
  if (!g_pcm.ring) return;
  uint8_t chunk[64];
  memset(chunk, 0x80, sizeof(chunk));
  for (uint32_t left = kSfxRate * uint32_t(ms) / 1000; left;) {
    const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
    audioRingPush(chunk, n);
    left -= n;
  }
}

void sfxPress() { sfxTone(1319, 35, 70, false); }

void sfxTick(int beat) {
  static const uint16_t kTickHz[] = {659, 784, 988};  // rising chant
  sfxTone(kTickHz[beat], 50, 70, false);
}

void sfxShoot() {
  sfxTone(1047, 45, 80, false);
  sfxTone(1568, 110, 80, false);
}

void sfxWin() {
  sfxTone(523, 80, 85, false);
  sfxTone(659, 80, 85, false);
  sfxTone(784, 80, 85, false);
  sfxTone(1047, 220, 90, false);
}

void sfxLose() {
  sfxTone(392, 130, 85, true);
  sfxTone(330, 130, 85, true);
  sfxTone(262, 260, 85, true);
}

void sfxDraw() {
  sfxTone(880, 70, 70, false);
  sfxRest(50);
  sfxTone(880, 70, 70, false);
}

// --- Rock, paper, scissors -------------------------------------------------
// Played against the hardware RNG, with an animated chant on the panel's
// fast register-LUT waveform and chiptune blips through the piezo. The
// three top buttons sit in a tight cluster right of center (see the pin
// comment up top), so the play screen anchors a legend under the real caps
// and the moves follow the physical order: left white = rock, right white =
// paper, green = scissors. The lifetime score persists in its own NVS
// namespace; the panel keeps whatever screen was up last, and the next
// photo session or MQTT image replaces it as usual.

constexpr uint32_t kGameIdleMs = 60 * 1000;
constexpr const char *kMoveNames[] = {"ROCK", "PAPER", "SCISSORS"};
constexpr const char *kWinReasons[] = {"Rock blunts scissors",
                                       "Paper wraps rock",
                                       "Scissors cut paper"};

// Screen columns under the measured button cap centers, and the wider fan
// the legend icons spread to so they stay readable (the caps sit only 45
// and 30 px apart).
constexpr int16_t kBtnCols[] = {440, 485, 515};
constexpr int16_t kLegendCols[] = {400, 485, 570};

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

void thickLine(Adafruit_GFX &g, float x0, float y0, float x1, float y1,
               float thickness, uint16_t color) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  const float ox = -dy * thickness / (2 * len);
  const float oy = dx * thickness / (2 * len);
  const int16_t ax = lroundf(x0 + ox), ay = lroundf(y0 + oy);
  const int16_t bx = lroundf(x0 - ox), by = lroundf(y0 - oy);
  const int16_t cx = lroundf(x1 + ox), cy = lroundf(y1 + oy);
  const int16_t ex = lroundf(x1 - ox), ey = lroundf(y1 - oy);
  g.fillTriangle(ax, ay, bx, by, cx, cy, color);
  g.fillTriangle(bx, by, ex, ey, cx, cy, color);
}

// A faceted boulder: a filled polygon fan with white crack lines meeting the
// silhouette at its corners so they read as facet edges.
void drawRockIcon(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t s) {
  const auto px = [&](float u) { return int16_t(lroundf(cx + u * s / 100)); };
  const auto py = [&](float v) { return int16_t(lroundf(cy + v * s / 100)); };
  static constexpr int8_t outline[][2] = {{-44, 34}, {-50, 4},  {-34, -24},
                                          {-10, -42}, {22, -38}, {44, -16},
                                          {48, 16},  {32, 40}};
  constexpr size_t n = sizeof(outline) / sizeof(outline[0]);
  for (size_t i = 1; i + 1 < n; ++i) {
    g.fillTriangle(px(outline[0][0]), py(outline[0][1]),
                   px(outline[i][0]), py(outline[i][1]),
                   px(outline[i + 1][0]), py(outline[i + 1][1]),
                   GxEPD_BLACK);
  }
  const auto crack = [&](float x0, float y0, float x1, float y1) {
    g.drawLine(px(x0), py(y0), px(x1), py(y1), GxEPD_WHITE);
    g.drawLine(px(x0) + 1, py(y0), px(x1) + 1, py(y1), GxEPD_WHITE);
    g.drawLine(px(x0), py(y0) + 1, px(x1), py(y1) + 1, GxEPD_WHITE);
  };
  crack(-10, -42, -4, -2);
  crack(-4, -2, -44, 34);
  crack(-4, -2, 48, 16);
}

// A ruled sheet with a dog-eared top-right corner.
void drawPaperIcon(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t s) {
  const int16_t w = s * 68 / 100;
  const int16_t h = s * 88 / 100;
  const int16_t x0 = cx - w / 2;
  const int16_t y0 = cy - h / 2;
  const int16_t fold = s * 26 / 100;
  int16_t border = s / 45;
  if (border < 3) border = 3;
  g.fillRect(x0, y0, w, h, GxEPD_BLACK);
  g.fillRect(x0 + border, y0 + border, w - 2 * border, h - 2 * border,
             GxEPD_WHITE);
  // Cut the corner outside the fold, then draw the turned-down flap; its
  // hypotenuse doubles as the cut edge.
  g.fillTriangle(x0 + w - fold, y0, x0 + w - 1, y0, x0 + w - 1,
                 y0 + fold, GxEPD_WHITE);
  g.fillTriangle(x0 + w - fold, y0, x0 + w - 1, y0 + fold, x0 + w - fold,
                 y0 + fold, GxEPD_BLACK);
  int16_t rule = s / 55;
  if (rule < 2) rule = 2;
  for (int i = 0; i < 4; ++i) {
    const int16_t y = y0 + fold + s * 8 / 100 + i * (s * 12 / 100);
    g.fillRect(x0 + s * 12 / 100, y,
               w - 2 * (s * 12 / 100) - (i == 3 ? w / 4 : 0), rule,
               GxEPD_BLACK);
  }
}

// Open scissors: two blade-to-handle strips crossing at a riveted pivot,
// pointed blades up, ringed handles down.
void drawScissorsIcon(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t s) {
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
    g.fillTriangle(lroundf(tipX), lroundf(tipY), lroundf(pivotX + ox),
                   lroundf(pivotY + oy), lroundf(pivotX - ox),
                   lroundf(pivotY - oy), GxEPD_BLACK);
    thickLine(g, pivotX, pivotY, ringX, ringY, s * 0.07f, GxEPD_BLACK);
    g.fillCircle(lroundf(ringX), lroundf(ringY), lroundf(s * 0.16f),
                 GxEPD_BLACK);
    g.fillCircle(lroundf(ringX), lroundf(ringY), lroundf(s * 0.09f),
                 GxEPD_WHITE);
  }
  g.fillCircle(lroundf(pivotX), lroundf(pivotY), lroundf(s * 0.05f),
               GxEPD_BLACK);
  g.fillCircle(lroundf(pivotX), lroundf(pivotY), lroundf(s * 0.02f),
               GxEPD_WHITE);
}

void drawMoveIcon(Adafruit_GFX &g, int move, int16_t cx, int16_t cy,
                  int16_t s) {
  if (move == 0) drawRockIcon(g, cx, cy, s);
  else if (move == 1) drawPaperIcon(g, cx, cy, s);
  else drawScissorsIcon(g, cx, cy, s);
}

String scoreLine(const RpsScore &score) {
  return "You " + String(score.wins) + " : " + String(score.losses) +
         " reTerminal   (" + String(score.draws) + " draws)";
}

// Button indices; kBtnLeft/kBtnRight are the adjacent white pair, green
// sits to their right.
constexpr int kBtnLeft = 0;
constexpr int kBtnGreen = 1;
constexpr int kBtnRight = 2;

// Moves follow the physical cap order (left white, right white, green), so
// the on-screen legend reads rock-paper-scissors straight across.
int moveForButton(int button) {
  if (button == kBtnLeft) return 0;   // rock
  if (button == kBtnRight) return 1;  // paper
  return 2;                           // green throws scissors
}

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

// Green distinguishes a tap (reported on release) from a hold: holding for
// kLongPressMs is the universal "open the menu" gesture, so green actions
// inside games stay on the tap. The white buttons keep firing on the press
// itself for snappy game moves.
constexpr uint32_t kLongPressMs = 900;

struct ButtonPress {
  int button;  // -1 = timeout
  bool longPress;
};

ButtonPress waitForPress(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (int32_t(deadline - millis()) > 0) {
    const int button = readButton();
    if (button < 0) {
      delay(10);
      continue;
    }
    delay(30);  // debounce: accept only if the same press is still down
    if (readButton() != button) continue;
    if (button != kBtnGreen) return {button, false};
    const uint32_t pressedAt = millis();
    while (digitalRead(kGreenButton) == LOW) {
      if (millis() - pressedAt >= kLongPressMs) return {kBtnGreen, true};
      delay(10);
    }
    return {kBtnGreen, false};
  }
  return {-1, false};
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

void drawSpriteImpl(Adafruit_GFX &g, const char *const rows[], int count,
                    int16_t x, int16_t y, int16_t scale) {
  for (int j = 0; j < count; ++j) {
    for (int i = 0; rows[j][i]; ++i) {
      const char cell = rows[j][i];
      if (cell == '.') continue;
      g.fillRect(x + i * scale, y + j * scale, scale, scale,
                 cell == 'X' ? GxEPD_BLACK : GxEPD_WHITE);
    }
  }
}

template <size_t N>
void drawSprite(Adafruit_GFX &g, const char *const (&rows)[N], int16_t x,
                int16_t y, int16_t scale) {
  drawSpriteImpl(g, rows, int(N), x, y, scale);
}

template <size_t N>
void drawSprite(const char *const (&rows)[N], int16_t x, int16_t y,
                int16_t scale) {
  drawSpriteImpl(display, rows, int(N), x, y, scale);
}

// Centers the sprite on cx with its feet on the ground line.
template <size_t N>
void drawSpriteOnGround(const char *const (&rows)[N], int16_t cx,
                        int16_t ground, int16_t scale) {
  const int16_t width = int16_t(strlen(rows[0])) * scale;
  drawSpriteImpl(display, rows, int(N), cx - width / 2,
                 ground - int16_t(N) * scale, scale);
}

// --- Rock, paper, scissors: screens and rounds -----------------------------
// The whole game renders into one full-screen GFXcanvas1 whose 1bpp buffer
// is exactly the panel wire format (bit set = white, MSB leftmost), so a
// frame can go out either through a stock full refresh or through the fast
// register-LUT window path the video player uses. Static header and hints
// live outside the arena band; every animated element stays inside it, so
// partial refreshes never leave stale chrome behind.

constexpr int16_t kArenaY = 104;   // the animated band: rows 104..423
constexpr int16_t kArenaH = 320;
constexpr int16_t kPlayerX = 210;
constexpr int16_t kMachineX = 590;

// Small icons at the physical button positions: a tab silhouette per real
// cap (whites hollow, the green cap solid and slimmer, like the hardware),
// a stem fanning out to an icon box, and the move icon with its name.
void drawButtonLegend(Adafruit_GFX &g) {
  for (int move = 0; move < 3; ++move) {
    const int16_t bx = kBtnCols[move];
    const int16_t cx = kLegendCols[move];
    if (move == 2) {
      g.fillRoundRect(bx - 12, 0, 24, 11, 4, GxEPD_BLACK);
    } else {
      g.drawRoundRect(bx - 15, 0, 30, 11, 4, GxEPD_BLACK);
      g.drawRoundRect(bx - 14, 1, 28, 9, 3, GxEPD_BLACK);
    }
    thickLine(g, bx, 12, cx, 26, 3, GxEPD_BLACK);
    g.drawRoundRect(cx - 24, 26, 48, 48, 8, GxEPD_BLACK);
    drawMoveIcon(g, move, cx, 50, 40);
    cellText(g, kMoveNames[move], cx, 78, 1);
  }
}

void clearArena(Adafruit_GFX &g) {
  g.fillRect(0, kArenaY, 800, kArenaH, GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);
}

// Idle arena: two closed fists (the rock silhouette) facing off.
void drawArenaIdle(Adafruit_GFX &g, const RpsScore &score) {
  clearArena(g);
  cellText(g, "YOU", kPlayerX, 128, 2);
  cellText(g, "RETERMINAL", kMachineX, 128, 2);
  drawRockIcon(g, kPlayerX, 250, 150);
  drawRockIcon(g, kMachineX, 250, 150);
  cellText(g, "VS", 400, 234, 4);
  centeredText(g, "PRESS A BUTTON TO THROW", 356, 3);
  centeredText(g, scoreLine(score).c_str(), 398, 2);
}

// One chant frame: both fists pump down on each spoken word.
void drawArenaCountdown(Adafruit_GFX &g, const RpsScore &score, int beat,
                        bool down) {
  clearArena(g);
  cellText(g, "YOU", kPlayerX, 128, 2);
  cellText(g, "RETERMINAL", kMachineX, 128, 2);
  const int16_t cy = down ? 268 : 240;
  drawRockIcon(g, kPlayerX, cy, 150);
  drawRockIcon(g, kMachineX, cy, 150);
  cellText(g, kMoveNames[beat], 400, 150, 4);
  centeredText(g, scoreLine(score).c_str(), 398, 2);
}

// The reveal: both icons pop in small, then full size with the verdict.
// diff is (player - machine + 3) % 3: 1 = player won, 2 = machine won.
void drawArenaReveal(Adafruit_GFX &g, const RpsScore &score, int player,
                     int machine, int diff, bool full) {
  clearArena(g);
  drawMoveIcon(g, player, kPlayerX, 265, full ? 180 : 120);
  drawMoveIcon(g, machine, kMachineX, 265, full ? 180 : 120);
  cellText(g, "VS", 400, 250, 4);
  if (!full) return;
  const char *verdict = diff == 1   ? "YOU WIN!"
                        : diff == 2 ? "RETERMINAL WINS"
                                    : "IT'S A DRAW";
  centeredText(g, verdict, 118, 4);
  if (diff != 0) {
    const int16_t winnerX = diff == 1 ? kPlayerX : kMachineX;
    for (int16_t inset = 0; inset < 3; ++inset) {
      g.drawRoundRect(winnerX - 118 + inset, 168 + inset, 236 - 2 * inset,
                      194 - 2 * inset, 14, GxEPD_BLACK);
    }
    if (diff == 1) {
      drawSprite(g, kSpriteSparkle, kPlayerX - 172, 180, 3);
      drawSprite(g, kSpriteSparkle, kPlayerX + 130, 300, 3);
    }
  }
  const char *reason = diff == 0 ? "Same move - nobody scores"
                                 : kWinReasons[diff == 1 ? player : machine];
  centeredText(g, reason, 372, 2);
  centeredText(g, scoreLine(score).c_str(), 398, 2);
}

void drawPlayScreen(Adafruit_GFX &g, const RpsScore &score) {
  g.fillScreen(GxEPD_WHITE);
  g.setTextColor(GxEPD_BLACK);
  g.setTextSize(3);
  g.setCursor(20, 16);
  g.print("ROCK PAPER SCISSORS");
  g.setTextSize(2);
  g.setCursor(20, 56);
  g.print("Beat the hardware dice");
  drawButtonLegend(g);
  drawArenaIdle(g, score);
  centeredText(g, "Each top button throws the move shown beneath it", 430, 2);
  centeredText(g, "Hold GREEN for the menu - a minute idle sleeps", 452, 2);
}

// Fast partial refreshes for the chant: the same UC8179 register-LUT trick
// the video player uses, with the proven stream settings baked in (five
// phases of one 28 ms frame, direct drive, ~175 ms per refresh); see
// tools/encode-video.py build_scripts for the register map. Ghosting from
// the weak waveform is flushed by the stock full refresh that closes every
// round.

uint8_t g_gameInit[48];
uint8_t g_gameLut[280];
size_t g_gameInitLen = 0, g_gameLutLen = 0;

void putScript(uint8_t *buf, size_t &at, uint8_t cmd,
               std::initializer_list<uint8_t> data) {
  buf[at++] = cmd;
  buf[at++] = uint8_t(data.size());
  for (uint8_t b : data) buf[at++] = b;
}

void putLut(uint8_t *buf, size_t &at, uint8_t reg, uint8_t level) {
  buf[at++] = reg;
  buf[at++] = 42;
  uint8_t lut[42] = {level, 1, 1, 1, 1, 1};
  memcpy(buf + at, lut, sizeof(lut));
  at += sizeof(lut);
}

void buildGameScripts() {
  if (g_gameInitLen) return;
  size_t at = 0;
  putScript(g_gameInit, at, 0x00, {0x1f});  // panel setting, OTP full LUT
  putScript(g_gameInit, at, 0x01, {0x07, 0x07, 0x3f, 0x3f, 0x09});  // power
  putScript(g_gameInit, at, 0x06, {0x17, 0x17, 0x28, 0x17});  // booster
  putScript(g_gameInit, at, 0x61, {0x03, 0x20, 0x01, 0xe0});  // 800x480
  putScript(g_gameInit, at, 0x15, {0x00});        // DUSPI off
  putScript(g_gameInit, at, 0x50, {0x29, 0x07});  // VCOM/CDI, N2OCP
  putScript(g_gameInit, at, 0x60, {0x22});        // TCON
  putScript(g_gameInit, at, 0xE3, {0x22});        // PWS
  putScript(g_gameInit, at, 0x04, {});            // power on (busy-waits)
  g_gameInitLen = at;
  at = 0;
  putScript(g_gameLut, at, 0x00, {0x3f});         // LUTs from registers
  putScript(g_gameLut, at, 0x82, {0x30});         // VCOM DC -2.5 V
  putScript(g_gameLut, at, 0x50, {0x39, 0x07});   // LUTBD, N2OCP
  putLut(g_gameLut, at, 0x20, 0x00);
  putLut(g_gameLut, at, 0x21, 0x00);
  putLut(g_gameLut, at, 0x22, 0xAA);  // black -> white, direct drive
  putLut(g_gameLut, at, 0x23, 0x55);  // white -> black, direct drive
  putLut(g_gameLut, at, 0x24, 0x00);
  putLut(g_gameLut, at, 0x25, 0x00);
  g_gameLutLen = at;
}

// Stock full refresh of a canvas frame; also flushes fast-mode ghosting.
void showCanvas(const uint8_t *screen) {
  display.epd2.selectSPI(epaperSpi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  display.epd2.writeNative(screen, nullptr, 0, 0, 800, 480, false, false,
                           false);
  display.epd2.refresh(false);
}

// Swaps the panel into register-LUT fast mode with both RAM planes seeded
// from the currently displayed screen, so the first partial refresh
// transitions from the true panel state.
void enterGameFast(const uint8_t *screen) {
  buildGameScripts();
  display.epd2.selectSPI(epaperSpi,
                         SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  display.epd2.runScript(g_gameInit, g_gameInitLen);
  display.epd2.runScript(g_gameLut, g_gameLutLen);
  display.epd2.writeVideoWindow(0x10, screen, 0, 480);
  display.epd2.writeVideoWindow(0x13, screen, 0, 480);
}

void pushArena(const uint8_t *screen) {
  display.epd2.writeVideoWindow(0x13, screen + size_t(kArenaY) * 100, kArenaY,
                                kArenaH);
  display.epd2.refreshRows(kArenaY, kArenaH, 1200);
}

// One full round: the ro-sham-bo chant on the fast waveform, the reveal,
// then a crisp stock refresh of the verdict while the stinger plays.
void playRpsRound(GFXcanvas1 &canvas, const RpsScore &score, int player,
                  int machine, int diff) {
  enterGameFast(canvas.getBuffer());
  for (int beat = 0; beat < 3; ++beat) {
    for (int up = 0; up < 2; ++up) {
      if (!up) sfxTick(beat);
      drawArenaCountdown(canvas, score, beat, up == 0);
      pushArena(canvas.getBuffer());
    }
  }
  sfxShoot();
  drawArenaReveal(canvas, score, player, machine, diff, false);
  pushArena(canvas.getBuffer());
  drawArenaReveal(canvas, score, player, machine, diff, true);
  pushArena(canvas.getBuffer());
  if (diff == 1) sfxWin();
  else if (diff == 2) sfxLose();
  else sfxDraw();
  showCanvas(canvas.getBuffer());
}

// Returns true when the player held green to go back to the menu; false on
// the idle timeout, which leaves the last screen on the panel.
bool playRockPaperScissors() {
  Serial.println("Arcade: rock paper scissors");
  RpsScore score = loadRpsScore();
  GFXcanvas1 *canvas = new GFXcanvas1(800, 480);
  if (!canvas || !canvas->getBuffer()) {
    delete canvas;
    Serial.println("RPS: no memory for the frame canvas");
    return false;
  }
  sfxBegin();
  waitForButtonsReleased();
  drawPlayScreen(*canvas, score);
  showCanvas(canvas->getBuffer());
  bool toMenu = false;
  for (;;) {
    const ButtonPress press = waitForPress(kGameIdleMs);
    if (press.button < 0) {
      Serial.println("Game idle; handing back to the standard flow");
      break;
    }
    if (press.button == kBtnGreen && press.longPress) {
      toMenu = true;
      break;
    }
    sfxPress();
    const int player = moveForButton(press.button);
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
    playRpsRound(*canvas, score, player, machine, diff);
    waitForButtonsReleased();
  }
  sfxEnd();
  delete canvas;
  waitForButtonsReleased();
  return toMenu;
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
      const ButtonPress press = waitForPress(kGameIdleMs);
      if (press.button < 0) {
        pet.onWall = true;
        pet.wallSum = savedImageChecksum();
        savePet(pet);
        Serial.println("Pet idle; it stays on the wall");
        return false;
      }
      waitForButtonsReleased();
      if (press.button == kBtnGreen) return true;  // tap or hold: the menu
      const PetStage stage = petStage(pet);
      if (stage == PetStage::Gone) {
        if (press.button == kBtnLeft) {
          pet = PetState();
          pet.adopted = true;
          break;
        }
      } else if (stage != PetStage::Egg) {
        if (press.button == kBtnLeft) {
          pet.fedMin = min(kPetFedMaxMin, pet.fedMin + kPetFeedMin);
          pet.starvedMin = 0;
          break;
        }
        if (press.button == kBtnRight) {
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

// --- Video player ----------------------------------------------------------
// Plays an RTV1 stream (see tools/encode-video.py). The container carries
// UC8179 panel scripts (full-refresh init, fast partial-update waveform,
// scene-cut ghost cleanup), an optional buzzer note track, and 1bpp frames
// delta-encoded as copy/literal runs, so playback behavior is tuned by
// re-encoding — no reflash. Pacing decodes every frame (deltas chain) but
// skips displaying when behind schedule; playback stats return through the
// MQTT command event. The melody runs on its own task on the other core so
// note timing survives the long SPI writes and busy waits.

struct VideoNote {
  uint32_t tMs;
  uint16_t freqHz;
  uint16_t durMs;
};

struct BuzzerTrack {
  VideoNote *notes;
  uint16_t count;
  uint32_t startMs;
  volatile bool stop;
  volatile bool done;
};

BuzzerTrack g_buzzer = {nullptr, 0, 0, false, true};

void buzzerTask(void *) {
  for (uint16_t i = 0; i < g_buzzer.count && !g_buzzer.stop; ++i) {
    const VideoNote &note = g_buzzer.notes[i];
    while (!g_buzzer.stop &&
           int32_t(g_buzzer.startMs + note.tMs - millis()) > 0)
      delay(5);
    if (g_buzzer.stop) break;
    ledcWriteTone(0, note.freqHz);
    const uint32_t offAt = g_buzzer.startMs + note.tMs + note.durMs;
    while (!g_buzzer.stop && int32_t(offAt - millis()) > 0) delay(5);
    ledcWriteTone(0, 0);
  }
  ledcWriteTone(0, 0);
  g_buzzer.done = true;
  vTaskDelete(nullptr);
}

bool readExact(Stream &stream, uint8_t *out, size_t want, uint32_t &stalls) {
  size_t got = 0;
  uint32_t lastProgress = millis();
  while (got < want) {
    const size_t n = stream.readBytes(out + got, want - got);
    if (n == 0) {
      ++stalls;
      if (millis() - lastProgress > 15000) return false;
      delay(2);
    } else {
      got += n;
      lastProgress = millis();
    }
  }
  return true;
}

// Copy runs leave the previous frame's bytes in place; literal runs
// overwrite, extend the dirty range (byte indices into the frame), and mark
// their bytes in the changed bitmap (one bit per frame byte).
bool decodeDelta(const uint8_t *payload, size_t length, uint8_t *frame,
                 size_t frameLen, size_t &dirtyLo, size_t &dirtyHi,
                 uint8_t *changedMap) {
  size_t pos = 0, out = 0;
  while (pos + 2 <= length) {
    const uint16_t token = payload[pos] | (uint16_t(payload[pos + 1]) << 8);
    pos += 2;
    const size_t run = token & 0x7FFF;
    if (out + run > frameLen) return false;
    if (token & 0x8000) {
      if (pos + run > length) return false;
      memcpy(frame + out, payload + pos, run);
      pos += run;
      if (run) {
        if (out < dirtyLo) dirtyLo = out;
        if (out + run - 1 > dirtyHi) dirtyHi = out + run - 1;
        if (changedMap) {
          for (size_t b = out; b < out + run; ++b)
            changedMap[b >> 3] |= uint8_t(1) << (b & 7);
        }
      }
    }
    out += run;
  }
  return out == frameLen && pos == length;
}

// The menu can start playback before the shared runtime brought Wi-Fi up;
// read the same wificaptive credentials the runtime uses.
bool connectVideoWifi() {
  Preferences preferences;
  if (!preferences.begin("wificaptive", true)) return false;
  int index = preferences.getInt("wifi_last_index", 0);
  if (index < 0 || index >= 5) index = 0;
  const String ssid =
      preferences.getString(("wifi_" + String(index) + "_ssid").c_str(), "");
  const String password =
      preferences.getString(("wifi_" + String(index) + "_pswd").c_str(), "");
  preferences.end();
  if (ssid.isEmpty()) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && int32_t(deadline - millis()) > 0)
    delay(100);
  return WiFi.status() == WL_CONNECTED;
}

bool playVideo(const String &url, String &detail, void (*serviceNetwork)()) {
  if (WiFi.status() != WL_CONNECTED && !connectVideoWifi()) {
    detail = "wifi unavailable";
    return false;
  }
  HTTPClient http;
  if (!url.startsWith("http://") || !http.begin(url)) {
    detail = "bad url";
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    detail = "http " + String(code);
    http.end();
    return false;
  }
  WiFiClient &stream = http.getStream();
  stream.setTimeout(1000);
  uint32_t stalls = 0;

  uint8_t header[25];
  if (!readExact(stream, header, sizeof(header), stalls) ||
      memcmp(header, "RTV1", 4) != 0) {
    detail = "bad header";
    http.end();
    return false;
  }
  const uint16_t width = header[4] | (header[5] << 8);
  const uint16_t height = header[6] | (header[7] << 8);
  const uint16_t intervalMs = header[8] | (header[9] << 8);
  const uint16_t frameCount = header[10] | (header[11] << 8);
  const uint16_t noteCount = header[12] | (header[13] << 8);
  const uint8_t spiMhz = header[14];
  const uint16_t busyTimeoutMs = uint16_t(header[15]) * 100;
  const uint8_t redrive = header[16] > 2 ? 2 : header[16];
  const uint16_t audioRate = header[17] | (header[18] << 8);
  const uint16_t initLen = header[19] | (header[20] << 8);
  const uint16_t videoLen = header[21] | (header[22] << 8);
  const uint16_t cleanupLen = header[23] | (header[24] << 8);
  const size_t frameBytes = size_t(width / 8) * height;
  if (width != 800 || height != 480 || frameCount == 0 || intervalMs == 0 ||
      initLen > 2048 || videoLen > 2048 || cleanupLen > 2048) {
    detail = "bad geometry";
    http.end();
    return false;
  }

  const size_t scriptBytes = size_t(initLen) + videoLen + cleanupLen;
  const size_t mapBytes = frameBytes / 8;  // one bit per frame byte
  uint8_t *scripts = (uint8_t *)malloc(scriptBytes);
  VideoNote *notes =
      noteCount ? (VideoNote *)malloc(sizeof(VideoNote) * noteCount) : nullptr;
  uint8_t *frame = (uint8_t *)malloc(frameBytes);
  uint8_t *payload = (uint8_t *)malloc(frameBytes + 128);
  // Changed-byte bitmaps for the current frame and up to two prior displayed
  // frames; bytes marked here get their old-data RAM inverted so the weak
  // waveform re-drives them (ghost-trail scrubbing).
  uint8_t *chgMap = (uint8_t *)calloc(1, mapBytes);
  uint8_t *r1Map = redrive >= 1 ? (uint8_t *)calloc(1, mapBytes) : nullptr;
  uint8_t *r2Map = redrive >= 2 ? (uint8_t *)calloc(1, mapBytes) : nullptr;
  g_pcm.ring = audioRate ? (uint8_t *)malloc(kAudioRingSize) : nullptr;
  // The arcade shares the ring globals; start from a clean queue.
  g_pcm.head = g_pcm.tail = 0;
  uint8_t *audioChunk = audioRate ? (uint8_t *)malloc(65536) : nullptr;
  bool ok = scripts && frame && payload && chgMap &&
            (redrive < 1 || r1Map) && (redrive < 2 || r2Map) &&
            (noteCount == 0 || notes) &&
            (audioRate == 0 || (g_pcm.ring && audioChunk &&
                                16000000u % audioRate == 0));
  if (ok) ok = readExact(stream, scripts, scriptBytes, stalls);
  if (ok && noteCount) {
    for (uint16_t i = 0; i < noteCount && ok; ++i) {
      uint8_t raw[8];
      ok = readExact(stream, raw, sizeof(raw), stalls);
      notes[i].tMs = uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) |
                     (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24);
      notes[i].freqHz = raw[4] | (uint16_t(raw[5]) << 8);
      notes[i].durMs = raw[6] | (uint16_t(raw[7]) << 8);
    }
  }
  if (!ok) {
    detail = scripts && frame && payload && chgMap ? "short stream" : "no memory";
    free(scripts);
    free(notes);
    free(frame);
    free(payload);
    free(chgMap);
    free(r1Map);
    free(r2Map);
    free(g_pcm.ring);
    g_pcm.ring = nullptr;
    free(audioChunk);
    http.end();
    return false;
  }
  const uint8_t *initScript = scripts;
  const uint8_t *videoScript = scripts + initLen;
  const uint8_t *cleanupScript = scripts + initLen + videoLen;

  Serial.print("Video: ");
  Serial.print(frameCount);
  Serial.print(" frames @ ");
  Serial.print(intervalMs);
  Serial.print(" ms, spi MHz = ");
  Serial.println(spiMhz);

  // Panel up: video SPI clock, full init from the stream, first frame with a
  // stock full refresh (also seeds the controller's old-data RAM via N2OCP),
  // then the fast video waveform.
  display.epd2.selectSPI(epaperSpi, SPISettings(uint32_t(spiMhz) * 1000000UL,
                                               MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  display.epd2.runScript(initScript, initLen);

  uint32_t shown = 0, skipped = 0, refreshMsSum = 0, refreshes = 0;
  uint32_t rowsSum = 0;
  bool aborted = false;
  memset(frame, 0xFF, frameBytes);
  uint32_t t0 = 0;
  uint32_t lastService = millis();
  // Union of undisplayed changes: skipped frames accumulate here so the next
  // displayed frame's window covers everything the panel has not seen yet.
  size_t dirtyLo = SIZE_MAX, dirtyHi = 0;
  // Extents of the re-drive maps (changed bytes of prior displayed frames).
  size_t r1Lo = SIZE_MAX, r1Hi = 0, r2Lo = SIZE_MAX, r2Hi = 0;
  const size_t rowBytes = width / 8;
  for (uint16_t i = 0; i < frameCount; ++i) {
    uint8_t rawHeader[4];
    if (!readExact(stream, rawHeader, sizeof(rawHeader), stalls)) break;
    const uint32_t lenAndFlags = uint32_t(rawHeader[0]) |
                                 (uint32_t(rawHeader[1]) << 8) |
                                 (uint32_t(rawHeader[2]) << 16) |
                                 (uint32_t(rawHeader[3]) << 24);
    const size_t payloadLen = lenAndFlags & 0xFFFFFF;
    const bool cleanup = (lenAndFlags >> 24) & 0x01;
    if (payloadLen > frameBytes + 128) break;
    if (!readExact(stream, payload, payloadLen, stalls)) break;
    if (audioRate) {
      uint8_t audioHeader[2];
      if (!readExact(stream, audioHeader, sizeof(audioHeader), stalls)) break;
      const uint16_t audioLen = audioHeader[0] | (audioHeader[1] << 8);
      if (audioLen && !readExact(stream, audioChunk, audioLen, stalls)) break;
      audioRingPush(audioChunk, audioLen);
    }
    if (!decodeDelta(payload, payloadLen, frame, frameBytes, dirtyLo, dirtyHi,
                     chgMap))
      break;

    if (i == 0) {
      display.epd2.writeVideoFrame(frame, frameBytes);
      display.epd2.refreshVideo(2500);  // stock full refresh from initScript
      display.epd2.runScript(videoScript, videoLen);
      dirtyLo = SIZE_MAX;
      dirtyHi = 0;
      memset(chgMap, 0, mapBytes);
      t0 = millis();
      if (noteCount) {
        g_buzzer.notes = notes;
        g_buzzer.count = noteCount;
        g_buzzer.startMs = t0;
        g_buzzer.stop = false;
        g_buzzer.done = false;
        ledcSetup(0, 440, 10);
        ledcAttachPin(kBuzzerPin, 0);
        xTaskCreatePinnedToCore(buzzerTask, "buzzer", 2048, nullptr, 1,
                                nullptr, 0);
      }
      if (audioRate) {
        // Frame 0's chunk is already in the ring; the sample clock starts
        // in step with the visible start of playback.
        audioStart(audioRate);
      }
      shown = 1;
      continue;
    }

    // Green held across two consecutive frames aborts playback.
    if (digitalRead(kGreenButton) == LOW) {
      delay(30);
      if (digitalRead(kGreenButton) == LOW) {
        aborted = true;
        break;
      }
    }
    if (serviceNetwork && millis() - lastService > 2000) {
      serviceNetwork();
      lastService = millis();
    }

    const uint32_t due = t0 + uint32_t(i) * intervalMs;
    if (int32_t(millis() - (due + intervalMs)) > 0) {
      ++skipped;  // behind schedule: the delta is applied, the panel waits
      continue;
    }
    if (cleanup) {
      display.epd2.writeVideoFrame(frame, frameBytes);
      display.epd2.runScript(cleanupScript, cleanupLen);
      display.epd2.refreshVideo(2500);
      display.epd2.runScript(videoScript, videoLen);
      // The strong cleanup drive saturated everything; all debts are paid.
      memset(chgMap, 0, mapBytes);
      if (r1Map) memset(r1Map, 0, mapBytes);
      if (r2Map) memset(r2Map, 0, mapBytes);
      r1Lo = r2Lo = SIZE_MAX;
      r1Hi = r2Hi = 0;
      ++shown;
    } else {
      // Window over this frame's changes plus the re-drive backlog.
      size_t winLo = dirtyLo, winHi = dirtyHi;
      if (r1Lo <= r1Hi) {
        winLo = min(winLo, r1Lo);
        winHi = max(winHi, r1Hi);
      }
      if (r2Lo <= r2Hi) {
        winLo = min(winLo, r2Lo);
        winHi = max(winHi, r2Hi);
      }
      if (winLo <= winHi) {
        const uint16_t yStart = uint16_t(winLo / rowBytes);
        const uint16_t rows = uint16_t(winHi / rowBytes) - yStart + 1;
        display.epd2.writeVideoWindow(0x13, frame + size_t(yStart) * rowBytes,
                                      yStart, rows);
        if (redrive) {
          // Spoof the old plane: bytes changed in this or the last N shown
          // frames get inverted old data, forcing the weak waveform to
          // drive all their pixels toward the current image again.
          const size_t base = size_t(yStart) * rowBytes;
          const size_t count = size_t(rows) * rowBytes;
          for (size_t b = 0; b < count; ++b) {
            const size_t idx = base + b;
            uint8_t marked = chgMap[idx >> 3];
            if (r1Map) marked |= r1Map[idx >> 3];
            if (r2Map) marked |= r2Map[idx >> 3];
            payload[b] = (marked >> (idx & 7)) & 1 ? uint8_t(~frame[idx])
                                                   : frame[idx];
          }
          display.epd2.writeVideoWindow(0x10, payload, yStart, rows);
        }
        refreshMsSum += display.epd2.refreshRows(yStart, rows, busyTimeoutMs);
        ++refreshes;
        rowsSum += rows;
      }
      // Rotate the re-drive backlog (only when a frame was displayed).
      if (r2Map) {
        memcpy(r2Map, r1Map, mapBytes);
        r2Lo = r1Lo;
        r2Hi = r1Hi;
      }
      if (r1Map) {
        memcpy(r1Map, chgMap, mapBytes);
        r1Lo = dirtyLo;
        r1Hi = dirtyHi;
      }
      memset(chgMap, 0, mapBytes);
      ++shown;
    }
    dirtyLo = SIZE_MAX;
    dirtyHi = 0;
    while (int32_t(due - millis()) > 0) delay(2);
  }

  g_buzzer.stop = true;
  for (uint32_t waitStart = millis();
       !g_buzzer.done && millis() - waitStart < 1000;)
    delay(10);
  if (noteCount) {
    ledcDetachPin(kBuzzerPin);
    pinMode(kBuzzerPin, OUTPUT);
    digitalWrite(kBuzzerPin, LOW);
  }
  if (audioRate) audioStop();
  http.end();

  // Back to normal panel operation: stock SPI clock and a fresh init. A
  // completed (or aborted) playback keeps its final frame on the panel,
  // redrawn with a stock full refresh that also flushes the video-mode
  // ghosting; the saved photo only returns via the menu or a new upload.
  // Early failures fall back to the photo (or a clear) instead of leaving
  // a half-written panel behind.
  display.epd2.selectSPI(epaperSpi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);
  if (shown > 1) {
    display.epd2.writeNative(frame, nullptr, 0, 0, 800, 480, false, false,
                             false);
    display.epd2.refresh(false);
  } else if (!restoreSavedPhoto()) {
    display.clearScreen();
  }
  free(scripts);
  free(notes);
  free(frame);
  free(payload);
  free(chgMap);
  free(r1Map);
  free(r2Map);
  free(g_pcm.ring);
  g_pcm.ring = nullptr;
  free(audioChunk);

  const uint32_t avgRefresh = refreshes ? refreshMsSum / refreshes : 0;
  const uint32_t avgRows = refreshes ? rowsSum / refreshes : 0;
  detail = "shown=" + String(shown) + " skipped=" + String(skipped) +
           " stalls=" + String(stalls) + " avg_refresh_ms=" +
           String(avgRefresh) + " avg_rows=" + String(avgRows) +
           " redrive=" + String(redrive) + (aborted ? " aborted=1" : "");
  if (audioRate) {
    detail += " audio_gap_ms=" +
              String(uint32_t(uint64_t(g_pcm.underruns) * 1000 / audioRate));
  }
  Serial.print("Video done: ");
  Serial.println(detail);
  return shown > 1;
}

// Replays the last MQTT-delivered video URL from the menu. Returns false
// when nothing is queued yet.
bool playLastVideo() {
  Preferences preferences;
  String url;
  if (preferences.begin("reterm-video", true)) {
    url = preferences.getString("url", "");
    preferences.end();
  }
  if (url.isEmpty()) return false;
  String detail;
  playVideo(url, detail, nullptr);
  return true;
}

// --- Arcade menu -----------------------------------------------------------

void drawMenuScreen(int selected) {
  static constexpr const char *entries[] = {
      "ROCK PAPER SCISSORS", "LUNA THE UNICORN", "SEND A PHOTO",
      "BACK TO THE PHOTO", "BAD APPLE"};
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    centeredText("RETERM ARCADE", 20, 4);
    for (int entry = 0; entry < 5; ++entry) {
      const int16_t y = 78 + entry * 70;
      if (entry == selected) {
        display.fillRoundRect(150, y, 510, 58, 12, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
      } else {
        display.drawRoundRect(150, y, 510, 58, 12, GxEPD_BLACK);
        display.drawRoundRect(151, y + 1, 508, 56, 12, GxEPD_BLACK);
      }
      cellText(entries[entry], 405, y + 17, 3);
      display.setTextColor(GxEPD_BLACK);
      if (entry == 0) {
        drawScissorsIcon(display, 110, y + 29, 48);
      } else if (entry == 1) {
        drawSprite(kSpriteUnicorn, 88, y + 5, 3);
      } else if (entry == 2) {
        // The photo frame with an up arrow: send a new photo to the frame.
        drawSprite(kSpriteFrame, 86, y + 5, 3);
        display.fillTriangle(110, y + 15, 98, y + 29, 122, y + 29, GxEPD_BLACK);
        display.fillRect(106, y + 29, 9, 12, GxEPD_BLACK);
      } else if (entry == 3) {
        drawSprite(kSpriteFrame, 86, y + 5, 3);
      } else {
        drawSprite(kSpriteApple, 90, y + 5, 4);
      }
    }
    centeredText("WHITE buttons scroll    GREEN chooses", 436, 2);
  } while (display.nextPage());
}

reterm::MenuAction runArcade() {
  Serial.println("Arcade menu");
  waitForButtonsReleased();
  int selected = 0;
  for (;;) {
    drawMenuScreen(selected);
    const ButtonPress press = waitForPress(kGameIdleMs);
    waitForButtonsReleased();
    if (press.button == kBtnLeft) {
      selected = (selected + 1) % 5;
      continue;
    }
    if (press.button == kBtnRight) {
      selected = (selected + 4) % 5;
      continue;
    }
    if (press.button == kBtnGreen) {
      if (selected == 0) {
        // A green hold in the game returns here; RPS otherwise leaves its
        // result screen up and a wall-mode pet reclaims the panel on the
        // next timer wake.
        if (playRockPaperScissors()) continue;
        return reterm::MenuAction::Sleep;
      }
      if (selected == 1) {
        if (runPetScreen()) continue;      // green: back to the menu
        return reterm::MenuAction::Sleep;  // idle: pet stays on the wall
      }
      if (selected == 2) {
        Serial.println("Menu: upload session requested");
        return reterm::MenuAction::UploadSession;
      }
      if (selected == 4) {
        if (playLastVideo()) return reterm::MenuAction::Sleep;
        drawMessageScreen("NO VIDEO QUEUED",
                          "Send a video command over MQTT first");
        delay(3000);
        continue;
      }
      PetState pet = loadPet();
      if (pet.onWall) {
        pet.onWall = false;
        savePet(pet);
      }
      if (!restoreSavedPhoto())
        Serial.println("No saved image available to restore");
      return reterm::MenuAction::Sleep;
    }
    // Menu idle: never leave the menu as wallpaper.
    const PetState pet = loadPet();
    if (pet.adopted && pet.onWall) drawPetScreen(pet);
    else if (!restoreSavedPhoto())
      Serial.println("No saved image available to restore");
    return reterm::MenuAction::Sleep;
  }
}

// Wake classification, filled in by setup() before reterm::run() starts.
bool g_wantSessionFromMenu = false;  // the pre-run menu chose SEND A PHOTO
bool g_suppressButtonWake = false;   // a green hold already ran the menu
bool g_wifiResetHold = false;        // green held five seconds through wake

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
    if (g_suppressButtonWake) return false;
    if (g_wantSessionFromMenu) return true;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) return true;
    for (int pin : kButtonPins) {
      if (digitalRead(pin) == LOW) return true;
    }
    return false;
  }
  // setup() measures the green hold once at wake; answer from that.
  bool wakeHoldRequestsWifiReset() override { return g_wifiResetHold; }
  // Holding green for about a second while the device is awake (upload
  // session or provisioning portal) opens the arcade. Non-blocking: the
  // runtime polls this from its wait loops.
  bool menuRequested() override {
    if (digitalRead(kGreenButton) == LOW) {
      if (menuHoldSince_ == 0) menuHoldSince_ = millis();
      else if (millis() - menuHoldSince_ >= kLongPressMs) {
        menuHoldSince_ = 0;
        return true;
      }
    } else {
      menuHoldSince_ = 0;
    }
    return false;
  }
  reterm::MenuAction onMenu() override { return runArcade(); }
  bool handleCommand(const String &action, const String &payload,
                     String &detail, void (*serviceNetwork)()) override {
    if (action != "video") return false;
    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
      detail = "bad json";
      return true;
    }
    const String url = doc["url"] | "";
    if (url.isEmpty()) {
      detail = "missing url";
      return true;
    }
    if (playVideo(url, detail, serviceNetwork)) {
      // Remember the stream so the menu's BAD APPLE entry can replay it.
      Preferences preferences;
      if (preferences.begin("reterm-video", false)) {
        preferences.putString("url", url);
        preferences.end();
      }
    }
    return true;
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

 private:
  uint32_t menuHoldSince_ = 0;
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
  // arcade, and the green wake is classified by hold length. All paths fall
  // through into run(), which sees the flags via wokeByButton() /
  // wakeHoldRequestsWifiReset() and otherwise takes its normal course.
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) petTimerTick();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    if (runArcade() == reterm::MenuAction::UploadSession)
      g_wantSessionFromMenu = true;
  }
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    // Classify the green press: a tap keeps today's photo session, about a
    // second of holding opens the arcade, five seconds still forgets the
    // saved Wi-Fi network.
    const uint32_t pressedAt = millis();
    while (digitalRead(kGreenButton) == LOW && millis() - pressedAt < 5000)
      delay(20);
    const uint32_t held = millis() - pressedAt;
    if (held >= 5000) {
      g_wifiResetHold = true;
    } else if (held >= kLongPressMs) {
      if (runArcade() == reterm::MenuAction::UploadSession)
        g_wantSessionFromMenu = true;
      else
        g_suppressButtonWake = true;
    }
  }
  reterm::run(board);
}

void loop() { delay(1000); }
