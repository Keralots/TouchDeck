#pragma once

#include <algorithm>
#include <stdint.h>

namespace deck {

constexpr int POPUP_PROGRESS = 1000;
constexpr int SLIDE_IN_MS = 150, SLIDE_OUT_MS = 200;
constexpr int DROPLET_SWELL_MS = 220, DROPLET_SHRINK_MS = 160;
constexpr int BEAD_DRIP_MS = 380, BEAD_RETURN_MS = 180, WOBBLE_MS = 520;
constexpr int BEAD_BUMP_MS = BEAD_DRIP_MS * 45 / 100, BEAD_PEAK_MS = BEAD_DRIP_MS * 80 / 100;
constexpr int CONTENT_IN_MS = 120, CONTENT_OUT_MS = 80;

enum class PopupStyle : uint8_t { Droplet, Edge, Bead, Pill };
enum class PopupMotion : uint8_t { None, Droplet, Drip, Slide, Wobble, Fade };

struct PopupFrame {
  int across, along, shift;
  int opacity, content;
};

struct PopupGeometry {
  PopupStyle style;
  int cx, cy, outX, outY, across, along;
  int slide, beadTravel;
};

struct PopupAnimation {
  PopupMotion kind;
  bool exiting;
  uint8_t speed;
  int duration;  // nominal ms
  int64_t startUs;
  PopupFrame from;
};

constexpr struct { int at, across, along; } WOBBLE_KEYS[] = {
  {0, 30, 70}, {350, 112, 90}, {580, 94, 106}, {780, 103, 98}, {1000, 100, 100}
};

inline int64_t popupEndUs(const PopupAnimation& a) {
  return a.startUs + (a.duration && a.speed ? ((int64_t)a.duration * 100000 + a.speed - 1) / a.speed : 0);
}

inline int popupProgress(const PopupAnimation& a, int64_t atUs) {
  if (!a.duration || atUs >= popupEndUs(a)) return POPUP_PROGRESS;
  if (atUs <= a.startUs) return 0;
  return (int)((atUs - a.startUs) * a.speed / (a.duration * 100));
}

inline int popupMix(int from, int to, int progress) {
  const int mixed = from * (POPUP_PROGRESS - progress) + to * progress;
  return (mixed + (mixed < 0 ? -POPUP_PROGRESS / 2 : POPUP_PROGRESS / 2)) / POPUP_PROGRESS;
}

inline int popupPhase(int elapsed, int start, int duration) {
  return std::clamp((elapsed - start * POPUP_PROGRESS) / duration, 0, POPUP_PROGRESS);
}

inline int popupEaseIn(int progress) {
  return progress * progress / POPUP_PROGRESS;
}

inline int popupEaseOut(int progress) {
  return POPUP_PROGRESS - popupEaseIn(POPUP_PROGRESS - progress);
}

inline int popupSmooth(int progress) {
  return progress * progress * (3 * POPUP_PROGRESS - 2 * progress) / (POPUP_PROGRESS * POPUP_PROGRESS);
}

inline int popupSlideEase(int progress, bool exiting) {
  if (progress == 0 || progress == POPUP_PROGRESS) return progress;
  if (exiting) progress = POPUP_PROGRESS - progress;
  const int target = progress * 4096 / 10;
  int lo = 0, hi = 4096;
  while (hi - lo > 1) {
    const int t = (lo + hi) / 2;
    const int u = 4096 - t;
    // CSS ease-out: (0, 0), (0.58, 1).
    const int x = (int)(((int64_t)t * t * (174 * u + 100 * t)) >> 24);
    if (x < target) lo = t;
    else hi = t;
  }
  const int y = (int)(((int64_t)hi * hi * (12288 - 2 * hi) * POPUP_PROGRESS + (1LL << 35)) >> 36);
  return exiting ? POPUP_PROGRESS - y : y;
}

inline PopupFrame popupRestFrame(const PopupGeometry& g) {
  return {g.across, g.along, 0, 255, 255};
}

inline int popupAnchorShift(const PopupGeometry& g, int across) {
  if (g.style != PopupStyle::Bead && g.style != PopupStyle::Pill) return 0;
  const int round = g.outX + g.outY > 0 ? 1 : 0;
  return (g.across + round) / 2 - (across + round) / 2;
}

inline PopupFrame popupStartFrame(const PopupGeometry& g, const PopupAnimation& a) {
  PopupFrame f = popupRestFrame(g);
  switch (a.kind) {
    case PopupMotion::Droplet: f.across = 0; f.along = g.along * 45 / 100; f.content = 0; break;
    case PopupMotion::Drip: f.across = 0; f.along = g.along / 2; f.shift = g.beadTravel; f.content = 0; break;
    case PopupMotion::Slide: f.shift = g.slide; f.opacity = f.content = 0; break;
    case PopupMotion::Wobble:
      f.across = g.across * WOBBLE_KEYS[0].across / 100;
      f.along = g.along * WOBBLE_KEYS[0].along / 100;
      f.shift = popupAnchorShift(g, f.across);
      f.opacity = f.content = 0;
      break;
    case PopupMotion::Fade: f.opacity = 0; break;
    case PopupMotion::None: break;
  }
  return f;
}

inline PopupFrame popupDripFrame(const PopupGeometry& g, const PopupAnimation& a, int elapsed) {
  PopupFrame f = popupRestFrame(g);
  const PopupFrame& from = a.from;
  if (a.exiting) {
    const int t = popupEaseIn(popupPhase(elapsed, CONTENT_OUT_MS, BEAD_RETURN_MS));
    f.across = popupMix(from.across, 0, t);
    f.along = popupMix(from.along, g.along * 40 / 100, t);
    f.shift = popupMix(from.shift, g.beadTravel, t);
    f.content = popupMix(from.content, 0, popupPhase(elapsed, 0, CONTENT_OUT_MS));
    return f;
  }
  const int dripMs = a.duration - CONTENT_IN_MS;
  f.content = popupMix(from.content, 255, popupPhase(elapsed, dripMs, CONTENT_IN_MS));
  if (!dripMs) return f;
  // Resume at takeoff if the bead is already partway through its return.
  const int age = elapsed + (BEAD_DRIP_MS - dripMs) * POPUP_PROGRESS;
  const int bumpAcross = g.across * 90 / 100, bumpAlong = g.along * 85 / 100;
  const int peakAcross = g.across * 108 / 100, peakAlong = g.along * 94 / 100;
  if (age < BEAD_BUMP_MS * POPUP_PROGRESS) {
    const int t = popupEaseOut(popupPhase(age, 0, BEAD_BUMP_MS));
    f.across = popupMix(from.across, bumpAcross, t);
    f.along = popupMix(from.along, bumpAlong, t);
    f.shift = g.beadTravel;
  } else if (age < BEAD_PEAK_MS * POPUP_PROGRESS) {
    const int t = popupSmooth(popupPhase(age, BEAD_BUMP_MS, BEAD_PEAK_MS - BEAD_BUMP_MS));
    const bool fullDrip = dripMs == BEAD_DRIP_MS;
    f.across = popupMix(fullDrip ? bumpAcross : from.across, peakAcross, t);
    f.along = popupMix(fullDrip ? bumpAlong : from.along, peakAlong, t);
    f.shift = popupMix(fullDrip ? g.beadTravel : from.shift, 0, t);
  } else {
    const int t = popupSmooth(popupPhase(age, BEAD_PEAK_MS, BEAD_DRIP_MS - BEAD_PEAK_MS));
    f.across = popupMix(peakAcross, g.across, t);
    f.along = popupMix(peakAlong, g.along, t);
  }
  return f;
}

inline PopupFrame popupWobbleFrame(const PopupGeometry& g, const PopupAnimation& a, int elapsed) {
  PopupFrame f = popupRestFrame(g);
  const PopupFrame& from = a.from;
  const int wobbleMs = a.duration - CONTENT_IN_MS;
  f.content = popupMix(from.content, 255, popupPhase(elapsed, wobbleMs, CONTENT_IN_MS));
  if (!wobbleMs || elapsed >= wobbleMs * POPUP_PROGRESS) return f;
  const int age = popupPhase(elapsed, 0, wobbleMs);
  int next = 1;
  while (age >= WOBBLE_KEYS[next].at) ++next;
  const auto& prev = WOBBLE_KEYS[next - 1];
  const auto& b = WOBBLE_KEYS[next];
  const int t = popupSmooth((age - prev.at) * POPUP_PROGRESS / (b.at - prev.at));
  f.across = popupMix(next == 1 ? from.across : g.across * prev.across / 100, g.across * b.across / 100, t);
  f.along = popupMix(next == 1 ? from.along : g.along * prev.along / 100, g.along * b.along / 100, t);
  f.shift = popupAnchorShift(g, f.across);
  if (next == 1) {
    f.shift += popupMix(from.shift - popupAnchorShift(g, from.across), 0, t);
    f.opacity = popupMix(from.opacity, 255, t);
  }
  return f;
}

inline PopupFrame popupFrame(const PopupGeometry& g, const PopupAnimation& a, int progress) {
  PopupFrame f = popupRestFrame(g);
  const PopupFrame& from = a.from;
  const int elapsed = progress * a.duration;  // Thousandths of a millisecond.
  switch (a.kind) {
    case PopupMotion::Droplet:
      if (a.exiting) {
        f.content = popupMix(from.content, 0, popupPhase(elapsed, 0, CONTENT_OUT_MS));
        const int shrink = popupEaseIn(popupPhase(elapsed, CONTENT_OUT_MS, DROPLET_SHRINK_MS));
        const int remaining = POPUP_PROGRESS - shrink;
        f.across = popupMix(0, from.across, remaining);
        f.along = popupMix(0, from.along, popupEaseIn(remaining));
      } else {
        const int swellMs = a.duration - CONTENT_IN_MS;
        if (swellMs) {
          constexpr int PEAK_MS = 143, STRETCH_MS = 195;
          const int peak = std::max(from.across, f.across * 112 / 100);
          if (elapsed < PEAK_MS * POPUP_PROGRESS) {
            f.across = popupMix(from.across, peak, popupEaseOut(popupPhase(elapsed, 0, PEAK_MS)));
          } else {
            const int t = popupPhase(elapsed, PEAK_MS, DROPLET_SWELL_MS - PEAK_MS);
            f.across = popupMix(peak, f.across, popupSmooth(t));
          }
          f.along = popupMix(from.along, f.along, popupEaseOut(popupPhase(elapsed, 0, STRETCH_MS)));
        }
        f.content = popupMix(from.content, 255, popupPhase(elapsed, swellMs, CONTENT_IN_MS));
      }
      break;
    case PopupMotion::Drip: return popupDripFrame(g, a, elapsed);
    case PopupMotion::Slide: {
      const int time = a.exiting ? SLIDE_OUT_MS : SLIDE_IN_MS;
      const int slide = popupSlideEase(popupPhase(elapsed, 0, time), a.exiting);
      f.shift = popupMix(from.shift, a.exiting ? g.slide : 0, slide);
      f.opacity = popupMix(from.opacity, a.exiting ? 0 : 255, slide);
      f.content = a.exiting ? 0 : popupMix(from.content, 255, popupPhase(elapsed, time, CONTENT_IN_MS));
      break;
    }
    case PopupMotion::Wobble: return popupWobbleFrame(g, a, elapsed);
    case PopupMotion::Fade:
      f.opacity = popupMix(from.opacity, a.exiting ? 0 : 255, popupSlideEase(progress, a.exiting));
      break;
    case PopupMotion::None: break;
  }
  return f;
}

}  // namespace deck
