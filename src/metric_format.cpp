#include "metric_format.h"
#include "settings.h"
#include <math.h>

namespace {

// A decimal only where it carries information: "3.5 GHz" yes, "24.0 GB" no.
void printScaled(char* buf, size_t len, float v) {
  snprintf(buf, len, (fabsf(v) < 10.0f) ? "%.1f" : "%.0f", v);
}

// Byte rates climb the same 1024 ladder as storage units (the companions send
// KB/s on Windows and MB/s on Linux).
const char* const RATE_UNITS[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
constexpr int8_t RATE_UNIT_COUNT = 5;

int8_t rateUnitIndex(const char* unit) {
  for (int8_t i = 0; i < RATE_UNIT_COUNT; i++)
    if (strcmp(unit, RATE_UNITS[i]) == 0) return i;
  return -1;
}

}  // namespace

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
    while (fabsf(v) >= 1024.0f && i < RATE_UNIT_COUNT - 1) {
      v /= 1024.0f;
      i++;
    }
    strlcpy(out.unit, RATE_UNITS[i], sizeof(out.unit));
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
    // Rails keep their decimal: 12.2 V and 12 V are different readings.
    snprintf(out.value, sizeof(out.value), "%.1f", raw);
  } else {
    snprintf(out.value, sizeof(out.value), "%.0f", raw);
  }
}

float metricScaleMax(const PcMetric& metric, uint16_t scaleMax) {
  switch (metric.unit[0]) {
    case '%': return 100.0f;
    case 'C': return scaleMax ? (float)scaleMax : (float)dispSettings.tempScaleMax;
    case 'W': return scaleMax ? (float)scaleMax : (float)dispSettings.powerScaleW;
    default:  return scaleMax ? (float)scaleMax : (float)GAUGE_FAN_SCALE_DEFAULT;
  }
}
