#include "reterm.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_sleep.h>

namespace reterm {
namespace {

constexpr uint32_t kUploadWindowMs = 5 * 60 * 1000;
constexpr uint32_t kUploadAbsoluteMaxMs = 30 * 60 * 1000;
constexpr char kAllowedOrigin[] = "https://elohmeier.github.io";
constexpr char kSavedImagePath[] = "/current-image.bin";
constexpr char kBackupImagePath[] = "/previous-image.bin";
constexpr char kPendingImagePath[] = "/pending-image.bin";

Board *g_board = nullptr;
Geometry g_geometry;
WebServer server(80);
File pendingImage;
bool imageStorageReady = false;

enum class StartupCommand { None, Image, Web };
String sessionToken;
String uartSessionToken;
IPAddress authorizedClientIp;
bool authorizedClientIpSet = false;
uint8_t *httpRow = nullptr;   // HTTP task row assembly, g_geometry.rowBytes
uint8_t *mainRow = nullptr;   // main task UART receive / restore buffer
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

// The shell only mounts the Pages-hosted editor. device-boot.js is a stable,
// unhashed loader regenerated on every site deploy; it injects the current
// hashed bundle, which then runs in this page's origin so the upload stays
// same-origin on iOS Safari. The token and model stay in the query string,
// where the editor reads them itself; no-referrer keeps them out of Pages
// request headers.
const char kLocalUploader[] PROGMEM = R"HTML(<!doctype html>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="referrer" content="no-referrer">
<title>reTerminal Photo Magic</title>
<div id="app" style="font-family:sans-serif;padding:24px">Loading the photo editor&hellip;</div>
<script src="https://elohmeier.github.io/reterm/device-boot.js?v=5"
onerror="document.getElementById('app').textContent='Could not fetch the editor from the internet. Give this phone internet access, then reload.'"></script>)HTML";

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
  if (!saved || saved.size() != g_geometry.imageBytes()) {
    if (saved) saved.close();
    return false;
  }

  Serial.println("Restoring saved image after unused upload session");
  for (int16_t y = 0; y < g_geometry.height; ++y) {
    if (saved.read(mainRow, g_geometry.rowBytes) != g_geometry.rowBytes) {
      saved.close();
      return false;
    }
    g_board->writeRow(mainRow, y);
  }
  saved.close();
  g_board->refresh();
  g_board->hibernate();
  Serial.println("Saved image restored");
  return true;
}

StartupCommand receiveStartupCommand() {
  static constexpr char imageMagic[] = "E1IMG001";
  static constexpr char webMagic[] = "E1WEB001";
  uint8_t header[8];

  Serial.print("READY ");
  Serial.print(g_board->uartName());
  Serial.println(" E1IMG001|E1WEB001");
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
    // end-to-end test. Physical UART access already permits reflashing these
    // non-secure-boot development units; normal button sessions always use RNG.
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

  for (int16_t y = 0; y < g_geometry.height; ++y) {
    if (Serial.readBytes(mainRow, g_geometry.rowBytes) != g_geometry.rowBytes) {
      Serial.print("IMAGE ERROR short row ");
      Serial.println(y);
      return StartupCommand::None;
    }
    g_board->writeRow(mainRow, y);
    if ((y % 200) == 199) {
      Serial.print("IMAGE ROWS ");
      Serial.println(y + 1);
    }
  }
  Serial.println("IMAGE RECEIVED");
  g_board->refresh();
  g_board->hibernate();
  Serial.println("IMAGE DISPLAYED");
  return StartupCommand::Image;
}

void goToSleep() {
  Serial.println("Entering deep sleep; press the button for upload mode");
  Serial.flush();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  g_board->prepareSleep();
  delay(50);
  esp_deep_sleep_start();
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
  server.send(200, "application/json", g_board->statusJson());
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
                         server.clientContentLength() == g_geometry.imageBytes()
                     ? 202
                     : (acceptingImages ? 401 : 503);
    if (httpResult == 202 && !beginPendingImage()) httpResult = 507;
    if (httpResult == 202) sessionLastActivity = millis();
    if (requestAuthorized() &&
        server.clientContentLength() != g_geometry.imageBytes()) httpResult = 413;
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
      const size_t count = min(g_geometry.rowBytes - httpRowFill,
                               raw.currentSize - input);
      memcpy(httpRow + httpRowFill, raw.buf + input, count);
      httpRowFill += count;
      httpBytes += count;
      input += count;
      if (httpRowFill == g_geometry.rowBytes) {
        g_board->writeRow(httpRow, httpY++);
        httpRowFill = 0;
      }
    }
    // WebServer drains a raw request in one synchronous parse loop. Yield on
    // every chunk so the idle task and watchdog can run during a large body.
    delay(1);
    return;
  }
  if (raw.status == RAW_END && httpResult == 202) {
    if (httpBytes == g_geometry.imageBytes() && httpY == g_geometry.height &&
        httpRowFill == 0 && commitPendingImage()) {
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
    server.send(413, "application/json",
                "{\"error\":\"expected " + String(g_geometry.imageBytes()) +
                    " bytes\"}");
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
  // unreliable while a panel was refreshing and could leave the QR's address
  // associated with a station that no longer answered ARP.
  WiFi.setSleep(false);
  WiFi.setHostname(g_board->hostname());
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

bool provisionWifi() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lx",
           static_cast<unsigned long>(chip & 0xffffff));
  const String apName = String(g_board->hostname()) + "-" + suffix;
  const String apPassword = makeToken().substring(0, 12);
  g_board->drawProvisionScreen(apName, apPassword);

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

void runUploadSession() {
  Serial.println("Starting Wi-Fi upload session");
  sessionToken = uartSessionToken.isEmpty() ? makeToken() : uartSessionToken;
  authorizedClientIpSet = false;
  uartSessionToken = "";
  // Serving the uploader from the device makes Safari's upload same-origin.
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
                       "/?token=" + sessionToken + "&model=" + g_board->model();
  startUploadServer();
  Serial.print("UPLOAD API http://");
  Serial.println(WiFi.localIP());
  g_board->drawUploadScreen(qrUrl);
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
    g_board->refresh();
    g_board->hibernate();
    Serial.println("HTTP image displayed");
  } else {
    Serial.println("Upload timed out");
    if (!restoreSavedImage()) Serial.println("No saved image available to restore");
  }
}

}  // namespace

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

void run(Board &board) {
  g_board = &board;
  g_geometry = board.geometry();
  httpRow = new uint8_t[g_geometry.rowBytes];
  mainRow = new uint8_t[g_geometry.rowBytes];

  imageStorageReady = SPIFFS.begin(true);
  Serial.print("Image storage ready = ");
  Serial.println(imageStorageReady ? "yes" : "no");

  const bool buttonWake = board.wokeByButton();
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
  board.hibernate();
  goToSleep();
}

}  // namespace reterm
