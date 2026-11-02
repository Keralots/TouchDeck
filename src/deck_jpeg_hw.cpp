// JPEG through the chip's hardware codec (ESP32-P4). The decoder needs the
// whole file in memory and always decodes at full size; resampling is left to
// the caller. Untested on hardware: colour order and odd sizes (600 is not a
// multiple of 16) still need a check.
#include "deck_jpeg.h"
#if SOC_JPEG_CODEC_SUPPORTED
#include <esp_heap_caps.h>
#include <driver/jpeg_decode.h>
#include <driver/jpeg_encode.h>

namespace deck {
namespace {

constexpr size_t MAX_FILE = 8 * 1024 * 1024;
constexpr int TIMEOUT_MS = 2000;

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

}  // namespace

JpegResult jpegDecode(JpegReadFn read, void* ctx, int minW, int minH, JpegImage& out, void (*tick)()) {
  out = {};
  size_t len = 0;
  uint8_t* file = readAll(read, ctx, len);
  if (!file) return JPEG_NO_MEMORY;
  if (tick) tick();
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
  if (in) memcpy(in, file, len);
  free(file);
  if (!px) {
    free(in);
    return JPEG_NO_MEMORY;
  }
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
  const size_t row = (size_t)info.width * 3;
  if (got >= (size_t)aw * info.height * 3 && aw != info.width)
    for (uint32_t y = 1; y < info.height; y++) memmove(px + y * row, px + (size_t)y * aw * 3, row);
  out.px = px;
  out.w = info.width;
  out.h = info.height;
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
