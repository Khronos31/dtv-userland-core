# UDTV 1.0 typed records

`dtv::wire::records` (`include/dtv/userland/records.h`, `src/records.cpp`, part of the
`dtv_userland_wire` target) is an independently written, bounded codec for the typed UDTV 1.0
records that follow the envelope and the HELLO/ACQUIRE subset. It reuses the envelope, the
fragment-tolerant `FrameReceiver`, and the existing HELLO request / ACQUIRE codecs unchanged; it
is not connected to any product client, daemon, or IPC endpoint.

## Encoding conventions

| Form | Bytes |
|---|---|
| `u8`/`u16`/`u32`/`u64`/`i32` | fixed width, little-endian |
| `bool` | `u8` 0 or 1 |
| `UUID`/epoch/token | 16 opaque bytes |
| `str[N]` | `u16` byte length + UTF-8 bytes, at most `N`, no embedded NUL |
| `bytes[N]` | `u32` length + bytes, at most `N` |
| `arr<T,N>` | `u16` count + elements, at most `N` |
| `opt<T>` | `u8` tag (0 absent / 1 present) + value when present |

Every record is stored in explicit fixed-size storage (`WString`, `BBytes`, `BArray`,
`Optional`); nothing allocates and no host struct is copied to or from the wire. The one
exception is `TsData`, which aliases the packet bytes in the input view instead of storing a
1 MiB array.

## Codec error contract

Decoders require the exact payload size. Within a record:

- `PROTOCOL_ERROR`: a size, count, length, optional tag, UTF-8, array-order or duplicate-ID
  violation, and trailing bytes after the last field.
- `INVALID_ARGUMENT`: a right-sized value that is out of range — an unknown enum, a reserved
  bit, a zero ID/lease/generation where the spec requires nonzero, an unsupported measurement in
  a declared set, or a semantic coupling such as `usable` without a matching generation.
- `RESOURCE_LIMIT`: a well-formed record whose encoded size would exceed `kMaxControlPayload`
  (65,536). It is returned before any byte is written and is distinct from `BUFFER_TOO_SMALL`,
  which means the caller's own buffer is too short. With the current caps every legal record fits
  under the control cap (the fully populated maximum STATUS is 42,350 bytes, including both
  enclosure observation optionals present); the guard is kept
  for future layouts.
- Encoders return `INVALID_ARGUMENT` for any invalid value and `BUFFER_TOO_SMALL` for a short
  output. An encoder measures the whole record with a counting pass first, so a short output
  leaves the caller buffer untouched and a record is never truncated.

## STATUS enclosure observations

Within a release, `StatusResponse` encodes `opt(enclosure)` followed by
`opt<bool> lnb_15v_enabled` and `opt<bool> card_inserted`, in that order, before `topology`.
Each optional boolean is a one-byte presence tag (0 absent, 1 present) followed by a one-byte
boolean (0 false, 1 true) when present. An absent value means unknown, unavailable, or unobserved;
it is distinct from an observed `false`. `card_inserted` represents an enclosure-level internal
card observation and is not derived by aggregating reader states. External reader observations
remain in their reader records.

The envelope version remains UDTV 1.0. This record layout is supported only between daemon and
client components from the same release. Mixed-release STATUS layouts, including the earlier
main layout without these two optional fields, are unsupported; the decoder retains its exact-size
and strict optional-tag checks.

## Validation split

Byte codecs check only what is local to a record plus the bounded containers. A separate set of
validators checks references and context, and must run before a snapshot is published or an
operation is dispatched:

- `validate_error_record`, `validate_scope`
- `validate_capabilities`, `validate_receiver_descriptor`, `validate_reader_descriptor`,
  `validate_topology`, `validate_limits`
- `validate_measurements` (metric ID set must equal the declared `measurement_mask`; each declared
  metric must be present and `unknown`/`valid`, never `unsupported`)
- `validate_stream_counters` (exactly ID5 and ID6), `validate_receiver_counters` (subset of ID1..4),
  including the saturation rule that `overflow=true` requires `value == UINT64_MAX`
- `validate_stream_final` (reason/error coupling), `validate_receiver_status`,
  `validate_reader_status`, `validate_card_observation`, `validate_card_session_result`,
  `validate_card_status_response`
- `validate_binding` (a `function_id` must exist in the same snapshot)
- `validate_list_response`
- `validate_status_response_scope` (shape only) and `validate_status_response` (aggregate)

