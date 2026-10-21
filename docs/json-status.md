# STATUS JSON projection

`dtv::userland::wire::status_json_size` and `encode_status_json` project a validated
`StatusResponse` to one compact UTF-8 JSON object. The caller supplies the canonical same-epoch
`ListResponse` used by `validate_status_response`. The encoder appends one newline, no NUL, and
does not allocate. The input, context and output storage must not overlap and must remain
unchanged during the call.

## Public field mapping

The emitted root fields are `schema`, `producer`, `context`, `snapshot`, `scope`, `enclosure`,
and `topology`. `usb_functions`, `receivers`, and `readers` are included only when their
corresponding optional collection is present.

| Emitted path | JSON representation |
| --- | --- |
| `producer.product`, `producer.version`, `producer.core_version` | strings; an absent core version is `null` |
| `context.epoch`, `context.instance`, `context.state`, `context.last_error` | lowercase hexadecimal epoch, nullable instance, readable state string, and nullable error |
| `snapshot.sequence`, `snapshot.observed_at_ns` | decimal strings preserving all 64 bits |
| `scope.kind`, `scope.id` | readable kind string; enclosure id is `null`, stream id is a decimal string, and receiver/reader ids are JSON numbers |
| `enclosure.id`, `enclosure.model`, `enclosure.serial` | JSON number; model name string or `null`; serial string or `null` |
| `enclosure.lnb_15v_enabled`, `enclosure.card_inserted` | JSON boolean or `null` |
| `enclosure.limits.max_concurrent_streams` | JSON number or `null` |
| `topology.state`, `topology.expected_function_count` | readable state string and JSON number or `null` |
| `usb_functions[]` | objects with `id`, `usb`, `mode`, `location.usb_path`, `location.bus`, `location.address`, `serial`, `model`, `present`, and `last_error` |
| `receivers[]` | objects with `descriptor`, `present`, `state`, `operation`, `leased`, `tune_revision`, `current_tune`, `last_successful_tune`, `stream`, `last_stream`, `measurements`, `statistics`, and `last_error` |
| `receivers[].descriptor` | `receiver` number, `systems` array of formal names such as `ISDB-T` and `ISDB-S`, and nullable `binding` |
| `receivers[].current_tune`, `receivers[].last_successful_tune` | nullable tune object; `selection.kind` is `auto`, `tsid`, or `slot`, and `selection.value` is omitted for `auto` |
| `receivers[].stream`, `receivers[].last_stream` | nullable active/final stream object; active stream uses `phase` |
| `receivers[].measurements`, `receivers[].statistics` | supported measurement objects and named counters; each collection reflects only entries represented by the typed STATUS value |
| `readers[]` | objects with `descriptor`, `present`, `card`, `operation`, `handles_open`, `transaction_active`, and `last_error`; `card` contains `state`, `sampled_at_ns`, and `reason` |
| error `code` within the Core `Error` enum | its defined symbol |
| error `code` outside that enum | `code: "UNKNOWN"` and the original numeric `numeric_code` |

Names such as `context`, `limits`, `measurements`, `statistics`, `tune.selection.kind`, and
`stream.phase` are retained from the implementation's existing projection. Their presence here
documents the emitted shape; it does not establish additional field semantics beyond the accepted
STATUS decisions and values validated by Core.

The public `model` value in STATUS and discovery JSON is the model **name only**, represented as a
string or `null`; a model identifier is not emitted. The example name form is `"PX-Q3U4"`. This
follows the accepted response 2.A / RC47 decision. `descriptor.capabilities` is not emitted;
capability publication, including LNA information, is deferred for later investigation.

`usb_functions[]` does not emit the internal `serial_state` or `identification_reason` fields.
Those fields describe internal query/identification state and are not part of this projection.

An absent optional string or observation is emitted as JSON `null`. Internal enclosure
`IdentitySerialState`, `Uniqueness`, `UsbFunction::serial_state`, and
`UsbFunction::identification_reason` are not public status vocabularies. `serial_unique` is a
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
