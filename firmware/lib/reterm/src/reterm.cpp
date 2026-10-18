#include "reterm.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include <algorithm>
#include <vector>

namespace reterm {
namespace {

constexpr uint32_t kUploadWindowMs = 5 * 60 * 1000;
constexpr uint32_t kUploadAbsoluteMaxMs = 30 * 60 * 1000;
// Each OTA slot in partitions.csv is 12 MiB.
constexpr size_t kMaxFirmwareBytes = 0xC00000;
constexpr char kAllowedOrigin[] = "https://elohmeier.github.io";
constexpr char kSavedImagePath[] = "/current-image.bin";
constexpr char kBackupImagePath[] = "/previous-image.bin";
constexpr char kPendingImagePath[] = "/pending-image.bin";

// Pages (see "Pages" below).
constexpr char kPagesPrefsNamespace[] = "reterm-pages";
constexpr char kPhotoPath[] = "/photo.bin";
constexpr size_t kMaxPages = 8;
constexpr uint32_t kReturnHomeSeconds = 15 * 60;  // another view stays this long

struct Page {
  String name;
  String title;
  String sha256;
};
std::vector<Page> g_pages;
RTC_DATA_ATTR int8_t rtcShownPage = -1;      // index into the page list
RTC_DATA_ATTR uint32_t rtcPageShownAt = 0;   // epoch of the last flip
bool copyFile(const char *from, const char *to);

// Both boards route VBAT through a 1:2 divider on GPIO1 (ADC1) gated by a
// load switch on GPIO21, so the divider only draws current while sampling.
constexpr int kBatteryAdcPin = 1;
constexpr int kBatteryEnablePin = 21;

// Home Assistant / MQTT settings, persisted in their own NVS namespace next
// to the wificaptive credentials. An empty broker host disables the feature
// entirely: no timer wake is armed and check-ins are skipped.
struct HaConfig {
  String host;
  uint16_t port = 1883;
  String user;
  String password;
  uint32_t wakeMinutes = 0;
  bool enabled() const { return host.length() > 0; }
};
constexpr char kHaPrefsNamespace[] = "reterm-ha";
constexpr uint32_t kMaxWakeMinutes = 24 * 60;
constexpr uint32_t kDefaultWakeMinutes = 60;

HaConfig g_haConfig;
int g_batteryMv = -1;  // sampled once per boot, before any radio powers up
// True once this boot parked the panel controller. A second hibernate on an
// already-sleeping controller stalls on BUSY timeouts, so guard against it.
bool g_panelHibernated = false;

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
int otaResult = 500;
String otaAuthDebug;
volatile bool otaReady = false;
volatile bool otaResponseSent = false;
volatile bool imageReady = false;
volatile bool httpTaskRunning = false;
volatile bool acceptingImages = false;
volatile bool uploadResponseSent = false;
volatile uint32_t sessionLastActivity = 0;
// Set when an awake wait loop saw Board::menuRequested(); the session (or
// portal) tears down cleanly and run() opens the board's menu afterwards.
bool g_menuInterrupt = false;

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

void notePanelImage(bool fromServer, const String &sha256, uint32_t issued);
void notePanelRestored();

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
  notePanelImage(false, "", 0);
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
  notePanelRestored();
  Serial.println("Saved image restored");
  return true;
}

// Samples VBAT while everything else is still quiet. Wi-Fi load sags the
// battery enough to skew the reading, so run() calls this before any radio
// starts. The first conversion after enabling the divider is discarded.
int readBatteryMillivolts() {
  pinMode(kBatteryEnablePin, OUTPUT);
  digitalWrite(kBatteryEnablePin, HIGH);
  delay(10);
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);
  analogReadMilliVolts(kBatteryAdcPin);
  uint32_t sum = 0;
  for (int i = 0; i < 8; ++i) sum += analogReadMilliVolts(kBatteryAdcPin);
  digitalWrite(kBatteryEnablePin, LOW);
  return int(sum / 8) * 2;
}

// Single-cell LiPo open-circuit voltage mapped to charge, linear between
// curve points. Only an approximation: these boards have no fuel gauge.
int batteryPercent(int millivolts) {
  struct Point { int mv; int pct; };
  static constexpr Point curve[] = {{4200, 100}, {4060, 90}, {3980, 80},
                                    {3920, 70},  {3870, 60}, {3820, 50},
                                    {3790, 40},  {3770, 30}, {3740, 20},
                                    {3680, 10},  {3300, 0}};
  if (millivolts >= curve[0].mv) return 100;
  for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    if (millivolts >= curve[i].mv) {
      const Point &high = curve[i - 1];
      const Point &low = curve[i];
      return low.pct + (high.pct - low.pct) * (millivolts - low.mv) /
                           (high.mv - low.mv);
    }
  }
  return 0;
}

const char *wakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
      return "button";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    default:
      return "power";
  }
}

void loadHaConfig() {
  Preferences preferences;
  if (!preferences.begin(kHaPrefsNamespace, true)) return;
  g_haConfig.host = preferences.getString("host", "");
  g_haConfig.port = preferences.getUShort("port", 1883);
  g_haConfig.user = preferences.getString("user", "");
  g_haConfig.password = preferences.getString("pass", "");
  g_haConfig.wakeMinutes = preferences.getUInt("wake_min", 0);
  preferences.end();
}

bool saveHaConfig() {
  Preferences preferences;
  if (!preferences.begin(kHaPrefsNamespace, false)) return false;
  preferences.putString("host", g_haConfig.host);
  preferences.putUShort("port", g_haConfig.port);
  preferences.putString("user", g_haConfig.user);
  preferences.putString("pass", g_haConfig.password);
  preferences.putUInt("wake_min", g_haConfig.wakeMinutes);
  preferences.end();
  return true;
}

// Retained MQTT commands are re-delivered on every wake; the id of the last
// processed command is persisted so duplicates are dropped, not re-run.
String loadLastCommandId() {
  Preferences preferences;
  if (!preferences.begin(kHaPrefsNamespace, true)) return "";
  const String id = preferences.getString("last_cmd", "");
  preferences.end();
  return id;
}

void storeLastCommandId(const String &id) {
  Preferences preferences;
  if (!preferences.begin(kHaPrefsNamespace, false)) return;
  preferences.putString("last_cmd", id);
  preferences.end();
}

// ---------------------------------------------------------------------------
// Clock, wake schedule, and staleness.
//
// The device keeps wall-clock time in the RTC across deep sleep and syncs it
// over SNTP from the DHCP gateway on every check-in. A server publishes a
// retained schedule on reterm/<id>/schedule, which the device reads but
// never clears:
//
//   {"wake":[epoch,...], "valid_until":epoch, "sha256":"...",
//    "badge":[x,y,w,h], "tz":"POSIX TZ"}
//
// The device sleeps until the first listed wake still ahead (falling back to
// the wake interval when the list runs out or the clock is unknown), and
// marks a server-delivered image stale once `valid_until` passed or the
// server meant a different image (sha256) than the panel shows. A stale
// image gets a small "STAND <day> <time>" badge, drawn by the board as a
// partial refresh and removed again once fresh content or schedule arrives.

constexpr char kSchedPrefsNamespace[] = "reterm-sched";
constexpr char kDefaultTz[] = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Berlin
constexpr time_t kClockValidAfter = 1700000000;  // earlier means never set
constexpr size_t kMaxScheduledWakes = 96;
constexpr uint32_t kMinSleepSeconds = 20;

struct Schedule {
  std::vector<uint32_t> wakes;  // ascending epochs
  uint32_t validUntil = 0;
  String sha256;
  int16_t badge[4] = {0, 0, 0, 0};
  bool hasBadge = false;
  String tz;
};

// What the panel shows. Only images delivered by an MQTT image command can
// go stale; manual uploads are never badged.
struct PanelState {
  bool fromServer = false;
  String sha256;
  uint32_t issued = 0;     // content time from the command, else display time
  bool clean = true;       // the panel shows the saved image, nothing else
  bool badge = false;      // a badge is drawn over it
  String badgeText;
  int16_t badgeBox[4] = {0, 0, 0, 0};
};

Schedule g_schedule;
PanelState g_panel;

// Latency marks live in RTC memory so wakes without a check-in (page flips,
// menus) report at the next one. Names are copied: a pointer into flash
// would dangle after an OTA update.
struct Mark {
  char name[12];
  uint32_t ms;
  uint16_t boot;
};
constexpr size_t kMaxMarks = 48;
RTC_DATA_ATTR Mark g_marks[kMaxMarks];
RTC_DATA_ATTR uint32_t g_markCount = 0;
RTC_DATA_ATTR uint16_t g_bootCount = 0;
bool g_clockSynced = false;  // SNTP completed during this boot
bool g_badgeChecked = false;  // updateBadge() already ran this boot
bool g_checkinOk = false;     // the last check-in reached the broker

