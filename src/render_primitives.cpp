// Renderer primitives: readings, slot selection, font ladders, meters and
// charts. Everything here is coarse-grained enough to cross a translation unit
// boundary without cost; the per-pixel work lives with its loops.
#include "renderer_internal.h"
#include "clock_mode.h"
#include "clock_runner.h"
#include "clock_pong.h"
#include "glass_surface.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  Monitor screen - configurable metric->gauge mapping.
//  Each of the NUM_GAUGE_SLOTS slots binds a PC metric (by id) to a gauge style,
//  full-scale range, and color (the active metric page, persisted in NVS, edited from the web
//  portal). Slots are laid out in a responsive 3-column grid sized to the panel.
// ---------------------------------------------------------------------------
static uint8_t classifyByUnit(const char* unit) {
  switch (unit[0]) {
    case 'C': return GAUGE_TYPE_TEMP;
    case 'W': return GAUGE_TYPE_POWER;
    case '%': return GAUGE_TYPE_PERCENT;
    default:  return GAUGE_TYPE_FAN;   // RPM, MB, MHz, ... -> generic value gauge
  }
}

uint16_t blend565(uint8_t alpha, uint16_t fg, uint16_t bg) {
  const uint8_t r = ((fg >> 11) & 0x1F) * alpha / 255 +
                    ((bg >> 11) & 0x1F) * (255 - alpha) / 255;
  const uint8_t g = ((fg >> 5) & 0x3F) * alpha / 255 +
                    ((bg >> 5) & 0x3F) * (255 - alpha) / 255;
  const uint8_t b = (fg & 0x1F) * alpha / 255 +
                    (bg & 0x1F) * (255 - alpha) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t autoContrast565(uint16_t bg) {
  const uint16_t r = ((bg >> 11) & 0x1F) * 255 / 31;
  const uint16_t g = ((bg >> 5) & 0x3F) * 255 / 63;
  const uint16_t b = (bg & 0x1F) * 255 / 31;
  const uint16_t luminance = (uint16_t)((r * 54 + g * 183 + b * 19) / 256);
  return luminance > 128 ? 0x0000 : 0xFFFF;
}

uint16_t themedLabelColor(uint16_t accent, uint16_t bg,
                                 uint16_t classicColor) {
  switch (themeSettings.labelMode) {
    case THEME_LABEL_CUSTOM: return themeSettings.labelColor;
    case THEME_LABEL_ACCENT: return accent;
    case THEME_LABEL_AUTO:   return autoContrast565(bg);
    default:                 return classicColor;
  }
}

uint16_t themedTileColor(uint16_t accent) {
  if (themeSettings.tileTintPct == 0) return themeSettings.tileColor;
  const uint8_t alpha = (uint8_t)(themeSettings.tileTintPct * 255 / 100);
  return blend565(alpha, accent, themeSettings.tileColor);
}


// A decimal place only where it carries information. A converted reading needs
// one ("3.5 GHz", "3.6 MB/s"), a two- or three-digit one does not: "24.0 GB"
// beside "38 %" spends a whole font-ladder rung on a digit that is always
// noise, which is what made the RAM GB tile render visibly smaller than the
// percent tile next to it.
static void printScaled(char* buf, size_t len, float v) {
  snprintf(buf, len, (fabsf(v) < 10.0f) ? "%.1f" : "%.0f", v);
}

// Byte-rate units climb the same 1024 ladder the storage units do. The Windows
// companion normalises every NIC sensor to KB/s and the Linux one to MB/s, and
// neither the panel nor the portal had any way to show a busy link as anything
// but a four-digit KB/s reading.
static const char* const RATE_UNITS[] = { "B/s", "KB/s", "MB/s", "GB/s", "TB/s" };
static const uint8_t RATE_UNIT_COUNT = 5;

// Position of unit on that ladder, or -1 when it is not a byte rate.
static int8_t rateUnitIndex(const char* unit) {
  for (uint8_t i = 0; i < RATE_UNIT_COUNT; i++)
    if (strcmp(unit, RATE_UNITS[i]) == 0) return (int8_t)i;
  return -1;
}

// One formatter feeds every monitor face so switching layouts never changes
// the meaning or precision of a reading. Large base-unit values use familiar
// compact forms while the companion protocol remains untouched.
void formatMetricText(const PcMetric& metric, float raw, MetricText& out) {
  if (!isfinite(raw)) {
    strlcpy(out.value, "--", sizeof(out.value));
    out.unit[0] = '\0';
    return;
  }

  const float magnitude = fabsf(raw);
  strlcpy(out.unit, metric.unit, sizeof(out.unit));

  const int8_t rate = rateUnitIndex(metric.unit);
  if (rate >= 0) {
    float v = raw;
    int8_t i = rate;
    while (fabsf(v) >= 1024.0f && i < (int8_t)RATE_UNIT_COUNT - 1) {
      v /= 1024.0f;
      i++;
    }
    strlcpy(out.unit, RATE_UNITS[i], sizeof(out.unit));
    // Below 10 the decimal is the whole reading (0.4 MB/s vs "0"), above it the
    // sensor has no such resolution to report.
    printScaled(out.value, sizeof(out.value), v);
    return;
  }

  if (strcmp(metric.unit, "RPM") == 0 && magnitude >= 1000.0f) {
    snprintf(out.value, sizeof(out.value), "%.1fk", raw / 1000.0f);
  } else if (strcmp(metric.unit, "MHz") == 0 && magnitude >= 1000.0f) {
    printScaled(out.value, sizeof(out.value), raw / 1000.0f);
    strlcpy(out.unit, "GHz", sizeof(out.unit));
  } else if (strcmp(metric.unit, "MB") == 0 && magnitude >= 1024.0f) {
    printScaled(out.value, sizeof(out.value), raw / 1024.0f);
    strlcpy(out.unit, "GB", sizeof(out.unit));
  } else if (strcmp(metric.unit, "KB") == 0 && magnitude >= 1024.0f) {
    printScaled(out.value, sizeof(out.value), raw / 1024.0f);
    strlcpy(out.unit, "MB", sizeof(out.unit));
  } else if (strcmp(metric.unit, "W") == 0 && magnitude >= 1000.0f) {
    printScaled(out.value, sizeof(out.value), raw / 1000.0f);
    strlcpy(out.unit, "kW", sizeof(out.unit));
  } else if (strcmp(metric.unit, "GHz") == 0 || strcmp(metric.unit, "GB") == 0) {
    printScaled(out.value, sizeof(out.value), raw);
  } else if (strcmp(metric.unit, "V") == 0 || strcmp(metric.unit, "A") == 0) {
    // Rails and currents keep their decimal at every magnitude - 12.2 V and
    // 12 V are different readings to anyone watching a rail.
    snprintf(out.value, sizeof(out.value), "%.1f", raw);
  } else {
    snprintf(out.value, sizeof(out.value), "%.0f", raw);
  }
}

// ---------------------------------------------------------------------------
//  Alternate monitor styles (Big numbers / Tiles / Hero) - shared helpers.
//  All styles read the same GaugeMapping slots; slot order is reading order.
// ---------------------------------------------------------------------------
// RAII SPI-transaction bracket, same rationale as display_gauges' ScopedWrite:
// one transaction per style frame so WiFi/UDP servicing never interleaves
// between the primitives (the gauge-flicker chassis invariant).

// True while the /screen.bmp handler renders into its capture sprite. The
// capture shares this renderer's code but must not consume the panel's
// pacing state (new-packet flags, layout counts, scale smoothing, previous
// text widths) - stealing those made the panel skip spark updates and
// rescale erratically whenever the portal preview auto-refreshed.
bool gCaptureRender = false;
void setCaptureRender(bool on) { gCaptureRender = on; }

// True when the panel content was actually wiped (screen-state transition,
// style/status flip, online flip). Styles repaint static chrome (tile card
// backgrounds) only then - a plain forceDisplayRedraw() (e.g. after every
// preview capture) repaints values opaquely without blanking anything.
bool gScreenCleared = true;
void markScreenCleared() {
  gScreenCleared = true;
  resetClock();
  resetRunnerClock();
  resetPongClock();
}

// Sparkline compose sprite, file-scope so /api/status can report whether the
// flicker-free path is actually active on this board (a failed allocation
// silently falls back to direct clear+redraw, which blinks at packet rate).
bool ensureSprite(lgfx::LGFX_Sprite& spr, int16_t& curW, int16_t& curH,
                         int16_t w, int16_t h) {
  if (curW == w && curH == h) return true;
  spr.deleteSprite();
  spr.setColorDepth(16);
#if defined(BOARD_IS_JC3248W535)
  // These compose sprites are only ever pushed into the PSRAM face sprite,
  // never to a DMA bus, and internal RAM here is held by the display
  // pipeline's DMA staging buffers and WiFi. Internal allocations left the
  // portal with ~12 KB of heap once a face with cards and charts was live.
  spr.setPsram(true);
#endif
  if (spr.createSprite(w, h)) { curW = w; curH = h; return true; }
  curW = curH = 0;
  return false;
}

static lgfx::LGFX_Sprite gSparkSpr;
static int16_t gSparkW = 0, gSparkH = 0;
static uint16_t gSparkFails = 0;
bool sparkSpriteActive() { return gSparkW > 0; }
uint16_t sparkSpriteFails() { return gSparkFails; }

// ---------------------------------------------------------------------------
//  Companion hiccup guards. The companion pushes ~every second; a packet that
//  momentarily drops metrics or flips the LHM status must not blank/relayout
//  the panel at packet rate (full-screen fillScreen at random intervals was
//  the "screen keeps flickering" report; count dips mid-relayout the
//  "graphs partially visible"). A change must persist across consecutive
//  frames before the layout reacts. Diagnostics count both raw wobble and
//  accepted transitions for /api/status.
// ---------------------------------------------------------------------------
static uint16_t gRawNChanges = 0, gRawStatusChanges = 0;
static uint16_t gRelayouts = 0, gStatusFlips = 0;
uint16_t rawNChanges()      { return gRawNChanges; }
uint16_t rawStatusChanges() { return gRawStatusChanges; }
uint16_t acceptedRelayouts()   { return gRelayouts; }
uint16_t acceptedStatusFlips() { return gStatusFlips; }

static uint8_t gStableN = 0xFF;
uint8_t gStableStatus = 0xFF;

// Returns true when this frame may render; false = transient count wobble,
// skip the frame (pixels keep their last state for a few hundred ms).
bool layoutCountReady(uint8_t n) {
  static uint8_t lastRawN = 0xFF, cand = 0xFF, cnt = 0;
  if (gCaptureRender) return true;
  if (lastRawN != n) { if (lastRawN != 0xFF) gRawNChanges++; lastRawN = n; }
  if (gStableN == 0xFF || n == gStableN) {
    gStableN = n;
    cand = 0xFF;
    cnt = 0;
    return true;
  }
  if (n == cand) {
    if (++cnt >= 3) {
      gStableN = n;
      cand = 0xFF;
      cnt = 0;
      gRelayouts++;
      return true;
    }
  } else {
    cand = n;
    cnt = 1;
  }
  return false;
}

// Debounced companion status for the badge + flip detection: a raw flap must
// persist ~5 frames before the screen reacts.
uint8_t debouncedStatus() {
  static uint8_t lastRaw = 0xFF, cand = 0xFF, cnt = 0;
  const uint8_t cur = pcData.status;
  if (gCaptureRender) return (gStableStatus != 0xFF) ? gStableStatus : cur;
  if (lastRaw != cur) { if (lastRaw != 0xFF) gRawStatusChanges++; lastRaw = cur; }
  if (gStableStatus == 0xFF) { gStableStatus = cur; return cur; }
  if (cur == gStableStatus) {
    cand = 0xFF;
    cnt = 0;
  } else if (cur == cand) {
    if (++cnt >= 5) {
      gStableStatus = cur;
      cand = 0xFF;
      cnt = 0;
      gStatusFlips++;
    }
  } else {
    cand = cur;
    cnt = 1;
  }
  return gStableStatus;
}


// Bound slots whose metric is present in the live packet, in slot order.
uint8_t collectVisibleSlots(VisSlot out[NUM_GAUGE_SLOTS]) {
  // Resolved once: out[].slot points into this mapping for the caller's whole
  // frame, so every slot in the result must come from the same page.
  const GaugeMapping& map = activeGaugeMap();
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    const GaugeSlot& s = map.slots[i];
    if (s.metricId == 0) continue;
    const PcMetric* m = pcMetricFindById(s.metricId);
    if (!m) continue;
    out[n].slotIdx = i;
    out[n].slot = &s;
    out[n].metric = m;
    out[n].label = gaugeDisplayLabel(i, m->name);
    n++;
  }
  return n;
}

