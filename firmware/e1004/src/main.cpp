#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <Wire.h>
#include <WebServer.h>
#include <WiFi.h>
#include <GxEPD2_7C.h>
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
constexpr uint32_t kUploadWindowMs = 5 * 60 * 1000;
constexpr uint32_t kUploadAbsoluteMaxMs = 30 * 60 * 1000;
constexpr char kAllowedOrigin[] = "https://elohmeier.github.io";
constexpr char kSavedImagePath[] = "/current-image.bin";
constexpr char kBackupImagePath[] = "/previous-image.bin";
constexpr char kPendingImagePath[] = "/pending-image.bin";

SPIClass epaperSpi(HSPI);
GxEPD2_7C<GxEPD2_T133A01_1200x1600, 40> display(
    GxEPD2_T133A01_1200x1600(kCs, kDc, kReset, kBusy, kCs1, kEnable));
WebServer server(80);
File pendingImage;
bool imageStorageReady = false;

enum class StartupCommand { None, Image, Web };
String sessionToken;
String uartSessionToken;
IPAddress authorizedClientIp;
bool authorizedClientIpSet = false;
uint8_t httpRow[kPackedRowBytes];
size_t httpRowFill = 0;
size_t httpBytes = 0;
int16_t httpY = 0;
int httpResult = 500;
String httpAuthDebug;
volatile bool imageReady = false;
volatile bool httpTaskRunning = false;
volatile bool acceptingImages = false;
volatile bool uploadResponseSent = false;
volatile uint32_t sessionLastActivity = 0;

const char kLocalUploader[] PROGMEM = R"HTML(<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="referrer" content="no-referrer">
<title>reTerminal Photo Magic</title>
<link rel="stylesheet" href="https://elohmeier.github.io/reterm/device-uploader.css?v=2">
<div id="reterm-uploader">Loading photo editor…</div>
<script>
window.RETERM_TOKEN=new URLSearchParams(location.search).get('token')||'';
// Keep cached pre-v2 uploader scripts compatible. Some iOS Safari versions
// reuse them despite a new QR navigation and omit their custom token header.
const retermOpen=XMLHttpRequest.prototype.open;
XMLHttpRequest.prototype.open=function(method,url,...rest){
  if(method==='POST'&&url==='/api/image')
    url='/api/image/'+encodeURIComponent(window.RETERM_TOKEN);
  return retermOpen.call(this,method,url,...rest);
};
</script>
<script defer src="https://elohmeier.github.io/reterm/device-uploader.js?v=2"></script>)HTML";

void abortPendingImage() {
  if (pendingImage) pendingImage.close();
  if (imageStorageReady && SPIFFS.exists(kPendingImagePath))
    SPIFFS.remove(kPendingImagePath);
}

bool beginPendingImage() {
  if (!imageStorageReady) return false;
  abortPendingImage();
  pendingImage = SPIFFS.open(kPendingImagePath, FILE_WRITE);
  return bool(pendingImage);
}

bool commitPendingImage() {
  if (!pendingImage) return false;
  pendingImage.flush();
  pendingImage.close();

  if (SPIFFS.exists(kBackupImagePath)) SPIFFS.remove(kBackupImagePath);
  if (SPIFFS.exists(kSavedImagePath) &&
      !SPIFFS.rename(kSavedImagePath, kBackupImagePath)) {
    SPIFFS.remove(kPendingImagePath);
    return false;
  }
  if (!SPIFFS.rename(kPendingImagePath, kSavedImagePath)) {
    if (SPIFFS.exists(kBackupImagePath))
      SPIFFS.rename(kBackupImagePath, kSavedImagePath);
    return false;
  }
  if (SPIFFS.exists(kBackupImagePath)) SPIFFS.remove(kBackupImagePath);
  return true;
}

