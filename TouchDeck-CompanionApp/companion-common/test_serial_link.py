"""USB transport tests with fake ports only."""

from collections import deque
import json
import threading
import time
from types import SimpleNamespace
from unittest import mock

import pytest

from app_state import AppState
from config_model import normalize_config
import pc_control
import serial_link
import server


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now


class FakeSerial:
    def __init__(self, chunks=(), clock=None):
        self.chunks = deque(chunks)
        self.clock = clock
        self.port = None
        self.dtr = True
        self.rts = True
        self.is_open = False
        self.writes = []
        self.open_levels = None
        self.write_error = False
        self.read_error = False
        self.read_failures = 0
        self.short_write = False

    def open(self):
        self.open_levels = (self.dtr, self.rts)
        self.is_open = True

    def close(self):
        self.is_open = False

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, size):
        if self.read_error or not self.is_open:
            self.read_failures += 1
            raise serial_link.serial.SerialException("Disconnected")
        if self.clock:
            self.clock.now += 0.2
        if not self.chunks:
            return b""
        chunk = self.chunks.popleft()
        if len(chunk) > size:
            self.chunks.appendleft(chunk[size:])
        return chunk[:size]

    def write(self, data):
        if self.write_error:
            raise serial_link.serial.SerialException("Disconnected")
        self.writes.append(data)
        return len(data) - int(self.short_write)


@pytest.fixture
def rig(monkeypatch):
    clock = Clock()
    state = AppState(normalize_config({"pc_id": "0123456789ab", "pc_key": "11" * 32}))
    port = FakeSerial(clock=clock)
    factory = mock.Mock(return_value=port)
    monkeypatch.setattr(serial_link.serial, "Serial", factory)
    monkeypatch.setattr(serial_link.list_ports, "comports", mock.Mock(return_value=[]))
    monkeypatch.setattr(serial_link, "time", clock)
    link = serial_link.SerialLink(state)
    yield SimpleNamespace(link=link, port=port, factory=factory, state=state, clock=clock)
    link.stop()


def connect(rig):
    rig.port.chunks.append(b'@TD {"hello":"TouchDeck","fw":"1.2.3"}\n')
    assert rig.link._connect("COM18", "auto")


def command(state, cmd="media_play_pause"):
    cfg = state.get_config()
    nonce = pc_control.NONCES.issue()
    return {"pc": cfg["pc_id"], "cmd": cmd, "nonce": nonce,
            "sig": pc_control.sign(cfg["pc_key"], cfg["pc_id"], nonce, cmd)}


def test_framing_ignores_logs_invalid_json_and_partial_lines(rig):
    rig.port.open()
    rig.port.chunks.extend([
        b'boot log\n @TD {"cmd":"wrong"}\n@TD broken\n@TD []\n@TD {"msg":"caf\xc3',
        b'\xa9"}', b'\n@TD {"next":1}\r\n@TD {"bad":"\xff"}\n',
    ])
    assert rig.link._read_packet(rig.port) is None
    assert rig.link._read_packet(rig.port) is None
    assert rig.link._read_packet(rig.port) == {"msg": "caf\u00e9"}
    assert rig.link._read_packet(rig.port) == {"next": 1}
    assert rig.link._read_packet(rig.port) is None


def test_unterminated_logs_are_bounded_and_discarded(rig):
    rig.port.open()
    rig.port.chunks.extend([b"x" * 4096] * 17)
    for _ in range(17):
        assert rig.link._read_packet(rig.port) is None
    rig.port.chunks.append(b'@TD {"bad":1}\n@TD {"good":1}\n')
    assert rig.link._read_packet(rig.port) == {"good": 1}


def test_handshake_accepts_only_touchdeck_and_sets_lines_before_open(rig):
    rig.port.chunks.extend([b'log\n@TD {"hello":"Other"}\n@TD {"hel',
                            b'lo":"TouchDeck","fw":"1.2.3"}\n@TD {"cmd":"next"}\n'])
    assert rig.link._connect("COM18", "auto")
    assert rig.port.open_levels == (False, False)
    rig.factory.assert_called_once_with(baudrate=115200, timeout=0.2, write_timeout=0.2,
                                       dsrdtr=False, rtscts=False)
    assert rig.port.writes == [b'@TD {"hello":1}\n']
    assert rig.link.is_connected()
    assert rig.link.port_name() == "COM18"
    assert rig.link._read_packet(rig.port) == {"cmd": "next"}