bool clockValid() { return time(nullptr) > kClockValidAfter; }

uint32_t nowEpoch() { return clockValid() ? uint32_t(time(nullptr)) : 0; }

void applyTimezone() {
  setenv("TZ", g_schedule.tz.length() ? g_schedule.tz.c_str() : kDefaultTz, 1);
  tzset();
}

bool parseSchedule(const String &json, Schedule &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  Schedule parsed;
  for (JsonVariant wake : doc["wake"].as<JsonArray>()) {
    if (parsed.wakes.size() >= kMaxScheduledWakes) break;
    const uint32_t at = wake | 0u;
    if (at > kClockValidAfter) parsed.wakes.push_back(at);
  }
  std::sort(parsed.wakes.begin(), parsed.wakes.end());
  parsed.validUntil = doc["valid_until"] | 0u;
  parsed.sha256 = String(doc["sha256"] | "");
  parsed.sha256.toLowerCase();
  JsonArray box = doc["badge"].as<JsonArray>();
  if (box.size() == 4) {
    for (int i = 0; i < 4; ++i) parsed.badge[i] = box[i] | 0;
    parsed.hasBadge = parsed.badge[2] > 0 && parsed.badge[3] > 0;
  }
  parsed.tz = String(doc["tz"] | "");
  out = parsed;
  return true;
}

void loadPanelState() {
  Preferences preferences;
  if (!preferences.begin(kSchedPrefsNamespace, true)) return;
  parseSchedule(preferences.getString("sched", ""), g_schedule);
  g_panel.fromServer = preferences.getBool("server", false);
  g_panel.sha256 = preferences.getString("sha", "");
  g_panel.issued = preferences.getUInt("issued", 0);
  g_panel.clean = preferences.getBool("clean", true);
  g_panel.badge = preferences.getBool("badge", false);
  g_panel.badgeText = preferences.getString("btext", "");
  preferences.getBytes("bbox", g_panel.badgeBox, sizeof(g_panel.badgeBox));
  preferences.end();
  applyTimezone();
}

void savePanelState() {
  Preferences preferences;
  if (!preferences.begin(kSchedPrefsNamespace, false)) return;
  preferences.putBool("server", g_panel.fromServer);
  preferences.putString("sha", g_panel.sha256);
  preferences.putUInt("issued", g_panel.issued);
  preferences.putBool("clean", g_panel.clean);
  preferences.putBool("badge", g_panel.badge);
  preferences.putString("btext", g_panel.badgeText);
  preferences.putBytes("bbox", g_panel.badgeBox, sizeof(g_panel.badgeBox));
  preferences.end();
}

void storeSchedule(const String &json) {
  Schedule parsed;
  if (!parseSchedule(json, parsed)) {
    Serial.println("Ignoring malformed schedule");
    return;
  }
  g_schedule = parsed;
  applyTimezone();
  Preferences preferences;
  if (!preferences.begin(kSchedPrefsNamespace, false)) return;
  preferences.putString("sched", json);
  preferences.end();
  Serial.print("Schedule stored, wakes = ");
  Serial.println(unsigned(g_schedule.wakes.size()));
}

// Any image that reached the panel through the runtime: a server image
// (fromServer, with its sha256 and content time) or a manual upload.
void notePanelImage(bool fromServer, const String &sha256, uint32_t issued) {
  g_panel.fromServer = fromServer;
  g_panel.sha256 = sha256;
  g_panel.sha256.toLowerCase();
  g_panel.issued = issued ? issued : nowEpoch();
  g_panel.clean = true;
  g_panel.badge = false;
  savePanelState();
}

void notePanelRestored() {
  g_panel.clean = true;
  g_panel.badge = false;
  savePanelState();
}

// One NTP request to the DHCP gateway (OPNsense-style networks serve NTP
// there and often block it elsewhere). lwIP's SNTP client delays its first
// request by seconds, which every check-in paid for; a single UDP exchange
// takes a few milliseconds. Best effort: the RTC carries the time across
// sleeps, a missed sync only adds drift.
void syncClock() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiUDP udp;
  if (!udp.begin(0)) return;
  uint8_t packet[48] = {0x23};  // LI 0, version 4, mode 3 (client)
  const IPAddress gateway = WiFi.gatewayIP();
  udp.beginPacket(gateway, 123);
  udp.write(packet, sizeof(packet));
  udp.endPacket();
  const uint32_t sentAt = millis();
  g_clockSynced = false;
  while (millis() - sentAt < 1000) {
    if (udp.parsePacket() >= 48 && udp.read(packet, sizeof(packet)) == 48) {
      const uint32_t seconds = uint32_t(packet[40]) << 24 |
                               uint32_t(packet[41]) << 16 |
                               uint32_t(packet[42]) << 8 | packet[43];
      const uint32_t fraction = uint32_t(packet[44]) << 24 |
                                uint32_t(packet[45]) << 16 |
                                uint32_t(packet[46]) << 8 | packet[47];
      constexpr uint32_t kNtpToUnix = 2208988800UL;
      if (seconds > kNtpToUnix + kClockValidAfter) {
        // Half the round trip approximates the server's send delay.
        const uint32_t halfTripUs = (millis() - sentAt) * 500;
        timeval now;
        now.tv_sec = time_t(seconds - kNtpToUnix);
        now.tv_usec = suseconds_t((uint64_t(fraction) * 1000000ULL) >> 32) + halfTripUs;
        if (now.tv_usec >= 1000000) {
          now.tv_sec += 1;
          now.tv_usec -= 1000000;
        }
        settimeofday(&now, nullptr);
        g_clockSynced = true;
      }
      break;
    }
    delay(2);
  }
  udp.stop();
  applyTimezone();
  Serial.print(g_clockSynced ? "Clock synced from " : "Clock sync failed via ");
  Serial.println(gateway);
  mark(g_clockSynced ? "ntp" : "ntp-fail");
}

// Seconds to sleep and the planned wall-clock wake (0 when unknown). Zero
// seconds means no timer wake at all (integration off or interval 0).
uint32_t plannedSleepSeconds(uint32_t *wakeAt = nullptr) {
  if (wakeAt) *wakeAt = 0;
  if (!g_haConfig.enabled() || g_haConfig.wakeMinutes == 0) return 0;
  const uint32_t fallback = g_haConfig.wakeMinutes * 60;
  const uint32_t now = nowEpoch();
  if (now) {
    for (uint32_t at : g_schedule.wakes) {
      if (at < now + kMinSleepSeconds) continue;
      if (at - now > kMaxWakeMinutes * 60) break;
      if (wakeAt) *wakeAt = at;
      return at - now;
    }
    if (wakeAt) *wakeAt = now + fallback;
  }
  return fallback;
}

// The longest planned sleep ahead, so Home Assistant does not mark the
// device unavailable across a scheduled quiet period.
uint32_t longestPlannedGapSeconds() {
  uint32_t longest = g_haConfig.wakeMinutes * 60;
  uint32_t previous = nowEpoch();
  if (!previous) return longest;
  for (uint32_t at : g_schedule.wakes) {
    if (at <= previous) continue;
    if (at - previous <= kMaxWakeMinutes * 60)
      longest = max(longest, at - previous);
    previous = at;
  }
  return longest;
}

bool panelStale() {
  if (!g_panel.fromServer || !g_schedule.validUntil) return false;
  if (g_schedule.sha256.length() == 64 && g_schedule.sha256 != g_panel.sha256)
    return true;
  const uint32_t now = nowEpoch();
  return now && now > g_schedule.validUntil;
}

String badgeText() {
  static constexpr const char *days[] = {"SO", "MO", "DI", "MI",
                                         "DO", "FR", "SA"};
  if (!g_panel.issued) return "NICHT AKTUELL";
  const time_t issued = g_panel.issued;
  struct tm local;
  localtime_r(&issued, &local);
  char text[32];
  snprintf(text, sizeof(text), "STAND %s %02d:%02d", days[local.tm_wday],
           local.tm_hour, local.tm_min);
  return text;
}

