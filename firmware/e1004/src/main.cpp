#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <WebServer.h>
#include <WiFi.h>
#include <GxEPD2_7C.h>
#include <Fonts/FreeSerifBoldItalic24pt7b.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <qrcode.h>

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
constexpr size_t kPackedImageBytes = kPackedRowBytes * 1600;
// The three E1004 capacitive controls are channels of an IQS323. Its shared
// active-low RDY/interrupt output is wired to ESP32-S3 GPIO 3.
constexpr gpio_num_t kButton = GPIO_NUM_3;
constexpr int kTouchSda = 39;
constexpr int kTouchScl = 40;
constexpr uint8_t kTouchAddress = 0x44;
constexpr uint32_t kUploadWindowMs = 60000;
constexpr char kAllowedOrigin[] = "https://elohmeier.github.io";

SPIClass epaperSpi(HSPI);
GxEPD2_7C<GxEPD2_T133A01_1200x1600, 40> display(
    GxEPD2_T133A01_1200x1600(kCs, kDc, kReset, kBusy, kCs1, kEnable));
WebServer server(80);

enum class StartupCommand { None, Image, Web };
String sessionToken;
String uartSessionToken;
uint8_t httpRow[kPackedRowBytes];
size_t httpRowFill = 0;
size_t httpBytes = 0;
int16_t httpY = 0;
int httpResult = 500;
bool imageReady = false;
bool cancelRequested = false;

const char kLocalUploader[] PROGMEM = R"HTML(<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="referrer" content="no-referrer">
<title>reTerminal Photo Magic</title>
<link rel="stylesheet" href="https://elohmeier.github.io/reterm/device-uploader.css">
<div id="reterm-uploader">Loading photo editor…</div>
<script>window.RETERM_TOKEN=new URLSearchParams(location.search).get('token')||''</script>
<script defer src="https://elohmeier.github.io/reterm/device-uploader.js"></script>)HTML";

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

void centered(const char *text, int16_t baseline) {
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text, 0, baseline, &x1, &y1, &width, &height);
  display.setCursor((display.width() - width) / 2 - x1, baseline);
  display.print(text);
}

StartupCommand receiveStartupCommand() {
  static constexpr char imageMagic[] = "E1IMG001";
  static constexpr char webMagic[] = "E1WEB001";
  uint8_t header[8];
  uint8_t row[kPackedRowBytes];

  Serial.println("READY E1004 E1IMG001|E1WEB001");
  Serial.setTimeout(5000);
  const uint32_t deadline = millis() + 3000;
  while (!Serial.available() && int32_t(deadline - millis()) > 0) delay(10);
  if (!Serial.available()) return StartupCommand::None;
  if (Serial.readBytes(header, sizeof(header)) != sizeof(header) ||
      (memcmp(header, imageMagic, sizeof(header)) != 0 &&
       memcmp(header, webMagic, sizeof(header)) != 0)) {
    Serial.println("IMAGE ERROR bad header");
    return StartupCommand::None;
  }
  if (memcmp(header, webMagic, sizeof(header)) == 0) {
    // A fixture may append a known 32-character token for an authenticated
    // end-to-end test. Physical UART access already permits reflashing this
    // non-secure-boot development unit; normal button sessions always use RNG.
    const uint32_t tokenDeadline = millis() + 250;
    while (Serial.available() < 32 && int32_t(tokenDeadline - millis()) > 0) delay(2);
    if (Serial.available() >= 32) {
      char testToken[33] = {};
      if (Serial.readBytes(reinterpret_cast<uint8_t *>(testToken), 32) == 32) {
        bool valid = true;
        for (size_t i = 0; i < 32; ++i) valid &= isxdigit(testToken[i]);
        if (valid) uartSessionToken = String(testToken);
      }
    }
    return StartupCommand::Web;
  }

  for (int16_t y = 0; y < 1600; ++y) {
    if (Serial.readBytes(row, sizeof(row)) != sizeof(row)) {
      Serial.print("IMAGE ERROR short row ");
      Serial.println(y);
      return StartupCommand::None;
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
  return StartupCommand::Image;
}

void goToSleep() {
  Serial.println("Entering deep sleep; press the button for upload mode");
  Serial.flush();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  pinMode(kButton, INPUT_PULLUP);
  prepareTouchWake();
  esp_sleep_enable_ext0_wakeup(kButton, 0);
  delay(50);
  esp_deep_sleep_start();
}

String makeToken() {
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  static constexpr char hex[] = "0123456789abcdef";
  char encoded[sizeof(random) * 2 + 1];
  for (size_t i = 0; i < sizeof(random); ++i) {
    encoded[i * 2] = hex[random[i] >> 4];
    encoded[i * 2 + 1] = hex[random[i] & 0x0f];
  }
  encoded[sizeof(encoded) - 1] = 0;
  return String(encoded);
}

bool constantTimeToken(const String &candidate) {
  if (candidate.length() != sessionToken.length()) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < sessionToken.length(); ++i) {
    difference |= uint8_t(candidate[i]) ^ uint8_t(sessionToken[i]);
  }
  return difference == 0;
}

bool originAllowed() {
  const String origin = server.header("Origin");
  return origin.isEmpty() || origin == kAllowedOrigin;
}

void addCorsHeaders() {
  if (server.header("Origin") == kAllowedOrigin) {
    server.sendHeader("Access-Control-Allow-Origin", kAllowedOrigin);
    server.sendHeader("Vary", "Origin");
  }
  server.sendHeader("Cache-Control", "no-store");
}

void handleOptions() {
  if (!originAllowed()) {
    server.send(403, "text/plain", "origin denied");
    return;
  }
  addCorsHeaders();
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers",
                    "Content-Type, X-Upload-Token");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Access-Control-Max-Age", "600");
  server.send(204);
}

