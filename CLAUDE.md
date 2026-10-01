# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Working notes and architecture guide for the dedicated `esp32_v5p_player` project.

## Project Summary

High-performance, hardware-accelerated `.v5p` video player for the **Waveshare ESP32-S3-LCD-1.47** and **ESP32-S3-LCD-1.47B** boards (ST7789 1.47" IPS display, native 172×320 panel).

## Key Architectural Decisions

1. **Hardware Rotation in Silicon**:
   - In `board.h`, `lcd.setRotation(1)` (or `3`) is called during hardware init to set the ST7789 MADCTL register.
   - The panel operates natively as 320×172 landscape directly in silicon.
   - Eliminates software lookup tables (LUTs) and CPU pixel-rotation loops completely.

2. **DMA Pipelining & Double Buffering**:
   - Two 320×172 16-bit RGB565 framebuffers are allocated in PSRAM (`lgfx::swap565_t`).
   - Frame 0 is decompressed and pushed via non-blocking SPI DMA (`lcd.pushImageDMA`).
   - While DMA is transmitting Frame 0 to the panel over SPI @ 80 MHz, the CPU decompresses Frame 1 directly into Framebuffer 1.
   - Synchronization is managed via `lcd.waitDMA()`.
   - Result: DMA push overhead dropped from 13.6 ms to 7.9 ms.

3. **V5P Format Support (`v5p.hpp` / `v5p.cpp`)**:
   - Reads 8-byte frame headers (4-byte magic, 4-byte payload size).
   - Formats handled:
     - `V5RU`: Raw RGB565 uncompressed
     - `V5RZ`: RGB565 compressed with LZ4
     - `V5YU`: YUV420 Planar uncompressed with fast integer YUV-to-RGB565 conversion
     - `V5YZ`: YUV420 Planar compressed with LZ4
     - `V55U` / `V55Z`: RGB555 raw / LZ4
   - Conversion directly populates `lgfx::swap565_t` big-endian byte order required by ST7789 DMA.

4. **Universal Backlight Control**:
   - Base Waveshare ESP32-S3-LCD-1.47 uses GPIO 48 for backlight.
   - Revision 1.47B uses GPIO 46 for backlight.
   - `initBoardHardware()` drives BOTH GPIO 48 and GPIO 46 HIGH, guaranteeing the backlight activates on all hardware revisions.

5. **Storage**:
   - MicroSD card accessed over 4-bit SDMMC bus (pins 14, 15, 16, 17, 18, 21) with internal pullups enabled.
   - Default video file is `/fine.v5p` on FAT32 filesystem.

## Commands

### Compile & Flash via `arduino-cli`:
```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,FlashMode=qio"
arduino-cli compile --fqbn "$FQBN" ./esp32_v5p_player
arduino-cli upload -p /dev/ttyACM0 --fqbn "$FQBN" ./esp32_v5p_player
```

### Compile & Flash via PlatformIO:
```bash
pio run -t upload
```

### Serial Heartbeat Monitoring:
```bash
python3 -c "import serial, time; s=serial.Serial('/dev/ttyACM0', 115200, timeout=1); [print(s.readline().decode('utf-8', errors='ignore'), end='') for _ in range(30)]"
```