// Full-scale for a slot's meter/fraction, mirroring drawSlotGauge's resolution
// so a bar shows the same fill the arc gauge would.
float slotScaleMax(const GaugeSlot& slot, const PcMetric& m) {
  uint8_t type = slot.type;
  if (type == GAUGE_TYPE_AUTO || type >= GAUGE_TYPE_COUNT) type = classifyByUnit(m.unit);
  switch (type) {
    case GAUGE_TYPE_PERCENT: return 100.0f;
    case GAUGE_TYPE_POWER:
      return slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.powerScaleW;
    case GAUGE_TYPE_FAN:
      return slot.scaleMax ? (float)slot.scaleMax : (float)GAUGE_FAN_SCALE_DEFAULT;
    default:
      return slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.tempScaleMax;
  }
}

float slotFraction(float value, float scale) {
  if (scale <= 0) return 0;
  float f = value / scale;
  return f < 0 ? 0 : (f > 1.0f ? 1.0f : f);
}

// Same warn rule the temp gauge applies (reading at or past warnThresholdPct
// percent of full-scale), but with hysteresis: enters at the threshold, exits
// 5 points below it, so a reading hovering around the line does not flap the
// warn color at the packet rate. Per-slot state.
// File scope, not function scope, so the paths that know the state has gone
// stale can clear it - see resetSlotWarnState().
static uint8_t gWarnState = 0;   // bitmask, NUM_GAUGE_SLOTS <= 8

