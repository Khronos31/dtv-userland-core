# STATUS JSON projection

`dtv::userland::wire::status_json_size` and `encode_status_json` project a validated
`StatusResponse` to one compact UTF-8 JSON object. The caller supplies the canonical same-epoch
`ListResponse` used by `validate_status_response`. The encoder appends one newline, no NUL, and
does not allocate. The input, context and output storage must not overlap and must remain
unchanged during the call.

## Public field mapping

| STATUS value | JSON representation |
| --- | --- |
| `producer` | `product`, `version`, `core_version` strings; missing core version is `null` |
| `context.epoch` | lowercase hexadecimal string; `instance` and `last_error` are nullable |
| `context.state`, topology, receiver, operation, stream and card states | readable lowercase strings |
| `snapshot.sequence`, monotonic timestamps, tune/stream revisions and counters | decimal strings, preserving all 64 bits |
| scope enclosure/receiver/reader IDs, function/receiver/reader IDs and local IDs | JSON numbers |
| enclosure model and serial | simple string or `null`; internal serial-state and uniqueness enums are omitted |
| enclosure LNB and internal CARD observation | `lnb_15v_enabled` and `card_inserted`, each JSON boolean or `null` |
| receiver broadcast systems | formal names such as `ISDB-T` and `ISDB-S` in an array |
| USB function VID:PID | lowercase four-digit hexadecimal string such as `0511:084a` |
| an error whose code is in the Core `Error` enum | its defined symbol in `code` |
| an error whose code is outside that enum | `code: "UNKNOWN"` and the original numeric `numeric_code` |

An absent optional string or observation is emitted as JSON `null`. Internal enclosure
`IdentitySerialState` and `Uniqueness` are not public status vocabularies. `serial_unique` is a
whole-scan discovery result and is not computed from one STATUS response. The enclosure CARD
field is the internal observation; it is not derived from readers, and reader state stays under
`readers[].card`. This projection does not build the `enclosures[].devices[].receivers[]`
whole-scan discovery hierarchy.

## Writer guarantees

Both public operations first run full `StatusResponse` validation against the canonical LIST.
`status_json_size` returns the exact byte count including the final newline. `encode_status_json`
counts before writing; invalid values, invalid output spans and short output buffers leave the
caller buffer unchanged. Strings escape JSON quotes, backslashes and control bytes while
preserving valid UTF-8 bytes. The internal writer uses bounded byte counting and does not expose a
writer-only public API.

The bounded writer behavior was independently implemented for the corrected public projection.
The prior Core #13 JSON work was consulted only for the size-counting, escaping and
validate-before-write approach; its old STATUS field map and fixture are not the public contract.
The mapping above follows the 2026-10-19 accepted JSON decisions and the 2026-10-22 typed
STATUS layout decision. See the agent-workspace `design/core-json-next-increment-plan-20261022.md`
and `DECISIONS-20261021.md` for the source decisions and scope.
