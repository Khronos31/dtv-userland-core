# Host worker lanes

`dtv::host_workers` is an opt-in, GPL-2.0-only target for the existing
`ControlWorkerLanes` and `CardService` implementations. It is disabled by
default. Build it with
`DTV_USERLAND_BUILD_RECEIVER=ON`, `DTV_USERLAND_BUILD_CARD=ON`,
`DTV_USERLAND_BUILD_HOST_TUNER=ON`, and
`DTV_USERLAND_BUILD_CONTROL=ON`, `DTV_USERLAND_BUILD_STATUS_PROJECTION=ON`,
`DTV_USERLAND_BUILD_AUTHORITY=ON`, and `DTV_USERLAND_BUILD_HOST_WORKERS=ON`.
Installed consumers request the
`host_workers` component and link `dtv::host_workers`:

```cmake
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_workers)
target_link_libraries(app PRIVATE dtv::host_workers)
```

The target reuses the existing TunerService owner, CARD ownership engine, and
ControlAuthority job values. A worker can execute Authority ACQUIRE, TUNE,
RELEASE, rollback RELEASE, or DISCONNECT against the same private TunerService
Engine and returns the typed authority completion values; it does not expose an
Engine accessor. A deferred disconnect intent is rejected until its preceding
ACQUIRE/rollback completion returns runnable cleanup. The worker owns submitted
task values; it does not retain a client, session, socket, or frame-buffer view.
There is no control server, socket, session coordinator, runtime dispatch,
production producer, or product connection in this target.

The bounded completion queue applies normal backpressure: while it is full,
the worker waits for the poll-side consumer to remove a completion and signal
the available slot. `request_stop()` drains accepted tasks but leaves their
completions for the caller to poll before `join()`. `stop_and_join()` is a
shutdown helper that drains and discards remaining completions before joining;
it is not a response-delivery path.

`error()` reports a sticky worker error. If a pthread synchronization error
prevents publishing a completion, the operation has already run and its
completion can be lost; the worker does not retry the operation or fabricate
a successful completion. A later authority bridge must treat this worker
error as fatal, terminate pending authority/session work, and keep
`CardService` and `TunerService` alive until the worker has stopped and been
joined. This target does not implement that bridge recovery policy or promise
completion delivery after primitive corruption.

The worker and CARD adapter sources and headers are GPL-2.0-only. Their
license text and per-file source hashes are installed under
`share/licenses/dtv_userland_core/host_workers/`; see
`HOST_WORKERS_PROVENANCE.json` and `COPYING`. The POSIX worker path and
device-free lifecycle fixture were verified on Linux. Windows compilation,
socket/server wiring, authority response completion, and product behavior are
not established by this target.
