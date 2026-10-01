// video.cpp — microSD MJPEG/AVI and LZ4-compressed V5P player.
//
// Supports:
// 1. V5P video format (from v5_video_player: V5RU/V5RZ/V5YU/V5YZ) with LZ4 decompression
// 2. AVI Motion-JPEG clips
#include "video.h"
#include "board.h"
#include "SD_MMC.h"
#include "FS.h"
#include "lz4.h"
#include <JPEGDEC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define MAX_CLIPS 64
#define MAX_JPEG  (48 * 1024)        // per-frame JPEG ceiling for AVI

static char  s_paths[MAX_CLIPS][80];
static char  s_names[MAX_CLIPS][40];
static int   s_count = 0;

static File     s_file;
static int      s_curClip = -1;
static uint32_t s_frameUs   = 33333;     // per-frame interval; default 30fps
static uint32_t s_lastUs    = 0;
static uint32_t s_decodeUs  = 0;

// --- AVI / JPEGDEC state ---
static JPEGDEC  jpeg;
static uint32_t s_moviStart = 0, s_moviEnd = 0, s_moviPos = 0;
static uint8_t* s_jpeg      = nullptr;
static uint16_t* s_tgt = nullptr;
static int       s_tgtW = 0, s_tgtH = 0;

// --- V5P state ---
static int      s_v5p_w = 0, s_v5p_h = 0;
static uint32_t s_v5p_fr = 30;
static bool     s_v5p_Z = false;
static int      s_v5p_CF = 0; // 0 = RGB, 1 = YUV420
static size_t   s_v5p_pc = 0, s_v5p_cw = 0, s_v5p_ch = 0, s_v5p_fs = 0, s_v5p_mc = 0;
static uint8_t* s_v5p_raw = nullptr;
static size_t   s_v5p_raw_cap = 0;
static uint8_t* s_v5p_cz = nullptr;
static size_t   s_v5p_cz_cap = 0;

// --- Helper functions ---
static inline uint8_t cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

static inline uint16_t rgbTo565be(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (uint16_t)((c >> 8) | (c << 8)); // byte-swapped for LovyanGFX DMA
}

static bool endsWith(const char* n, const char* ext) {
  int L = strlen(n); int el = strlen(ext);
  if (L < el) return false;
  return strcasecmp(n + L - el, ext) == 0;
}

// --- AVI Helpers ---
static uint32_t rdU32() { uint8_t b[4]; if (s_file.read(b, 4) != 4) return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static bool rdTag(char t[5]) { t[4] = 0; return s_file.read((uint8_t*)t, 4) == 4; }
static bool tagEq(const char* a, const char* b) { return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]; }

static int jpegDraw(JPEGDRAW* p) {
  for (int r = 0; r < p->iHeight; r++) {
    int y = p->y + r;
    if (y < 0 || y >= s_tgtH) continue;
    int w = p->iWidth; int x = p->x;
    if (x + w > s_tgtW) w = s_tgtW - x;
    if (w > 0) memcpy(s_tgt + y * s_tgtW + x, p->pPixels + r * p->iWidth, w * 2);
  }
  return 1;
}

static bool parseAvi() {
  s_file.seek(0);
  char t[5];
  if (!rdTag(t) || !tagEq(t, "RIFF")) return false;
  rdU32();
  if (!rdTag(t) || !tagEq(t, "AVI ")) return false;

  s_moviStart = 0; s_frameUs = 50000;
  uint32_t sz = s_file.size();
  while (s_file.position() + 8 <= sz) {
    uint32_t pos = s_file.position();
    if (!rdTag(t)) break;
    uint32_t csz = rdU32();
    if (tagEq(t, "LIST")) {
      char lt[5]; if (!rdTag(lt)) break;
      if (tagEq(lt, "movi")) { s_moviStart = pos + 12; s_moviEnd = pos + 8 + csz; break; }
      if (tagEq(lt, "hdrl")) continue;
      s_file.seek(pos + 8 + csz + (csz & 1));
    } else if (tagEq(t, "avih")) {
      uint32_t usec = rdU32();
      if (usec) s_frameUs = usec;
      s_file.seek(pos + 8 + csz + (csz & 1));
    } else {
      s_file.seek(pos + 8 + csz + (csz & 1));
    }
  }
  if (!s_moviStart) return false;
  if (s_frameUs < 16000)  s_frameUs = 16000;
  if (s_frameUs > 500000) s_frameUs = 500000;
  s_moviPos = s_moviStart;
  return true;
}