@pytest.mark.parametrize("reply", [b"", b'@TD {"hello":"Other"}\n',
                                   b'{"hello":"TouchDeck"}\n', b'@TD {"hello":1}\n'])
def test_handshake_rejects_and_remembers_port_for_60_seconds(rig, reply):
    rig.port.chunks.append(reply)
    assert not rig.link._connect("COM18", "auto")
    assert not rig.port.is_open
    assert not rig.link.is_connected()
    assert rig.link.port_name() == ""
    assert rig.clock.now >= 2.0
    assert rig.link._rejected["COM18"] == rig.clock.now + 60.0


def test_auto_scan_never_opens_the_unrelated_esp_board(rig, monkeypatch):
    ports = [SimpleNamespace(device="COM11", vid=0x303A, pid=0x8167),
             SimpleNamespace(device="COM18", vid=0x303A, pid=0x1001),
             SimpleNamespace(device="COM2", vid=None, pid=None)]
    enumerate_ports = mock.Mock(return_value=ports)
    monkeypatch.setattr(serial_link.list_ports, "comports", enumerate_ports)
    assert rig.link._candidates("auto") == ["COM18"]
    enumerate_ports.assert_called_once_with()
    enumerate_ports.reset_mock()
    assert rig.link._candidates("/dev/ttyACM0") == ["/dev/ttyACM0"]
    assert rig.link._candidates("off") == []
    enumerate_ports.assert_not_called()


def test_send_uses_compact_json_and_preserves_the_payload(rig):
    assert not rig.link.send({"value": 1})
    connect(rig)
    payload = {"metrics": [{"id": 1, "v": 42}], "pc_id": "0123456789ab",
               "pc_name": "PC", "cmd_port": 4211, "nonces": ["abc"]}
    assert rig.link.send(payload)
    assert rig.port.writes[-1] == (
        b'@TD {"metrics":[{"id":1,"v":42}],"pc_id":"0123456789ab",'
        b'"pc_name":"PC","cmd_port":4211,"nonces":["abc"]}\n')


def test_concurrent_senders_write_whole_lines(rig, monkeypatch):
    connect(rig)
    barrier = threading.Barrier(4)
    active = []
    overlaps = []
    original_write = rig.port.write

    def write(data):
        active.append(data)
        if len(active) > 1:
            overlaps.append(True)
        threading.Event().wait(0.01)
        result = original_write(data)
        active.remove(data)
        return result

    def send(index):
        barrier.wait(timeout=2.0)
        rig.link.send({"value": index})

    monkeypatch.setattr(rig.port, "write", write)
    threads = [threading.Thread(target=send, args=(i,)) for i in range(3)]
    for thread in threads:
        thread.start()
    barrier.wait(timeout=2.0)
    for thread in threads:
        thread.join(timeout=2.0)
        assert not thread.is_alive()
    assert not overlaps
    assert sorted(json.loads(line[4:])["value"] for line in rig.port.writes[1:]) == [0, 1, 2]


def test_signed_command_is_executed_and_acknowledged_once(rig, monkeypatch):
    connect(rig)
    packet = command(rig.state)
    rig.port.chunks.append(serial_link._frame(packet))
    run = mock.Mock()
    monkeypatch.setattr(pc_control, "run_command", run)
    rig.link._dispatch(rig.link._read_packet(rig.port))
    run.assert_called_once_with("media_play_pause")
    assert rig.port.writes[-1] == serial_link._frame(
        {"ack": packet["nonce"], "ok": True, "msg": ""})
    rig.link._dispatch(packet)
    run.assert_called_once()
    assert json.loads(rig.port.writes[-1][4:]) == {
        "ack": packet["nonce"], "ok": False, "msg": "expired or reused"}


def test_bad_signature_and_execution_failure_are_acknowledged(rig, monkeypatch):
    connect(rig)
    packet = command(rig.state)
    run = mock.Mock(side_effect=OSError("Command failed"))
    monkeypatch.setattr(pc_control, "run_command", run)
    rig.link._dispatch(dict(packet, sig="bad"))
    run.assert_not_called()
    assert json.loads(rig.port.writes[-1][4:])["msg"] == "bad signature"
    rig.link._dispatch(packet)
    assert json.loads(rig.port.writes[-1][4:]) == {
        "ack": packet["nonce"], "ok": False, "msg": "Command failed"}


