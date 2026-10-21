# Discovery JSON projection

`discovery_json.h` projects a complete scan result into the public
`userland.discovery` JSON shape. It consumes a scan envelope rather than a
single `ListResponse`, so enclosure grouping and serial uniqueness are decided
over the same observed scan.

The output keeps the physical hierarchy:

```json
{
  "schema": {"name": "userland.discovery", "major": 1, "minor": 0},
  "producer": {"product": "px4", "version": "1.0.0"},
  "scan_complete": true,
  "enclosures": [{
    "enclosure": 1,
    "model": "PX-Q3U4",
    "serial": "serial-value",
    "serial_unique": true,
    "lnb_15v_supported": null,
    "readers": [{"reader": 0, "device": 2}],
    "devices": [{
      "device": 2,
      "usb": "0511:084a",
      "receivers": [{"receiver": 3, "device": 2, "local": 0,
                     "systems": ["ISDB-T", "ISDB-S"]}]
    }]
  }]
}
```

IDs are JSON numbers. VID and PID are one lowercase hexadecimal `VID:PID`
string. A device whose receiver inventory is unknown omits `receivers`; a
known zero-receiver device emits an empty array. The projector only emits
enclosures and devices supplied by the scan and never invents missing parents
or children.

`serial_unique` is null if the serial was not observed or the scan was
incomplete and no duplicate was observed. Matching serials on distinct
enclosures are false even in an incomplete scan. A non-duplicate observed
serial is true only after a complete scan. This describes uniqueness among
enclosures in this scan, not a global guarantee.

`lnb_15v_supported` is the enclosure's static LNB capability; it is `null`
when unknown and is not the current `lnb_15v_enabled` status. `readers` is
omitted while the reader inventory is unknown, and is an array (including
`[]`) when that inventory is known. Each reader identifies its reader number
and the observed device that supplies it. Receiver numbers must be unique
across the whole enclosure, including within one device.

`discovery_json_size` validates and measures the whole input. The encoder
validates and measures before writing, returns `BUFFER_TOO_SMALL` for a short
output, and leaves the output unchanged on either failure. The API is part of
the existing `dtv::json` library; it adds no writer-only target. Scan arrays
are borrowed pointer/count pairs, so the adapter can supply every observed
enclosure without a fixed discovery-count limit; keep the pointed-to arrays
unchanged for the duration of either call.
