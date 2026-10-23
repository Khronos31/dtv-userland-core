# Host client/runtime closure

The optional GPL host client/runtime targets close the reusable client-side symbols currently needed by the Siano consumer: process signals and Windows argument conversion (`dtv::host_facade`), platform tuner nonces and protected local endpoint connections (`dtv::host_runtime`), `list`/`status` CLI (`dtv::host_ctl`), and the shared typed wire TS runner plus platform output (`dtv::host_ts`). All options default OFF. Installed CMake components export the same targets for downstream consumers.

The runtime connector resolves an endpoint under `<runtime>/<product>/v1/<instance>/<endpoint>`. POSIX private-user mode checks the socket path owner and mode, then checks `SO_PEERCRED`; shared-group mode permits group access while still requiring the expected peer UID. Windows requires an owner matching the current user SID, a protected DACL, and allowed ACEs limited to that SID; it rejects shared-group access and verifies the endpoint ACL before connecting. The tests exercise a real local connection, endpoint owner/mode rejection, peer-UID rejection, the registered Windows CTRL_BREAK handler path, and a Windows DACL negative control that adds an Everyone allow ACE.

`dtv::host_ts` uses HELLO/LIST/STATUS and the typed ACQUIRE/TUNE/START/ATTACH/TS_DATA/STOP/RELEASE wire flow. The previous internal-IPC runner is not built or exported. The adapter does not provide a server listener, daemon `Context`, or product hardware composition.

## Siano closure map

Against Siano main `1a35d6b8bb4d2a72b2084658029cb1a825a9a683`, the new exports cover these reusable references:

| Siano consumer references | Export provided here |
| --- | --- |
| `px4d_signals.h`, `px4_windows_args.h` | `dtv::host_facade` |
| `px4/posix_tuner_nonce.h`, `px4/windows_tuner_nonce.h`, `TunerNonceSource` | `dtv::host_runtime` |
| `px4/posix_ipc.h`, `EnclosureFamily`, protected client endpoint transport | `dtv::host_runtime` |
| `host_ctl::run_control_cli` | `dtv::host_ctl` |
| `Px4TsRunner::run_wire` and POSIX/Windows output | `dtv::host_ts` |
| `tests/runtime_composition_proof.cpp` reference to `Px4TsRunner::run` | Not exported: this is the removed legacy internal-IPC path; the downstream proof should check `run_wire` only. |

This is a reusable header/link closure only. `sianod`, `sianoctl`, and `siano-wire-ts` still cannot form a product-complete server because the current Core has no listener/`Context` target to create the per-enclosure family and publish the control and stream endpoints. No Siano product target or pin is changed here.
