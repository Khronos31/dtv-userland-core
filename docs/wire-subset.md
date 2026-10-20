# UDTV 1.0 wire subset

`dtv::wire` (`include/dtv/userland/wire.h`, `src/wire.cpp`) is a new, independently written
implementation of the first slice of the common UDTV wire. It covers the envelope, a
fragment-tolerant frame receiver, and three typed payloads. It is not a complete protocol and is
not connected to any existing product client, daemon, or IPC file.

## Envelope

Every integer is little-endian. Fields are written and read byte by byte; no struct is copied to
or from the wire, so host endianness, padding, and ABI do not leak.

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `UDTV`. The legacy `PX4U` and `ASCN` magics are rejected and reported separately as `HeaderStatus::legacy_magic`; their payloads are never interpreted. |
| 4 | 2 | major | Must be 1. |
| 6 | 2 | minor | Must be 0. Any other major or minor is `VERSION_MISMATCH`. |
| 8 | 2 | opcode | Raw u16. Unknown values are accepted. |
| 10 | 2 | flags | 0, 1, or 3; anything else is rejected. |
| 12 | 4 | request_id | See kinds below. |
| 16 | 4 | payload_length | Checked against the cap before any payload byte is read or stored. |

Bit 15 of the opcode marks an event (`is_event_opcode`). This rule applies to every opcode,
known or not. These are the only valid combinations:

| Kind | opcode bit 15 | flags | request_id |
|---|---:|---:|---|
| request | 0 | 0 | nonzero |
| response | 0 | 1 | nonzero |
| error_response | 0 | 3 | nonzero |
| event | 1 | 0 | 0 |

Every other combination is a malformed envelope (`PROTOCOL_ERROR`), even when payload dispatch for
that opcode is out of scope:

- Flags 2 or any reserved bit: `bad_flags`.
- An event opcode with flags 1 or 3: `bad_kind_for_opcode`. Example: STREAM_END `0x80ff` with
  flags 1 and id 1.
- A normal opcode with request_id 0, or an event opcode with a nonzero request_id:
  `bad_request_id`. Example: `0x0010` with flags 0 and id 0.

A valid envelope with an unknown opcode, whether a normal opcode with a nonzero id or an event
opcode with flags 0 and id 0, is accepted and reaches the dispatcher unchanged. Answering it with
`UNSUPPORTED` is the dispatcher's job. The encoder enforces the same table.

### Payload caps

- TS_DATA event: `kMaxTsDataPayload` = 1,048,476 + 28 = 1,048,504 bytes. 1,048,476 is the
  largest multiple of 188 that does not exceed 1 MiB, and 28 bytes are the fixed TS_DATA fields
  before the packets. The cap applies to the whole payload, not to the raw packet bytes, and the
  envelope does not check how many packets there are or whether the count is a multiple of 188.
  That belongs to the TS_DATA codec, which is not part of this subset.
- Every other frame, including unknown opcodes and other events: `kMaxControlPayload` = 65,536.

The legacy PX4 TS_DATA cap (1,048,596) is above the UDTV cap and is rejected.

## API

- `check_header(ByteView, Header*)` returns a detailed `HeaderStatus`, and
  `decode_header(ByteView)` returns `Result<Header>` with the status mapped by `header_error()`:
  a view error or a size other than 20 is `INVALID_ARGUMENT`, a version error is
  `VERSION_MISMATCH`, and everything else is `PROTOCOL_ERROR`. Exactly 20 bytes are required,
  and the payload is never touched.
- `encode_header` and `encode_frame` enforce the same rules as decode. Invalid headers are
  `INVALID_ARGUMENT`, a short output is `BUFFER_TOO_SMALL`, and nothing is written on failure.
  `encode_frame` requires `payload.size == header.payload_length`.
- `decode_frame(ByteView)` requires exactly `20 + payload_length` bytes; a short or long input
  is `PROTOCOL_ERROR`. The returned `Frame::payload` aliases the input.