// Draws or removes the badge to match panelStale(). Returns true when the
// panel was touched (and hibernated by the board).
bool updateBadge() {
  const bool stale = panelStale();
  if (stale && !g_panel.badge && g_panel.clean && g_schedule.hasBadge) {
    const String text = badgeText();
    const int16_t *box = g_schedule.badge;
    if (!g_board->drawBadge(box[0], box[1], box[2], box[3], text.c_str(), true))
      return false;
    Serial.print("Stale image badged: ");
    Serial.println(text);
    g_panel.badge = true;
    g_panel.badgeText = text;
    memcpy(g_panel.badgeBox, box, sizeof(g_panel.badgeBox));
    savePanelState();
    return true;
  }
  if (!stale && g_panel.badge && g_panel.clean) {
    const int16_t *box = g_panel.badgeBox;
    if (!g_board->drawBadge(box[0], box[1], box[2], box[3],
                            g_panel.badgeText.c_str(), false))
      return false;
    Serial.println("Image current again; badge removed");
    g_panel.badge = false;
    savePanelState();
    return true;
  }
  return false;
}

// The board supplies the static geometry object; reopen it and append the
// per-boot fields so /api/status and MQTT report live device state.
String dynamicStatusJson() {
  String json(g_board->statusJson());
  json.remove(json.length() - 1);
  json += ",\"fw\":\"" RETERM_FW_VERSION "\",\"battery_mv\":";
  json += g_batteryMv;
  json += ",\"battery_pct\":";
  json += batteryPercent(g_batteryMv);
  json += ",\"rssi\":";
  json += WiFi.RSSI();
  json += ",\"wake\":\"";
  json += wakeReason();
  json += "\",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"wake_interval_min\":";
  json += g_haConfig.wakeMinutes;
  uint32_t wakeAt = 0;
  plannedSleepSeconds(&wakeAt);
  json += ",\"next_wake\":";
  json += wakeAt;
  if (wakeAt) {
    const time_t at = wakeAt;
    struct tm utc;
    gmtime_r(&at, &utc);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &utc);
    json += ",\"next_wake_iso\":\"";
    json += iso;
    json += "\"";
  }
  json += ",\"clock\":";
  json += clockValid() ? "true" : "false";
  json += ",\"stale\":";
  json += panelStale() ? "true" : "false";
  json += ",\"badge\":";
  json += g_panel.badge ? "true" : "false";
  json += "}";
  return json;
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
    // end-to-end test. That fixed token must never ship in release builds;
    // tools/send-image.py enables RETERM_UPLOAD_FIXTURE when it builds a test
    // firmware for --transport http. Normal button sessions always use RNG.
    const uint32_t tokenDeadline = millis() + 250;
    while (Serial.available() < 32 && int32_t(tokenDeadline - millis()) > 0) delay(2);
    if (Serial.available() >= 32) {
      char testToken[33] = {};
      if (Serial.readBytes(reinterpret_cast<uint8_t *>(testToken), 32) == 32) {
#if defined(RETERM_UPLOAD_FIXTURE)
        // Receipt of the 32-byte fixture suffix is the opt-in signal. Use a
        // stable printable token even if this high-speed UART corrupts a byte.
        uartSessionToken = "0123456789abcdef0123456789abcdef";
        Serial.println("UART test token accepted");
#else
        // Drain the suffix so it cannot linger in the receive buffer, then
        // fall through to a normal randomly tokenized session.
        Serial.println("UART fixture disabled; using a random session token");
#endif
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
  notePanelImage(false, "", 0);
  g_board->refresh();
  g_board->hibernate();
  Serial.println("IMAGE DISPLAYED");
  return StartupCommand::Image;
}

void goToSleep() {
  Serial.println("Entering deep sleep; press the button for upload mode");
  uint32_t wakeAt = 0;
  const uint32_t sleepSeconds = plannedSleepSeconds(&wakeAt);
  if (sleepSeconds) {
    Serial.print("Timer wake armed for Home Assistant check-in, seconds = ");
    Serial.print(sleepSeconds);
    Serial.print(wakeAt ? ", at epoch " : "");
    if (wakeAt) Serial.print(wakeAt);
    Serial.println();
  }
  Serial.flush();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  g_board->prepareSleep();
  if (sleepSeconds) esp_sleep_enable_timer_wakeup(uint64_t(sleepSeconds) * 1000000ULL);
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

bool tokenizedRoute() {
  // Only the exact, randomly generated per-session URIs are registered with
  // handlers. If dispatch reached one of them, its path is already the bearer
  // credential; avoid redundantly comparing mutable String storage.
  return server.uri().startsWith("/api/image/") ||
         server.uri().startsWith("/api/firmware/");
}

bool requestAuthorized() {
  if (!originAllowed()) return false;
  if (tokenizedRoute()) return true;
  if (constantTimeToken(server.header("X-Upload-Token"))) return true;
  if (authorizedClientIpSet && server.client().remoteIP() == authorizedClientIp)
    return true;
  return false;
}

String requestAuthDebug() {
  const String origin = server.header("Origin");
  String detail = "route=";
  detail += tokenizedRoute() ? "tokenized" : "legacy";
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
  server.send(200, "application/json", dynamicStatusJson());
}

void forgetWifiCredentials() {
  Preferences preferences;
  if (preferences.begin("wificaptive", false)) {
    preferences.clear();
    preferences.end();
  }
}

// Clears the saved home network. The current association stays up so the
// response can be delivered; the next button wake opens the captive portal.
void handleWifiForget() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sessionLastActivity = millis();
  forgetWifiCredentials();
  Serial.println("Saved Wi-Fi credentials cleared by API request");
  server.send(200, "application/json", "{\"status\":\"forgotten\"}");
}

String haConfigJson() {
  JsonDocument doc;
  doc["mqtt_host"] = g_haConfig.host;
  doc["mqtt_port"] = g_haConfig.port;
  doc["mqtt_user"] = g_haConfig.user;
  doc["mqtt_password_set"] = g_haConfig.password.length() > 0;
  doc["wake_interval_min"] = g_haConfig.wakeMinutes;
  String json;
  serializeJson(doc, json);
  return json;
}

void handleConfigGet() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sessionLastActivity = millis();
  server.send(200, "application/json", haConfigJson());
}

