// JPEG decode and encode for the Daily pictures. The S3 uses ROM TJpgDec and
// the esp32-camera encoder (deck_jpeg_sw.cpp); a chip with a JPEG codec uses
// the hardware (deck_jpeg_hw.cpp).
#ifndef DECK_JPEG_H
#define DECK_JPEG_H

#include <Arduino.h>
#include <soc/soc_caps.h>

namespace deck {

enum JpegResult : uint8_t { JPEG_OK, JPEG_PROGRESSIVE, JPEG_TOO_SMALL, JPEG_NO_MEMORY, JPEG_CUT, JPEG_BAD };

// Up to n bytes into buf (nullptr: skip them); fewer only at the end of the stream.
using JpegReadFn = size_t (*)(void* ctx, uint8_t* buf, size_t n);
using JpegWriteFn = size_t (*)(void* ctx, const uint8_t* data, size_t n);

// A decoded picture: RGB888 in R, G, B order, PSRAM, release with free().
struct JpegImage {
  uint8_t* px = nullptr;
  int w = 0, h = 0;
};

// Decode the stream to at least minW x minH pixels (scaled down by a power of
// two where the decoder can). `tick` runs now and then during long decodes.
JpegResult jpegDecode(JpegReadFn read, void* ctx, int minW, int minH, JpegImage& out, void (*tick)());

// The encoder takes RGB888 in B, G, R order when this is true.
#if SOC_JPEG_CODEC_SUPPORTED
constexpr bool JPEG_ENCODE_BGR = false;
#else
constexpr bool JPEG_ENCODE_BGR = true;
#endif

// Encode w x h RGB888 at quality 1..100 through `write`.
bool jpegEncode(const uint8_t* rgb, int w, int h, int quality, JpegWriteFn write, void* ctx);

}  // namespace deck

#endif  // DECK_JPEG_H
