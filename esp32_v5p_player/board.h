#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>

// --- Pins (Waveshare ESP32-S3-LCD-1.47 & 1.47B) -----------------------------
#define PIN_BL_BASE 48  // LCD backlight for base ESP32-S3-LCD-1.47
#define PIN_BL_147B 46  // LCD backlight for ESP32-S3-LCD-1.47B
#define PIN_BTN     0   // BOOT button (active LOW)
#define PIN_RGB     38  // Onboard WS2812 RGB LED

// MicroSD SDMMC 4-bit bus
#define PIN_SD_CLK  14
#define PIN_SD_CMD  15
#define PIN_SD_D0   16
#define PIN_SD_D1   18
#define PIN_SD_D2   17
#define PIN_SD_D3   21

// Display dimensions in portrait (MADCTL hardware rotated 180 degrees)
static const int SCREEN_WIDTH  = 172;
static const int SCREEN_HEIGHT = 320;

// --- Verified ST7789 Panel Config ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI3_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 80000000; // 80 MHz SPI bus
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 40;
      cfg.pin_mosi    = 45;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 41;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 42;
      cfg.pin_rst          = 39;
      cfg.pin_busy         = -1;
      cfg.memory_width     = 240;
      cfg.memory_height    = 320;
      cfg.panel_width      = 172;
      cfg.panel_height     = 320;
      cfg.offset_x         = 34; // 172 panel centered in 240 memory
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

inline void initBoardHardware(LGFX& lcd, uint8_t rotation = 2) {
  lcd.init();
  lcd.setRotation(rotation); // Hardware MADCTL rotation: 2 = Portrait 180 degrees

  // Backlight: drive both GPIO 48 and 46 HIGH to support both board revisions
  pinMode(PIN_BL_BASE, OUTPUT);
  digitalWrite(PIN_BL_BASE, HIGH);
  pinMode(PIN_BL_147B, OUTPUT);
  digitalWrite(PIN_BL_147B, HIGH);

  // Enable internal pullups on SDMMC data and cmd lines
  pinMode(PIN_SD_CMD, INPUT_PULLUP);
  pinMode(PIN_SD_D0,  INPUT_PULLUP);
  pinMode(PIN_SD_D1,  INPUT_PULLUP);
  pinMode(PIN_SD_D2,  INPUT_PULLUP);
  pinMode(PIN_SD_D3,  INPUT_PULLUP);
}
