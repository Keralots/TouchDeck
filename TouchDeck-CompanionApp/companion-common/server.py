"""Local HTTP server for the TouchDeck desktop companion."""

import copy
import json
import os
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error as urllib_error
from urllib import request as urllib_request
from urllib.parse import parse_qs, urlparse

from config_model import (
    CONFIG_VERSION,
    MAX_STREAM_LABEL,
    clean_pc_name,
    clean_stream_label,
    metric_id_for,
    normalize_config,
    sensor_key,
)
from constants import MAX_METRICS


_CTYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
}
_DIRECT_HTTP = urllib_request.build_opener(urllib_request.ProxyHandler({}))


def webui_dir():
    """Return bundled assets in script and PyInstaller modes."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, "webui")


def ensure_discovered(core, rescan=False):
    core.ensure_discovered(rescan)


def source_text(core):
    return core.source_text()


def source_banner(core):
    return core.source_banner()


def find_sensor_by_key(core, key):
    for category in core.sensor_database:
        for sensor in core.sensor_database[category]:
            if sensor_key(sensor) == key:
                return sensor
    return None


def flatten_sensors(core, config):
    """Return discovered sensors with selection, persistent ID, and alias data."""
    selected = {sensor_key(metric): metric for metric in config.get("metrics", [])}
    out = []
    order = ["system", "gpu", "temperature", "fan", "load", "clock",
             "power", "data", "throughput", "other"]
    for category in order:
        for sensor in core.sensor_database.get(category, []):
            key = sensor_key(sensor)
            metric = selected.get(key)
            out.append({
                "key": key,
                "name": sensor.get("name", ""),
                "display_name": sensor.get("display_name", sensor.get("name", "")),
                "sensor_name": sensor.get("wmi_sensor_name", ""),
                "hardware": sensor.get("hardware", ""),
                "hardware_detail": sensor.get("hardware_detail", ""),
                "nic_state": sensor.get("nic_state", ""),
                "unit": sensor.get("unit", ""),
                "type": sensor.get("type", ""),
                "source": sensor.get("source", ""),
                "category": category,
                "current_value": sensor.get("current_value", 0),
                "active": bool(sensor.get("is_active_nic")),
                "selected": metric is not None,
                "metric_id": metric.get("id") if metric else None,
                "custom_label": metric.get("custom_label", "") if metric else "",
            })
    return out


def live_metrics(state):
    config = state.get_config()
    values = state.get_values()
    out = []
    for metric in config.get("metrics", []):
        metric_id = metric.get("id")
        out.append({
            "id": metric_id,
            "key": sensor_key(metric),
            "name": metric.get("name", ""),
            "label": metric.get("custom_label", ""),
            "unit": metric.get("unit", ""),
            "value": values.get(metric_id, metric.get("current_value")),
        })
    return out


def _device_address(value):
    value = (value or "").strip()
    if not value:
        raise ValueError("Enter the device address first.")
    if value.startswith("http://") or value.startswith("https://"):
        parsed = urlparse(value)
        value = parsed.hostname or ""
    value = value.strip().rstrip("/")
    if not value or "/" in value or ":" in value or any(ch.isspace() for ch in value):
        raise ValueError("Enter an IP address or hostname without a port or path.")
    return value


def _device_url(address, path):
    return "http://%s%s" % (_device_address(address), path)


def _fetch_device(address, path, timeout=2.0):
    req = urllib_request.Request(
        _device_url(address, path),
        headers={"User-Agent": "TouchDeck-Companion/%s" % CONFIG_VERSION},
    )
    with _DIRECT_HTTP.open(req, timeout=timeout) as response:
        return response.read(), response.headers.get("Content-Type", "application/octet-stream")


def _fetch_device_json(address, path="/api/status", timeout=2.0):
    body, _content_type = _fetch_device(address, path, timeout)
    return json.loads(body.decode("utf-8"))


def device_status(state, address=None, timeout=2.0):
    config = state.get_config()
    address = address if address is not None else config.get("esp32_ip", "")
    try:
        address = _device_address(address)
        data = _fetch_device_json(address, timeout=timeout)
        product = data.get("product", "")
        compatible = product == "TouchDeck"
        state.set_reachable(True)
        result = dict(data)
        result.update({
            "reachable": True,
            "compatible": compatible,
            "address": address,
            "message": ("TouchDeck is online." if compatible else
                        "A web device responded, but it reports product '%s'." % (product or "unknown")),
        })
        return result
    except (OSError, ValueError, json.JSONDecodeError, urllib_error.URLError) as exc:
        state.set_reachable(False)
        return {
            "reachable": False,
            "compatible": False,
            "address": address or "",
            "message": "Could not read TouchDeck status: %s" % exc,
        }


def apply_select(core, state, keys):
    """Update the in-memory selection without changing existing metric IDs."""
    ensure_discovered(core)
    config = normalize_config(state.get_config())
    previous = {sensor_key(metric): metric for metric in config.get("metrics", [])}
    metrics = []
    seen = set()
    for key in (keys or []):
        key = str(key)
        if key in seen or len(metrics) >= MAX_METRICS:
            continue
        sensor = find_sensor_by_key(core, key)
        if not sensor:
            continue
        seen.add(key)
        metric = copy.deepcopy(sensor)
        metric["id"] = metric_id_for(config, key)
        old = previous.get(key, {})
        metric["custom_label"] = clean_stream_label(old.get("custom_label", ""))
        metrics.append(metric)
    config["metrics"] = metrics
    state.set_config(normalize_config(config))
    return {"success": True, "count": len(metrics)}


def apply_labels(state, labels):
    config = normalize_config(state.get_config())
    labels = labels if isinstance(labels, dict) else {}
    for metric in config.get("metrics", []):
        key = sensor_key(metric)
        if key in labels:
            metric["custom_label"] = clean_stream_label(labels[key])
    state.set_config(normalize_config(config))
    return {"success": True, "maxLength": MAX_STREAM_LABEL}


def apply_save(core, state):
    config = normalize_config(state.get_config())
    if not core.save_config(config):
        return {"success": False, "message": "Could not write the configuration file."}
    state.set_config(config)
    return {"success": True, "message": "Sensor stream configuration saved."}


def apply_connection(core, state, form):
    def field(name, default=""):
        value = form.get(name)
        return value[0] if isinstance(value, list) and value else default

    supports_source = bool(getattr(core, "SUPPORTS_SOURCE_SELECT", False))
    sources = tuple(getattr(core, "SENSOR_SOURCES", ("auto",)))
    try:
        address = _device_address(field("esp32_ip", ""))
        port = int(field("udp_port", "4210"))
        interval = float(field("update_interval", "3"))
        source = field("sensor_source", "auto").lower()
        pc_name = clean_pc_name(field("pc_name", ""))
        if port < 1 or port > 65535:
            raise ValueError("Port must be between 1 and 65535.")
        if interval < 0.5:
            raise ValueError("Update interval must be at least 0.5 seconds.")
        if supports_source and source not in sources:
            raise ValueError("Unknown sensor source.")
    except ValueError as exc:
        return {"success": False, "message": str(exc)}

    config = normalize_config(state.get_config())
    config["esp32_ip"] = address
    config["udp_port"] = port
    config["update_interval"] = interval
    if pc_name:
        config["pc_name"] = pc_name
    if supports_source:
        config["sensor_source"] = source
    if not core.save_config(config):
        return {"success": False, "message": "Could not write the configuration file."}
    state.set_config(config)

    resolved = None
    if supports_source:
        try:
            resolved = core.resolve_source(source)
            state.set_source_text(core.source_text())
        except Exception as exc:
            print("source switch failed: %s" % exc)
    result = {"success": True, "esp32_ip": address, "udp_port": port,
              "update_interval": interval}
    if resolved:
        result.update({"sensor_source": source, "resolved_source": resolved})
    return result


def do_test(state, form):
    value = form.get("esp32_ip", [""])
    address = value[0] if isinstance(value, list) and value else ""
    return device_status(state, address=address, timeout=2.5)


def _post_device(address, path, params, timeout=4.0):
    """POST to the device's API; returns (status, json body)."""
    from urllib.parse import urlencode
    req = urllib_request.Request(
        _device_url(address, path + "?" + urlencode(params)), data=b"", method="POST",
        headers={"User-Agent": "TouchDeck-Companion/%s" % CONFIG_VERSION},
    )
    try:
        with _DIRECT_HTTP.open(req, timeout=timeout) as response:
            return response.status, json.loads(response.read().decode("utf-8") or "{}")
    except urllib_error.HTTPError as exc:
        try:
            return exc.code, json.loads(exc.read().decode("utf-8") or "{}")
        except Exception:
            return exc.code, {}