bool requestAuthorized() {
  return originAllowed() && constantTimeToken(server.header("X-Upload-Token"));
}

void handleStatus() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  server.send(200, "application/json",
              "{\"model\":\"reterminal-e1004\",\"width\":1200,"
              "\"height\":1600,\"bytes\":960000,"
              "\"format\":\"gxepd2-4bpp\","
              "\"palette\":[\"black\",\"white\",\"green\","
              "\"blue\",\"red\",\"yellow\"]}");
}

void handleRawImage() {
  HTTPRaw &raw = server.raw();
  if (raw.status == RAW_START) {
    httpRowFill = 0;
    httpBytes = 0;
    httpY = 0;
    httpResult = requestAuthorized() &&
                         server.clientContentLength() == kPackedImageBytes
                     ? 202
                     : 401;
    if (requestAuthorized() &&
        server.clientContentLength() != kPackedImageBytes) httpResult = 413;
    return;
  }
  if (raw.status == RAW_ABORTED) {
    httpResult = 400;
    return;
  }
  if (raw.status == RAW_WRITE && httpResult == 202) {
    size_t input = 0;
    while (input < raw.currentSize) {
      const size_t count = min(sizeof(httpRow) - httpRowFill,
                               raw.currentSize - input);
      memcpy(httpRow + httpRowFill, raw.buf + input, count);
      httpRowFill += count;
      httpBytes += count;
      input += count;
      if (httpRowFill == sizeof(httpRow)) {
        display.epd2.writeNative(httpRow, nullptr, 0, httpY++, 1200, 1);
        httpRowFill = 0;
      }
    }
    return;
  }
  if (raw.status == RAW_END && httpResult == 202) {
    if (httpBytes == kPackedImageBytes && httpY == 1600 && httpRowFill == 0) {
      imageReady = true;
    } else {
      httpResult = 400;
    }
  }
}

void handleImageResult() {
  addCorsHeaders();
  if (httpResult == 202 && imageReady) {
    server.send(202, "application/json", "{\"status\":\"refreshing\"}");
  } else if (httpResult == 413) {
    server.send(413, "application/json", "{\"error\":\"expected 960000 bytes\"}");
  } else if (httpResult == 401) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"incomplete image\"}");
  }
}

void handleCancel() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  cancelRequested = true;
  server.send(200, "application/json", "{\"status\":\"cancelled\"}");
}

bool connectSavedWifi() {
  Preferences preferences;
  if (!preferences.begin("wificaptive", true)) return false;
  int index = preferences.getInt("wifi_last_index", 0);
  if (index < 0 || index >= 5) index = 0;
  String ssid = preferences.getString(("wifi_" + String(index) + "_ssid").c_str(), "");
  String password = preferences.getString(("wifi_" + String(index) + "_pswd").c_str(), "");
  preferences.end();
  if (ssid.isEmpty()) return false;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("reterm-e1004");
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t deadline = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && int32_t(deadline - millis()) > 0) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
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
    for (uint8_t y = 0; y < qr.size; ++y) {
      for (uint8_t x = 0; x < qr.size; ++x) {
        if (qrcode_getModule(&qr, x, y)) {
          display.fillRect(left + x * scale, top + y * scale, scale, scale,
                           GxEPD_BLACK);
        }
      }
    }
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

