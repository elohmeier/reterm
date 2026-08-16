// Shared runtime for the reTerminal E-series custom firmware: the UART
// startup protocol, the QR-driven Wi-Fi upload session with its HTTP API and
// token security model, credential provisioning, framebuffer persistence, and
// deep-sleep orchestration. Everything panel- or board-specific stays behind
// the Board interface implemented by each device's main.cpp.
#pragma once

#include <Arduino.h>
#include <qrcode.h>

class Adafruit_GFX;

// Reported over /api/status and MQTT. CI builds override this with the deploy
// label; local builds report "dev".
#ifndef RETERM_FW_VERSION
#define RETERM_FW_VERSION "dev"
#endif

namespace reterm {

// Both boards stream full-screen images over their CH341 UART at this rate.
constexpr uint32_t kImageBaud = 921600;

struct Geometry {
  int16_t width;
  int16_t height;
  size_t rowBytes;  // packed native bytes per row (4bpp: width/2, 1bpp: width/8)
  size_t imageBytes() const { return rowBytes * size_t(height); }
};

// What the board's menu wants after it closes.
enum class MenuAction { Sleep, UploadSession };

class Board {
 public:
  virtual ~Board() = default;
  virtual const char *model() const = 0;     // e.g. "reterminal-e1004"
  virtual const char *hostname() const = 0;  // Wi-Fi hostname and AP name prefix
  virtual const char *uartName() const = 0;  // READY line name, e.g. "E1004"
  virtual Geometry geometry() const = 0;
  virtual const char *statusJson() const = 0;  // full /api/status body

  // Stream one packed native row into controller memory without refreshing.
  virtual void writeRow(const uint8_t *row, int16_t y) = 0;
  virtual void refresh() = 0;
  virtual void hibernate() = 0;

  virtual void drawProvisionScreen(const String &ssid, const String &password) = 0;
  virtual void drawUploadScreen(const String &url) = 0;

  // Board-specific MQTT command extension: called for retained command
  // actions the shared runtime does not know. Return true when handled; the
  // runtime then acks, dedupes, and clears the retained command as usual and
  // publishes `detail` (plain text, no quotes) in the event. Long-running
  // handlers must call serviceNetwork() at least every ~30 s to keep the
  // MQTT connection alive.
  virtual bool handleCommand(const String &action, const String &payload,
                             String &detail, void (*serviceNetwork)()) {
    (void)action; (void)payload; (void)detail; (void)serviceNetwork;
    return false;
  }

  // Polled from the runtime's awake wait loops (upload session, provisioning
  // portal). Return true to close the current mode cleanly and open the
  // board's menu; the implementation must be non-blocking and cheap. Boards
  // without an on-device menu keep the defaults.
  virtual bool menuRequested() { return false; }
  // Runs the board's menu after a mode closed for it. Returning
  // UploadSession starts a fresh upload session; the menu may be reopened
  // from that session again any number of times.
  virtual MenuAction onMenu() { return MenuAction::Sleep; }

  // True when this boot was caused by the board's wake control.
  virtual bool wokeByButton() const = 0;
  // Called once right after a button wake. Return true when the user keeps
  // holding the wake control long enough to request forgetting the saved
  // Wi-Fi credentials. Boards without hold detection keep the default.
  virtual bool wakeHoldRequestsWifiReset() { return false; }
  // Configure wake sources; called immediately before deep sleep.
  virtual void prepareSleep() = 0;
};

// Runs the full boot flow: UART command window, upload session on button
// wake, and deep sleep. Serial and the panel must already be initialized.
// Never returns.
void run(Board &board);

// 32 hex characters from the hardware RNG.
String makeToken();

// Draws the QR modules that intersect [pageTop, pageBottom) for use inside a
// board's paged-drawing loop.
void drawQrModules(Adafruit_GFX &gfx, const QRCode &qr, int16_t left,
                   int16_t top, int16_t scale, int16_t pageTop,
                   int16_t pageBottom, uint16_t color);

}  // namespace reterm
