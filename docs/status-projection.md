# Engine-backed STATUS projection

`dtv::status_projection` is an internal, default-OFF component built with
`DTV_USERLAND_BUILD_STATUS_PROJECTION=ON`. It requires
`DTV_USERLAND_BUILD_CONTROL=ON` and `DTV_USERLAND_BUILD_RECEIVER=ON`, links to
`dtv::control` and `dtv::receiver`, and does not provide a socket, service
operation, executor, or data path.

The caller obtains `ReceiverEngineStatus` from `ReceiverEngine::status()`, builds
a `StatusResponse` shell with `wire::build_status_shell()` from the canonical
LIST, context, snapshot, and requested scope, and then calls
`project_receiver_engine_status()`. The projection updates only receiver state,
operation, lease state, tune revision, and current/last-successful tune metadata.
It preserves caller-provided receiver presence, measurements, errors, and
enclosure observations. The caller owns all snapshots and response storage; the
projection takes references and creates no second `StatusResponse` value.

The engine snapshot does not report physical presence, lock/CNR/signal
measurements, LNB power, card insertion, or detailed errors. Callers provide
those observations when known. `present` is a non-optional wire boolean, so a
caller that cannot establish true or false must not publish that STATUS record.
Unobserved supported measurements are emitted as `unknown/not_sampled`; LNB and
card booleans remain absent when unobserved. `lnb_15v_enabled` is one
enclosure-wide state, not a per-receiver value. `card_inserted` must come from
the PIX internal observation contract and is not inferred by aggregating readers.
The engine lease bit is copied separately from `ReceiverState` so an engine
`error` state does not guess whether a lease remains active.

Tune frequency remains exact for native-Hz engine records. Legacy engine
kHz records are represented in the STATUS `frequency_hz` field by the exact
unit conversion from kHz to Hz. `programmed_hz` remains the backend-reported
command value and is not an RF measurement. Verification remains `commands`;
lock measurement is unknown unless the caller supplies an observation. Stream
IDs, reader/card state, LNB state, and `last_error` are never synthesized from
engine state. Engine receiver IDs not present in canonical LIST are ignored;
each receiver present in the requested canonical STATUS scope must have a
matching engine slot or projection fails. The complete response is validated
against the canonical LIST before the caller may encode or publish it.

Use `find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS status_projection)`
and link `dtv::status_projection`. The package config loads the receiver/control
dependencies and Threads for the consumer.
