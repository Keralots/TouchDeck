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

// Takes the picture as it is decoded. begin: decoded size and the tallest
// block, false = no memory. block: pixels x0..x1, y0..y1 (inclusive) as packed
// RGB888 rows in R, G, B order; a block reaching x = w - 1 ends a band.
struct JpegSink {
  bool (*begin)(void* ctx, int w, int h, int bandH);
  void (*block)(void* ctx, int x0, int y0, int x1, int y1, const uint8_t* rgb);
  void* ctx;
};

// Decode the stream to at least minW x minH pixels (scaled down by a power of
// two where the decoder can).
JpegResult jpegDecode(JpegReadFn read, void* readCtx, int minW, int minH, const JpegSink& sink);

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
