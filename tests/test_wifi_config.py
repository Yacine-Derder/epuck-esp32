import unittest
from pathlib import Path


class WifiConfigTests(unittest.TestCase):
    source = (Path(__file__).resolve().parents[1] / "main" / "main.c").read_text()

    def test_station_policy(self):
        assignments = {
            "scan_method": "WIFI_ALL_CHANNEL_SCAN",
            "sort_method": "WIFI_CONNECT_AP_BY_SIGNAL",
            "bssid_set": "false",
            "channel": "0",
            "threshold.authmode": "WIFI_AUTH_WPA2_PSK",
        }
        for field, value in assignments.items():
            with self.subTest(field=field):
                self.assertIn(f".{field} = {value},", self.source)
        self.assertIn("esp_wifi_set_ps(WIFI_PS_NONE)", self.source)

    def test_link_events_retry_and_gate_transport_on_ip(self):
        handler = self.source.split("static void wifi_event_handler", 1)[1].split(
            "static void wifi_init_sta", 1
        )[0]
        start, connected = handler.split("WIFI_EVENT_STA_CONNECTED", 1)
        connected, disconnected = connected.split("WIFI_EVENT_STA_DISCONNECTED", 1)
        disconnected, got_ip = disconnected.split("IP_EVENT_STA_GOT_IP", 1)
        self.assertIn("esp_wifi_connect();", start)
        self.assertIn("esp_wifi_sta_get_ap_info", connected)
        self.assertNotIn("xEventGroupSetBits", connected)
        loss = disconnected.index("bridge_transport_wifi_lost(reason)")
        clear = disconnected.index("xEventGroupClearBits")
        reconnect = disconnected.index("esp_wifi_connect();")
        self.assertLess(loss, clear)
        self.assertLess(clear, reconnect)
        self.assertIn("xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT)", got_ip)


if __name__ == "__main__":
    unittest.main()