`validate_status_response(value, StatusContext{list})` is the complete path a caller runs before
publishing a snapshot or dispatching an operation. The context is a canonical, validated
same-epoch `ListResponse`; it supplies the epoch the scope belongs to and the complete published
function/receiver/reader descriptors. Every validator first runs the exact codec local checks
over the value, so a direct typed caller cannot bypass enum/state/reason/reserved/cap rules with
a `static_cast`. The aggregate then checks:

- the response context epoch equals the canonical list epoch;
- duplicate IDs and membership: every response function/receiver/reader ID must exist in the
  canonical list, and a receiver/reader descriptor must match the canonical one in `systems`,
  `binding` and `capabilities`;
- for a global (`enclosure`) scope the response returns the complete same-epoch function,
  receiver and reader sets (counts and IDs equal to the canonical list), and collection presence
  must match the canonical topology: a known canonical topology requires both collections, a
  genuinely unknown canonical topology requires both omitted. A known empty reader set is a
  present array of count 0;
- for a partial scope the single returned receiver/reader is a canonical member with a matching
  descriptor; references are still resolved against the canonical function set;
- `Limits.max_concurrent_streams` against the canonical public receiver count, and every valid
  measurement/counter/card sample time against `snapshot.observed_at_ns`;
- a retained (last) stream target requires the receiver still `leased`.

When the canonical context is missing (or its epoch does not match), the function returns
`INVALID_ARGUMENT`: the partial record's own claims never substitute for a validated LIST.
`validate_status_response_scope` remains a shape-only pre-check.

## Not in this increment

- Dispatcher and connection state: HELLO first, negotiated version, reused/past request IDs,
  `UNSUPPORTED` for missing required features, STATE_EVENT subscription, request/response
  matching.
- The 5-second frame and HELLO deadlines, sockets, timeouts, daemon/hardware integration.
- PID-filter feature gating, profile range checks for frequency/tsid/slot, and the JSON exporter
  and CLI translation.
- Any product endpoint, CLI, JSON cutover, version bump or publication.

## Rules taken as adopted, not open

The review that preceded this revision flagged several points that the design documents do
determine; they are implemented as fixed rules, not recorded as ambiguities:

- Capabilities carry the LNA/LNB bodies directly under the `controls` bits with **no** optional
  presence tag, so minimal capabilities are four zero bytes.
- Issued lease/stream/card handle/generation IDs are nonzero; a stream scope target is nonzero
  too; a present `Error` has a nonzero code.
- `overflow=true` requires `value == UINT64_MAX`; a saturated counter is never a wrap.
- A STOP completion with `quiesced=false` must carry `final.error`.
- A TRANSMIT command is 1..4096 bytes.
- ISDB-T cannot select TSID or slot (`selection` stays `automatic`).
- An absent `UsbFunction.model` requires an identification reason other than `none`; a known cold
  model may still carry `cold_model`.
- When LNA is provided, its defaults must cover exactly the descriptor's supported systems (one
  entry per supported system, no missing or extra, no duplicates).
- `frequency_hz` and a present `programmed_hz` are positive and at most 2^53-1; the profile range
  is checked outside the codec.
- A STATUS `enclosure.identity` never asserts scan uniqueness: uniqueness is `unknown` when the
  serial state is `present` or `unknown` (an unknown state is not confirmed absence), and
  `not_applicable` only on confirmed `absent`. `unique_in_scan` and `duplicate` remain
  LIST/discovery values.
- A known topology returns both `receivers` and `readers`; a known-empty reader set is a present
  array of count 0, not an absent one. Unknown topology omits both.
- TUNE `timeout_ms` and the other TUNE optionals are explicitly defined by the design; the
  "no unrequested timeout option" sentence is card-specific.
- The detailed STOP/card/partial-scope records refine the basic draft rather than contradict it.

## Open ambiguities

Only points that cannot be derived from the existing documents remain:

1. **Zero-packet TS_DATA.** The layout fixes the maximum packet bytes and the 188 alignment but
   does not say whether a TS_DATA event with zero packets is legal. The codec accepts it.
2. **PID array order.** The draft requires no duplicates but does not require ascending order; the
   codec rejects duplicates with a bitmap and preserves the transmitted order.
3. **Peer-advertised HELLO limits.** `max_control_payload` and `max_ts_bytes` describe the peer and
   are carried unchanged; no cross-check against the local caps is performed.

## Provenance

Requirements come from `shared/design/common-wire-draft.md`, `wire-record-layout.md`,
`common-api-contract.md`, `json-record-contract.md` and `cli-contract-draft.md`, under the
conditional adoption in `adoption-status.md`. The code is an independent MIT implementation
(`independent-code-policy.md`); it copies no product payload, card protocol, hardware sequence or
vendor asset.
