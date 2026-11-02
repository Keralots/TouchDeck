// JPEG on chips without a codec (the S3): ROM TJpgDec, which can scale while
// it decodes, and the esp32-camera software encoder.
#include "deck_jpeg.h"
#if !SOC_JPEG_CODEC_SUPPORTED
#include <esp_heap_caps.h>
#include "img_converters.h"
#include "esp32s3/rom/tjpgd.h"

namespace deck {
namespace {

// Called by address (esp32s3.rom.ld) because LVGL defines the same names.
using PrepareFn = JRESULT (*)(JDEC*, UINT (*)(JDEC*, BYTE*, UINT), void*, UINT, void*);
using DecompFn = JRESULT (*)(JDEC*, UINT (*)(JDEC*, void*, JRECT*), BYTE);
const PrepareFn romPrepare = (PrepareFn)0x40000858;
const DecompFn romDecomp = (DecompFn)0x40000864;

struct Dec {
  JpegReadFn read;
  void* ctx;
  JpegImage* img;
  void (*tick)();
  uint32_t blocks;
};

UINT input(JDEC* jd, BYTE* buf, UINT n) {
  Dec& d = *(Dec*)jd->device;
  return (UINT)d.read(d.ctx, buf, n);
}

UINT output(JDEC* jd, void* bitmap, JRECT* rect) {
  Dec& d = *(Dec*)jd->device;
  const JpegImage& img = *d.img;
  const uint8_t* src = (const uint8_t*)bitmap;
  const int rw = rect->right - rect->left + 1;
  for (int y = rect->top; y <= rect->bottom; y++, src += rw * 3) {
    if (y >= img.h) continue;
    const int n = std::min(rw, img.w - (int)rect->left);
    if (n > 0) memcpy(img.px + ((size_t)y * img.w + rect->left) * 3, src, n * 3);
  }
  // Let the core 0 idle task feed its watchdog during long decodes.
  if ((++d.blocks & 255) == 0) {
    if (d.tick) d.tick();
    vTaskDelay(1);
  }
  return 1;
}

struct Enc {
  JpegWriteFn write;
  void* ctx;
};

size_t encOut(void* arg, size_t, const void* data, size_t len) {
  Enc& e = *(Enc*)arg;
  return e.write(e.ctx, (const uint8_t*)data, len);
}

}  // namespace

JpegResult jpegDecode(JpegReadFn read, void* ctx, int minW, int minH, JpegImage& out, void (*tick)()) {
  out = {};
  void* pool = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!pool) return JPEG_NO_MEMORY;
  Dec d = {read, ctx, &out, tick, 0};
  JDEC jd;
  JRESULT r = romPrepare(&jd, input, pool, 4096, &d);
  JpegResult res = JPEG_OK;
  if (r == JDR_OK) {
    uint8_t scale = 0;
    while (scale < 3 && (int)(jd.width >> (scale + 1)) >= minW && (int)(jd.height >> (scale + 1)) >= minH) scale++;
    out.w = (jd.width + (1 << scale) - 1) >> scale;
    out.h = (jd.height + (1 << scale) - 1) >> scale;
    if (out.w < minW || out.h < minH) {
      res = JPEG_TOO_SMALL;
    } else {
      out.px = (uint8_t*)heap_caps_malloc((size_t)out.w * out.h * 3, MALLOC_CAP_SPIRAM);
      r = out.px ? romDecomp(&jd, output, scale) : JDR_MEM1;
    }
  }
  free(pool);
  if (res == JPEG_OK && r != JDR_OK)
    res = r == JDR_FMT3 ? JPEG_PROGRESSIVE : r == JDR_MEM1 ? JPEG_NO_MEMORY : r == JDR_INP ? JPEG_CUT : JPEG_BAD;
  if (res != JPEG_OK) {
    free(out.px);
    out = {};
  }
  return res;
}

bool jpegEncode(const uint8_t* rgb, int w, int h, int quality, JpegWriteFn write, void* ctx) {
  Enc e = {write, ctx};
  return fmt2jpg_cb((uint8_t*)rgb, (size_t)w * h * 3, w, h, PIXFORMAT_RGB888, quality, encOut, &e);
}

}  // namespace deck

#endif  // !SOC_JPEG_CODEC_SUPPORTED