bool restoreSavedImage() {
  if (!imageStorageReady) return false;
  const char *path = SPIFFS.exists(kSavedImagePath) ? kSavedImagePath
                                                    : kBackupImagePath;
  File saved = SPIFFS.open(path, FILE_READ);
  if (!saved || saved.size() != kPackedImageBytes) {
    if (saved) saved.close();
    return false;
  }

  uint8_t row[kPackedRowBytes];
  Serial.println("Restoring saved image after unused upload session");
  for (int16_t y = 0; y < 1600; ++y) {
    if (saved.read(row, sizeof(row)) != sizeof(row)) {
      saved.close();
      return false;
    }
    display.epd2.writeNative(row, nullptr, 0, y, 1200, 1);
  }
  saved.close();
  display.epd2.refresh(false);
  display.hibernate();
  Serial.println("Saved image restored");
  return true;
}

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
        // Receipt of the 32-byte fixture suffix is the opt-in signal. Use a
        // stable printable token even if this high-speed UART corrupts a byte.
        uartSessionToken = "0123456789abcdef0123456789abcdef";
        Serial.println("UART test token accepted");
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
  const String localOrigin = "http://" + WiFi.localIP().toString();
  return origin.isEmpty() || origin == kAllowedOrigin || origin == localOrigin;
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
  if (!originAllowed()) return false;
  // Only the exact, randomly generated per-session URI is registered with an
  // image handler. If dispatch reached that handler, its path is already the
  // bearer credential; avoid redundantly comparing mutable String storage.
  if (server.uri().startsWith("/api/image/")) return true;
  if (constantTimeToken(server.header("X-Upload-Token"))) return true;
  if (authorizedClientIpSet && server.client().remoteIP() == authorizedClientIp)
    return true;
  return false;
}

String requestAuthDebug() {
  const String origin = server.header("Origin");
  String detail = "route=";
  detail += server.uri().startsWith("/api/image/") ? "tokenized" : "legacy";
  detail += ", header=";
  detail += server.header("X-Upload-Token").length();
  detail += ", peer=";
  detail += server.client().remoteIP().toString();
  detail += ", bound=";
  detail += authorizedClientIpSet ? authorizedClientIp.toString() : "none";
  detail += ", origin=";
  detail += origin.isEmpty() ? "none" : (originAllowed() ? "allowed" : "denied");
  detail += ", ready=";
  detail += acceptingImages ? "yes" : "no";
  return detail;
}

void rememberAuthorizedClient() {
  if (constantTimeToken(server.arg("token"))) {
    authorizedClientIp = server.client().remoteIP();
    authorizedClientIpSet = true;
  }
}

void handleStatus() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sessionLastActivity = millis();
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
    // The web shell can load while the slow QR refresh is still completing.
    // Hold an early POST until the display bus is safe instead of forcing the
    // user to retry after a transient 503.
    const uint32_t readyDeadline = millis() + 60000;
    while (!acceptingImages && httpTaskRunning &&
           int32_t(readyDeadline - millis()) > 0) {
      delay(10);
    }
    httpRowFill = 0;
    httpBytes = 0;
    httpY = 0;
    httpAuthDebug = requestAuthDebug();
    httpResult = acceptingImages && requestAuthorized() &&
                         server.clientContentLength() == kPackedImageBytes
                     ? 202
                     : (acceptingImages ? 401 : 503);
    if (httpResult == 202 && !beginPendingImage()) httpResult = 507;
    if (httpResult == 202) sessionLastActivity = millis();
    if (requestAuthorized() &&
        server.clientContentLength() != kPackedImageBytes) httpResult = 413;
    return;
  }
  if (raw.status == RAW_ABORTED) {
    abortPendingImage();
    httpResult = 400;
    return;
  }
  if (raw.status == RAW_WRITE && httpResult == 202) {
    sessionLastActivity = millis();
    if (pendingImage.write(raw.buf, raw.currentSize) != raw.currentSize) {
      abortPendingImage();
      httpResult = 507;
      return;
    }
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
    // WebServer drains a raw request in one synchronous parse loop. Yield on
    // every chunk so the idle task and watchdog can run during a 960 KB body.
    delay(1);
    return;
  }
  if (raw.status == RAW_END && httpResult == 202) {
    if (httpBytes == kPackedImageBytes && httpY == 1600 && httpRowFill == 0 &&
        commitPendingImage()) {
      imageReady = true;
    } else {
      abortPendingImage();
      httpResult = 400;
    }
  }
}

