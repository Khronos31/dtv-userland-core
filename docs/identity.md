# Portable identity codecs (I1)

`dtv::identity` (`include/dtv/userland/identity.h`, `src/identity.cpp`) implements the adopted
portable identity representation once, without OS discovery, security or daemon cutover. It is
pure and bounded: no allocation, no exceptions/RTTI, no USB/OS state. Physical discovery, serial
trust/uniqueness, permissions/ACL and USB FD ownership stay in the OS adapters.

## Device selector

`parse_device_selector` accepts the adopted grammar:

- `serial:<string>`: one nonempty strict UTF-8 suffix up to 512 bytes, no NUL or ASCII control.
  Spaces, colons, commas and non-ASCII are serial bytes; there is no trim, percent-decode,
  normalization or case fold.
- `usb:<path>[,<path>...]`: 1..8 paths of `bus-port[.port...]`, each a positive ASCII decimal
  (no leading zeros, u32 range) at most 64 bytes. The whole suffix is bounded by a shared
  `kMaxFunctions*kMaxUsbPathBytes + (kMaxFunctions-1)` before the delimiter scan, so an
  overlong input is rejected without scanning. Duplicate paths are rejected before the canonical
  ASCII byte sort; paths are never silently deduplicated. The order is the path order, not the
  input function-role numbering.

Parsing proves syntax only. It never proves enclosure grouping or OS location existence.

## Instance

`canonicalize_instance` accepts 1..64 ASCII characters, first alphanumeric, rest
alphanumeric/underscore/dot/hyphen, and returns that spelling unchanged. Case is preserved:
`RoomA`, `rooma` and `ROOMA` are distinct instance tokens, because a product's adopted instance
namespace may be case-sensitive. Lowercasing is not an adopted cross-OS rule; a product that wants
case-insensitive collision detection keeps that as a separate comparison key in its adapter. The
function rejects a terminal dot and, matched case-insensitively, the reserved first-dot stems
`con/prn/aux/nul/com1..com9/lpt1..lpt9`.

## Descriptors

`decode_langid_first` and `decode_string_descriptor` use the exact actual-transfer span: the
declared `bLength` must equal the supplied span, the type must be 3, and lengths must be even.
The LANGID table needs at least one language. The string decoder performs strict UTF-16LE to
lossless UTF-8, rejecting isolated or invalid surrogate pairs, and never inserts replacement
characters. An empty decoded string is valid here; `make_serial_selector` applies the serial
rules (nonempty, no NUL/control, at most 512 bytes) separately.

## Auto-identity

`encode_identity` writes the versioned little-endian representation:

```
4 bytes ASCII "DTVI"
u16 encoding_version = 1
LP(product slug UTF-8)          # px4-userland / asicen-userland / pix-userland / siano-userland / torne-userland
u8 identity_kind                # 1 = serial, 2 = usb-topology
kind=1: LP(serial UTF-8)
kind=2: u32 path_count; LP(canonical ASCII path) * path_count
```

`LP(x)` is a u32 byte length followed by the bytes. Only kinds 1 and 2 are accepted; any other
discriminator (0/3/255) is rejected before any output byte is written, by `encode_identity`,
`encode_identity_bytes` and `auto_instance` alike.

`resolve_selector(product, selector)` is a validated conversion: it rejects an undefined
`SelectorKind`, an invalid product, a malformed serial and an invalid topology, reuses the same
serial/topology validators as the encoder (duplicate rejection and canonical ASCII sorting), and
returns the validated resolved value. It does not establish enclosure grouping, OS presence or
serial trust.

`auto_instance` returns `auto-` plus the first 16 SHA-256 bytes of the encoded identity as 32
lowercase hex characters. Product, kind or identity changes produce independent names; topology is
canonical ASCII-sorted. There is no model/VID:PID/receiver-count/random fallback.

## Shared internals

`include/dtv/userland/detail/text.h` holds the single shared capacity definitions (8 functions,
64-byte paths, 512-byte serial, 64-byte instance) and the strict UTF-8 validator used by both the
wire records and the identity codecs; the wire byte set is unchanged by this extraction.
`include/dtv/userland/detail/sha256.h` is the neutral SHA-256 class extracted under the standing
independent-code policy (class-body sha256 `4117bc2b38ac968a5b1519a3e30fc5224aa94f15ddfb02a8b38d34590a35279b`).

## Not in this increment

Physical discovery, serial trust/uniqueness decisions, OS string fetching, Windows argv
conversion, permissions/ACL, USB FD ownership, and any CLI/daemon/JSON/endpoint cutover.
