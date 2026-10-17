# Common tune input resolution

`dtv::tuning::resolve` implements the pure shared channel/Hz/system/selection resolver from the adopted CLI contract. It accepts typed values, uses the wire `System` and `Selection` enums, and allocates no memory. It does not parse numeric option strings, construct a wire request, apply backend capability checks, or quantize requested Hz.

Exactly one of `Request::channel` and `Request::frequency_hz` is required. A channel determines its system; a supplied system must be the same recognized value. A frequency requires an explicit recognized system and must be in 1..9007199254740991 Hz. The returned frequency is the requested software target.

Channels are exact ASCII tokens: `13`..`62` or `T13`..`T62`; odd `BS01`..`BS23`, optionally followed by `_0`..`_11`; and even `CS2`..`CS24`, with `CS02`, `CS04`, `CS06`, and `CS08` also accepted. BS suffixes use unpadded decimal as an explicit lexical clarification in the implementation spec; `_00` and `_01` are rejected. The maximum token length is seven bytes.

Terrestrial channels use automatic selection and reject slot/TSID. An unsuffixed BS channel requires one slot or TSID. A suffixed BS channel supplies its slot and rejects any extra slot/TSID. CS channels default to slot 0; an explicit slot or TSID replaces that default. A satellite frequency requires an explicit slot or TSID. Explicit slot and TSID conflict. Slot range is 0..11 and TSID range is 1..65534.

All malformed, incomplete, out-of-range, conflicting, and unknown-enum inputs return `INVALID_ARGUMENT`. Later adapters remain responsible for device support, physical tuning, and hardware quantization. This is a common helper only; no product CLI is switched to it by this increment.
