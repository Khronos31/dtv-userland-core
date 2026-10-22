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
  "scan": {"complete": true, "issues": []},
  "enclosures": [{
    "id": 1,
    "model": "PX-Q3U4",
    "serial": "serial-value",
    "serial_unique": true,
    "status": "ready",
    "lnb_15v_supported": null,
    "readers": [{"reader": 0, "device": 2}],
    "devices": [{
      "id": 2,
      "usb": "0511:084a",
      "serial": "usb-serial",
      "bus": 1,
      "address": 6,
      "port": "1-2.3",
      "receivers": [{"receiver": 3, "device": 2, "local": 0,
                     "systems": ["ISDB-T", "ISDB-S"]}]
    }],
    "candidates": []
  }],
  "ungrouped_usb_devices": []
}
```

IDs are JSON numbers. VID and PID are one lowercase hexadecimal `VID:PID`
string. A device whose receiver inventory is unknown omits `receivers`; a
known zero-receiver device emits an empty array. The projector only emits
enclosures and devices supplied by the scan and never invents missing parents
or children.

Enclosure `id` and USB function `id` use the same identifier meanings as the
STATUS projection. In receiver records, `receiver` is the receiver number and
`device` references the containing USB function id; `local` is the receiver's
local index on that device. Reader `device` also references a USB function id.
Candidate `device` is the legacy device slot within the candidate observation,
not the enclosure id or an assertion that the candidate is a child device.

`serial_unique` describes how to select this enclosure when starting a daemon:
`true` means `--device serial:…` can select it, while `false` means to use
`--device usb:…` with its USB path because a duplicate enclosure or matching
serial makes serial selection ambiguous. `null` retains the existing meaning:
the serial was not observed, or the scan was incomplete and no duplicate was
observed. For an observed serial, a duplicate status or matching serial is
false even in an incomplete scan. A non-duplicate observed serial is true only
after a complete scan. This describes uniqueness among enclosures in this
scan, not a global guarantee.

Earlier daemon startup accepted only serial selection. M1UR and S1UR units can
share the serial `000000000000001`, making that selection ambiguous; the
`--device`/`--usb-path` options were added for USB-path selection and are now
represented by the unified `--device serial:`/`usb:` forms.

scan.complete and scan.issues describe enumeration of the whole scan.
Issues carry a stable code and human-readable detail. Enclosure status
describes grouping and is independent: ready, incomplete, duplicate, or
invalid_observation (or unknown when the adapter cannot classify it).
candidates preserves alternative USB observations for
a duplicate enclosure; those observations are not asserted as its child
devices. Each candidate carries its device slot, nullable serial and nullable
bus/address/port. USB observations that cannot be assigned to an enclosure
remain in ungrouped_usb_devices, with nullable serial/model/location, VID:PID
and a status of usable, unsupported, invalid_serial, insufficient_speed,
invalid_topology, open_failed, or unknown.

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