static uint32_t nextFrame() {
  bool looped = false;
  char t[5];
  for (;;) {
    if (s_moviPos + 8 > s_moviEnd) {
      if (looped) return 0;
      looped = true; s_moviPos = s_moviStart; continue;
    }
    s_file.seek(s_moviPos);
    if (!rdTag(t)) return 0;
    uint32_t csz = rdU32();
    uint32_t adv = 8 + csz + (csz & 1);
    if (tagEq(t, "LIST")) { s_moviPos += 12; continue; }
    bool isVideo = (t[2] == 'd' && (t[3] == 'c' || t[3] == 'b'));
    if (isVideo && csz > 0 && csz <= MAX_JPEG && s_jpeg) {
      if (s_file.read(s_jpeg, csz) != (int)csz) { s_moviPos += adv; continue; }
      s_moviPos += adv;
      return csz;
    }
    s_moviPos += adv;
  }
}

static bool openClip(int clip) {
  if (s_file) s_file.close();
  s_curClip = -1;
  s_file = SD_MMC.open(s_paths[clip], FILE_READ);
  if (!s_file) return false;
  if (!parseAvi()) { s_file.close(); return false; }
  s_curClip = clip; s_lastUs = 0;
  return true;
}

// --- V5P Implementation ---
static bool openV5PClip(int clip) {
  if (s_file) s_file.close();
  s_curClip = -1;
  s_file = SD_MMC.open(s_paths[clip], FILE_READ);
  if (!s_file) {
    Serial.printf("[v5p] failed to open '%s'\n", s_paths[clip]);
    return false;
  }
  uint8_t H[16];
  if (s_file.read(H, 16) != 16) {
    Serial.printf("[v5p] file too short: '%s'\n", s_paths[clip]);
    s_file.close();
    return false;
  }
  bool Z = false;
  int CF = 0;
  if (!memcmp(H, "V5RU", 4))      { Z = false; CF = 0; }
  else if (!memcmp(H, "V5RZ", 4)) { Z = true;  CF = 0; }
  else if (!memcmp(H, "V5YU", 4)) { Z = false; CF = 1; }
  else if (!memcmp(H, "V5YZ", 4)) { Z = true;  CF = 1; }
  else {
    Serial.printf("[v5p] bad magic in '%s'\n", s_paths[clip]);
    s_file.close();
    return false;
  }

  uint16_t a = 0, b = 0, c = 0;
  memcpy(&a, H + 4, 2);
  memcpy(&b, H + 6, 2);
  memcpy(&c, H + 8, 2);
  s_v5p_w = a;
  s_v5p_h = b;
  s_v5p_fr = c ? c : 30u;
  s_v5p_Z = Z;
  s_v5p_CF = CF;
  s_v5p_pc = (size_t)s_v5p_w * s_v5p_h;
  s_v5p_cw = (size_t)s_v5p_w / 2;
  s_v5p_ch = (size_t)s_v5p_h / 2;
  s_v5p_fs = CF == 0 ? s_v5p_pc * 3 : s_v5p_pc + s_v5p_cw * s_v5p_ch * 2;
  s_v5p_mc = (size_t)LZ4_compressBound((int)s_v5p_fs);
  s_frameUs = 1000000u / (s_v5p_fr ? s_v5p_fr : 30u);
  s_lastUs = 0;

  Serial.printf("[v5p] %s: %dx%d @ %ufps, %s, %s (raw %u B, max comp %u B)\n",
                s_paths[clip], s_v5p_w, s_v5p_h, (unsigned)s_v5p_fr,
                CF == 0 ? "RGB" : "YUV420", Z ? "LZ4" : "RAW",
                (unsigned)s_v5p_fs, (unsigned)s_v5p_mc);

  if (s_v5p_raw_cap < s_v5p_fs) {
    if (s_v5p_raw) free(s_v5p_raw);
    s_v5p_raw = (uint8_t*)ps_malloc(s_v5p_fs);
    if (!s_v5p_raw) s_v5p_raw = (uint8_t*)malloc(s_v5p_fs);
    s_v5p_raw_cap = s_v5p_raw ? s_v5p_fs : 0;
  }
  if (Z && s_v5p_cz_cap < s_v5p_mc) {
    if (s_v5p_cz) free(s_v5p_cz);
    s_v5p_cz = (uint8_t*)ps_malloc(s_v5p_mc);
    if (!s_v5p_cz) s_v5p_cz = (uint8_t*)malloc(s_v5p_mc);
    s_v5p_cz_cap = s_v5p_cz ? s_v5p_mc : 0;
  }
  if (!s_v5p_raw || (Z && !s_v5p_cz)) {
    Serial.println("[v5p] buffer alloc failed");
    s_file.close();
    return false;
  }
  s_curClip = clip;
  return true;
}

