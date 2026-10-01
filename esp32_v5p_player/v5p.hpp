#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "board.h"
#include "lz4.h"

enum class V5PColorFormat {
  RGB888 = 0,
  YUV420 = 1,
  RGB565 = 2
};

struct V5PStats {
  uint32_t fps;
  uint32_t decodeUs;
  uint32_t pushUs;
  uint32_t totalFrames;
};

class V5PPlayer {
public:
  explicit V5PPlayer(LGFX& display);
  ~V5PPlayer();

  bool begin();
  bool play(const char* filepath, bool loop = true);
  void stop();
  bool isPlaying() const { return _playing; }

  const V5PStats& getStats() const { return _stats; }

private:
  LGFX& _lcd;
  File  _file;
  bool  _playing = false;
  bool  _stopRequested = false;

  int            _width = 0;
  int            _height = 0;
  uint32_t       _fps = 30;
  uint32_t       _frameIntervalUs = 33333;
  bool           _compressed = false;
  V5PColorFormat _format = V5PColorFormat::YUV420;

  size_t _frameSize = 0;
  size_t _maxCompressedSize = 0;

  // Frame decompression buffers in PSRAM
  uint8_t* _compBuffer = nullptr;
  size_t   _compCapacity = 0;
  uint8_t* _rawBuffer = nullptr;
  size_t   _rawCapacity = 0;

  // Double-buffered DMA buffers in PSRAM (320x172x2 = 110,080 bytes each)
  lgfx::swap565_t* _dmaBuffers[2] = { nullptr, nullptr };
  int              _dmaIndex = 0;

  V5PStats _stats = { 0, 0, 0, 0 };

  int _dispWidth = 172;
  int _dispHeight = 320;

  // Precomputed coordinate scaling lookups (capacity 320 for either orientation)
  int _sx_lut[320];
  int _scx_lut[320];

  bool readHeader();
  bool readAndDecompressFrame();
  void convertFrameToDmaBuffer(lgfx::swap565_t* dst, int dstW, int dstH);
  void allocateBuffers();
  void freeBuffers();
};