void handleImageResult() {
  addCorsHeaders();
  if (httpResult == 202 && imageReady) {
    server.send(202, "application/json", "{\"status\":\"refreshing\"}");
    // RAW_END marks the framebuffer complete before WebServer invokes this
    // request handler. Do not let the main task stop the server in that gap.
    uploadResponseSent = true;
  } else if (httpResult == 413) {
    server.send(413, "application/json", "{\"error\":\"expected 960000 bytes\"}");
  } else if (httpResult == 401) {
    Serial.print("Upload unauthorized: ");
    Serial.println(httpAuthDebug);
    server.send(401, "application/json",
                "{\"error\":\"unauthorized (" + httpAuthDebug + ")\"}");
  } else if (httpResult == 503) {
    server.send(503, "application/json", "{\"error\":\"display is starting\"}");
  } else if (httpResult == 507) {
    server.send(507, "application/json", "{\"error\":\"could not save image\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"incomplete image\"}");
  }
}

void serveHttp(void *) {
  while (httpTaskRunning) {
    server.handleClient();
    delay(2);
  }
  vTaskDelete(nullptr);
}

void startUploadServer() {
  const String imagePath = "/api/image/" + sessionToken;
  const char *headers[] = {"Origin", "X-Upload-Token", "Content-Type",
                           "Access-Control-Request-Private-Network"};
  server.collectHeaders(headers, 4);
  server.on("/", HTTP_GET, [] {
    rememberAuthorizedClient();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", kLocalUploader);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/image", HTTP_POST, handleImageResult, handleRawImage);
  // Safari has intermittently omitted the custom header on large XHR bodies.
  // The token is already present in the QR URL, so an exact per-session path
  // provides an equivalent same-origin authentication channel.
  server.on(imagePath, HTTP_POST, handleImageResult, handleRawImage);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/image", HTTP_OPTIONS, handleOptions);
  server.on(imagePath, HTTP_OPTIONS, handleOptions);
  server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();
  httpTaskRunning = true;
  // Core 0 hosts the ESP32 Wi-Fi/system work. Running the synchronous raw-body
  // parser there can starve IDLE0 long enough to trigger the task watchdog.
  xTaskCreatePinnedToCore(serveHttp, "upload-http", 6144, nullptr, 1, nullptr, 1);
}

bool beginSavedWifi() {
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
  // Upload mode is brief and latency-sensitive. Modem power saving proved
  // unreliable while the color panel was refreshing and could leave the QR's
  // address associated with a station that no longer answered ARP.
  WiFi.setSleep(false);
  WiFi.setHostname("reterm-e1004");
  WiFi.begin(ssid.c_str(), password.c_str());
  return true;
}

bool waitForWifi(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && int32_t(deadline - millis()) > 0) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectSavedWifi() {
  return beginSavedWifi() && waitForWifi(20000);
}

void drawQrPage(QRCode &qr, int16_t left, int16_t top, int16_t scale,
                uint16_t page) {
  const int16_t pageTop = page * display.pageHeight();
  const int16_t pageBottom = pageTop + display.pageHeight();
  for (uint8_t y = 0; y < qr.size; ++y) {
    const int16_t moduleTop = top + y * scale;
    if (moduleTop + scale <= pageTop || moduleTop >= pageBottom) continue;
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(&qr, x, y)) {
        display.fillRect(left + x * scale, moduleTop, scale, scale,
                         GxEPD_BLACK);
      }
    }
  }
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
    drawQrPage(qr, left, top, scale, page++);
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
    drawQrPage(qr, left, top, scale, page++);
    display.setTextColor(GxEPD_GREEN);
    display.setTextSize(2);
    display.setCursor(190, 1220);
    display.print("Choose, crop, preview, upload!");
  } while (display.nextPage());
}

