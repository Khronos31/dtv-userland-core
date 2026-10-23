# Host Authority bridge

`dtv::host_authority_bridge` is an opt-in GPL host target. It owns two
single-use `ControlServiceSession` slots and composes `HostAuthorityOwner` with
the adopted `ControlWorkerLanes`; both caller domains therefore use the one
`TunerService` Engine supplied to the bridge. It has no listener or socket.

The poll owner supplies decoded frames, context, snapshots, and the acceptance
clock. It writes a returned response frame and calls `response_sent(true)` only
after the complete frame has been written. A partial write, would-block result,
or transport error must be reported as `response_sent(false)` so disconnect
cleanup remains owned by the bounded table. Async responses are retrieved with
`take_response()`; the byte view remains owned by its session until the next
session operation.

The adopted worker lanes may also contain completions from the existing
caller. Install a `LegacyCompletionSink` before polling to return those values
to that caller. Without a sink, the bridge retains at most one completion per
lane and exposes it through `take_legacy_completion()`; it does not discard or
reinterpret those results.

Closing a session creates a tombstone. Accepted earlier work is drained before
the disconnect value is submitted; a deferred ACQUIRE or rollback keeps its
session object alive until its typed completion supplies cleanup. The slot is
reused only after successful disconnect completion. Queue rejection retains the
cleanup value for a later `poll()`. A cleanup execution error, unknown
completion key, or worker failure makes that slot `fatal_unresolved`; it is
never silently freed or retried as an accepted operation.

Register the other caller's ingress-stop callback before operation begins.
`begin_shutdown()` invokes it before closing bridge sessions, and a worker fatal
invokes the same callback before requesting worker stop. Normal shutdown
advances only after sessions and tombstones are
resolved and workers are idle; it then requests stop, drains completions, and
joins. A worker error enters fatal stop early, but the bridge continues draining
until all worker threads exit and completion queues are empty. Fatal unresolved
leases keep the owner alive and produce `INTERNAL`; this bridge does not claim
device quiescence or provide recovery for a permanently blocked backend.

The first bridge increment handles LIST/STATUS and ACQUIRE/TUNE/RELEASE/
DISCONNECT. Product listeners, START/STOP, ATTACH/data, CARD/LNB dispatch, and
product pin changes remain downstream integration work.
