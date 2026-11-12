// JPEG through the chip's hardware codec (ESP32-P4). The decoder needs the
// whole file in memory and always decodes at full size; resampling is left to
// the caller. A picture whose full-size output does not fit in PSRAM (4K
// Spotlight: 24.9 MB RGB888) goes through JPEGDEC at a reduced scale instead.
#include "deck_jpeg.h"
#if SOC_JPEG_CODEC_SUPPORTED
#include <new>
#include <esp_heap_caps.h>
#include <driver/jpeg_decode.h>
#include <driver/jpeg_encode.h>
#include <JPEGDEC.h>

namespace deck {
namespace {

constexpr size_t MAX_FILE = 8 * 1024 * 1024;
constexpr int TIMEOUT_MS = 2000;
constexpr int STRIP_ROWS = 16;

// Progressive (SOF2) pictures are not supported by the decoder: look for the
// frame header among the segments before the scan.
bool isProgressive(const uint8_t* p, size_t n) {
  size_t i = 2;
  while (i + 4 <= n && p[i] == 0xFF) {
    const uint8_t m = p[i + 1];
    if (m == 0xC2) return true;
    if (m == 0xDA) break;
    i += 2 + (p[i + 2] << 8 | p[i + 3]);
  }
  return false;
}

// The whole stream into PSRAM; nullptr when it does not fit.
uint8_t* readAll(JpegReadFn read, void* ctx, size_t& len) {
  size_t cap = 256 * 1024;
  len = 0;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  while (buf) {
    if (len == cap) {
      if (cap >= MAX_FILE) break;
      uint8_t* more = (uint8_t*)heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
      if (!more) break;
      buf = more;
      cap *= 2;
    }
    const size_t want = cap - len;
    const size_t got = read(ctx, buf + len, want);
    len += got;
    if (got < want) return buf;  // short read = end of stream
  }
  free(buf);
  return nullptr;
}

struct SwDecode {
  JPEGDEC dec;
  const JpegSink* sink;
  int w, h;
  uint8_t rgb[MAX_BUFFERED_PIXELS * 3];
};

int swDraw(JPEGDRAW* d) {
  SwDecode& s = *(SwDecode*)d->pUser;
  if (d->x >= s.w || d->y >= s.h) return 1;
  const int bw = std::min(d->iWidthUsed, s.w - d->x), bh = std::min(d->iHeight, s.h - d->y);
  uint8_t* o = s.rgb;
  for (int y = 0; y < bh; y++)
    for (int x = 0; x < bw; x++) {
      const uint16_t c = d->pPixels[y * d->iWidth + x];
      *o++ = (c >> 8 & 0xF8) | c >> 13;
      *o++ = (c >> 3 & 0xFC) | (c >> 9 & 3);
      *o++ = (c << 3 & 0xF8) | (c >> 2 & 7);
    }
  s.sink->block(s.sink->ctx, d->x, d->y, d->x + bw - 1, d->y + bh - 1, s.rgb);
  return 1;
}

// Software decode at the smallest power-of-two scale that still covers
// minW x minH; JPEG_NO_MEMORY when even that is not smaller than full size.
JpegResult swDecode(uint8_t* file, size_t len, int fullW, int fullH, int minW, int minH, const JpegSink& sink) {
  int div = 1;
  while (div < 8 && fullW / (div * 2) >= minW && fullH / (div * 2) >= minH) div *= 2;
  if (div == 1) return JPEG_NO_MEMORY;
  void* mem = heap_caps_malloc(sizeof(SwDecode), MALLOC_CAP_SPIRAM);
  if (!mem) return JPEG_NO_MEMORY;
  SwDecode* s = new (mem) SwDecode();
  s->sink = &sink;
  s->w = fullW / div;
  s->h = fullH / div;
  JpegResult r = JPEG_BAD;
  if (s->dec.openRAM(file, (int)len, swDraw)) {
    s->dec.setPixelType(RGB565_LITTLE_ENDIAN);
    s->dec.setUserPointer(s);
    if (!sink.begin(sink.ctx, s->w, s->h, 16)) r = JPEG_NO_MEMORY;
    else if (s->dec.decode(0, 0, div)) r = JPEG_OK;
    s->dec.close();
  }
  s->~SwDecode();
  heap_caps_free(mem);
  return r;
}

}  // namespace

JpegResult jpegDecode(JpegReadFn read, void* readCtx, int minW, int minH, const JpegSink& sink) {
  size_t len = 0;
  uint8_t* file = readAll(read, readCtx, len);
  if (!file) return JPEG_NO_MEMORY;
  if (len < 4 || file[0] != 0xFF || file[1] != 0xD8) {
    free(file);
    return len ? JPEG_BAD : JPEG_CUT;
  }
  if (isProgressive(file, len)) {
    free(file);
    return JPEG_PROGRESSIVE;
  }
  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(file, len, &info) != ESP_OK) {
    free(file);
    return JPEG_BAD;
  }
  if ((int)info.width < minW || (int)info.height < minH) {
    free(file);
    return JPEG_TOO_SMALL;
  }
  // The engine writes whole MCUs: room for 16-aligned rows and columns.
  const uint32_t aw = (info.width + 15) & ~15u, ah = (info.height + 15) & ~15u;
  jpeg_decode_memory_alloc_cfg_t inCfg = {JPEG_DEC_ALLOC_INPUT_BUFFER};
  jpeg_decode_memory_alloc_cfg_t outCfg = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t inSize = 0, outSize = 0;
  uint8_t* in = (uint8_t*)jpeg_alloc_decoder_mem(len, &inCfg, &inSize);
  uint8_t* px = in ? (uint8_t*)jpeg_alloc_decoder_mem((size_t)aw * ah * 3, &outCfg, &outSize) : nullptr;
  if (!px) {
    free(in);
    const JpegResult r = swDecode(file, len, info.width, info.height, minW, minH, sink);
    free(file);
    return r;
  }
  memcpy(in, file, len);
  free(file);
  jpeg_decoder_handle_t dec = nullptr;
  jpeg_decode_engine_cfg_t eng = {};
  eng.timeout_ms = TIMEOUT_MS;
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t got = 0;
  esp_err_t e = jpeg_new_decoder_engine(&eng, &dec);
  if (e == ESP_OK) e = jpeg_decoder_process(dec, &cfg, in, len, px, outSize, &got);
  if (dec) jpeg_del_decoder_engine(dec);
  free(in);
  if (e != ESP_OK) {
    free(px);
    return JPEG_BAD;
  }
  // Rows padded to the aligned width: pack them.
  const int w = info.width, h = info.height;
  const size_t row = (size_t)w * 3;
  if (got >= (size_t)aw * h * 3 && aw != (uint32_t)w)
    for (int y = 1; y < h; y++) memmove(px + y * row, px + (size_t)y * aw * 3, row);
  // Handed over in full-width strips, so the sink needs no full-size copy.
  if (!sink.begin(sink.ctx, w, h, STRIP_ROWS)) {
    free(px);
    return JPEG_NO_MEMORY;
  }
  for (int y = 0; y < h; y += STRIP_ROWS)
    sink.block(sink.ctx, 0, y, w - 1, std::min(y + STRIP_ROWS, h) - 1, px + y * row);
  free(px);
  return JPEG_OK;
}

