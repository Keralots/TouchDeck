"""Tests for leaving WMI once LHM's REST server comes up after startup."""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "companion-common"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import touchdeck_companion as core

LHM_CONFIG = {"sensor_source": "auto",
              "metrics": [{"id": 3, "source": "wmi", "wmi_identifier": "/lpc/nct6687d/0/fan/1"}]}


class FakeState:
    def __init__(self):
        self.text = None

    def set_source_text(self, text):
        self.text = text


class SourceRecovery(unittest.TestCase):
    def setUp(self):
        self.patches = [
            mock.patch.object(core, "use_rest_api", False),
            mock.patch.object(core, "_last_rest_reprobe", -1e9),
            mock.patch.object(core, "ensure_discovered"),
            mock.patch.object(core, "check_rest_api_connectivity", return_value=(True, 500, None)),
        ]
        for p in self.patches:
            p.start()
        self.state = FakeState()

    def tearDown(self):
        for p in reversed(self.patches):
            p.stop()

    def test_empty_wmi_snapshot_rescans_when_rest_answers(self):
        core.recover_source(self.state, LHM_CONFIG, core.Snapshot({}, is_rest=False))
        core.ensure_discovered.assert_called_once_with(rescan=True)
        self.assertIsNotNone(self.state.text)

    def test_rest_still_down_keeps_wmi(self):
        core.check_rest_api_connectivity.return_value = (False, 0, "Connection failed")
        core.recover_source(self.state, LHM_CONFIG, None)
        core.ensure_discovered.assert_not_called()

    def test_probe_is_rate_limited(self):
        core.check_rest_api_connectivity.return_value = (False, 0, "Connection failed")
        core.recover_source(self.state, LHM_CONFIG, None)
        core.recover_source(self.state, LHM_CONFIG, None)
        self.assertEqual(core.check_rest_api_connectivity.call_count, 1)

    def test_wmi_with_readings_is_left_alone(self):
        core.recover_source(self.state, LHM_CONFIG, core.Snapshot({"/lpc/nct6687d/0/fan/1": 2400.0}, is_rest=False))
        core.check_rest_api_connectivity.assert_not_called()

    def test_pinned_wmi_is_left_alone(self):
        core.recover_source(self.state, dict(LHM_CONFIG, sensor_source="wmi"), None)
        core.check_rest_api_connectivity.assert_not_called()

    def test_no_lhm_metrics_skips_the_probe(self):
        core.recover_source(self.state, {"sensor_source": "auto", "metrics": [{"id": 1, "source": "psutil"}]}, None)
        core.check_rest_api_connectivity.assert_not_called()

    def test_already_on_rest_is_left_alone(self):
        with mock.patch.object(core, "use_rest_api", True):
            core.recover_source(self.state, LHM_CONFIG, None)
        core.check_rest_api_connectivity.assert_not_called()


if __name__ == "__main__":
    unittest.main()
