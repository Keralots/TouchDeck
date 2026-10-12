// Text and full scale of a PC metric for the deck's metric tiles.
#ifndef METRIC_FORMAT_H
#define METRIC_FORMAT_H

#include "pc_metrics.h"

struct MetricText {
  char value[12];
  char unit[8];
};

// "3.5 GHz", "1.2k RPM", "24 GB": compact forms of big base-unit values.
void formatMetricText(const PcMetric& metric, float raw, MetricText& out);

// Full scale for bars and the warning colour: scaleMax when set, else by unit
// (percent 100, temperature / power from the display settings, else 3000).
float metricScaleMax(const PcMetric& metric, uint16_t scaleMax);

#endif  // METRIC_FORMAT_H
