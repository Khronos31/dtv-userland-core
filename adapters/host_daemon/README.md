# Optional host daemon adapter

This GPL-2.0-only adapter owns the shared PX4 host daemon services, worker
coordination, enclosure lifetime, and control server implementation extracted
from `Khronos31/px4-userland` source commit
`03d4f1ce0c5e39fb864cd845bfd069f891ab5932`. The ASICEN `request_stop` hooks
merged into the server and services are from source commit
`5501b03bec9d768f5c890eab0955f6ab125fbec4`. `PROVENANCE.json` records those
origins. `COPYING` preserves the source license text.

`dtv_userland_host_daemon` is opt-in, build-tree-only, and is not installed or
exported. It links the lower-level host facade, runtime, TS adapter, and the
shared MIT engines. Product-specific device discovery and backend assembly
remain in each product.