static bool nextV5PFrame() {
  if (!s_file) return false;
  if (s_v5p_Z) {
    uint32_t ps = 0;
    if (s_file.read((uint8_t*)&ps, 4) != 4) {
      s_file.seek(16);
      if (s_file.read((uint8_t*)&ps, 4) != 4) return false;
    }
    if (!ps || ps > s_v5p_mc) {
      Serial.printf("[v5p] bad compressed size %u\n", (unsigned)ps);
      s_file.seek(16);
      return false;
    }
    if (s_file.read(s_v5p_cz, ps) != (int)ps) {
      Serial.println("[v5p] truncated frame read");
      s_file.seek(16);
      return false;
    }
    int dc = LZ4_decompress_safe((const char*)s_v5p_cz, (char*)s_v5p_raw, (int)ps, (int)s_v5p_fs);
    if (dc < 0 || (size_t)dc != s_v5p_fs) {
      Serial.printf("[v5p] LZ4 fail: %d\n", dc);
      return false;
    }
  } else {
    if (s_file.read(s_v5p_raw, s_v5p_fs) != (int)s_v5p_fs) {
      s_file.seek(16);
      if (s_file.read(s_v5p_raw, s_v5p_fs) != (int)s_v5p_fs) return false;
    }
  }
  return true;
}

static int s_v5p_rot = 1; // 1 = 90 deg Clockwise by default

void videoSetRotation(int rot) {
  s_v5p_rot = rot % 4;
}

static void renderV5PFrameToBuffer(uint16_t* buf, int targetW, int targetH) {
  if (!s_v5p_raw) return;

  if (s_v5p_rot == 1) {
    // 90 degrees Clockwise rotation:
    // Video width maps to screen height, video height maps to screen width
    if (s_v5p_CF == 0) { // RGB
      for (int j = 0; j < targetH; j++) {
        int sx = (j * s_v5p_w) / targetH;
        uint16_t* out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sy = ((targetW - 1 - i) * s_v5p_h) / targetW;
          const uint8_t* p = s_v5p_raw + ((size_t)sy * s_v5p_w + sx) * 3;
          out_row[i] = rgbTo565be(p[0], p[1], p[2]);
        }
      }
    } else { // YUV420
      const uint8_t *yp = s_v5p_raw;
      const uint8_t *up = yp + s_v5p_pc;
      const uint8_t *vp = up + s_v5p_cw * s_v5p_ch;

      int sy_lut[320];
      int scy_lut[320];
      for (int i = 0; i < targetW; i++) {
        sy_lut[i] = ((targetW - 1 - i) * s_v5p_h) / targetW;
        scy_lut[i] = sy_lut[i] / 2;
      }

      for (int j = 0; j < targetH; j++) {
        int sx = (j * s_v5p_w) / targetH;
        int scx = sx / 2;
        uint16_t* out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sy = sy_lut[i];
          int scy = scy_lut[i];
          int C = (int)yp[(size_t)sy * s_v5p_w + sx] - 16;
          int D = (int)up[(size_t)scy * s_v5p_cw + scx] - 128;
          int E = (int)vp[(size_t)scy * s_v5p_cw + scx] - 128;
          uint8_t r = cl((298 * C + 459 * E + 128) >> 8);
          uint8_t g = cl((298 * C - 55 * D - 136 * E + 128) >> 8);
          uint8_t b = cl((298 * C + 541 * D + 128) >> 8);
          out_row[i] = rgbTo565be(r, g, b);
        }
      }
    }
  } else if (s_v5p_rot == 3) {
    // 90 degrees CCW
    if (s_v5p_CF == 0) {
      for (int j = 0; j < targetH; j++) {
        int sx = ((targetH - 1 - j) * s_v5p_w) / targetH;
        uint16_t* out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sy = (i * s_v5p_h) / targetW;
          const uint8_t* p = s_v5p_raw + ((size_t)sy * s_v5p_w + sx) * 3;
          out_row[i] = rgbTo565be(p[0], p[1], p[2]);
        }
      }
    } else {
      const uint8_t *yp = s_v5p_raw;
      const uint8_t *up = yp + s_v5p_pc;
      const uint8_t *vp = up + s_v5p_cw * s_v5p_ch;
      int sy_lut[320];
      int scy_lut[320];
      for (int i = 0; i < targetW; i++) {
        sy_lut[i] = (i * s_v5p_h) / targetW;
        scy_lut[i] = sy_lut[i] / 2;
      }
      for (int j = 0; j < targetH; j++) {
        int sx = ((targetH - 1 - j) * s_v5p_w) / targetH;
        int scx = sx / 2;
        uint16_t* out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sy = sy_lut[i];
          int scy = scy_lut[i];
          int C = (int)yp[(size_t)sy * s_v5p_w + sx] - 16;
          int D = (int)up[(size_t)scy * s_v5p_cw + scx] - 128;
          int E = (int)vp[(size_t)scy * s_v5p_cw + scx] - 128;
          uint8_t r = cl((298 * C + 459 * E + 128) >> 8);
          uint8_t g = cl((298 * C - 55 * D - 136 * E + 128) >> 8);
          uint8_t b = cl((298 * C + 541 * D + 128) >> 8);
          out_row[i] = rgbTo565be(r, g, b);
        }
      }
    }
  } else {
    // Unrotated
    if (s_v5p_CF == 0) {
      for (int j = 0; j < targetH; j++) {
        int sy = (j * s_v5p_h) / targetH;
        const uint8_t* row = s_v5p_raw + (size_t)sy * s_v5p_w * 3;
        uint16_t* out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sx = (i * s_v5p_w) / targetW;
          const uint8_t* p = row + sx * 3;
          out_row[i] = rgbTo565be(p[0], p[1], p[2]);
        }
      }
    } else {
      const uint8_t *yp = s_v5p_raw;
      const uint8_t *up = yp + s_v5p_pc;
      const uint8_t *vp = up + s_v5p_cw * s_v5p_ch;
      for (int j = 0; j < targetH; j++) {
        int sy = (j * s_v5p_h) / targetH;
        const uint8_t *yr = yp + (size_t)sy * s_v5p_w;
        const uint8_t *ur = up + (size_t)(sy / 2) * s_v5p_cw;
        const uint8_t *vr = vp + (size_t)(sy / 2) * s_v5p_cw;
        uint16_t *out_row = buf + (size_t)j * targetW;
        for (int i = 0; i < targetW; i++) {
          int sx = (i * s_v5p_w) / targetW;
          int C = (int)yr[sx] - 16;
          int D = (int)ur[sx / 2] - 128;
          int E = (int)vr[sx / 2] - 128;
          uint8_t r = cl((298 * C + 459 * E + 128) >> 8);
          uint8_t g = cl((298 * C - 55 * D - 136 * E + 128) >> 8);
          uint8_t b = cl((298 * C + 541 * D + 128) >> 8);
          out_row[i] = rgbTo565be(r, g, b);
        }
      }
    }
  }
}