void resetSlotWarnState() { gWarnState = 0; }

bool slotWarn(uint8_t slotIdx, float value, float scale) {
  if (dispSettings.warnThresholdPct == 0 || scale <= 0) return false;
  const float pct = (value / scale) * 100.0f;
  const float enter = (float)dispSettings.warnThresholdPct;
  const bool on = gWarnState & (1u << slotIdx);
  const bool next = on ? (pct > enter - 5.0f) : (pct >= enter);
  if (!gCaptureRender) {
    if (next) gWarnState |= (1u << slotIdx);
    else      gWarnState &= ~(1u << slotIdx);
  }
  return next;
}

// Widest realistic reading for a slot ("3000" for a 3000 RPM full-scale).
// Fonts are sized against this probe instead of the live value, so a reading
// gaining or losing a digit between packets never makes the text jump sizes.
void slotProbe(const GaugeSlot& s, const PcMetric& m, char* buf, size_t len) {
  MetricText scaleText, liveText;
  formatMetricText(m, slotScaleMax(s, m), scaleText);
  formatMetricText(m, m.value, liveText);
  const char* widest = strlen(liveText.value) > strlen(scaleText.value)
    ? liveText.value : scaleText.value;
  strlcpy(buf, widest, len);
}

// ---------------------------------------------------------------------------
//  Font ladders and UNIFORM GROUP SIZING.
//
//  Two ladders: TEXT_LADDER carries the full charset (used wherever the drawn
//  string includes its unit or is a label), VALUE_LADDER adds the oversized
//  digits-only faces for bare readings.
//
//  Every cell of a face used to fit its own string independently. Cells differ
//  in label width, unit width and reading width, so an identical-looking grid
//  could render "38" two rungs above "24.0 GB" in the cell beside it, which
//  reads as a fault rather than as a fit. Each face now runs a cheap pre-pass
//  that fits every member, keeps the SMALLEST rung any of them needed (the
//  largest index - both ladders run big to small), and renders the whole group
//  at that one size.
//
//  A face MUST fold the group rung into each cell's repaint key. When the group
//  steps down, the members whose own reading did not change still have to
//  repaint, or half the grid keeps the previous size.
// ---------------------------------------------------------------------------
extern const FontID TEXT_LADDER[] = { FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL };
extern const uint8_t TEXT_LADDER_N = 4;