// Accepts form fields mqtt_host, mqtt_port, mqtt_user, mqtt_password, and
// wake_interval_min; only supplied fields change. Setting a broker host with
// no interval on record enables the default hourly check-in, and an empty
// host disables the Home Assistant integration entirely.
void handleConfigPost() {
  addCorsHeaders();
  if (!requestAuthorized()) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  sessionLastActivity = millis();
  if (server.hasArg("mqtt_host")) g_haConfig.host = server.arg("mqtt_host");
  if (server.hasArg("mqtt_port")) {
    const long port = server.arg("mqtt_port").toInt();
    if (port > 0 && port <= 65535) g_haConfig.port = uint16_t(port);
  }
  if (server.hasArg("mqtt_user")) g_haConfig.user = server.arg("mqtt_user");
  if (server.hasArg("mqtt_password"))
    g_haConfig.password = server.arg("mqtt_password");
  if (server.hasArg("wake_interval_min")) {
    const long minutes = server.arg("wake_interval_min").toInt();
    g_haConfig.wakeMinutes = uint32_t(constrain(minutes, 0L, long(kMaxWakeMinutes)));
  } else if (g_haConfig.enabled() && g_haConfig.wakeMinutes == 0) {
    g_haConfig.wakeMinutes = kDefaultWakeMinutes;
  }
  if (!saveHaConfig()) {
    server.send(500, "application/json", "{\"error\":\"could not save config\"}");
    return;
  }
  Serial.println("Home Assistant configuration updated by API request");
  server.send(200, "application/json", haConfigJson());
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
      // The last manual upload stays reachable as the "Foto" page.
      copyFile(kSavedImagePath, kPhotoPath);
      rtcShownPage = int8_t(g_pages.size());
      rtcPageShownAt = nowEpoch();
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

// Streams an application image into the inactive OTA slot. partitions.csv
// reserves app0/app1; Update selects the slot that is not currently running,
// validates the image header while writing, and flips otadata on success.
void handleFirmwareRaw() {
  HTTPRaw &raw = server.raw();
  if (raw.status == RAW_START) {
    // Match the image path: hold an early POST until the QR refresh has
    // finished so flash writes never overlap a panel refresh.
    const uint32_t readyDeadline = millis() + 60000;
    while (!acceptingImages && httpTaskRunning &&
           int32_t(readyDeadline - millis()) > 0) {
      delay(10);
    }
    otaAuthDebug = requestAuthDebug();
    const size_t length = server.clientContentLength();
    otaResult = acceptingImages && requestAuthorized()
                    ? 202
                    : (acceptingImages ? 401 : 503);
    if (otaResult == 202 && (length == 0 || length > kMaxFirmwareBytes))
      otaResult = 413;
    if (otaResult == 202 && !Update.begin(length, U_FLASH)) {
      Update.abort();
      otaResult = 507;
    }
    if (otaResult == 202) sessionLastActivity = millis();
    return;
  }
  if (raw.status == RAW_ABORTED) {
    Update.abort();
    if (otaResult == 202) otaResult = 400;
    return;
  }
  if (raw.status == RAW_WRITE && otaResult == 202) {
    sessionLastActivity = millis();
    if (Update.write(raw.buf, raw.currentSize) != raw.currentSize) {
      Update.abort();
      otaResult = 507;
      return;
    }
    // Yield like the image path so IDLE0 and the watchdog can run while the
    // synchronous raw parser drains a multi-megabyte body.
    delay(1);
    return;
  }
  if (raw.status == RAW_END && otaResult == 202) {
    if (Update.end()) {
      otaReady = true;
    } else {
      Update.abort();
      otaResult = 400;
    }
  }
}

void handleFirmwareResult() {
  addCorsHeaders();
  if (otaResult == 202 && otaReady) {
    server.send(202, "application/json", "{\"status\":\"rebooting\"}");
    otaResponseSent = true;
  } else if (otaResult == 413) {
    server.send(413, "application/json",
                "{\"error\":\"expected 1 to " + String(kMaxFirmwareBytes) +
                    " firmware bytes\"}");
  } else if (otaResult == 401) {
    Serial.print("Firmware update unauthorized: ");
    Serial.println(otaAuthDebug);
    server.send(401, "application/json",
                "{\"error\":\"unauthorized (" + otaAuthDebug + ")\"}");
  } else if (otaResult == 503) {
    server.send(503, "application/json", "{\"error\":\"display is starting\"}");
  } else if (otaResult == 507) {
    server.send(507, "application/json",
                "{\"error\":\"could not start update (" +
                    String(Update.errorString()) + ")\"}");
  } else {
    server.send(400, "application/json",
                "{\"error\":\"incomplete firmware (" +
                    String(Update.errorString()) + ")\"}");
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
  const String firmwarePath = "/api/firmware/" + sessionToken;
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
  server.on("/api/firmware", HTTP_POST, handleFirmwareResult, handleFirmwareRaw);
  server.on(firmwarePath, HTTP_POST, handleFirmwareResult, handleFirmwareRaw);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/config", HTTP_OPTIONS, handleOptions);
  server.on("/api/image", HTTP_OPTIONS, handleOptions);
  server.on(imagePath, HTTP_OPTIONS, handleOptions);
  server.on("/api/firmware", HTTP_OPTIONS, handleOptions);
  server.on(firmwarePath, HTTP_OPTIONS, handleOptions);
  server.on("/api/wifi/forget", HTTP_OPTIONS, handleOptions);
  server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();
  httpTaskRunning = true;
  // Core 0 hosts the ESP32 Wi-Fi/system work. Running the synchronous raw-body
  // parser there can starve IDLE0 long enough to trigger the task watchdog.
  xTaskCreatePinnedToCore(serveHttp, "upload-http", 6144, nullptr, 1, nullptr, 1);
}

// Fast reconnect: the last association's channel and BSSID survive deep
// sleep in RTC memory, so the next connection skips the scan. Lost on power
// loss, and dropped after a failed fast attempt.
RTC_DATA_ATTR uint8_t rtcWifiBssid[6];
RTC_DATA_ATTR int32_t rtcWifiChannel = 0;
RTC_DATA_ATTR uint32_t rtcWifiSsidHash = 0;
String g_wifiSsid;
String g_wifiPassword;
bool g_wifiFastAttempt = false;
constexpr uint32_t kFastConnectMs = 4000;

uint32_t fnv1a(const String &text) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < text.length(); ++i)
    hash = (hash ^ uint8_t(text[i])) * 16777619u;
  return hash;
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
  g_wifiSsid = ssid;
  g_wifiPassword = password;
  g_wifiFastAttempt = rtcWifiChannel > 0 && rtcWifiSsidHash == fnv1a(ssid);
  if (g_wifiFastAttempt)
    WiFi.begin(ssid.c_str(), password.c_str(), rtcWifiChannel, rtcWifiBssid);
  else
    WiFi.begin(ssid.c_str(), password.c_str());
  return true;
}

bool waitForWifi(uint32_t timeoutMs) {
  const uint32_t started = millis();
  const uint32_t deadline = started + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && int32_t(deadline - millis()) > 0) {
    if (g_wifiFastAttempt && millis() - started > kFastConnectMs) {
      // The cached access point did not answer (moved channel, roamed, or
      // gone): forget it and scan like a cold start.
      Serial.println("Fast Wi-Fi reconnect failed; scanning");
      g_wifiFastAttempt = false;
      rtcWifiChannel = 0;
      WiFi.disconnect();
      WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());
    }
    delay(20);
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  rtcWifiChannel = WiFi.channel();
  memcpy(rtcWifiBssid, WiFi.BSSID(), sizeof(rtcWifiBssid));
  rtcWifiSsidHash = fnv1a(g_wifiSsid);
  mark(g_wifiFastAttempt ? "wifi-fast" : "wifi-scan");
  return true;
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
  notePanelShowsSavedImage(false);
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
    if (g_board->menuRequested()) {
      Serial.println("Menu requested; closing the provisioning portal");
      g_menuInterrupt = true;
      break;
    }
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
    if (g_menuInterrupt) return;  // the menu repaints; skip the restore
    Serial.println("Wi-Fi connection/provisioning failed");
    // The panel still shows the dead provisioning screen; bring back the last
    // complete image like the session-timeout paths do.
    if (!restoreSavedImage()) Serial.println("No saved image available to restore");
    return;
  }
  Serial.print("Wi-Fi ready ms = ");
  Serial.println(millis() - wifiStartedAt);
  const String qrUrl = "http://" + WiFi.localIP().toString() +
                       "/?token=" + sessionToken + "&model=" + g_board->model();
  startUploadServer();
  Serial.print("UPLOAD API http://");
  Serial.println(WiFi.localIP());
  notePanelShowsSavedImage(false);
  g_board->drawUploadScreen(qrUrl);
  mark("qr");
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
  while (!uploadResponseSent && !otaResponseSent &&
         millis() - sessionLastActivity < kUploadWindowMs &&
         millis() - sessionStarted < kUploadAbsoluteMaxMs) {
    if (g_board->menuRequested()) {
      Serial.println("Menu requested; closing the upload session");
      g_menuInterrupt = true;
      break;
    }
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
  if (uploadResponseSent || otaResponseSent) delay(1000);
  acceptingImages = false;
  httpTaskRunning = false;
  delay(10);
  server.stop();
  sessionToken = "";

  if (otaReady) {
    // Update.end() already validated the image and flipped otadata to the
    // other slot. The new firmware boots, finds no host command, preserves
    // the panel image, and deep sleeps.
    Serial.println("Firmware update received; rebooting into the new image");
    Serial.flush();
    delay(100);
    ESP.restart();
  }

  if (imageReady) {
    Serial.println("HTTP image received; refreshing display");
    g_board->refresh();
    g_board->hibernate();
    Serial.println("HTTP image displayed");
  } else if (g_menuInterrupt) {
    // The board's menu repaints the panel next; just drop any partial body.
    abortPendingImage();
  } else {
    Serial.println("Upload timed out");
    if (!restoreSavedImage()) Serial.println("No saved image available to restore");
  }
}

// Runs an upload session, then services any menu request that interrupted it
// (or that a wait loop recorded on the way). The board's menu may chain
// straight into another session — e.g. a "send a photo" entry — repeatedly.
void runSessionAndMenu() {
  runUploadSession();
  while (g_menuInterrupt) {
    g_menuInterrupt = false;
    if (g_board->onMenu() == MenuAction::UploadSession) runUploadSession();
  }
}

// ---------------------------------------------------------------------------
// Home Assistant check-in: on every timer wake (and after upload sessions)
// the device publishes its stats over MQTT with HA discovery metadata, then
// consumes retained command topics. HA can never reach a deep-sleeping
// device, so retained messages are the command channel: whatever HA last
// published is applied on the next wake.

WiFiClient mqttSocket;
PubSubClient mqtt(mqttSocket);
String mqttCmdTopic;
String mqttSetWakeTopic;
String mqttStateTopic;
String mqttEventTopic;
String mqttScheduleTopic;
String mqttSyncTopic;
bool pendingSyncSeen = false;
String pendingCmdPayload;
String pendingWakePayload;
String pendingSchedulePayload;
bool pendingCmdSeen = false;
bool pendingWakeSeen = false;
bool pendingScheduleSeen = false;

// The AP-name suffix scheme: stable across reflashes, unique per unit.
String haDeviceId() {
  const uint64_t chip = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lx",
           static_cast<unsigned long>(chip & 0xffffff));
  return String(g_board->hostname()) + "-" + suffix;
}

