# UDTV control protocol gate

`dtv::control` is an internal Core component for callers that need per-control-connection protocol checks over the existing `dtv::wire` records. It is built with `DTV_USERLAND_BUILD_CONTROL=ON` and found with `find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS control)`. It links to `dtv::wire` only and does not require Threads.

This increment provides `ControlConnection`, request/response helper values, and bounded frame encoding helpers. The caller supplies complete decoded frames and an output buffer. The gate requires HELLO first, negotiates the supported version/features, enforces increasing request IDs and one outstanding request, checks the daemon epoch, and decodes supported control requests into owned typed values. `dispatch` means only that a request is ready for the caller; it does not execute ACQUIRE, TUNE, START, STOP, or RELEASE and does not indicate those operations succeeded. `complete()` releases the outstanding-request gate after the caller has completed the response write.

For TUNE, this gate decodes and classifies the request only. The caller checks the receiver's LNB/LNA capabilities and applies the corresponding support policy.

The component owns no socket, listener, thread, timer, client, service, receiver, capture store, USB handle, or product adapter. ATTACH and TS_DATA belong to the separate data path and are not handled here. A well-formed opcode outside this gate's control subset receives `UNSUPPORTED` according to the existing wire records.

UDTV between daemon and ctl is an internal contract. Daemon and ctl from different releases are unsupported; exporting this CMake component does not establish a stable public wire or cross-release API. See `docs/wire-records.md` for the current record layout, including the name-only Model and the STATUS `lnb_15v_enabled` / `card_inserted` optional fields before topology.