// Rung of base itself, i.e. where a fit starting from base begins.
uint8_t textLadderIndex(FontID base) {
  uint8_t i = 0;
  while (i + 1 < TEXT_LADDER_N && TEXT_LADDER[i] != base) i++;
  return i;
}

// Rung of the widest face at or below base whose rendering of s fits maxW.
// Leaves that face active. FONT_XLARGE quietly renders as FONT_LARGE on boards
// without the 22pt blob, which is exactly the wanted degradation.
uint8_t fitTextRung(lgfx::LovyanGFX& gfx, const char* s, int16_t maxW,
                           FontID base) {
  uint8_t i = textLadderIndex(base);
  setFont(gfx, TEXT_LADDER[i]);
  while (i + 1 < TEXT_LADDER_N && (int16_t)gfx.textWidth(s) > maxW)
    setFont(gfx, TEXT_LADDER[++i]);
  return i;
}

// Pick the widest font (from base downwards) whose rendering of s fits maxW.
// Leaves the chosen font active.
FontID fitFontForWidth(const char* s, int16_t maxW, FontID base) {
  return TEXT_LADDER[fitTextRung(tft, s, maxW, base)];
}

// True when the canvas is materially bigger than the 240-wide square panels -
// i.e. the 320x480 Guition in either orientation (min dimension 320), never a
// 240x240 or 240x320 board. Every layout change made for the big panel hangs
// off this flag as an added branch, so the 240x240 boards keep executing the
// exact code they always did rather than a "should be equivalent" rewrite.
bool largeCanvas(int16_t w, int16_t h) {
  return (w < h ? w : h) >= 300;
}

// Pick the largest value face that fits natively, with NO setTextSize()
// magnification. Replaces the old fitFontForWidth + scaleValueToCell pair on
// every board: magnifying a VLW glyph stretches its 8-bit alpha ramp and looks
// blurry (up to 2x on the 240x240 panels, which is where it was worst), and the
// digits-only faces give real 30/38/48/68px glyphs instead. Constrains height as
// well as width, so a value only grows into room the cell actually has - which
// is what makes one ladder serve every panel size and metric count.
//
// DIGITS ONLY at the top four rungs - callers must pass a probe/value string
// from formatMetricText() or slotProbe(), never a label or unit.
// Takes the target explicitly so the sprite-composed faces can use it too.
// Leaves the chosen font ACTIVE at size 1.0 and returns it.
//
// Descent of a face, from its reported line box. Inter's descent is a stable
// 20-21% of fontHeight across every face in this project (3/15, 4/20, 6/28,
// 7/33, 8/38, 9/47, 12/59, 17/83), and LovyanGFX exposes no descent accessor.
int16_t fontDescent(int16_t fontHeight) {
  return (int16_t)((fontHeight * 21) / 100);
}

// How far a value's glyph box may reach above its band. The value is drawn
// with an OPAQUE background, so the box - not the ink - is what lands on the
// panel, and the rows just above a band belong to the label's empty descent.
// Overrunning by that much is invisible; overrunning by more erases the label,
// which is exactly what an earlier ink-based fit did to the duo bands (it let
// a 59px face into a 47px band and the opaque box ate the name above it).
// FONT_SMALL is the label face everywhere here, so its descent is the budget.
extern const int16_t VALUE_BOX_SLACK = 3;

// Baseline for a value fitted by fitValueFont, given the band below the label.
// Centers the glyph box in the band and pins its bottom to the band's bottom
// when the face is taller - the band plus the slack above it is the whole
// region the value may paint.
int16_t valueBaseline(int16_t gapY, int16_t band, int16_t fh) {
  const int16_t baseY = gapY + (band + fh) / 2;
  return (baseY > gapY + band) ? (int16_t)(gapY + band) : baseY;
}

extern const FontID VALUE_LADDER[] = {
  FONT_NUM_XXL, FONT_NUM_XL, FONT_NUM_L, FONT_NUM_M,
  FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL
};
extern const uint8_t VALUE_LADDER_N = 8;

// Rung (0 = biggest) of the largest value face that fits. Leaves it active.
uint8_t fitValueRung(lgfx::LovyanGFX& gfx, const char* s,
                            int16_t maxW, int16_t maxH) {
  gfx.setTextSize(1.0f);
  for (uint8_t i = 0; i + 1 < VALUE_LADDER_N; i++) {
    setFont(gfx, VALUE_LADDER[i]);
    if ((int16_t)gfx.textWidth(s) <= maxW &&
        (int16_t)gfx.fontHeight() <= maxH + VALUE_BOX_SLACK) {
      return i;
    }
  }
  setFont(gfx, VALUE_LADDER[VALUE_LADDER_N - 1]);
  return VALUE_LADDER_N - 1;
}

FontID fitValueFont(lgfx::LovyanGFX& gfx, const char* s,
                           int16_t maxW, int16_t maxH) {
  return VALUE_LADDER[fitValueRung(gfx, s, maxW, maxH)];
}