@pytest.mark.parametrize("failing", ["verify", "execute"])
def test_dispatch_survives_unexpected_command_exceptions(rig, monkeypatch, capsys, failing):
    connect(rig)
    verify = mock.Mock(return_value=(True, ""))
    execute = mock.Mock(return_value=(True, ""))
    (verify if failing == "verify" else execute).side_effect = [RuntimeError("packet failed"), (True, "")]
    monkeypatch.setattr(pc_control, "verify", verify)
    monkeypatch.setattr(pc_control, "execute", execute)
    rig.link._dispatch(command(rig.state))
    assert not json.loads(rig.port.writes[-1][4:])["ok"]
    assert "packet failed" in capsys.readouterr().out
    rig.link._dispatch(command(rig.state))
    assert json.loads(rig.port.writes[-1][4:])["ok"]
    assert rig.link.is_connected()


@pytest.mark.parametrize("short_write", [False, True])
def test_write_failure_disconnects(rig, short_write):
    connect(rig)
    rig.port.short_write = short_write
    rig.port.write_error = not short_write
    assert not rig.link.send({"value": 1})
    assert not rig.link.is_connected()
    assert not rig.port.is_open


def test_scan_interval_and_rejected_port_cooldown(rig, monkeypatch):
    attempts = []
    scans = []

    def candidates(setting):
        scans.append(rig.clock.now)
        return ["COM18"]

    def connect_port(name, setting):
        attempts.append(rig.clock.now)
        rig.link._rejected[name] = rig.clock.now + 60.0
        return False

    def wait(_):
        rig.clock.now += 1.0
        if rig.clock.now > 61.0:
            rig.link._stop.set()

    monkeypatch.setattr(rig.link, "_candidates", candidates)
    monkeypatch.setattr(rig.link, "_connect", connect_port)
    monkeypatch.setattr(rig.link._stop, "wait", wait)
    rig.link._run()
    assert scans == list(range(0, 61, 5))
    assert attempts == [0.0, 60.0]


def test_read_failure_reconnects_and_setting_off_closes_port(rig, monkeypatch):
    rig.port.chunks.append(b'@TD {"hello":"TouchDeck"}\n')
    second = FakeSerial([b'@TD {"hello":"TouchDeck"}\n'], rig.clock)
    rig.factory.side_effect = [rig.port, second]
    scans = mock.Mock(return_value=["COM18"])
    monkeypatch.setattr(rig.link, "_candidates", scans)

    def wait(_):
        rig.clock.now += 1.0
        if rig.port.is_open:
            rig.port.read_error = True
        elif second.is_open:
            rig.state.set_config(dict(rig.state.get_config(), serial_port="off"))
        elif rig.state.get_config()["serial_port"] == "off":
            rig.link._stop.set()

    monkeypatch.setattr(rig.link._stop, "wait", wait)
    rig.link._run()
    assert rig.port.read_failures == 1
    assert second.writes == [b'@TD {"hello":1}\n']
    assert not rig.port.is_open
    assert not second.is_open
    assert not rig.link.is_connected()
    assert scans.call_args_list == [mock.call("auto"), mock.call("auto")]


def test_daemon_obeys_shared_stop_and_exposes_status(rig, monkeypatch):
    rig.port.chunks.append(b'@TD {"hello":"TouchDeck"}\n')
    monkeypatch.setattr(serial_link.list_ports, "comports", mock.Mock(return_value=[
        SimpleNamespace(device="COM18", vid=0x303A, pid=0x1001)]))
    monkeypatch.setattr(serial_link, "_link", None)
    assert serial_link.status() == {"connected": False, "port": ""}
    rig.link.start()
    assert rig.link._thread.daemon
    deadline = time.monotonic() + 2.0
    while not rig.link.is_connected() and time.monotonic() < deadline:
        threading.Event().wait(0.01)
    assert serial_link.status() == {"connected": True, "port": "COM18"}
    rig.link._app_stop.set()
    rig.link._thread.join(timeout=1.0)
    assert not rig.link._thread.is_alive()
    assert not rig.port.is_open
    assert serial_link.status() == {"connected": False, "port": ""}


