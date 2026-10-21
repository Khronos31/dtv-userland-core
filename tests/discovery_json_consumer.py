#!/usr/bin/env python3
import json
import subprocess
import sys


result = subprocess.run([sys.argv[1], "--json"], check=True, capture_output=True, text=True)
document = json.loads(result.stdout)

assert document["schema"] == {"name": "userland.discovery", "major": 1, "minor": 0}
assert document["producer"] == {"product": "px4", "version": "1.0.0"}
assert document["scan"] == {"complete": True, "issues": []}
assert isinstance(document["enclosures"][0]["enclosure"], int)
assert document["enclosures"][0]["serial"] == "serial-a"
assert document["enclosures"][0]["serial_unique"] is True
assert document["enclosures"][0]["status"] == "ready"
assert document["enclosures"][1]["status"] == "incomplete"
device = document["enclosures"][0]["devices"][0]
assert device["device"] == 2 and device["usb"] == "0511:084a"
assert device["serial"] == "device-a"
assert device["bus"] == 1 and device["address"] == 6 and device["port"] == "1-2.3"
receiver = device["receivers"][0]
assert receiver == {"receiver": 3, "device": 2, "local": 7, "systems": ["ISDB-T", "ISDB-S"]}
assert "receivers" not in document["enclosures"][1]["devices"][0]
assert document["ungrouped_usb_devices"][0] == {
    "serial": None,
    "model": "PX-Q3U4",
    "usb": "0511:084a",
    "status": "open_failed",
    "bus": None,
    "address": 9,
    "port": None,
}