// Upgrade the unit face beside a fitted value. The value is always fitted
// against the FONT_SMALL unit reservation first; a bigger unit face is chosen
// only when the leftover width absorbs the growth and the value glyph stays
// taller than the unit. So a narrow % grows beside big digits while a wide
// RPM simply stays small - the value never shrinks for its unit. Leaves the
// font state changed; the caller re-applies the value font afterwards.
// Takes the target explicitly so the sprite-composed glass faces can use it.
//
// slackW MUST be measured against the slot PROBE, never the live reading.
// Inter's digits are proportional, so "1.9k" is narrower than "2.0k" - feeding
// the live width in made a pump crossing 2000 RPM hand back enough slack to
// jump its unit a whole face, and the RPM visibly resized every few packets.
// The probe is the widest realistic reading and does not move between packets,
// which is exactly the stability this needs.
FontID upgradeUnitFont(lgfx::LovyanGFX& gfx, const char* unit,
                              int16_t slackW, int16_t valueFh,
                              int16_t smallUnitW) {
  if (!unit || !unit[0]) return FONT_SMALL;
  if (valueFh >= 35) {
    setFont(gfx, FONT_LARGE);
    if ((int16_t)gfx.textWidth(unit) - smallUnitW <= slackW) return FONT_LARGE;
  }
  if (valueFh >= 24) {
    setFont(gfx, FONT_BODY);
    if ((int16_t)gfx.textWidth(unit) - smallUnitW <= slackW) return FONT_BODY;
  }
  return FONT_SMALL;
}

// The three faces upgradeUnitFont can return, biggest first. A face that sizes
// its units per cell ends up with a wide RPM at 10px next to a narrow % at
// 19px, because only the narrow one had the slack to grow. Groups therefore
// take the SMALLEST face any of their members could use, the same way they take
// the smallest value rung.
extern const FontID UNIT_LADDER[] = { FONT_LARGE, FONT_BODY, FONT_SMALL };

uint8_t unitLadderIndex(FontID f) {
  return (f == FONT_LARGE) ? 0 : (f == FONT_BODY) ? 1 : 2;
}

// Baseline offset that makes a unit sit on the value's baseline when both are
// drawn with a BOTTOM datum. BL_DATUM places a box BOTTOM, not a baseline, so
// a shared y drops the smaller face by the difference in descents - 14 px under
// an 83 px value, which is what left the % and RPM hanging below their numbers
// on the glass faces. Subtract this from the value's baseline y.
int16_t unitBaselineShift(int16_t valueFh, int16_t unitFh) {
  return (int16_t)(fontDescent(valueFh) - fontDescent(unitFh));
}

// Horizontal meter: full-width track + fraction fill, rounded ends.
void drawMeterBar(int16_t x, int16_t y, int16_t w, int16_t h,
                         float frac, uint16_t color) {
  int16_t r = h / 2;
  tft.fillRoundRect(x, y, w, h, r, dispSettings.trackColor);
  int16_t fw = (int16_t)(frac * w + 0.5f);
  if (fw > 0) {
    if (fw < h) fw = h;   // keep the rounded cap from degenerating
    tft.fillRoundRect(x, y, fw, h, r, color);
  }
}

// Chart core shared by the sparkline path and the strips lanes: smoothed
// bounds plus the column-wise area/line render, drawn into any target (panel
// or a compose sprite) at the given offset. The caller owns the background.
// Smoothed per-slot chart bounds, shared by the flat sparkline and the glass
// chart so switching faces never rescales a series differently. Expands to the
// window immediately (a new extreme must be visible at once) and contracts
// slowly (5% per redraw), so the chart does not rescale-jump every time an
// extreme leaves the ring. Returns false when there is nothing to plot yet.
// File scope for the same reason as gWarnState: these smoothed bounds describe
// whatever metric the slot held when they were last written, so a rebind or a
// page change has to clear them or the new metric's chart opens on the old
// one's scale.
static float gSparkLo[NUM_GAUGE_SLOTS], gSparkHi[NUM_GAUGE_SLOTS];
static uint8_t gSparkInit = 0;

void resetSparkBoundsState() { gSparkInit = 0; }

bool sparkBounds(const SlotHistory& hist, uint8_t slotIdx, bool advance,
                        float& lo, float& hi) {
  if (hist.count < 2) return false;
  float wlo = pcHistoryAt(hist, 0), whi = wlo;
  for (uint8_t i = 1; i < hist.count; i++) {
    float v = pcHistoryAt(hist, i);
    if (v < wlo) wlo = v;
    if (v > whi) whi = v;
  }
  // Most sensors arrive quantised (whole percent, whole RPM), and auto-scaling
  // a nearly flat window amplifies ONE quantum into a huge vertical jump: at 60
  // samples across ~105 px a run of equal readings is a 2 px wide plateau, so
  // the series reads as a staircase of blocks rather than a line. Estimate the
  // quantum from the smallest real change in the ring and keep several of them
  // in view. A genuinely continuous sensor has a tiny quantum, so this costs it
  // no detail.
  float quantum = 0.0f;
  for (uint8_t i = 1; i < hist.count; i++) {
    float d = pcHistoryAt(hist, i) - pcHistoryAt(hist, i - 1);
    if (d < 0) d = -d;
    if (d > 1e-4f && (quantum == 0.0f || d < quantum)) quantum = d;
  }
  const float minSpan = quantum * 6.0f;
  if (minSpan > 0.0f && (whi - wlo) < minSpan) {
    const float mid = (whi + wlo) * 0.5f;
    wlo = mid - minSpan * 0.5f;
    whi = mid + minSpan * 0.5f;
  }

  float pad = (whi - wlo) * 0.15f;
  if (pad < 0.5f) pad = 0.5f;
  wlo -= pad;
  whi += pad;

  const uint8_t bit = (uint8_t)(1u << slotIdx);
  if (gCaptureRender) {
    lo = (gSparkInit & bit) ? gSparkLo[slotIdx] : wlo;
    hi = (gSparkInit & bit) ? gSparkHi[slotIdx] : whi;
    return true;
  }
  // Mutate the smoothed bounds only when a new sample arrived - a plain full
  // redraw (portal preview cycle) must reproduce identical pixels, otherwise
  // the chart creeps every 4 s while the preview is open.
  if (hist.count <= 2 || !(gSparkInit & bit)) {
    gSparkLo[slotIdx] = wlo;
    gSparkHi[slotIdx] = whi;
    gSparkInit |= bit;
  } else if (advance) {
    if (wlo < gSparkLo[slotIdx]) gSparkLo[slotIdx] = wlo;
    else gSparkLo[slotIdx] += (wlo - gSparkLo[slotIdx]) * 0.05f;
    if (whi > gSparkHi[slotIdx]) gSparkHi[slotIdx] = whi;
    else gSparkHi[slotIdx] += (whi - gSparkHi[slotIdx]) * 0.05f;
  }
  lo = gSparkLo[slotIdx];
  hi = gSparkHi[slotIdx];
  return true;
}

