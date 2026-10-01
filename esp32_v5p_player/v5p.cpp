#include "v5p.hpp"

static inline uint8_t cl(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

V5PPlayer::V5PPlayer(LGFX& display) : _lcd(display) {}

V5PPlayer::~V5PPlayer() {
  stop();
  freeBuffers();
}

bool V5PPlayer::begin() {
  // Mount SD card in 4-bit bus mode
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
  bool mounted = SD_MMC.begin("/sdcard", false);
  if (!mounted) {
    Serial.println("[v5p] SD_MMC 4-bit highspeed failed, trying 20MHz...");
    mounted = SD_MMC.begin("/sdcard", false, false, 20000);
  }
  if (!mounted) {
    Serial.println("[v5p] SD_MMC 4-bit failed, falling back to 1-bit...");
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    mounted = SD_MMC.begin("/sdcard", true);
  }
  if (!mounted) {
    Serial.println("[v5p] SD_MMC mount failed completely");
    return false;
  }

  Serial.printf("[v5p] SD mounted: size=%lluMB, used=%lluMB\n",
                SD_MMC.cardSize() / (1024ULL * 1024ULL),
                SD_MMC.usedBytes() / (1024ULL * 1024ULL));

  // Allocate double-buffered DMA display buffers in PSRAM
  const size_t dmaBytes = (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(lgfx::swap565_t);
  for (int i = 0; i < 2; i++) {
    if (!_dmaBuffers[i]) {
      _dmaBuffers[i] = (lgfx::swap565_t*)ps_malloc(dmaBytes);
      if (!_dmaBuffers[i]) {
        _dmaBuffers[i] = (lgfx::swap565_t*)malloc(dmaBytes);
      }
    }
    if (!_dmaBuffers[i]) {
      Serial.printf("[v5p] failed to allocate DMA buffer %d\n", i);
      return false;
    }
    memset(_dmaBuffers[i], 0, dmaBytes);
  }

  return true;
}

void V5PPlayer::allocateBuffers() {
  if (_rawCapacity < _frameSize) {
    if (_rawBuffer) free(_rawBuffer);
    _rawBuffer = (uint8_t*)ps_malloc(_frameSize);
    if (!_rawBuffer) _rawBuffer = (uint8_t*)malloc(_frameSize);
    _rawCapacity = _rawBuffer ? _frameSize : 0;
  }

  if (_compressed && _compCapacity < _maxCompressedSize) {
    if (_compBuffer) free(_compBuffer);
    _compBuffer = (uint8_t*)ps_malloc(_maxCompressedSize);
    if (!_compBuffer) _compBuffer = (uint8_t*)malloc(_maxCompressedSize);
    _compCapacity = _compBuffer ? _maxCompressedSize : 0;
  }
}

void V5PPlayer::freeBuffers() {
  if (_rawBuffer)  { free(_rawBuffer);  _rawBuffer = nullptr;  _rawCapacity = 0; }
  if (_compBuffer) { free(_compBuffer); _compBuffer = nullptr; _compCapacity = 0; }
  for (int i = 0; i < 2; i++) {
    if (_dmaBuffers[i]) {
      free(_dmaBuffers[i]);
      _dmaBuffers[i] = nullptr;
    }
  }
}

bool V5PPlayer::readHeader() {
  if (!_file) return false;

  uint8_t H[16];
  if (_file.read(H, 16) != 16) {
    Serial.println("[v5p] header read failed: file too short");
    return false;
  }

  // Parse Magic
  if (!memcmp(H, "V5RU", 4)) {
    _compressed = false; _format = V5PColorFormat::RGB888;
  } else if (!memcmp(H, "V5RZ", 4)) {
    _compressed = true;  _format = V5PColorFormat::RGB888;
  } else if (!memcmp(H, "V5YU", 4)) {
    _compressed = false; _format = V5PColorFormat::YUV420;
  } else if (!memcmp(H, "V5YZ", 4)) {
    _compressed = true;  _format = V5PColorFormat::YUV420;
  } else if (!memcmp(H, "V55U", 4)) {
    _compressed = false; _format = V5PColorFormat::RGB565;
  } else if (!memcmp(H, "V55Z", 4)) {
    _compressed = true;  _format = V5PColorFormat::RGB565;
  } else {
    Serial.printf("[v5p] unknown magic: %c%c%c%c\n", H[0], H[1], H[2], H[3]);
    return false;
  }

  uint16_t w = 0, h = 0, fr = 0;
  memcpy(&w, H + 4, 2);
  memcpy(&h, H + 6, 2);
  memcpy(&fr, H + 8, 2);

  _width  = w;
  _height = h;
  _fps    = fr ? fr : 30u;
  _frameIntervalUs = 1000000u / _fps;

  if (_width <= 0 || _height <= 0 || _width > 1920 || _height > 1080) {
    Serial.printf("[v5p] invalid video dimensions: %dx%d\n", _width, _height);
    return false;
  }

  // Calculate uncompressed frame size
  const size_t pc = (size_t)_width * _height;
  if (_format == V5PColorFormat::RGB888) {
    _frameSize = pc * 3;
  } else if (_format == V5PColorFormat::YUV420) {
    const size_t cw = (size_t)_width / 2;
    const size_t ch = (size_t)_height / 2;
    _frameSize = pc + cw * ch * 2;
  } else { // RGB565
    _frameSize = pc * 2;
  }

  _maxCompressedSize = (size_t)LZ4_compressBound((int)_frameSize);

  // Precompute horizontal scaling lookups
  for (int x = 0; x < SCREEN_WIDTH; x++) {
    _sx_lut[x]  = (x * _width) / SCREEN_WIDTH;
    _scx_lut[x] = _sx_lut[x] / 2;
  }

  allocateBuffers();
  if (!_rawBuffer || (_compressed && !_compBuffer)) {
    Serial.println("[v5p] buffer allocation failed");
    return false;
  }

  const char* fmtStr = (_format == V5PColorFormat::RGB888) ? "RGB888" :
                       (_format == V5PColorFormat::YUV420) ? "YUV420" : "RGB565";
  Serial.printf("[v5p] playing: %dx%d @ %ufps, format=%s, lz4=%s, frameSize=%u\n",
                _width, _height, (unsigned)_fps, fmtStr,
                _compressed ? "yes" : "no", (unsigned)_frameSize);

  return true;
}

bool V5PPlayer::readAndDecompressFrame() {
  if (!_file) return false;

  if (_compressed) {
    uint32_t compSize = 0;
    if (_file.read((uint8_t*)&compSize, 4) != 4) {
      return false; // EOF
    }
    if (compSize == 0 || compSize > _maxCompressedSize) {
      Serial.printf("[v5p] invalid compressed frame size: %u\n", (unsigned)compSize);
      return false;
    }
    if (_file.read(_compBuffer, compSize) != (int)compSize) {
      Serial.println("[v5p] truncated frame read");
      return false;
    }
    int dc = LZ4_decompress_safe((const char*)_compBuffer, (char*)_rawBuffer,
                                 (int)compSize, (int)_frameSize);
    if (dc < 0 || (size_t)dc != _frameSize) {
      Serial.printf("[v5p] LZ4 decompression failed (%d)\n", dc);
      return false;
    }
  } else {
    if (_file.read(_rawBuffer, _frameSize) != (int)_frameSize) {
      return false; // EOF
    }
  }

  return true;
}

void V5PPlayer::convertFrameToDmaBuffer(lgfx::swap565_t* dst, int dstW, int dstH) {
  if (!_rawBuffer || !dst) return;

  if (_format == V5PColorFormat::YUV420) {
    const uint8_t* yp = _rawBuffer;
    const size_t pc   = (size_t)_width * _height;
    const size_t cw   = (size_t)_width / 2;
    const uint8_t* up = yp + pc;
    const uint8_t* vp = up + (cw * ((size_t)_height / 2));

    for (int y = 0; y < dstH; y++) {
      const int sy  = (y * _height) / dstH;
      const int scy = sy / 2;

      const uint8_t* y_row = yp + (size_t)sy * _width;
      const uint8_t* u_row = up + (size_t)scy * cw;
      const uint8_t* v_row = vp + (size_t)scy * cw;
      lgfx::swap565_t* dst_row = dst + (size_t)y * dstW;

      for (int x = 0; x < dstW; x++) {
        const int sx  = _sx_lut[x];
        const int scx = _scx_lut[x];

        const int C = (int)y_row[sx] - 16;
        const int D = (int)u_row[scx] - 128;
        const int E = (int)v_row[scx] - 128;

        const uint8_t r = cl((298 * C + 459 * E + 128) >> 8);
        const uint8_t g = cl((298 * C - 55 * D - 136 * E + 128) >> 8);
        const uint8_t b = cl((298 * C + 541 * D + 128) >> 8);

        const uint16_t c565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        dst_row[x].raw = (uint16_t)((c565 >> 8) | (c565 << 8));
      }
    }
  } else if (_format == V5PColorFormat::RGB888) {
    for (int y = 0; y < dstH; y++) {
      const int sy = (y * _height) / dstH;
      const uint8_t* src_row = _rawBuffer + (size_t)sy * _width * 3;
      lgfx::swap565_t* dst_row = dst + (size_t)y * dstW;

      for (int x = 0; x < dstW; x++) {
        const int sx = _sx_lut[x];
        const uint8_t* p = src_row + sx * 3;
        const uint16_t c565 = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
        dst_row[x].raw = (uint16_t)((c565 >> 8) | (c565 << 8));
      }
    }
  } else { // RGB565
    if (_width == dstW && _height == dstH) {
      // Direct raw blit
      const uint16_t* src = (const uint16_t*)_rawBuffer;
      for (int i = 0; i < dstW * dstH; i++) {
        uint16_t c = src[i];
        dst[i].raw = (uint16_t)((c >> 8) | (c << 8));
      }
    } else {
      for (int y = 0; y < dstH; y++) {
        const int sy = (y * _height) / dstH;
        const uint16_t* src_row = (const uint16_t*)_rawBuffer + (size_t)sy * _width;
        lgfx::swap565_t* dst_row = dst + (size_t)y * dstW;
        for (int x = 0; x < dstW; x++) {
          const int sx = _sx_lut[x];
          const uint16_t c = src_row[sx];
          dst_row[x].raw = (uint16_t)((c >> 8) | (c << 8));
        }
      }
    }
  }
}

bool V5PPlayer::play(const char* filepath, bool loop) {
  _file = SD_MMC.open(filepath, FILE_READ);
  if (!_file) {
    Serial.printf("[v5p] failed to open file: %s\n", filepath);
    return false;
  }

  if (!readHeader()) {
    _file.close();
    return false;
  }

  _playing = true;
  _stopRequested = false;
  _dmaIndex = 0;

  uint32_t lastFrameUs = 0;
  uint32_t tLog = millis();
  uint32_t frameCount = 0;
  uint32_t totalDecodeUs = 0;
  uint32_t totalPushUs = 0;

  _lcd.startWrite();

  while (_playing && !_stopRequested) {
    // 1. Read and decompress frame into _rawBuffer
    uint32_t t0 = micros();
    if (!readAndDecompressFrame()) {
      if (loop) {
        _file.seek(16); // Seek past header back to first frame
        if (!readAndDecompressFrame()) {
          Serial.println("[v5p] loop seek failed");
          break;
        }
      } else {
        break; // Playback finished
      }
    }

    // 2. Convert directly to double-buffered DMA memory
    convertFrameToDmaBuffer(_dmaBuffers[_dmaIndex], SCREEN_WIDTH, SCREEN_HEIGHT);
    uint32_t decodeUs = micros() - t0;
    totalDecodeUs += decodeUs;

    // 3. Wait for the prior DMA push to finish before launching the new frame
    uint32_t tPush0 = micros();
    _lcd.waitDMA();

    // 4. Launch concurrent DMA transfer of the new frame
    _lcd.pushImageDMA(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, _dmaBuffers[_dmaIndex]);
    uint32_t pushUs = micros() - tPush0;
    totalPushUs += pushUs;

    // 5. Swap DMA buffer for the next frame
    _dmaIndex = 1 - _dmaIndex;
    frameCount++;
    _stats.totalFrames++;

    // 6. Frame pacing
    uint32_t now = micros();
    if (lastFrameUs > 0) {
      int32_t elapsed = (int32_t)(now - lastFrameUs);
      int32_t waitTime = (int32_t)_frameIntervalUs - elapsed;
      if (waitTime > 1500) {
        delayMicroseconds(waitTime);
      }
    }
    lastFrameUs = micros();

    // 7. FPS Logging once per second
    if (millis() - tLog >= 1000) {
      uint32_t elapsedMs = millis() - tLog;
      _stats.fps = (frameCount * 1000) / elapsedMs;
      _stats.decodeUs = totalDecodeUs / (frameCount ? frameCount : 1);
      _stats.pushUs = totalPushUs / (frameCount ? frameCount : 1);

      Serial.printf("[v5p] fps=%lu  decode=%luus  push=%luus  frames=%lu\n",
                    (unsigned long)_stats.fps,
                    (unsigned long)_stats.decodeUs,
                    (unsigned long)_stats.pushUs,
                    (unsigned long)_stats.totalFrames);

      frameCount = 0;
      totalDecodeUs = 0;
      totalPushUs = 0;
      tLog = millis();
    }
  }

  _lcd.waitDMA();
  _lcd.endWrite();

  _file.close();
  _playing = false;
  return true;
}

void V5PPlayer::stop() {
  _stopRequested = true;
}
