#!/usr/bin/env python3
import json
import subprocess
import sys


result = subprocess.run([sys.argv[1], "--json"], check=True, capture_output=True, text=True)
document = json.loads(result.stdout)

assert document["schema"] == {"name": "userland.discovery", "major": 1, "minor": 0}
assert document["producer"] == {"product": "px4", "version": "1.0.0"}
assert document["scan_complete"] is True
assert isinstance(document["enclosures"][0]["enclosure"], int)
assert document["enclosures"][0]["serial"] == "serial-a"
assert document["enclosures"][0]["serial_unique"] is True
device = document["enclosures"][0]["devices"][0]
assert device["device"] == 2 and device["usb"] == "0511:084a"
receiver = device["receivers"][0]
assert receiver == {"receiver": 3, "device": 2, "local": 7, "systems": ["ISDB-T", "ISDB-S"]}
assert "receivers" not in document["enclosures"][1]["devices"][0]
