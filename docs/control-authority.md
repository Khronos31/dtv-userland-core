# Socketless control authority

`dtv::authority` is an internal, default-OFF component. Build it with
`DTV_USERLAND_BUILD_AUTHORITY=ON`; this requires the existing control gate,
receiver engine, and STATUS projection options. Installed consumers request
`find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS authority)` and
link `dtv::authority`. The component adds no socket, OS thread, timer, data
path, product adapter, or USB dependency.

The host owner seam configures Authority IDs in `2^63..UINT64_MAX`; the legacy
TunerService domain is `1..2^63-1`. Both domains allocate monotonically and do
not wrap or reuse IDs. This range is an internal identity boundary and does
not change wire records. The current Core main has no `ControlServer` counter;
downstream server integration must apply the low-half ceiling to its own
allocator before using the shared Engine.

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
`AuthorityJob` owns its `SessionKey` and admitted tune ticket or acquire,
release, rollback, and disconnect identifiers by value; it contains no
session pointer. The host worker invokes `ControlAuthority::execute_job_value()`
outside the poll loop and posts the typed `AuthorityCompletion` back to that
same loop. ACQUIRE dispatch only queues a value job; the worker performs
`ReceiverEngine::acquire()` and returns the lease ID and receiver. The authority
checks key and job kind before producing a wire reply. If a successful acquire
with a valid lease ID cannot be recorded or encoded, completion returns one
rollback-release value job; the poll loop does not call Engine release. A
success completion without a lease ID closes the client through disconnect
cleanup because there is no scalar lease identity to release. A rejected
cleanup submit remains owned by the caller, and a repeated or stale completion
cannot produce the cleanup value again.

Closing while ACQUIRE or rollback is pending marks the session closed but keeps
its storage in the authority table. `close()` returns a deferred disconnect
intent that must not be submitted. The matching late completion returns the
runnable disconnect cleanup value; only its successful completion removes the
session. Rollback failure transfers cleanup to disconnect in sequence, never
with both operations runnable for the same lease. For these retained sessions,
the caller reports disconnect submission with `cleanup_submitted(job, accepted)`:
a rejected value remains caller-owned, and a retry is marked accepted before
its completion is delivered. Other close paths retain the existing close
contract. Engine/backend ownership must outlive all returned jobs and their
completions.

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
them. START is dispatched on the same Engine. STOP validates the session lease
and stream epoch before asking the executor for the metadata-only stop result;
the default executor returns `UNSUPPORTED` until it provides that operation.
The host bridge turns an accepted STOP into exact-ticket cleanup and retains a
pending tombstone when cleanup submission is temporarily rejected.

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