@pytest.mark.parametrize("value, expected", [(None, "auto"), ("", "auto"), ("   ", "auto"),
    (" auto ", "auto"), (" off ", "off"), (" COM18 ", "COM18"),
    (" /dev/ttyACM0 ", "/dev/ttyACM0"), ("x" * 80, "x" * 64)])
def test_serial_config_normalization(value, expected):
    assert normalize_config({"serial_port": value})["serial_port"] == expected
    assert normalize_config({})["serial_port"] == "auto"


def test_connection_api_saves_serial_setting_without_an_ip(rig):
    core = SimpleNamespace(save_config=mock.Mock(return_value=True))
    result = server.apply_connection(core, rig.state, {"serial_port": [" COM18 "]})
    assert result["success"]
    assert core.save_config.call_args.args[0]["serial_port"] == "COM18"
    assert rig.state.get_config()["esp32_ip"] == ""
    assert server.apply_connection(core, rig.state, {"serial_port": ["off"]})["success"]
    assert rig.state.get_config()["serial_port"] == "off"


def test_connection_api_preserves_serial_setting_when_field_is_omitted(rig):
    rig.state.set_config(dict(rig.state.get_config(), serial_port="/dev/ttyACM0"))
    core = SimpleNamespace(save_config=mock.Mock(return_value=True))
    result = server.apply_connection(core, rig.state, {"esp32_ip": ["touchdeck.local"]})
    assert result["success"]
    assert rig.state.get_config()["serial_port"] == "/dev/ttyACM0"


def test_info_api_reports_setting_and_live_status(rig, monkeypatch):
    handler = object.__new__(server._Handler)
    handler.CORE = SimpleNamespace()
    handler.STATE = rig.state
    handler.path = "/api/info"
    handler._json = mock.Mock()
    monkeypatch.setattr(serial_link, "_link", rig.link)
    connect(rig)
    handler.do_GET()
    info = handler._json.call_args.args[0]
    assert info["serial_port"] == "auto"
    assert info["serial_link"] == {"connected": True, "port": "COM18"}


@pytest.mark.parametrize("connected, address", [(True, ""), (True, "touchdeck.local"),
                                                (False, "touchdeck.local")])
def test_monitor_prefers_usb_and_preserves_udp_payload(rig, monkeypatch, connected, address):
    import app_window

    cfg = dict(rig.state.get_config(), esp32_ip=address, metrics=[{"id": 1}])
    rig.state.set_config(cfg)
    payload = {"metrics": [{"id": 1, "v": 42}]}
    core = SimpleNamespace(PYTHONCOM_AVAILABLE=False, use_rest_api=False, STATUS_OK=0,
        psutil=SimpleNamespace(cpu_percent=mock.Mock()), build_snapshot=mock.Mock(return_value={}),
        collect_metrics=mock.Mock(return_value=(payload, {1: 42}, None, {}, None)))
    stop = threading.Event()
    monkeypatch.setattr(stop, "wait", lambda _: stop.set())
    link = mock.Mock()
    link.is_connected.return_value = connected
    sock = mock.Mock()
    monkeypatch.setattr(app_window, "_core", core)
    monkeypatch.setattr(app_window, "_state", rig.state)
    monkeypatch.setattr(app_window, "_stop", stop)
    monkeypatch.setattr(app_window, "_serial_link", link)
    monkeypatch.setattr(app_window.socket, "socket", mock.Mock(return_value=sock))
    monkeypatch.setattr(app_window.socket, "create_connection", mock.MagicMock())
    app_window._monitor_loop()
    if connected:
        link.send.assert_called_once_with(payload)
        sock.sendto.assert_not_called()
    else:
        link.send.assert_not_called()
        data, destination = sock.sendto.call_args.args
        assert json.loads(data) == payload
        assert destination == (address, 4210)
    assert payload["pc_id"] == cfg["pc_id"]
    assert payload["pc_name"] == cfg["pc_name"]
    assert payload["cmd_port"] == cfg["cmd_port"]
    assert len(payload["nonces"]) == 4
    for nonce in payload["nonces"]:
        packet = {"pc": cfg["pc_id"], "cmd": "media_next", "nonce": nonce,
                  "sig": pc_control.sign(cfg["pc_key"], cfg["pc_id"], nonce, "media_next")}
        assert pc_control.verify(cfg, packet) == (True, "")