bool jpegEncode(const uint8_t* rgb, int w, int h, int quality, JpegWriteFn write, void* ctx) {
  jpeg_encode_memory_alloc_cfg_t inCfg = {JPEG_ENC_ALLOC_INPUT_BUFFER};
  jpeg_encode_memory_alloc_cfg_t outCfg = {JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  const size_t rawSize = (size_t)w * h * 3;
  size_t inSize = 0, outSize = 0;
  uint8_t* in = (uint8_t*)jpeg_alloc_encoder_mem(rawSize, &inCfg, &inSize);
  uint8_t* out = in ? (uint8_t*)jpeg_alloc_encoder_mem(rawSize / 2, &outCfg, &outSize) : nullptr;
  bool ok = false;
  if (out) {
    memcpy(in, rgb, rawSize);
    jpeg_encoder_handle_t enc = nullptr;
    jpeg_encode_engine_cfg_t eng = {};
    eng.timeout_ms = TIMEOUT_MS;
    jpeg_encode_cfg_t cfg = {};
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = quality;
    uint32_t len = 0;
    esp_err_t e = jpeg_new_encoder_engine(&eng, &enc);
    if (e == ESP_OK) e = jpeg_encoder_process(enc, &cfg, in, rawSize, out, outSize, &len);
    if (enc) jpeg_del_encoder_engine(enc);
    if (e != ESP_OK) Serial.printf("JPEG: encode %dx%d failed: %s\n", w, h, esp_err_to_name(e));
    ok = e == ESP_OK && write(ctx, out, len) == len;
  }
  free(in);
  free(out);
  return ok;
}

}  // namespace deck

#endif  // SOC_JPEG_CODEC_SUPPORTED
