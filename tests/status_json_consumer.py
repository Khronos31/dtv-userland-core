#!/usr/bin/env python3
import json
import subprocess
import sys


result = subprocess.run([sys.argv[1], "--json"], check=True, capture_output=True, text=True)
document = json.loads(result.stdout)

assert document["schema"] == {"name": "userland.status", "major": 1, "minor": 0}
assert isinstance(document["snapshot"]["sequence"], str)
assert isinstance(document["scope"]["id"], type(None))
assert isinstance(document["enclosure"]["id"], int)
assert isinstance(document["enclosure"]["lnb_15v_enabled"], bool)
assert isinstance(document["enclosure"]["card_inserted"], bool)
assert document["receivers"][0]["descriptor"]["receiver"] == 0
assert document["receivers"][0]["descriptor"]["systems"] == ["ISDB-T", "ISDB-S"]
assert isinstance(document["receivers"][0]["tune_revision"], str)
assert isinstance(document["usb_functions"][0]["id"], int)
assert document["usb_functions"][0]["usb"] == "0511:084a"
assert document["usb_functions"][0]["serial"] is None
assert "serial_state" not in document["enclosure"]
assert "uniqueness" not in document["enclosure"]
