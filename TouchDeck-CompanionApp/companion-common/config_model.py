"""TouchDeck companion configuration normalization and metric identity."""

import copy

from constants import MAX_METRICS


CONFIG_VERSION = "4.1"
MAX_METRIC_ID = 255
MAX_STREAM_LABEL = 15


def sensor_key(sensor):
    """Return the persistent identity used across rescans and selection changes."""
    return (sensor.get("wmi_identifier") or
            "%s_%s" % (sensor.get("source", ""), sensor.get("display_name", "")))


def clean_stream_label(value):
    """Keep labels safe for the firmware's fixed 16-byte metric-name buffer."""
    text = "" if value is None else str(value)
    return "".join(ch for ch in text if 32 <= ord(ch) <= 126)[:MAX_STREAM_LABEL]


def _valid_metric_id(value):
    try:
        metric_id = int(value)
    except (TypeError, ValueError):
        return None
    return metric_id if 1 <= metric_id <= MAX_METRIC_ID else None


def _next_metric_id(used):
    for metric_id in range(1, MAX_METRIC_ID + 1):
        if metric_id not in used:
            return metric_id
    raise ValueError("No free metric IDs remain.")


def normalize_config(config):
    """Migrate a copied SmallOLED/v4 config into the TouchDeck schema.

    Existing valid metric IDs win so installed layouts keep their bindings.
    A persistent sensor-key map then remembers IDs while sensors are deselected,
    reordered, or temporarily missing during discovery.
    """
    out = copy.deepcopy(config or {})
    out["version"] = CONFIG_VERSION
    out.setdefault("esp32_ip", "")
    out.setdefault("udp_port", 4210)
    out.setdefault("update_interval", 3)
    out.setdefault("metrics", [])

    old_map = out.get("metric_ids")
    if not isinstance(old_map, dict):
        old_map = {}

    metrics = [m for m in out.get("metrics", []) if isinstance(m, dict)][:MAX_METRICS]
    used = set()
    id_map = {}

    for metric in metrics:
        key = sensor_key(metric)
        metric_id = _valid_metric_id(metric.get("id"))
        if metric_id in used:
            metric_id = None
        if metric_id is None:
            mapped = _valid_metric_id(old_map.get(key))
            metric_id = mapped if mapped not in used else None
        if metric_id is None:
            metric_id = _next_metric_id(used)
        used.add(metric_id)
        metric["id"] = metric_id
        metric["custom_label"] = clean_stream_label(metric.get("custom_label", ""))
        if key:
            id_map[key] = metric_id

    # Keep remembered IDs for deselected sensors, provided they do not conflict
    # with currently selected metrics or an earlier remembered identity.
    remembered_used = set(used)
    for key, value in old_map.items():
        key = str(key)
        if not key or key in id_map:
            continue
        metric_id = _valid_metric_id(value)
        if metric_id is None or metric_id in remembered_used:
            continue
        id_map[key] = metric_id
        remembered_used.add(metric_id)

    out["metrics"] = metrics
    out["metric_ids"] = id_map
    # OLED layout state is deliberately not part of the color companion. The
    # TouchDeck device portal owns screen slots, aliases, styles, and theme.
    out.pop("layout", None)
    return out


def metric_id_for(config, key):
    """Return or allocate the persistent ID for a sensor identity."""
    id_map = config.setdefault("metric_ids", {})
    existing = _valid_metric_id(id_map.get(key))
    if existing is not None:
        return existing
    used = {_valid_metric_id(value) for value in id_map.values()}
    used.discard(None)
    metric_id = _next_metric_id(used)
    id_map[key] = metric_id
    return metric_id
