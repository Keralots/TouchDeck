import unittest

from config_model import clean_stream_label, metric_id_for, normalize_config, sensor_key


def metric(key, metric_id, label=""):
    return {
        "id": metric_id,
        "name": key.upper(),
        "display_name": key,
        "source": "test",
        "wmi_identifier": key,
        "unit": "%",
        "custom_label": label,
    }


class ConfigMigration(unittest.TestCase):
    def test_existing_ids_survive_reordering(self):
        config = normalize_config({"metrics": [metric("cpu", 1), metric("gpu", 7)]})
        config["metrics"].reverse()
        migrated = normalize_config(config)
        self.assertEqual([7, 1], [m["id"] for m in migrated["metrics"]])

    def test_deselected_sensor_keeps_its_id(self):
        config = normalize_config({"metrics": [metric("cpu", 1), metric("gpu", 2)]})
        config["metrics"] = [config["metrics"][0]]
        config = normalize_config(config)
        self.assertEqual(2, metric_id_for(config, "gpu"))

    def test_duplicate_and_invalid_ids_are_repaired(self):
        config = normalize_config({"metrics": [metric("cpu", 4), metric("gpu", 4), metric("ram", 999)]})
        ids = [m["id"] for m in config["metrics"]]
        self.assertEqual(3, len(set(ids)))
        self.assertTrue(all(1 <= value <= 255 for value in ids))

    def test_old_oled_layout_is_removed(self):
        config = normalize_config({"metrics": [], "layout": {"row_mode": 0}})
        self.assertNotIn("layout", config)

    def test_stream_labels_are_ascii_and_firmware_sized(self):
        self.assertEqual("CPU package 123", clean_stream_label("CPU package 123456"))
        self.assertEqual("GPU", clean_stream_label("GPU\u2603"))

    def test_sensor_key_matches_existing_identity_rule(self):
        self.assertEqual("/cpu/0/load/0", sensor_key({"wmi_identifier": "/cpu/0/load/0"}))
        self.assertEqual("psutil_CPU", sensor_key({"source": "psutil", "display_name": "CPU"}))


if __name__ == "__main__":
    unittest.main()