void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  String value;
  value.reserve(length);
  for (unsigned int i = 0; i < length; ++i) value += char(payload[i]);
  if (mqttCmdTopic == topic) {
    pendingCmdPayload = value;
    pendingCmdSeen = true;
  } else if (mqttSetWakeTopic == topic) {
    pendingWakePayload = value;
    pendingWakeSeen = true;
  } else if (mqttScheduleTopic == topic) {
    pendingSchedulePayload = value;
    pendingScheduleSeen = true;
  } else if (mqttSyncTopic == topic) {
    pendingSyncSeen = true;
  }
}

void clearRetained(const String &topic) {
  mqtt.publish(topic.c_str(), nullptr, 0, true);
}

void publishEvent(const String &json) {
  mqtt.publish(mqttEventTopic.c_str(), json.c_str(), false);
}

String haStatePayload() { return dynamicStatusJson(); }

// One retained device-based discovery payload (HA 2024.6+) declaring every
// entity. Republished each check-in so a wiped broker heals itself.
String haDiscoveryPayload(const String &id) {
  JsonDocument doc;
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"][0] = id;
  device["name"] = String("reTerminal ") + g_board->uartName() + " " +
                   id.substring(id.length() - 6);
  device["manufacturer"] = "Seeed Studio";
  device["model"] = g_board->model();
  device["sw_version"] = RETERM_FW_VERSION;
  JsonObject origin = doc["origin"].to<JsonObject>();
  origin["name"] = "reterm";
  origin["url"] = "https://github.com/elohmeier/reterm";
  JsonObject components = doc["components"].to<JsonObject>();

  const auto sensor = [&](const char *key, const char *name,
                          const char *deviceClass, const char *unit,
                          const char *valueTemplate, bool diagnostic) {
    JsonObject component = components[key].to<JsonObject>();
    component["platform"] = "sensor";
    component["unique_id"] = id + "_" + key;
    component["name"] = name;
    if (deviceClass) component["device_class"] = deviceClass;
    if (unit) {
      component["unit_of_measurement"] = unit;
      component["state_class"] = "measurement";
    }
    component["value_template"] = valueTemplate;
    component["state_topic"] = mqttStateTopic;
    if (diagnostic) component["entity_category"] = "diagnostic";
    // Retained state persists across HA restarts; expiry marks the device
    // unavailable when it misses the next check-in by a full interval, also
    // across the longest scheduled gap (quiet hours).
    if (g_haConfig.wakeMinutes > 0)
      component["expire_after"] =
          longestPlannedGapSeconds() + g_haConfig.wakeMinutes * 60 + 300;
  };
  sensor("battery", "Battery", "battery", "%",
         "{{ value_json.battery_pct }}", false);
  sensor("voltage", "Battery voltage", "voltage", "mV",
         "{{ value_json.battery_mv }}", true);
  sensor("rssi", "Wi-Fi RSSI", "signal_strength", "dBm",
         "{{ value_json.rssi }}", true);
  sensor("wake", "Wake reason", nullptr, nullptr, "{{ value_json.wake }}",
         true);
  sensor("ip", "IP address", nullptr, nullptr, "{{ value_json.ip }}", true);
  sensor("next_wake", "Next wake", "timestamp", nullptr,
         "{{ value_json.next_wake_iso | default(None) }}", true);

  JsonObject stale = components["stale"].to<JsonObject>();
  stale["platform"] = "binary_sensor";
  stale["unique_id"] = id + "_stale";
  stale["name"] = "Image out of date";
  stale["device_class"] = "problem";
  stale["state_topic"] = mqttStateTopic;
  stale["value_template"] = "{{ 'ON' if value_json.stale else 'OFF' }}";

  JsonObject interval = components["wake_interval"].to<JsonObject>();
  interval["platform"] = "number";
  interval["unique_id"] = id + "_wake_interval";
  interval["name"] = "Wake interval";
  interval["command_topic"] = mqttSetWakeTopic;
  interval["state_topic"] = mqttStateTopic;
  interval["value_template"] = "{{ value_json.wake_interval_min }}";
  interval["min"] = 0;
  interval["max"] = kMaxWakeMinutes;
  interval["step"] = 5;
  interval["unit_of_measurement"] = "min";
  interval["mode"] = "box";
  interval["entity_category"] = "config";
  // The broker must hold the change until the sleeping device's next wake.
  interval["retain"] = true;

  String json;
  serializeJson(doc, json);
  return json;
}

// Downloads a full packed framebuffer (the device wire format, same bytes as
// POST /api/image) and displays it, mirroring the HTTP upload path's SPIFFS
// pending/commit sequence. The optional sha256 is verified before the panel
// refreshes or the image is committed.
bool fetchAndDisplayImage(const String &url, const String &sha256Hex,
                          uint32_t issued) {
  HTTPClient http;
  if (!url.startsWith("http://") || !http.begin(url)) {
    Serial.println("Image command rejected: only http:// URLs are supported");
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("Image fetch failed, HTTP ");
    Serial.println(code);
    http.end();
    return false;
  }
  if (http.getSize() != int(g_geometry.imageBytes())) {
    Serial.print("Image fetch rejected: expected bytes = ");
    Serial.println(g_geometry.imageBytes());
    http.end();
    return false;
  }
  if (!beginPendingImage()) {
    http.end();
    return false;
  }

  const bool wantHash = sha256Hex.length() == 64;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (wantHash) mbedtls_sha256_starts_ret(&sha, 0);

  WiFiClient &stream = http.getStream();
  stream.setTimeout(15000);
  bool ok = true;
  for (int16_t y = 0; y < g_geometry.height && ok; ++y) {
    if (stream.readBytes(mainRow, g_geometry.rowBytes) != g_geometry.rowBytes) {
      Serial.print("Image fetch failed: short row ");
      Serial.println(y);
      ok = false;
      break;
    }
    if (pendingImage.write(mainRow, g_geometry.rowBytes) !=
        g_geometry.rowBytes) {
      ok = false;
      break;
    }
    if (wantHash) mbedtls_sha256_update_ret(&sha, mainRow, g_geometry.rowBytes);
    g_board->writeRow(mainRow, y);
    // A large body outlasts the MQTT keepalive; service the connection.
    if ((y % 100) == 99) mqtt.loop();
  }
  http.end();

  if (ok && wantHash) {
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    char hex[65];
    static constexpr char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
      hex[i * 2] = alphabet[digest[i] >> 4];
      hex[i * 2 + 1] = alphabet[digest[i] & 0x0f];
    }
    hex[64] = 0;
    String expected = sha256Hex;
    expected.toLowerCase();
    if (expected != hex) {
      Serial.println("Image fetch failed: sha256 mismatch");
      ok = false;
    }
  }
  mbedtls_sha256_free(&sha);

  if (!ok) {
    abortPendingImage();
    return false;
  }
  if (!commitPendingImage()) return false;
  notePanelImage(true, wantHash ? sha256Hex : String(), issued);
  rtcShownPage = -1;  // a single image, outside any page set
  Serial.println("MQTT image received; refreshing display");
  g_board->refresh();
  g_board->hibernate();
  g_panelHibernated = true;
  Serial.println("MQTT image displayed");
  return true;
}

// ---------------------------------------------------------------------------
// Pages: named full-screen images from a `pages` command, stored as
// /pg-<name>.bin with their sha256 in NVS. The first is the home view and
// also becomes the saved image, so restore and staleness logic see it like
// any server image. A manually uploaded photo is kept as /photo.bin and
// offered as a last, local "Foto" page.


String pagePath(const String &name) { return "/pg-" + name + ".bin"; }

void loadPages() {
  g_pages.clear();
  Preferences preferences;
  if (!preferences.begin(kPagesPrefsNamespace, true)) return;
  const String json = preferences.getString("index", "[]");
  preferences.end();
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (JsonObject page : doc.as<JsonArray>())
    g_pages.push_back({page["n"] | "", page["t"] | "", page["s"] | ""});
}

void savePages() {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  for (const Page &page : g_pages) {
    JsonObject entry = list.add<JsonObject>();
    entry["n"] = page.name;
    entry["t"] = page.title;
    entry["s"] = page.sha256;
  }
  String json;
  serializeJson(doc, json);
  Preferences preferences;
  if (!preferences.begin(kPagesPrefsNamespace, false)) return;
  preferences.putString("index", json);
  preferences.end();
}