// --- Public API ---
int videoInit() {
  s_count = 0;

  // Activate internal pull-ups on SDMMC lines
  pinMode(PIN_SD_CMD, INPUT_PULLUP);
  pinMode(PIN_SD_D0,  INPUT_PULLUP);
  pinMode(PIN_SD_D1,  INPUT_PULLUP);
  pinMode(PIN_SD_D2,  INPUT_PULLUP);
  pinMode(PIN_SD_D3,  INPUT_PULLUP);

  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
  bool mounted = SD_MMC.begin("/sdcard", false);       // 4-bit default
  if (!mounted) {
    Serial.println("[video] SD_MMC 4-bit highspeed failed, trying 20MHz...");
    mounted = SD_MMC.begin("/sdcard", false, false, 20000);
  }
  if (!mounted) {
    Serial.println("[video] SD_MMC 4-bit failed, trying 1-bit...");
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    mounted = SD_MMC.begin("/sdcard", true);
  }
  if (!mounted) {
    Serial.println("[video] SD_MMC 1-bit failed, trying 1-bit 20MHz...");
    mounted = SD_MMC.begin("/sdcard", true, false, 20000);
  }
  if (!mounted) {
    Serial.println("[video] no SD card / mount failed");
    return 0;
  }

  // Pre-allocate JPEG buffer in PSRAM for AVI
  if (!s_jpeg) s_jpeg = (uint8_t*)ps_malloc(MAX_JPEG);
  if (!s_jpeg) s_jpeg = (uint8_t*)malloc(MAX_JPEG);

  Serial.printf("[video] card type=%d size=%lluMB total=%lluMB used=%lluMB\n",
                (int)SD_MMC.cardType(),
                SD_MMC.cardSize()   / (1024ULL * 1024ULL),
                SD_MMC.totalBytes() / (1024ULL * 1024ULL),
                SD_MMC.usedBytes()  / (1024ULL * 1024ULL));

  // 1. Scan root directory for *.v5p and *.avi
  File root = SD_MMC.open("/");
  if (root) {
    for (File f = root.openNextFile(); f && s_count < MAX_CLIPS; f = root.openNextFile()) {
      Serial.printf("[video] entry: name='%s' path='%s' dir=%d size=%u\n",
                    f.name(), f.path(), (int)f.isDirectory(), (unsigned)f.size());
      if (!f.isDirectory() && (endsWith(f.name(), ".v5p") || endsWith(f.name(), ".avi"))) {
        strncpy(s_paths[s_count], f.path(), sizeof(s_paths[0]) - 1);
        strncpy(s_names[s_count], f.name(), sizeof(s_names[0]) - 1);
        s_count++;
      }
      f.close();
    }
    root.close();
  }

  // 2. Explicit direct check for fine.v5p
  const char* explicitFiles[] = { "/fine.v5p", "fine.v5p", "/FINE.V5P", "FINE.V5P" };
  for (const char* fn : explicitFiles) {
    bool already = false;
    for (int i = 0; i < s_count; i++) {
      if (strcasecmp(s_names[i], "fine.v5p") == 0) { already = true; break; }
    }
    if (already || s_count >= MAX_CLIPS) break;

    if (SD_MMC.exists(fn)) {
      File f = SD_MMC.open(fn, FILE_READ);
      if (f && !f.isDirectory()) {
        Serial.printf("[video] found explicit fine.v5p at %s (size %u)\n", fn, (unsigned)f.size());
        strncpy(s_paths[s_count], fn, sizeof(s_paths[0]) - 1);
        strncpy(s_names[s_count], "fine.v5p", sizeof(s_names[0]) - 1);
        s_count++;
        f.close();
        break;
      }
      if (f) f.close();
    }
  }

  // 3. Manifest fallback (/clips.txt)
  if (s_count == 0) {
    File mf = SD_MMC.open("/clips.txt", FILE_READ);
    if (mf) {
      while (mf.available() && s_count < MAX_CLIPS) {
        String line = mf.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;
        char path[80];
        snprintf(path, sizeof(path), "%s%s", line[0] == '/' ? "" : "/", line.c_str());
        File f = SD_MMC.open(path, FILE_READ);
        if (f && !f.isDirectory()) {
          Serial.printf("[video] manifest clip: %s (%u bytes)\n", path, (unsigned)f.size());
          f.close();
          strncpy(s_paths[s_count], path, sizeof(s_paths[0]) - 1);
          const char* nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
          strncpy(s_names[s_count], nm, sizeof(s_names[0]) - 1);
          s_count++;
        } else if (f) f.close();
      }
      mf.close();
    }
  }

  Serial.printf("[video] SD mounted, %d clip(s) found\n", s_count);
  for (int i = 0; i < s_count; i++) Serial.printf("[video]   #%d: %s (%s)\n", i, s_names[i], s_paths[i]);
  return s_count;
}

