# ESP32-S3 V5P Video Player (`esp32_v5p_player`)

High-performance, hardware-accelerated `.v5p` video player for the **Waveshare ESP32-S3-LCD-1.47** and **ESP32-S3-LCD-1.47B** boards (ST7789 1.47" IPS display, native 172×320 panel).

## Key Features

- **Dedicated V5P Decoding**: Real-time decompression and playback of `.v5p` video streams with support for:
  - `V5RU` / `V5RZ` (Raw RGB565 / LZ4 compressed)
  - `V5YU` / `V5YZ` (YUV420 Planar / LZ4 compressed with fast integer RGB conversion)
  - `V55U` / `V55Z` (Raw RGB555 / LZ4 compressed)
- **Hardware Display Rotation**: ST7789 hardware MADCTL configuration (`setRotation(1)`) renders natively in 320×172 landscape mode directly in silicon—completely eliminating software lookup tables (LUTs) and manual pixel re-mapping loops.
- **Asynchronous DMA Pipeline**: Double-buffered PSRAM framebuffers allow the CPU to decompress the next frame concurrently while LovyanGFX's SPI DMA controller pushes the previous frame to the display (push time dropped from 13.6 ms to 7.9 ms).
- **High-Speed SDMMC**: Reads video directly from FAT32 microSD cards over high-speed 4-bit SDMMC bus.
- **Universal Board Compatibility**: Simultaneously activates GPIO 48 and GPIO 46 backlights, ensuring out-of-the-box operation on both standard ESP32-S3-LCD-1.47 (GPIO 48) and ESP32-S3-LCD-1.47B (GPIO 46).

---

## Hardware Pinout (Waveshare ESP32-S3-LCD-1.47 / 1.47B)

| Function | Pin (GPIO) | Notes |
|---|---|---|
| **LCD SCLK** | GPIO 40 | SPI Clock @ 80 MHz |
| **LCD MOSI** | GPIO 45 | SPI Master Out |
| **LCD CS** | GPIO 42 | Chip Select |
| **LCD DC** | GPIO 41 | Data / Command |
| **LCD RST** | GPIO 39 | Reset |
| **LCD Backlight** | GPIO 48 & GPIO 46 | Driven HIGH (Pin 48 for base 1.47, Pin 46 for 1.47B) |
| **SDMMC CLK** | GPIO 14 | 4-bit SDMMC clock |
| **SDMMC CMD** | GPIO 15 | Command line (internal pullup) |
| **SDMMC D0** | GPIO 16 | Data line 0 (internal pullup) |
| **SDMMC D1** | GPIO 18 | Data line 1 (internal pullup) |
| **SDMMC D2** | GPIO 17 | Data line 2 (internal pullup) |
| **SDMMC D3** | GPIO 21 | Data line 3 (internal pullup) |
| **WS2812 RGB** | GPIO 38 | Status RGB LED |

---

## Project Structure

```
.
├── esp32_v5p_player/
│   ├── board.h               # Hardware pin configuration & LovyanGFX setup
│   ├── esp32_v5p_player.ino  # Main application entry point
│   ├── lz4.c / lz4.h         # Embedded LZ4 decompressor
│   └── v5p.hpp / v5p.cpp     # Dedicated V5P video decoding and DMA player engine
├── platformio.ini            # PlatformIO build configuration
└── README.md
```

---

## Build & Flash

### Option A: `arduino-cli` (Recommended)

1. Make sure `arduino-cli` is installed with `esp32:esp32` core and `LovyanGFX` library.
2. Compile:
   ```bash
   arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,FlashMode=qio" ./esp32_v5p_player
   ```
3. Upload to board:
   ```bash
   arduino-cli upload -p /dev/ttyACM0 --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,FlashMode=qio" ./esp32_v5p_player
   ```

### Option B: PlatformIO

```bash
pio run -t upload
```

---

## Usage

1. Format a microSD card as **FAT32**.
2. Copy your `.v5p` video file (e.g. `fine.v5p`) to the root of the microSD card.
3. Insert the microSD card into the slot on the board.
4. Power up or reset the ESP32-S3. The player will mount the card, initialize the display in hardware landscape, and continuously loop playback.