bool validPageName(const String &name) {
  if (name.isEmpty() || name.length() > 12 || name == "photo") return false;
  for (char c : name)
    if (!(isdigit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  return true;
}

// Downloads one packed framebuffer into `path` (via a temp file), checking
// the mandatory sha256; the panel is not touched.
bool fetchToFile(const String &url, const String &sha256Hex, const String &path) {
  if (!imageStorageReady || sha256Hex.length() != 64) return false;
  HTTPClient http;
  if (!url.startsWith("http://") || !http.begin(url)) return false;
  http.setTimeout(15000);
  if (http.GET() != HTTP_CODE_OK || http.getSize() != int(g_geometry.imageBytes())) {
    http.end();
    return false;
  }
  const String temp = path + ".tmp";
  File file = SPIFFS.open(temp, FILE_WRITE);
  if (!file) {
    http.end();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient &stream = http.getStream();
  stream.setTimeout(15000);
  bool ok = true;
  for (int16_t y = 0; y < g_geometry.height && ok; ++y) {
    ok = stream.readBytes(mainRow, g_geometry.rowBytes) == g_geometry.rowBytes &&
         file.write(mainRow, g_geometry.rowBytes) == g_geometry.rowBytes;
    if (ok) mbedtls_sha256_update_ret(&sha, mainRow, g_geometry.rowBytes);
    if ((y % 100) == 99) mqtt.loop();
  }
  http.end();
  file.close();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  String expected = sha256Hex;
  expected.toLowerCase();
  if (!ok || expected != hex) {
    SPIFFS.remove(temp);
    return false;
  }
  SPIFFS.remove(path);
  return SPIFFS.rename(temp, path);
}

// Shows a stored image file and makes it the saved image (full refresh).
bool displayFile(const char *path) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file || file.size() != g_geometry.imageBytes() || !beginPendingImage()) {
    if (file) file.close();
    return false;
  }
  bool ok = true;
  for (int16_t y = 0; y < g_geometry.height && ok; ++y) {
    ok = file.read(mainRow, g_geometry.rowBytes) == g_geometry.rowBytes &&
         pendingImage.write(mainRow, g_geometry.rowBytes) == g_geometry.rowBytes;
    if (ok) g_board->writeRow(mainRow, y);
  }
  file.close();
  if (!ok || !commitPendingImage()) {
    abortPendingImage();
    return false;
  }
  g_board->refresh();
  g_board->hibernate();
  g_panelHibernated = true;
  return true;
}

bool copyFile(const char *from, const char *to) {
  File in = SPIFFS.open(from, FILE_READ);
  File out = SPIFFS.open(to, FILE_WRITE);
  bool ok = in && out;
  while (ok && in.available()) {
    const size_t got = in.read(mainRow, g_geometry.rowBytes);
    ok = out.write(mainRow, got) == got;
  }
  if (in) in.close();
  if (out) out.close();
  return ok;
}

// Shows the home page (page 0) again if the panel shows another view and
// the last flip is at least `minAgeSeconds` old. Returns true when it drew.
bool returnHome(uint32_t minAgeSeconds) {
  if (g_pages.empty() || rtcShownPage == 0) return false;
  const uint32_t now = nowEpoch();
  if (minAgeSeconds && now && rtcPageShownAt && now - rtcPageShownAt < minAgeSeconds)
    return false;
  if (!displayFile(pagePath(g_pages[0].name).c_str())) return false;
  notePanelImage(true, g_pages[0].sha256, g_panel.issued);
  rtcShownPage = 0;
  Serial.println("Home view restored");
  return true;
}

// The `pages` command: fetch what changed, drop what is gone, show page 0
// when it is new or another view is up.
bool applyPages(JsonArray list, uint32_t issued, String &detail) {
  std::vector<Page> next;
  std::vector<std::pair<String, String>> urls;
  for (JsonObject entry : list) {
    if (next.size() >= kMaxPages) break;
    Page page{entry["name"] | "", entry["title"] | "", entry["sha256"] | ""};
    page.sha256.toLowerCase();
    if (!validPageName(page.name) || page.sha256.length() != 64) {
      detail = "bad page entry";
      return false;
    }
    next.push_back(page);
    urls.push_back({page.name, entry["url"] | ""});
  }
  if (next.empty()) {
    detail = "no pages";
    return false;
  }
  int fetched = 0;
  for (size_t i = 0; i < next.size(); ++i) {
    const Page &page = next[i];
    const auto known = std::find_if(g_pages.begin(), g_pages.end(),
                                    [&](const Page &p) { return p.name == page.name; });
    if (known != g_pages.end() && known->sha256 == page.sha256 &&
        SPIFFS.exists(pagePath(page.name)))
      continue;
    if (!fetchToFile(urls[i].second, page.sha256, pagePath(page.name))) {
      detail = "fetch failed: " + page.name;
      return false;
    }
    ++fetched;
  }
  for (const Page &old : g_pages) {
    if (std::none_of(next.begin(), next.end(),
                     [&](const Page &p) { return p.name == old.name; }))
      SPIFFS.remove(pagePath(old.name));
  }
  g_pages = next;
  savePages();
  bool shown = false;
  // Someone flipped to another view a moment ago: let them look; the home
  // view returns on a later timer wake (returnHome).
  const uint32_t now = nowEpoch();
  const bool browsing = rtcShownPage > 0 && now && rtcPageShownAt &&
                        now - rtcPageShownAt < kReturnHomeSeconds;
  if (!browsing &&
      (!g_panel.fromServer || g_panel.sha256 != g_pages[0].sha256 || rtcShownPage != 0)) {
    shown = displayFile(pagePath(g_pages[0].name).c_str());
    if (shown) {
      notePanelImage(true, g_pages[0].sha256, issued);
      rtcShownPage = 0;
    }
  }
  detail = "fetched " + String(fetched) + " of " + String(g_pages.size()) +
           (shown ? ", home shown" : "");
  return true;
}

// Status of the last successful check-in, for the board's status screen.
void persistCheckinStatus() {
  Preferences preferences;
  if (!preferences.begin("reterm-status", false)) return;
  preferences.putUInt("last", nowEpoch());
  preferences.putInt("rssi", WiFi.RSSI());
  preferences.putString("ssid", WiFi.SSID());
  preferences.putString("ip", WiFi.localIP().toString());
  preferences.end();
}

// Streams a firmware image into the inactive OTA slot. The sha256 is
// mandatory and checked before the slot becomes bootable. The bootloader
// boots the new image once in "pending verify"; confirmFirmware() keeps it
// only after its first successful check-in, so a firmware that crashes or
// cannot reach the broker falls back to the previous one on the next boot.
bool fetchAndInstallFirmware(const String &url, const String &sha256Hex,
                             String &detail) {
  if (sha256Hex.length() != 64) {
    detail = "sha256 required";
    return false;
  }
  HTTPClient http;
  if (!url.startsWith("http://") || !http.begin(url)) {
    detail = "only http:// urls";
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  const int length = http.getSize();
  if (code != HTTP_CODE_OK || length <= 0 || size_t(length) > kMaxFirmwareBytes) {
    detail = "fetch failed: http " + String(code) + ", bytes " + String(length);
    http.end();
    return false;
  }
  if (!Update.begin(size_t(length), U_FLASH)) {
    detail = String("update begin: ") + Update.errorString();
    http.end();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient &stream = http.getStream();
  stream.setTimeout(15000);
  static uint8_t chunk[4096];
  size_t remaining = size_t(length);
  bool ok = true;
  uint32_t lastService = millis();
  while (remaining && ok) {
    const size_t want = min(remaining, sizeof(chunk));
    const size_t got = stream.readBytes(chunk, want);
    if (got != want || Update.write(chunk, got) != got) {
      detail = got != want ? "short read" : String("write: ") + Update.errorString();
      ok = false;
      break;
    }
    mbedtls_sha256_update_ret(&sha, chunk, got);
    remaining -= got;
    if (millis() - lastService > 1000) {
      mqtt.loop();  // a multi-megabyte body outlasts the keepalive
      lastService = millis();
    }
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (ok) {
    char hex[65];
    static constexpr char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
      hex[i * 2] = alphabet[digest[i] >> 4];
      hex[i * 2 + 1] = alphabet[digest[i] & 0x0f];
    }
    hex[64] = 0;
    String expected = sha256Hex;
    expected.toLowerCase();
    if (expected != hex) {
      detail = "sha256 mismatch";
      ok = false;
    }
  }
  if (!ok) {
    Update.abort();
    return false;
  }
  if (!Update.end()) {
    detail = String("update end: ") + Update.errorString();
    return false;
  }
  detail = "installed " + String(length) + " bytes; rebooting";
  return true;
}

// Keeps a freshly installed firmware (see fetchAndInstallFirmware). Returns
// true when this boot was the one under verification.
bool confirmFirmware() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY)
    return false;
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.println("Firmware confirmed after a successful check-in");
  return true;
}

enum class CommandOutcome { None, Handled, SessionRequested };

CommandOutcome processCommand(const String &payload, bool allowSession) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("Ignoring malformed MQTT command");
    publishEvent("{\"event\":\"error\",\"detail\":\"malformed command\"}");
    clearRetained(mqttCmdTopic);
    return CommandOutcome::Handled;
  }
  const String action = doc["action"] | "";
  const String commandId = doc["id"] | "";
  if (commandId.length() && commandId == loadLastCommandId()) {
    // Retained duplicate of a command that already ran; drop it silently.
    clearRetained(mqttCmdTopic);
    return CommandOutcome::None;
  }

  if (action == "image") {
    const String url = doc["url"] | "";
    const String sha = doc["sha256"] | "";
    const bool ok =
        url.length() && fetchAndDisplayImage(url, sha, doc["issued"] | 0u);
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    publishEvent(String("{\"event\":\"image\",\"ok\":") +
                 (ok ? "true" : "false") + ",\"id\":\"" + commandId + "\"}");
    return CommandOutcome::Handled;
  }
  if (action == "wifi") {
    // Roams the device to another network remotely: writes the wificaptive
    // credentials the provisioning portal would have saved. Takes effect on
    // the next connection; if they are wrong, recovery is the physical
    // captive-portal path, so the ack (without the password) matters.
    const String ssid = doc["ssid"] | "";
    const String password = doc["password"] | "";
    bool ok = ssid.length() >= 1 && ssid.length() <= 32 &&
              password.length() <= 63;
    if (ok) {
      Preferences preferences;
      ok = preferences.begin("wificaptive", false);
      if (ok) {
        preferences.putInt("wifi_last_index", 0);
        preferences.putString("wifi_0_ssid", ssid);
        preferences.putString("wifi_0_pswd", password);
        preferences.end();
      }
    }
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    publishEvent(String("{\"event\":\"wifi\",\"ok\":") + (ok ? "true" : "false") +
                 ",\"ssid\":\"" + ssid + "\",\"id\":\"" + commandId + "\"}");
    Serial.println(ok ? "Wi-Fi credentials updated over MQTT; applied on next connection"
                      : "Wi-Fi command rejected: invalid credentials");
    return CommandOutcome::Handled;
  }
  if (action == "pages") {
    String detail;
    const bool ok = applyPages(doc["pages"].as<JsonArray>(), doc["issued"] | 0u, detail);
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    detail.replace("\"", "'");
    publishEvent(String("{\"event\":\"pages\",\"ok\":") + (ok ? "true" : "false") +
                 ",\"detail\":\"" + detail + "\",\"id\":\"" + commandId + "\"}");
    return CommandOutcome::Handled;
  }
  if (action == "ota") {
    // Record and clear first: a retained duplicate must never re-flash.
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    const String version = doc["version"] | "";
    String detail;
    bool ok;
    if (version.length() && version == RETERM_FW_VERSION) {
      ok = true;
      detail = "already running " + version;
    } else {
      ok = fetchAndInstallFirmware(doc["url"] | "", doc["sha256"] | "", detail);
    }
    detail.replace("\"", "'");
    publishEvent(String("{\"event\":\"ota\",\"ok\":") + (ok ? "true" : "false") +
                 ",\"detail\":\"" + detail + "\",\"id\":\"" + commandId + "\"}");
    Serial.print("OTA: ");
    Serial.println(detail);
    if (ok && !(version.length() && version == RETERM_FW_VERSION)) {
      const uint32_t drain = millis() + 500;
      while (int32_t(drain - millis()) > 0) {
        mqtt.loop();
        delay(10);
      }
      mqtt.disconnect();
      if (!g_panelHibernated) g_board->hibernate();
      Serial.flush();
      ESP.restart();
    }
    return CommandOutcome::Handled;
  }
  if (action == "session") {
    if (!allowSession) {
      // A button session is already running or just ended. Leave the retained
      // command untouched so the next timer wake starts the requested session.
      return CommandOutcome::None;
    }
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    // Publish the tokenized URL before the session begins: the QR screen and
    // upload loop monopolize the device once the session starts. Anyone who
    // can read this topic can upload for the next five minutes.
    uartSessionToken = makeToken();
    const String url = "http://" + WiFi.localIP().toString() +
                       "/?token=" + uartSessionToken +
                       "&model=" + g_board->model();
    publishEvent("{\"event\":\"session\",\"url\":\"" + url + "\"}");
    Serial.println("MQTT session command accepted");
    return CommandOutcome::SessionRequested;
  }

  // Give the board a chance before rejecting: board-specific actions (e.g.
  // the E1001 video player) live behind Board::handleCommand.
  String detail;
  if (g_board->handleCommand(action, payload, detail,
                             [] { mqtt.loop(); })) {
    if (commandId.length()) storeLastCommandId(commandId);
    clearRetained(mqttCmdTopic);
    detail.replace("\"", "'");
    detail.replace("\\", "/");
    publishEvent("{\"event\":\"" + action + "\",\"detail\":\"" + detail +
                 "\",\"id\":\"" + commandId + "\"}");
    return CommandOutcome::Handled;
  }

  Serial.print("Ignoring unsupported MQTT command action: ");
  Serial.println(action);
  if (commandId.length()) storeLastCommandId(commandId);
  clearRetained(mqttCmdTopic);
  publishEvent("{\"event\":\"error\",\"detail\":\"unsupported action\"}");
  return CommandOutcome::Handled;
}

