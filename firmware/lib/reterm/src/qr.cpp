#include "reterm.h"

#include <Adafruit_GFX.h>

namespace reterm {

void drawQrModules(Adafruit_GFX &gfx, const QRCode &qr, int16_t left,
                   int16_t top, int16_t scale, int16_t pageTop,
                   int16_t pageBottom, uint16_t color) {
  for (uint8_t y = 0; y < qr.size; ++y) {
    const int16_t moduleTop = top + y * scale;
    if (moduleTop + scale <= pageTop || moduleTop >= pageBottom) continue;
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (qrcode_getModule(const_cast<QRCode *>(&qr), x, y)) {
        gfx.fillRect(left + x * scale, moduleTop, scale, scale, color);
      }
    }
  }
}

}  // namespace reterm