bool provisionWifi() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lx",
           static_cast<unsigned long>(chip & 0xffffff));
  const String apName = "reterm-e1004-" + String(suffix);
  const String apPassword = makeToken().substring(0, 12);
  drawProvisionQr(apName, apPassword);

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(apName.c_str(), apPassword.c_str())) return false;
  DNSServer dns;
  WebServer provisioning(80);
  bool saved = false;
  dns.start(53, "*", WiFi.softAPIP());

  static constexpr char page[] =
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<style>body{font:18px system-ui;max-width:32rem;margin:3rem auto;padding:1rem;"
      "background:#fff7df;color:#17213a}form{display:grid;gap:1rem}input,button{"
      "font:inherit;padding:.8rem;border:2px solid;border-radius:.7rem}button{"
      "background:#1259ba;color:white;font-weight:bold}</style>"
      "<h1>reTerminal Wi-Fi</h1><p>Enter your home Wi-Fi. Credentials stay on "
      "the device.</p><form method=post action=/save><label>Network name"
      "<input name=ssid required maxlength=32></label><label>Password"
      "<input name=password type=password maxlength=63></label>"
      "<button>Save and connect</button></form>";

  provisioning.on("/save", HTTP_POST, [&] {
    const String ssid = provisioning.arg("ssid");
    const String password = provisioning.arg("password");
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
      provisioning.send(400, "text/plain", "Invalid Wi-Fi credentials");
      return;
    }
    Preferences preferences;
    if (!preferences.begin("wificaptive", false)) {
      provisioning.send(500, "text/plain", "Could not save credentials");
      return;
    }
    preferences.putInt("wifi_last_index", 0);
    preferences.putString("wifi_0_ssid", ssid);
    preferences.putString("wifi_0_pswd", password);
    preferences.end();
    saved = true;
    provisioning.send(200, "text/html",
                      "<h1>Saved!</h1><p>Reconnect to your home Wi-Fi, then "
                      "scan the next QR code on the display.</p>");
  });
  provisioning.onNotFound([&] { provisioning.send(200, "text/html", page); });
  provisioning.on("/", HTTP_GET, [&] { provisioning.send(200, "text/html", page); });
  provisioning.begin();

  const uint32_t deadline = millis() + 180000;
  while (!saved && int32_t(deadline - millis()) > 0) {
    dns.processNextRequest();
    provisioning.handleClient();
    delay(2);
  }
  provisioning.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  delay(300);
  return saved && connectSavedWifi();
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
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLUE);
    display.setTextSize(4);
    display.setCursor(215, 120);
    display.print("SEND A PHOTO");
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(265, 220);
    display.print("Scan within one minute");
    display.fillRect(left - 28, top - 28, qrPixels + 56, qrPixels + 56,
                     GxEPD_WHITE);
    for (uint8_t y = 0; y < qr.size; ++y) {
      for (uint8_t x = 0; x < qr.size; ++x) {
        if (qrcode_getModule(&qr, x, y)) {
          display.fillRect(left + x * scale, top + y * scale, scale, scale,
                           GxEPD_BLACK);
        }
      }
    }
    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    display.setCursor(190, 1220);
    display.print("Choose, crop, preview, upload!");
    display.setTextColor(GxEPD_RED);
    display.setCursor(300, 1320);
    display.print("Button cancels");
  } while (display.nextPage());
}

void runUploadSession() {
  Serial.println("Starting Wi-Fi upload session");
  if (!connectSavedWifi() && !provisionWifi()) {
    Serial.println("Wi-Fi connection/provisioning failed");
    return;
  }

  sessionToken = uartSessionToken.isEmpty() ? makeToken() : uartSessionToken;
  uartSessionToken = "";
  // Serving the uploader from the E1004 makes Safari's upload same-origin;
  // iOS otherwise blocks an HTTPS GitHub Pages fetch to this local HTTP API.
  const String qrUrl = "http://" + WiFi.localIP().toString() +
                       "/?token=" + sessionToken;
  drawUploadQr(qrUrl);

  const char *headers[] = {"Origin", "X-Upload-Token", "Content-Type",
                           "Access-Control-Request-Private-Network"};
  server.collectHeaders(headers, 4);
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", kLocalUploader);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/image", HTTP_POST, handleImageResult, handleRawImage);
  server.on("/api/cancel", HTTP_POST, handleCancel);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/image", HTTP_OPTIONS, handleOptions);
  server.on("/api/cancel", HTTP_OPTIONS, handleOptions);
  server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();
  Serial.print("UPLOAD API http://");
  Serial.println(WiFi.localIP());

  const uint32_t deadline = millis() + kUploadWindowMs;
  bool buttonArmed = digitalRead(kButton) == HIGH;
  while (!imageReady && !cancelRequested && int32_t(deadline - millis()) > 0) {
    server.handleClient();
    if (!buttonArmed && digitalRead(kButton) == HIGH) buttonArmed = true;
    if (buttonArmed && digitalRead(kButton) == LOW) cancelRequested = true;
    delay(2);
  }
  server.stop();
  sessionToken = "";

  if (imageReady) {
    Serial.println("HTTP image received; refreshing display");
    display.epd2.refresh(false);
    display.hibernate();
    Serial.println("HTTP image displayed");
  } else {
    Serial.println(cancelRequested ? "Upload cancelled" : "Upload timed out");
  }
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
  Wire.begin(kTouchSda, kTouchScl);
  Wire.setClock(100000);
  display.epd2.selectSPI(epaperSpi,
                         SPISettings(10000000, MSBFIRST, SPI_MODE0));
  display.init(115200);
  display.setRotation(0);

  pinMode(kButton, INPUT_PULLUP);
  const bool buttonWake =
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ||
      digitalRead(kButton) == LOW;
  const StartupCommand command = receiveStartupCommand();
  if (command == StartupCommand::Image) goToSleep();
  if (command == StartupCommand::Web || buttonWake) {
    runUploadSession();
    goToSleep();
  }

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
  goToSleep();
}

void loop() { delay(1000); }