def pair_start(state):
    """Ask the device to show a pairing code for this PC."""
    config = state.get_config()
    try:
        code, body = _post_device(config.get("esp32_ip", ""), "/api/pc/pair", {"id": config.get("pc_id", "")})
    except Exception as exc:
        return {"success": False, "message": "Could not reach the device: %s" % exc}
    return {"success": code == 200, "message": body.get("message", "HTTP %d" % code)}


def pair_confirm(core, state, code_text):
    """Send the code shown on the device; store the key it answers with."""
    config = normalize_config(state.get_config())
    code_text = "".join(ch for ch in str(code_text or "") if ch.isdigit())
    if len(code_text) != 6:
        return {"success": False, "message": "Enter the 6 digits shown on the device."}
    try:
        status, body = _post_device(config.get("esp32_ip", ""), "/api/pc/pair/confirm",
                                    {"id": config.get("pc_id", ""), "code": code_text})
    except Exception as exc:
        return {"success": False, "message": "Could not reach the device: %s" % exc}
    key = str(body.get("key") or "")
    if status != 200 or len(key) != 64:
        return {"success": False, "message": body.get("message", "HTTP %d" % status)}
    config["pc_key"] = key
    if not core.save_config(config):
        return {"success": False, "message": "Could not write the configuration file."}
    state.set_config(config)
    return {"success": True, "message": "Paired. Control tiles on the device can now reach this PC."}


