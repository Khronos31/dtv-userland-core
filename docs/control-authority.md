# Socketless control authority

`dtv::authority` is an internal, default-OFF component. Build it with
`DTV_USERLAND_BUILD_AUTHORITY=ON`; this requires the existing control gate,
receiver engine, and STATUS projection options. Installed consumers request
`find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS authority)` and
link `dtv::authority`. The component adds no socket, OS thread, timer, data
path, product adapter, or USB dependency.

`ControlAuthority` holds one `ReceiverEngine` reference, the immutable server
HELLO values, and the canonical LIST. Caller-owned `ControlServiceSession`
objects register in its intrusive session table. Each session has a
non-reusable process-lifetime client ID, its `ControlConnection`, an owned
response frame, one bounded request slot, and a STATUS output record. Callers
must serialize authority methods on the event-loop owner, call `close()` and
hand off its returned cleanup job before destroying a live session, then keep
the authority, Engine, backend, and executor alive until all queued/running
jobs drain and workers join. Recreate the authority/client-ID space only after
that drain. The session's response storage is `kHeaderSize +
kMaxControlPayload` bytes; bounded tune/PID request storage remains with the
session instead of being copied into worker closures.

The injected `ControlAuthorityExecutor` is a nonblocking publication seam. A
`AuthorityJob` owns its `SessionKey` and admitted tune ticket or release and
disconnect identifiers by value; it contains no session pointer. The host
worker invokes `ControlAuthority::execute_job()` outside the poll loop and
posts its `(SessionKey, Error)` completion back to that same loop. The
authority resolves the key in its live session table while producing a reply.
Closing removes the session from the table first and revokes each pending tune
ticket before the disconnect cleanup job is handed off. Queued jobs then fail
Engine ticket revalidation; running work completes against the live Engine,
and its later key lookup is discarded. A completion discarded after its
session has been removed (including a duplicate completion) has no reply frame
and no cleanup job. The host must not close a still-live connection because of
that discard result. Engine/backend ownership must outlive both outcomes.

The host executor must provide parallel progress for cleanup on one receiver
while another receiver is blocked in TUNE. `ReceiverEngine` already serializes
physical operations per receiver, so this component does not add per-receiver
lanes. `submit()` must return without waiting. TUNE publication rejection
calls `abort_unstarted_tune()` once. Accepted work calls
`execute_admitted_tune()` once. A rejected disconnect publication returns the
cleanup job through the reply/output parameter; the caller must retain it for
an external teardown worker and must not run blocking cleanup in the poll
loop.

LIST is the canonical supplied record. STATUS builds a shell from the
canonical LIST plus caller-supplied context/snapshot and delegates receiver
projection to `dtv::status_projection`; unobserved LNB/CARD values stay absent.
ACQUIRE, TUNE, and RELEASE are bound to the session's non-reused Engine client
ID. TUNE retains exact requested Hz and replies only after its Engine
completion. Wire validation remains in `ControlConnection`; receiver system,
selection, and typed LNB values are checked against the canonical descriptor
before admission. A host may provide an additional profile validator. LNA and
PID requests are rejected because this Engine operation does not yet apply
them. START/STOP return `UNSUPPORTED` in this slice.

RELEASE is queued as blocking work and responds only after Engine cleanup. A
successful completion emits `released=true, quiesced=true`; failure preserves
the Engine error and emits both flags false. Caller code must call
`response_sent(session, true, ...)` only after the entire response frame is
written. Partial write or transport failure closes the session and schedules
cleanup; queueing a frame or writing only a prefix never releases the
one-outstanding-request gate. The host still owns transport writes and must
not report a send completion after failure or close.

For the first Siano host bridge, the existing pinned `TunerService` remains
the current Engine owner until a separate host increment changes the dispatch
boundary. Do not instantiate this authority beside that service for the same
adapter/device. The bridge must map typed jobs into its existing worker task
types, return completions to the existing poll loop, and retain one Engine
owner through worker drain/join. PX4, ASICEN, and PIX follow the Siano API
connection in parallel work; no product source or pin is changed here.