// Defined with the chart helpers further down; the flat faces and the glass
// faces share both so a smoothing setting means the same thing on every face.
uint8_t buildChartSeries(const SlotHistory& hist, float* out, uint8_t passes);
float histSmooth(const float* s, int n, float fi);

void sparkPlot(lgfx::LovyanGFX& g, const SlotHistory& hist,
                      uint8_t slotIdx, int16_t ox, int16_t oy,
                      int16_t w, int16_t h, uint16_t color, bool advance,
                      uint16_t scrollQ8 = 0) {
  float lo, hi;
  if (sparkBounds(hist, slotIdx, advance, lo, hi)) {
    // Column-wise render: a dim area fill under a 2px connected line. A bare
    // 1px polyline disappears on the physical panel wherever the series goes
    // flat near the box edge; the filled area keeps the shape readable.
    // Same series treatment as the glass charts: an own copy so the optional
    // low pass cannot touch the ring the readings print from, sampled with the
    // Catmull-Rom rather than a bare lerp.
    float series[PC_HISTORY_LEN];
    const int n = (int)buildChartSeries(hist, series, chartSmoothing);
    const int16_t dotR = (h >= 24) ? 2 : 1;
    const int16_t plotW = w - dotR - 1;   // reserve so the endpoint dot stays inside
    const uint16_t areaColor = (uint16_t)((color >> 2) & 0x39E7);  // ~1/4 brightness
    // Fixed samples per pixel, newest pinned to the right edge, so a partly
    // filled ring grows in from the right instead of re-fitting itself across
    // the full width on every packet. Both callers clear the box before this,
    // so the columns ahead of xStart can simply be left alone.
    const float pdenom = (plotW > 1) ? (float)(plotW - 1) : 1.0f;
    const float pstep = (float)(PC_HISTORY_LEN - 1) / pdenom;
    // Lag one sample and slide across the packet interval so the newest
    // reading walks in instead of popping: at scroll 0 the window ends on the
    // second-newest, at full scroll exactly on the newest.
    const bool glide = (n >= 3);
    const float shift = glide ? ((float)scrollQ8 / 256.0f) : 0.0f;
    const float pRight = (glide ? (float)(n - 2) : (float)(n - 1)) + shift;
    int16_t xStart = (int16_t)(plotW - 1 - (int32_t)(pRight / pstep));
    if (xStart < 0) xStart = 0;
    int16_t prevY = 0, sy = 0;
    for (int16_t xi = xStart; xi < plotW; xi++) {
      const float fi = pRight - (float)(plotW - 1 - xi) * pstep;
      const float v = histSmooth(series, n, fi < 0.0f ? 0.0f : fi);
      sy = oy + (h - 1) - (int16_t)((v - lo) * (float)(h - 1) / (hi - lo));
      g.drawFastVLine(ox + xi, sy, oy + h - sy, areaColor);
      const int16_t lineTop = (xi > xStart && prevY < sy) ? prevY : sy;
      const int16_t lineBot = (xi > xStart && prevY > sy) ? prevY : sy;
      int16_t lineH = lineBot - lineTop + 2;   // 2px stroke
      if (lineTop + lineH > oy + h) lineH = oy + h - lineTop;  // stay inside the box
      g.drawFastVLine(ox + xi, lineTop, lineH, color);
      prevY = sy;
    }
    // Keep the endpoint dot fully inside the box (matters on the direct-draw
    // fallback, which has no sprite clipping under it).
    int16_t dotY = sy;
    if (dotY < oy + dotR) dotY = oy + dotR;
    if (dotY > oy + h - 1 - dotR) dotY = oy + h - 1 - dotR;
    g.fillCircle(ox + plotW - 1, dotY, dotR, themeSettings.valueColor);
  }
}

// Translucent scrim behind overlay text. The strips lanes print their label
// and reading on top of the chart, and a bright area fill swallows both (a
// panel photo of the CLK and VRAM lanes made that plain). Blending the pixels
// under the type toward the background gives it a surface to sit on while the
// chart shape stays visible underneath - a solid box would punch a hole in the
// chart, and a drawn outline turns to mush at 12px.
//
// Sprite targets only. This reads back what was just drawn, which is a RAM
// access on a sprite but a slow per-pixel bus read on the panel (and not all
// panels can read at all), so the no-sprite fallback keeps drawing over the
// chart the way it always did.
void drawTextScrim(lgfx::LGFX_Sprite& spr, int16_t x, int16_t y,
                          int16_t w, int16_t h, uint16_t bg, uint8_t alpha) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int16_t)spr.width())  w = (int16_t)spr.width() - x;
  if (y + h > (int16_t)spr.height()) h = (int16_t)spr.height() - y;
  if (w <= 0 || h <= 0) return;

  // Clipped corners so the shade reads as a soft panel rather than a hard box.
  const int16_t r = (w > 8 && h > 8) ? 3 : 0;
  for (int16_t yy = 0; yy < h; yy++) {
    const int16_t dy = (yy < r) ? (int16_t)(r - yy)
                     : (yy >= h - r) ? (int16_t)(yy - (h - 1 - r)) : (int16_t)0;
    for (int16_t xx = 0; xx < w; xx++) {
      const int16_t dx = (xx < r) ? (int16_t)(r - xx)
                       : (xx >= w - r) ? (int16_t)(xx - (w - 1 - r)) : (int16_t)0;
      if (dx && dy && dx * dx + dy * dy > r * r) continue;
      const int16_t px = x + xx, py = y + yy;
      spr.drawPixel(px, py, blend565(alpha, bg, spr.readPixel(px, py)));
    }
  }
}

