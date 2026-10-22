# Receiver v1 stream lifecycle

`dtv::receiver` keeps the legacy stream entrance and the v1 entrance as
separate contract modes. Configure v1 before the first ACQUIRE. A successful
tune publishes a current tune revision; START must use that revision and
returns a new stream ID and token with a finite arm deadline.

The data worker reserves the exact START identity, calls `prepare_attach_v1()`
to start capture and attach the data plane, then calls `commit_attach_v1()`
only after the attachment is active. The service acknowledges the stream only
after that commit succeeds. Abort and STOP retain the stream's terminal result
and transfer physical cleanup to at most one owner. `cleanup_owner` on the
returned STOP result identifies the caller that issued this cleanup; a final
result by itself does not mean that hardware is quiesced.

When `cleanup_owner` is true, the assigned worker calls
`run_stream_cleanup_v1()` with the exact ticket. It serializes against prepare,
re-reads physical capture/attachment ownership after acquiring the receiver
mutex, and performs the required stop/detach before recording quiescence.
`complete_stream_cleanup_v1()` is a metadata-only completion path for a caller
that has already performed and verified the physical cleanup elsewhere. It
does not stop capture or detach a data session; its `quiesced` argument records
the caller's observation and must not be set true before that physical cleanup
is complete.

The engine uses `TuneReadiness` for strict exact-Hz tuning. Legacy kHz tuning
continues to poll lock. `terminal_lock` requires a valid positive backend lock
measurement, `commands` records command-only verification, and `ts_sync`
requires the backend synchronization result. A missing terminal-lock sample is
an error; it is not inferred from a successful programming command.