int  videoCount() { return s_count; }
const char* videoName(int clip) { return (clip >= 0 && clip < s_count) ? s_names[clip] : ""; }

void videoRenderFrame(uint16_t* buf, int w, int h, int clip) {
  if (clip < 0 || clip >= s_count) { memset(buf, 0, w * h * 2); return; }

  // Pace to the clip frame rate
  uint32_t now = micros();
  if (s_lastUs) {
    int32_t due = (int32_t)(s_frameUs - (now - s_lastUs));
    if (due > 1500) vTaskDelay(pdMS_TO_TICKS(due / 1000));
  }
  s_lastUs = micros();

  uint32_t d0 = micros();

  if (endsWith(s_paths[clip], ".v5p")) {
    if (clip != s_curClip && !openV5PClip(clip)) { memset(buf, 0, w * h * 2); return; }
    if (!nextV5PFrame()) { memset(buf, 0, w * h * 2); s_decodeUs = micros() - d0; return; }
    renderV5PFrameToBuffer(buf, w, h);
  } else {
    // AVI
    if (clip != s_curClip && !openClip(clip)) { memset(buf, 0, w * h * 2); return; }
    if (!s_jpeg) { memset(buf, 0, w * h * 2); return; }
    uint32_t sz = nextFrame();
    if (!sz) { memset(buf, 0, w * h * 2); s_decodeUs = micros() - d0; return; }

    s_tgt = buf; s_tgtW = w; s_tgtH = h;
    if (jpeg.openRAM(s_jpeg, sz, jpegDraw)) {
      jpeg.setPixelType(RGB565_BIG_ENDIAN);
      jpeg.decode(0, 0, 0);
      jpeg.close();
    }
  }

  s_decodeUs = micros() - d0;
}

uint32_t videoLastDecodeUs() { return s_decodeUs; }