// Sparkline over a slot's history ring, scaled to the local min/max (padded so
// a flat line does not hug an edge), 1px polyline with a bright endpoint dot.
// Composed in a reusable offscreen sprite and pushed in one blit so the
// clear+redraw never flashes at the packet cadence; falls back to direct
// drawing (slightly blinky) only if the sprite cannot be allocated.
void drawSparkline(const SlotHistory& hist, uint8_t slotIdx,
                          int16_t x, int16_t y,
                          int16_t w, int16_t h, uint16_t color, uint16_t bg,
                          bool advance, uint16_t scrollQ8 = 0) {
  if (w < 8 || h < 8) return;

  if (gSparkW != w || gSparkH != h) {
    gSparkSpr.deleteSprite();
    gSparkSpr.setColorDepth(16);
#if defined(BOARD_IS_JC3248W535)
    gSparkSpr.setPsram(true);  // see ensureSprite()
#endif
    if (gSparkSpr.createSprite(w, h)) { gSparkW = w; gSparkH = h; }
    else { gSparkW = gSparkH = 0; gSparkFails++; }
  }
  const bool off = (gSparkW == w && gSparkH == h);
  lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)gSparkSpr : (lgfx::LovyanGFX&)tft;
  const int16_t ox = off ? 0 : x;
  const int16_t oy = off ? 0 : y;

  g.fillRect(ox, oy, w, h, bg);
  sparkPlot(g, hist, slotIdx, ox, oy, w, h, color, advance, scrollQ8);
  if (off) {
    gSparkSpr.pushSprite(tft_ptr, x, y);
    // pushSprite may queue the transfer via SPI DMA and return; the shared
    // sprite gets refilled for the NEXT chart immediately after. Without this
    // barrier the in-flight transfer reads the half-overwritten buffer -
    // tiles then show each other's colors and wedge-shaped partial fills.
    tft.waitDMA();
  }
}


// --- motion -----------------------------------------------------------------
//
// The companion pushes roughly once a second. Stepping the plot a whole column
// per packet is a visible jerk, so the chart glides by a fraction of a sample
// between packets and the newest reading slides in over the interval. That
// costs one packet of latency at the right edge, which is invisible, and is
// what buys continuous motion instead of a 1 Hz stutter.
//
// The reading itself is NOT eased. Running the digits through intermediate
// values would make them change on every frame instead of once per packet -
// more churn, and a stats readout that shows numbers the sensor never
// reported. Motion belongs to the chart; the number stays exact.
static uint32_t gPktIntervalMs = 1000;
static uint32_t gPrevRx = 0;
uint16_t gScrollQ8 = 0;
bool gNewSample = false;

void advanceChartMotion() {
  if (gCaptureRender) return;      // capture must not consume pacing state
  const uint32_t rx = pcData.lastReceived;
  gNewSample = false;
  if (rx == 0) { gScrollQ8 = 0; return; }
  if (rx != gPrevRx) {
    if (gPrevRx != 0) {
      const uint32_t dt = rx - gPrevRx;
      if (dt >= 150 && dt <= 10000) gPktIntervalMs = (gPktIntervalMs * 3 + dt) / 4;
    }
    gPrevRx = rx;
    gNewSample = true;
  }
  const uint32_t elapsed = millis() - rx;
  const uint32_t span = gPktIntervalMs ? gPktIntervalMs : 1000;
  const uint32_t q = (elapsed * 256) / span;
  gScrollQ8 = (uint16_t)(q > 256 ? 256 : q);
}

// Catmull-Rom sample with a monotone clamp. Only visibly different from a
// straight lerp while the ring is still filling (few samples across many
// columns), but that is exactly when the stepped polyline looks worst; the
// clamp stops the spline overshooting a percentage below zero.
// Copy the ring oldest->newest, optionally low-passed. Each pass is a binomial
// [1,2,1]/4 kernel, which rounds one-sample needles without moving the series
// sideways the way a trailing average would. The two ENDPOINTS are left exact
// so the live end of the chart still agrees with the printed reading.
uint8_t buildChartSeries(const SlotHistory& hist, float* out, uint8_t passes) {
  const uint8_t n = hist.count;
  for (uint8_t i = 0; i < n; i++) out[i] = pcHistoryAt(hist, i);
  if (n < 3) return n;
  for (uint8_t p = 0; p < passes; p++) {
    float prev = out[0];
    for (uint8_t i = 1; i + 1 < n; i++) {
      const float cur = out[i];
      out[i] = (prev + 2.0f * cur + out[i + 1]) * 0.25f;
      prev = cur;
    }
  }
  return n;
}