// Returns true when a retained session command asks for a full upload
// session; the caller runs it after the MQTT connection is torn down.
bool runHaCheckin(bool allowSession) {
  g_checkinOk = false;
  if (!g_haConfig.enabled()) return false;
  if (WiFi.status() != WL_CONNECTED && !connectSavedWifi()) {
    Serial.println("Check-in skipped: Wi-Fi unavailable");
    return false;
  }

  const String id = haDeviceId();
  mqttStateTopic = "reterm/" + id + "/state";
  mqttCmdTopic = "reterm/" + id + "/cmd";
  mqttSetWakeTopic = "reterm/" + id + "/set/wake-interval";
  mqttEventTopic = "reterm/" + id + "/event";
  mqttScheduleTopic = "reterm/" + id + "/schedule";
  mqttSyncTopic = "reterm/" + id + "/sync";
  pendingCmdSeen = pendingWakeSeen = pendingScheduleSeen = pendingSyncSeen = false;
  syncClock();

  mqtt.setServer(g_haConfig.host.c_str(), g_haConfig.port);
  mqtt.setBufferSize(4096);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqttMessage);
  const bool connected =
      g_haConfig.user.length()
          ? mqtt.connect(id.c_str(), g_haConfig.user.c_str(),
                         g_haConfig.password.c_str())
          : mqtt.connect(id.c_str());
  if (!connected) {
    Serial.print("Check-in skipped: MQTT connect failed, state = ");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("MQTT connected; publishing state and reading commands");
  mark("mqtt");
  g_checkinOk = true;

  mqtt.subscribe(mqttCmdTopic.c_str());
  mqtt.subscribe(mqttSetWakeTopic.c_str());
  mqtt.subscribe(mqttScheduleTopic.c_str());
  mqtt.subscribe(mqttSyncTopic.c_str());
  // The broker sends each subscription's retained message right after its
  // SUBACK, in order, so a message published to our own sync topic after
  // the subscriptions arrives behind all of them: then everything is in.
  // The deadline covers a broker that drops it.
  mqtt.publish(mqttSyncTopic.c_str(), "1", false);
  const uint32_t retainedDeadline = millis() + 3000;
  while (!pendingSyncSeen && int32_t(retainedDeadline - millis()) > 0) {
    mqtt.loop();
    delay(5);
  }
  mark(pendingSyncSeen ? "retained" : "ret-tmout");
  persistCheckinStatus();

  // The schedule stays retained: it is the server's standing plan.
  if (pendingScheduleSeen && pendingSchedulePayload.length())
    storeSchedule(pendingSchedulePayload);

  // Apply a pending interval change first so the discovery expiry and the
  // state below already reflect it, then consume the retained value.
  if (pendingWakeSeen && pendingWakePayload.length()) {
    const long minutes = pendingWakePayload.toInt();
    const uint32_t wanted =
        uint32_t(constrain(minutes, 0L, long(kMaxWakeMinutes)));
    if (wanted != g_haConfig.wakeMinutes) {
      g_haConfig.wakeMinutes = wanted;
      saveHaConfig();
      Serial.print("Wake interval updated over MQTT, minutes = ");
      Serial.println(wanted);
    }
    clearRetained(mqttSetWakeTopic);
  }

  mqtt.publish(("homeassistant/device/" + id + "/config").c_str(),
               haDiscoveryPayload(id).c_str(), true);
  mqtt.publish(mqttStateTopic.c_str(), haStatePayload().c_str(), true);

  if (g_markCount) {
    // [name, ms since that boot, boot number]; each boot starts with a mark
    // named after its wake reason.
    String json = "{\"event\":\"timing\",\"marks\":[";
    for (size_t i = 0; i < min<size_t>(g_markCount, kMaxMarks); ++i) {
      if (i) json += ",";
      json += "[\"" + String(g_marks[i].name) + "\"," + String(g_marks[i].ms) +
              "," + String(g_marks[i].boot) + "]";
    }
    json += "]}";
    publishEvent(json);
    g_markCount = 0;
  }
  if (confirmFirmware())
    publishEvent("{\"event\":\"ota\",\"ok\":true,\"detail\":\"confirmed " RETERM_FW_VERSION "\"}");

  CommandOutcome outcome = CommandOutcome::None;
  if (pendingCmdSeen && pendingCmdPayload.length())
    outcome = processCommand(pendingCmdPayload, allowSession);
  if (updateBadge()) g_panelHibernated = true;
  g_badgeChecked = true;
  // Commands and the badge may have changed what the state reports; leave a
  // fresh retained state behind.
  mqtt.publish(mqttStateTopic.c_str(), haStatePayload().c_str(), true);

  const uint32_t drainDeadline = millis() + 200;
  while (int32_t(drainDeadline - millis()) > 0) {
    mqtt.loop();
    delay(10);
  }
  mqtt.disconnect();
  return outcome == CommandOutcome::SessionRequested;
}

}  // namespace

