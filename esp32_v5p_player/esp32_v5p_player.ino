#include <Arduino.h>
#include "board.h"
#include "v5p.hpp"

LGFX lcd;
V5PPlayer player(lcd);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[esp32_v5p_player] booting...");

  // Initialize board hardware:
  // - ST7789 in native silicon landscape (320x172 via hardware MADCTL rotation 1)
  // - Backlight driven HIGH for both base 1.47 (GPIO 48) and 1.47B (GPIO 46)
  // - SDMMC 4-bit bus pullups enabled
  initBoardHardware(lcd, 1);

  // Initialize V5P video player and mount SD card
  if (!player.begin()) {
    Serial.println("[esp32_v5p_player] player / SD initialization failed!");
    return;
  }

  Serial.println("[esp32_v5p_player] launching video: /fine.v5p");
}

void loop() {
  // Play /fine.v5p in a continuous loop
  if (!player.play("/fine.v5p", true)) {
    Serial.println("[esp32_v5p_player] playback halted or file missing, retrying in 2s...");
    delay(2000);
  }
}
