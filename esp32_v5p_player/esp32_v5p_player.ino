#include <Arduino.h>
#include <vector>
#include "board.h"
#include "v5p.hpp"

LGFX lcd;
V5PPlayer player(lcd);
std::vector<String> playlist;

void discoverVideos() {
  playlist.clear();

  // 1. Check known video file paths
  const char* knownFiles[] = {
    "/bad_apple.v5p",
    "/bad_apple_mmd.v5p",
    "/boneless_wing.v5p",
    "/butcher_vanity.v5p",
    "/fine.v5p",
    "/video.v5p"
  };

  for (const char* path : knownFiles) {
    if (SD_MMC.exists(path)) {
      playlist.push_back(String(path));
      Serial.printf("[esp32_v5p_player] found known video: %s\n", path);
    }
  }

  // 2. Also attempt root directory enumeration
  File root = SD_MMC.open("/");
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String name = file.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (name.endsWith(".v5p")) {
          bool alreadyAdded = false;
          for (const auto& item : playlist) {
            if (item.equalsIgnoreCase(name)) {
              alreadyAdded = true;
              break;
            }
          }
          if (!alreadyAdded) {
            playlist.push_back(name);
            Serial.printf("[esp32_v5p_player] discovered: %s\n", name.c_str());
          }
        }
      }
      file = root.openNextFile();
    }
    root.close();
  }

  Serial.printf("[esp32_v5p_player] total videos in playlist: %d\n", (int)playlist.size());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[esp32_v5p_player] booting...");

  // Initialize board hardware:
  // - ST7789 in portrait 180 degrees (172x320 via hardware MADCTL rotation 2)
  // - Backlight driven HIGH for both base 1.47 (GPIO 48) and 1.47B (GPIO 46)
  // - SDMMC 4-bit bus pullups enabled
  initBoardHardware(lcd, 2);

  // Initialize V5P video player and mount SD card
  if (!player.begin()) {
    Serial.println("[esp32_v5p_player] player / SD initialization failed!");
    return;
  }

  discoverVideos();
}

void loop() {
  if (playlist.empty()) {
    discoverVideos();
    if (playlist.empty()) {
      Serial.println("[esp32_v5p_player] no .v5p videos found on SD, checking again in 3s...");
      delay(3000);
      return;
    }
  }

  bool loopSingle = (playlist.size() == 1);

  for (const String& path : playlist) {
    Serial.printf("[esp32_v5p_player] starting playback: %s (loop=%s)\n",
                  path.c_str(), loopSingle ? "true" : "false");

    if (!player.play(path.c_str(), loopSingle)) {
      Serial.printf("[esp32_v5p_player] playback stopped for %s\n", path.c_str());
    }

    if (loopSingle) break;
    delay(200);
  }
}