def pair_forget(core, state):
    config = normalize_config(state.get_config())
    config["pc_key"] = ""
    if not core.save_config(config):
        return {"success": False, "message": "Could not write the configuration file."}
    state.set_config(config)
    return {"success": True, "message": "Unpaired. Forget this PC in the device portal too."}


def apply_revert(core, state):
    config = normalize_config(core.load_config() or dict(core.DEFAULT_CONFIG))
    state.set_config(config)
    return {"success": True}


def apply_import(core, state, config):
    if not isinstance(config, dict) or not isinstance(config.get("metrics"), list):
        return {"success": False, "message": "This is not a companion configuration."}
    migrated = normalize_config(config)
    if not core.save_config(migrated):
        return {"success": False, "message": "Could not write the imported configuration."}
    state.set_config(migrated)
    return {"success": True, "message": "Configuration imported and migrated to version %s." % CONFIG_VERSION}


class _Handler(BaseHTTPRequestHandler):
    CORE = None
    STATE = None

    def log_message(self, *args):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _asset(self, name):
        path = os.path.join(webui_dir(), name)
        if not os.path.isfile(path):
            self.send_error(404, "not found")
            return
        with open(path, "rb") as handle:
            body = handle.read()
        self.send_response(200)
        self.send_header("Content-Type", _CTYPES.get(os.path.splitext(name)[1].lower(),
                                                     "application/octet-stream"))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        try:
            length = int(self.headers.get("Content-Length", 0))
        except (TypeError, ValueError):
            length = 0
        return self.rfile.read(length) if length > 0 else b""

    def _form(self):
        return parse_qs(self._body().decode("utf-8", "replace"))

    def _json_body(self):
        try:
            return json.loads(self._body().decode("utf-8", "replace"))
        except Exception:
            return None

    def _device_screen(self):
        config = self.STATE.get_config()
        try:
            body, content_type = _fetch_device(config.get("esp32_ip", ""), "/screen.bmp", 4.0)
            self.send_response(200)
            self.send_header("Content-Type", content_type or "image/bmp")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            self._json({"success": False, "message": str(exc)}, 502)

    def do_GET(self):
        core, state = self.CORE, self.STATE
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        try:
            if path in ("/", "/index.html"):
                return self._asset("index.html")
            if path == "/portal.css":
                return self._asset("portal.css")
            if path == "/portal.js":
                return self._asset("portal.js")
            if path == "/api/status":
                result = state.status()
                result["version"] = CONFIG_VERSION
                return self._json(result)
            if path == "/api/device/status":
                return self._json(device_status(state))
            if path == "/api/device/screen":
                return self._device_screen()
            if path == "/api/live":
                return self._json({"metrics": live_metrics(state)})
            if path == "/api/info":
                config = state.get_config()
                return self._json({
                    "version": CONFIG_VERSION,
                    "ip": config.get("esp32_ip", ""),
                    "udp_port": config.get("udp_port", 4210),
                    "update_interval": config.get("update_interval", 3),
                    "pc_name": config.get("pc_name", ""),
                    "paired": bool(config.get("pc_key")),
                    "cmd_port": config.get("cmd_port", 4211),
                    "source_select": bool(getattr(core, "SUPPORTS_SOURCE_SELECT", False)),
                    "sensor_source": config.get("sensor_source", "auto"),
                    "max_metrics": MAX_METRICS,
                })
            if path == "/api/sensors":
                ensure_discovered(core, rescan=("rescan" in query))
                state.set_source_text(source_text(core))
                config = state.get_config()
                return self._json({
                    "sensors": flatten_sensors(core, config),
                    "max": MAX_METRICS,
                    "banner": source_banner(core),
                    "source": source_text(core),
                })
            if path == "/api/export":
                return self._json(normalize_config(state.get_config()))
            if path == "/api/autostart":
                return self._json({"enabled": bool(core.is_autostart_enabled())})
            self.send_error(404, "not found")
        except Exception as exc:
            self._json({"success": False, "message": str(exc)}, 500)

    def do_POST(self):
        core, state = self.CORE, self.STATE
        path = urlparse(self.path).path
        try:
            if path == "/api/save":
                return self._json(apply_save(core, state))
            if path == "/api/select":
                data = self._json_body() or {}
                return self._json(apply_select(core, state, data.get("keys", [])))
            if path == "/api/labels":
                data = self._json_body() or {}
                return self._json(apply_labels(state, data.get("labels", {})))
            if path == "/api/connection":
                return self._json(apply_connection(core, state, self._form()))
            if path == "/api/test":
                return self._json(do_test(state, self._form()))
            if path == "/api/import":
                return self._json(apply_import(core, state, self._json_body()))
            if path == "/api/pair/start":
                return self._json(pair_start(state))
            if path == "/api/pair/confirm":
                return self._json(pair_confirm(core, state, self._form().get("code", [""])[0]))
            if path == "/api/pair/forget":
                return self._json(pair_forget(core, state))
            if path == "/api/revert":
                return self._json(apply_revert(core, state))
            if path == "/api/autostart":
                enabled = self._form().get("enable", ["0"])[0] == "1"
                core.setup_autostart(enabled)
                return self._json({"enabled": bool(core.is_autostart_enabled())})
            self.send_error(404, "not found")
        except Exception as exc:
            self._json({"success": False, "message": str(exc)}, 500)


DEFAULT_UI_PORT = 8750


class _UiServer(ThreadingHTTPServer):
    # Windows SO_REUSEADDR lets a second process bind a port another process is
    # already serving and silently take the connections, so bind() never fails
    # and the fallback below never runs. Two companions then answer on one port
    # and each window can end up driving the other app.
    allow_reuse_address = False


def make_server(core, state, host="127.0.0.1", port=DEFAULT_UI_PORT):
    """Build the local server, using an ephemeral port if the preferred one is busy."""
    _Handler.CORE = core
    _Handler.STATE = state
    try:
        httpd = _UiServer((host, port), _Handler)
    except OSError:
        httpd = _UiServer((host, 0), _Handler)
        port = httpd.server_address[1]
    httpd.daemon_threads = True
    return httpd, port


def serve(httpd):
    import threading
    threading.Thread(target=httpd.serve_forever, daemon=True, name="http-server").start()