void mark(const char *name) {
  static bool started = false;  // the first mark of a boot opens it
  if (!started) {
    started = true;
    ++g_bootCount;
  }
  if (g_markCount >= kMaxMarks) return;
  Mark &entry = g_marks[g_markCount++];
  strlcpy(entry.name, name, sizeof(entry.name));
  entry.ms = millis();
  entry.boot = g_bootCount;
}

void notePanelShowsSavedImage(bool shows) {
  Preferences preferences;
  if (preferences.begin(kSchedPrefsNamespace, false)) {
    preferences.putBool("clean", shows);
    if (shows) preferences.putBool("badge", false);
    preferences.end();
  }
  g_panel.clean = shows;
  if (shows) g_panel.badge = false;
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

bool g_begun = false;

void begin(Board &board) {
  if (g_begun) return;
  g_begun = true;
  mark(wakeReason());
  g_board = &board;
  g_geometry = board.geometry();
  httpRow = new uint8_t[g_geometry.rowBytes];
  mainRow = new uint8_t[g_geometry.rowBytes];

  g_batteryMv = readBatteryMillivolts();
  Serial.print("Battery mV = ");
  Serial.println(g_batteryMv);
  loadHaConfig();
  loadPanelState();
  // Without a broker there is no check-in to prove a new firmware healthy.
  if (!g_haConfig.enabled()) confirmFirmware();

  imageStorageReady = SPIFFS.begin(true);
  Serial.print("Image storage ready = ");
  Serial.println(imageStorageReady ? "yes" : "no");
  loadPages();
  // RTC memory is lost on power-up; the panel shows page 0 when the saved
  // server image is it.
  if (rtcShownPage < 0 && !g_pages.empty() && g_panel.fromServer &&
      g_panel.sha256 == g_pages[0].sha256)
    rtcShownPage = 0;
}

size_t pageCount() {
  return g_pages.size() + (imageStorageReady && SPIFFS.exists(kPhotoPath) ? 1 : 0);
}

String pageTitle(size_t index) {
  if (index < g_pages.size())
    return g_pages[index].title.length() ? g_pages[index].title : g_pages[index].name;
  return index < pageCount() ? "Foto" : "";
}

bool readPage(size_t index, uint8_t *buffer) {
  if (index >= pageCount()) return false;
  const String path = index < g_pages.size() ? pagePath(g_pages[index].name) : String(kPhotoPath);
  File file = SPIFFS.open(path, FILE_READ);
  const bool ok = file && file.size() == g_geometry.imageBytes() &&
                  file.read(buffer, g_geometry.imageBytes()) == g_geometry.imageBytes();
  if (file) file.close();
  return ok;
}

int shownPage() { return rtcShownPage; }

void notePageShown(size_t index) {
  rtcShownPage = int8_t(index);
  rtcPageShownAt = nowEpoch();
  if (index == 0 && !g_pages.empty()) {
    // Home is the saved image again (a manual upload may have replaced it),
    // so restores and the staleness badge refer to it.
    if (copyFile(pagePath(g_pages[0].name).c_str(), kSavedImagePath))
      notePanelImage(true, g_pages[0].sha256, g_panel.issued);
  } else {
    notePanelShowsSavedImage(false);
  }
}

Status status() {
  Status out{};
  out.batteryMv = g_batteryMv;
  out.batteryPct = batteryPercent(g_batteryMv);
  Preferences preferences;
  if (preferences.begin("reterm-status", true)) {
    out.lastCheckin = preferences.getUInt("last", 0);
    out.rssi = preferences.getInt("rssi", 0);
    out.ssid = preferences.getString("ssid", "");
    out.ip = preferences.getString("ip", "");
    preferences.end();
  }
  plannedSleepSeconds(&out.nextWake);
  out.issued = g_panel.issued;
  out.clock = clockValid();
  out.stale = panelStale();
  out.serverImage = g_panel.fromServer;
  out.wakeMinutes = g_haConfig.wakeMinutes;
  out.brokerConfigured = g_haConfig.enabled();
  out.firmware = RETERM_FW_VERSION;
  return out;
}

bool checkInNow(bool &changed) {
  const String before = g_panel.sha256;
  const int shownBefore = rtcShownPage;
  runHaCheckin(false);
  changed = g_panel.sha256 != before || rtcShownPage != shownBefore;
  return g_checkinOk;
}

void run(Board &board) {
  begin(board);

  // A timer wake exists only for the Home Assistant check-in: publish stats,
  // apply whatever HA left on the retained command topics, sleep again. The
  // panel is untouched unless a command replaced the image.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Timer wake: Home Assistant check-in");
    const bool session = runHaCheckin(true);
    // Another view has been up for a while: back to the home page.
    if (returnHome(kReturnHomeSeconds)) g_badgeChecked = false;
    // Also when the check-in failed: a stale image is exactly that case.
    if (!g_badgeChecked && updateBadge()) g_panelHibernated = true;
    if (session) {
      runSessionAndMenu();
    } else if (!g_panelHibernated) {
      // setup() reset the panel controller; park it again like the
      // no-command path so it does not draw standby current through sleep.
      board.hibernate();
    }
    goToSleep();
  }

  const bool buttonWake = board.wokeByButton();
  if (buttonWake && board.wakeHoldRequestsWifiReset()) {
    Serial.println("Wake control held; forgetting saved Wi-Fi credentials");
    forgetWifiCredentials();
  }
  // A physical wake is unambiguous and should react immediately. Retain the
  // three-second UART recovery window only for reset/power-on host workflows.
  // The UART window serves the workbench tools after a reset or power-on;
  // any wake from deep sleep skips it.
  const bool sleepWake =
      esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
  const StartupCommand command = buttonWake || sleepWake
                                     ? StartupCommand::None
                                     : receiveStartupCommand();
  if (command == StartupCommand::Image) goToSleep();
  if (command == StartupCommand::Web || buttonWake) {
    runSessionAndMenu();
    // Wi-Fi is usually still associated here, so the extra state publish is
    // nearly free. Session commands are refused: one session per wake.
    runHaCheckin(false);
    goToSleep();
  }

  // Power-on or reset (including the reboot after an OTA install): check in
  // right away rather than waiting a full interval, which also confirms a
  // freshly installed firmware.
  Serial.println("No host command; preserving the current panel image");
  // Board-handled button wakes (page flips, menus) end here without Wi-Fi;
  // their marks wait in RTC memory for the next check-in.
  if (g_haConfig.enabled() && !sleepWake) {
    if (runHaCheckin(true)) {
      runSessionAndMenu();
      goToSleep();
    }
    if (!g_badgeChecked && updateBadge()) g_panelHibernated = true;
  }
  if (!g_panelHibernated) board.hibernate();
  goToSleep();
}

}  // namespace reterm

// Arduino marks every boot's firmware valid unless this hook says otherwise;
// the runtime confirms after a successful check-in instead (see
// confirmFirmware), so the rollback-enabled bootloader can undo a bad OTA.
extern "C" bool verifyRollbackLater() { return true; }