void runUploadSession() {
  Serial.println("Starting Wi-Fi upload session");
  sessionToken = uartSessionToken.isEmpty() ? makeToken() : uartSessionToken;
  authorizedClientIpSet = false;
  uartSessionToken = "";
  // Serving the uploader from the E1004 makes Safari's upload same-origin.
  // Use the assigned numeric address: multicast DNS is unreliable on guest or
  // client-isolated WLANs, while the QR is regenerated for every session.
  const bool wifiStarted = beginSavedWifi();
  const uint32_t wifiStartedAt = millis();
  if ((!wifiStarted || !waitForWifi(20000)) && !provisionWifi()) {
    Serial.println("Wi-Fi connection/provisioning failed");
    return;
  }
  Serial.print("Wi-Fi ready ms = ");
  Serial.println(millis() - wifiStartedAt);
  const String qrUrl = "http://" + WiFi.localIP().toString() +
                       "/?token=" + sessionToken;
  startUploadServer();
  Serial.print("UPLOAD API http://");
  Serial.println(WiFi.localIP());
  drawUploadQr(qrUrl);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped during panel refresh; reconnecting");
    WiFi.reconnect();
    if (!waitForWifi(20000)) {
      Serial.println("Wi-Fi reconnect failed");
      httpTaskRunning = false;
      delay(10);
      server.stop();
      if (!restoreSavedImage()) Serial.println("No saved image available to restore");
      return;
    }
    Serial.print("Wi-Fi restored at http://");
    Serial.println(WiFi.localIP());
  }
  acceptingImages = true;

  const uint32_t sessionStarted = millis();
  sessionLastActivity = sessionStarted;
  uint32_t lastReconnectAttempt = sessionStarted;
  while (!uploadResponseSent &&
         millis() - sessionLastActivity < kUploadWindowMs &&
         millis() - sessionStarted < kUploadAbsoluteMaxMs) {
    if (WiFi.status() != WL_CONNECTED &&
        millis() - lastReconnectAttempt >= 2000) {
      lastReconnectAttempt = millis();
      Serial.println("Wi-Fi disconnected; reconnecting");
      WiFi.reconnect();
    }
    delay(2);
  }
  // server.send() can return once the small 202 response is queued locally.
  // Keep the HTTP task and socket alive long enough for the peer to ACK it;
  // stopping after the old 10 ms grace could emit a TCP reset on real Wi-Fi.
  if (uploadResponseSent) delay(1000);
  acceptingImages = false;
  httpTaskRunning = false;
  delay(10);
  server.stop();
  sessionToken = "";

  if (imageReady) {
    Serial.println("HTTP image received; refreshing display");
    display.epd2.refresh(false);
    display.hibernate();
    Serial.println("HTTP image displayed");
  } else {
    Serial.println("Upload timed out");
    if (!restoreSavedImage()) Serial.println("No saved image available to restore");
  }
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
  imageStorageReady = SPIFFS.begin(true);
  Serial.print("Image storage ready = ");
  Serial.println(imageStorageReady ? "yes" : "no");

  pinMode(kButton, INPUT_PULLUP);
  const bool buttonWake =
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ||
      digitalRead(kButton) == LOW;
  // A physical wake is unambiguous and should react immediately. Retain the
  // three-second UART recovery window only for reset/power-on host workflows.
  const StartupCommand command =
      buttonWake ? StartupCommand::None : receiveStartupCommand();
  if (command == StartupCommand::Image) goToSleep();
  if (command == StartupCommand::Web || buttonWake) {
    runUploadSession();
    goToSleep();
  }

  Serial.println("No host command; preserving the current panel image");
  display.hibernate();
  goToSleep();
}

void loop() { delay(1000); }