float histSmooth(const float* s, int n, float fi) {
  int i1 = (int)fi;
  if (i1 < 0) i1 = 0;
  if (i1 > n - 1) i1 = n - 1;
  const float t = fi - (float)i1;
  const int i0 = i1 > 0 ? i1 - 1 : 0;
  const int i2 = i1 + 1 < n ? i1 + 1 : n - 1;
  const int i3 = i1 + 2 < n ? i1 + 2 : n - 1;
  const float p0 = s[i0], p1 = s[i1];
  const float p2 = s[i2], p3 = s[i3];
  const float v = 0.5f * ((2.0f * p1) +
                          (-p0 + p2) * t +
                          (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                          (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
  // Clamp to the four-point neighbourhood, NOT to [p1,p2]. Pinning to the
  // segment holds every run of equal samples perfectly flat and then jumps at
  // the riser, which is exactly what turns a quantised series into flat blocks.
  // The wider bound lets a plateau ease into the next one while still keeping
  // the spline inside the data, so it cannot overshoot below zero either.
  float lo = p0, hi = p0;
  if (p1 < lo) lo = p1;
  if (p1 > hi) hi = p1;
  if (p2 < lo) lo = p2;
  if (p2 > hi) hi = p2;
  if (p3 < lo) lo = p3;
  if (p3 > hi) hi = p3;
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
//  Flicker-free value regions. The old scheme cleared the whole cell/row and
//  repainted it, which blinks at the packet cadence. Instead: opaque glyphs
//  overwrite the previous text in one pass, and only the pixels the new text
//  no longer covers get cleared - painting bg over bg is invisible, so the
//  only pixels that visibly change are the ones that actually changed.
// ---------------------------------------------------------------------------
// Left-aligned value + a small dim unit set against the value height.
// Caller has already selected the value font (fitValueFont); resets the text
// size to 1.0 itself. bandH covers the tallest glyph box a previous draw may
// have used.
void drawValueRegionL(int16_t x, int16_t baseY, int16_t bandW, int16_t bandH,
                             const char* val, const char* unit,
                             uint16_t fg, uint16_t bg, int16_t* prevVw,
                             FontID unitFont = FONT_SMALL) {
  const int16_t fh = (int16_t)tft.fontHeight();
  // A face can report more height than the band holds - its descent is empty
  // for digits, which is why fitValueFont let it through. Clip every clear to
  // the band so the region never paints over the label above it.
  const int16_t boxH = (fh > bandH) ? bandH : fh;
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(val, x, baseY);
  const int16_t vw = tft.textWidth(val);
  tft.setTextSize(1.0f);
  // The band above the glyph box is background whenever the region is
  // healthy, so repainting it is invisible - and it evicts stale pixels left
  // by an earlier taller font or a layout that once painted there.
  if (bandH > boxH) tft.fillRect(x, baseY - bandH, bandW, bandH - boxH, bg);
  // The unit and the vacated-pixel clears only need repainting when the
  // value's glyph box changed - doing it every update made the unit blink at
  // the packet rate. The key folds in the font height so a font change with
  // an equal pixel width still repaints (the unit rides the glyph height).
  // prevVw==nullptr forces a full repaint without touching state (capture).
  const int16_t key = (int16_t)(uint16_t)((vw & 0x01FF) | ((fh & 0x7F) << 9));
  if (prevVw && *prevVw == key) return;
  if (bandW > vw) tft.fillRect(x + vw, baseY - boxH, bandW - vw, boxH, bg);
  if (unit && unit[0]) {
    setFont(tft, unitFont);
    const int16_t unitFh = (int16_t)tft.fontHeight();
    // Centering the unit against the glyph box reads fine when the two faces
    // are close in size, but a floating mid-height unit looks detached once the
    // value towers over it. Sit it on the value's baseline then, the way "15 %"
    // is normally set. The large canvas always does this (it never renders a
    // small value here); the square panels now reach 38-59px values too, so
    // they get the same treatment whenever the gap is wide.
    //
    // BL_DATUM places a box BOTTOM, not a baseline, so passing baseY straight
    // through drops the unit by the difference in descents - about 9px under a
    // 59px value, which reads as the unit having slipped below the number.
    // Lift it by that difference so the two baselines actually meet.
    const bool baselineUnit =
        largeCanvas((int16_t)tft.width(), (int16_t)tft.height()) ||
        (fh - unitFh) >= 16;
    const int16_t unitBaseY =
        baselineUnit ? (int16_t)(baseY - (fontDescent(fh) - fontDescent(unitFh)))
                     : (int16_t)(baseY - (fh - unitFh) / 2);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(themeSettings.secondaryColor, bg);
    tft.drawString(unit, x + vw + 5, unitBaseY);
  }
  if (prevVw) *prevVw = key;
}

// Right-aligned (middle datum) value whose left edge moves as digits change:
// clears only the vacated pixels between leftBound and the new glyph box.
void drawValueRegionR(int16_t leftBound, int16_t rightX, int16_t cy,
                             int16_t bandH, const char* s,
                             uint16_t fg, uint16_t bg) {
  const int16_t fh = (int16_t)tft.fontHeight();
  const int16_t boxTop = cy - fh / 2;
  const int16_t bandTop = cy - bandH / 2;
  const int16_t regionW = rightX - leftBound;
  if (regionW <= 0) return;
  if (boxTop > bandTop) {
    tft.fillRect(leftBound, bandTop, regionW, boxTop - bandTop, bg);
    tft.fillRect(leftBound, boxTop + fh, regionW, bandTop + bandH - boxTop - fh, bg);
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(s, rightX, cy);
  const int16_t vw = tft.textWidth(s);
  if (regionW > vw) tft.fillRect(leftBound, boxTop, regionW - vw, fh, bg);
}