- `FrameReceiver` reassembles frames from fragments of any size, coalesced frames included,
  without sockets or allocation. The caller supplies payload storage. The header is checked as
  soon as its 20 bytes are present, so an oversized or malformed length is rejected before any
  payload byte is copied. A payload larger than the caller storage is `BUFFER_TOO_SMALL`. Each
  complete frame goes to `FrameHandler::on_frame` unchanged, including frames with unknown
  opcodes; that is the dispatcher seam. Any failure, including a handler failure, poisons the
  receiver until `reset()`, the in-memory equivalent of closing the connection.
  `header_status()` tells the caller why a header was rejected.
- Call `FrameReceiver::finish()` when the input reaches EOF. It succeeds only on a clean frame
  boundary (`pending_size() == 0`). EOF inside a header or payload means the protocol ended in
  the middle of a frame: `finish()` returns `PROTOCOL_ERROR`, sets `truncated()`, and poisons
  the receiver. Bytes fed afterwards are refused, and there is no attempt to resynchronize to a
  new frame. On a receiver that has already failed, `finish()` returns that failure unchanged.
  `pending_size()` reports how many bytes of the incomplete frame are buffered, bounded by
  20 + payload cap and the caller storage.
- Null-pointer views with a nonzero size are rejected everywhere with `INVALID_ARGUMENT`. A null
  view with size 0 is a valid empty view.

A receiver that would rather allocate per frame can read 20 bytes, call `decode_header`, and
allocate only `payload_length` bytes after it succeeds. The cap guarantees the allocation is
bounded.

## Typed payloads

All typed decoders require the exact payload size and return `PROTOCOL_ERROR` otherwise. Values
that have the right size but are semantically invalid are `INVALID_ARGUMENT`. Encoders return
`BUFFER_TOO_SMALL` for a short output and never write a partial value.

| Payload | Layout | Size |
|---|---|---:|
| `HelloRequest` | `u16 min_major, u16 min_minor, u16 max_major, u16 max_minor, u64 required_features, u64 optional_features` | 24 |
| `AcquireRequest` | `u8 epoch[16], u16 receiver_id` | 18 |
| `AcquireResponse` | `u8 epoch[16], u64 lease_id` | 24 |

### HELLO version range

- Versions are compared as (major, minor). The encoder and decoder reject a range with
  min > max as `INVALID_ARGUMENT`.
- `negotiate_version` returns 1.0 when min <= 1.0 <= max. Otherwise it returns
  `VERSION_MISMATCH`, and an inverted range is `INVALID_ARGUMENT`. 1.0 is the only version
  this subset can select. The spec picks the highest minor both sides share within a major;
  since this implementation supports only 1.0, that minor is always 0.
- `required_features` and `optional_features` are carried unchanged, unknown and high bits
  included. The codec does not judge them; feature policy belongs to the dispatcher.

### ACQUIRE

- `epoch` is opaque and copied verbatim in both directions.
- `receiver_id` can be any u16, including 0 and values above 255. Whether the ID is in the
  public receiver set is decided by the daemon, not the codec.
- `AcquireResponse::lease_id` must be nonzero: both the encoder and decoder reject 0 with
  `INVALID_ARGUMENT`, whatever the epoch. Any other u64 is valid. There is no nonce; the attach
  token belongs to START in the later wire.

## Not in this subset

- HELLO response, error-response payload (code and detail), RELEASE, TUNE, START/STOP, STATS,
  LIST/STATUS records, card operations, ATTACH, and the TS_DATA payload codec, including the
  188-byte packet alignment check and the meaning of the 28-byte prefix. These come in later
  codecs.
- Connection state: HELLO or ATTACH_STREAM as the first frame, header version matching the
  negotiated version, one outstanding request per connection, rejecting reused or past
  request_ids (PROTOCOL_ERROR in the spec), matching a response to its request, UNSUPPORTED for
  missing required features, and STATE_EVENT subscription. These need a dispatcher and are not
  in this subset.
- Time limits: the 5-second limit on an incomplete frame and on an unfinished HELLO.
  `FrameReceiver` has no clock; the OS adapter must enforce these and then call `finish()` or
  close.
- Sockets, timeouts, and daemon or hardware integration.
- `FrameReceiver` is single-threaded and not reentrant; a handler must not call `feed` on the
  same receiver.
