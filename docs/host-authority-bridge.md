# Host Authority bridge

`dtv::host_authority_bridge` is an opt-in GPL host target. It owns 32
single-use `ControlServiceSession` slots and a separate bounded pool of 32
data connections, and composes `HostAuthorityOwner` with the adopted
`ControlWorkerLanes`; both caller domains therefore use the one `TunerService`
Engine supplied to the bridge. It has no listener or socket.

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

The fixed control table is allocated on the heap; construction failure rejects
new control sessions with `RESOURCE_LIMIT`. Data handles use a separate
generation-stamped pool, so a stale close cannot close a reused connection.
Each pool rejects its 33rd live or retained entry independently.

Configure `TunerService` with the v1 stream epoch before creating its workers,
then register that engine epoch and the caller-supplied finite START arm budget
with the bridge before opening sessions. The wire UUID epoch is checked against
the server epoch and is not converted into the engine's numeric epoch.
Authority START is handled by `ControlAuthority` on the same Engine. Data
ATTACH reserves the exact Engine ticket, queues physical prepare to its receiver
worker, and exposes a typed response only after prepare completion;
`data_attach_response_sent(true)` is the only path that commits the ticket.
An incomplete response aborts the ticket and retains the data tombstone until
the exact-ticket cleanup completes. `stop_stream()` issues metadata STOP and
queues cleanup against that retained ticket; its first result may therefore
report `quiesced == false` until worker cleanup records the terminal state. A
cleanup submit rejection keeps the ticket and tombstone pending for a later
`poll()` retry; it does not release the data slot or treat the stream as
quiesced.

`read_data()` pulls complete packets through the ticket's exact attachment,
returns a `TsData` view over caller storage, and advances a nonwrapping sequence
per data connection. During STOP the data connection remains readable until
its queue reaches EOF. After cleanup and drain, `take_data_stream_end()` exposes
the exact retained final snapshot; `data_stream_end_sent(true)` releases it and
the data slot. A closed data connection drops the final output but still waits
for physical cleanup before reusing its slot. The caller owns encoding and
full-write handling for `TsData` and `StreamEndEvent`.

The socketless control-frame `accept()` path dispatches wire START and STOP on
the same `ControlAuthority` and Engine. Wire STOP returns the correlated
`StopResponse` after metadata STOP; when cleanup submission is rejected, the
response reports `quiesced == false` with `BUSY`, while the tombstone and exact
ticket remain pending for a later `poll()` retry. Full response-send
confirmation still releases the one-outstanding-request gate; partial response
handling closes the session through the existing path. CARD/LNB dispatch, OS
listeners, product producers/pins, and deployment remain outside this increment.
