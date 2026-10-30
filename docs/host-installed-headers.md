# Installed host header contract probes

`tests/consumer/host_headers/` is an installed-package fixture for the three
host headers that products share:

| Canonical installed header | Historical forwarder | Compiled entries pinned by the probe |
|---|---|---|
| `<dtv/userland/host_daemon/tuner_service.h>` | `<px4/tuner_service.h>` | `TunerServiceBackend` receiver lifecycle and strict-profile virtuals, `TunerNonceSource::generate`, `TunerServiceTime`, `TunerService::acquire`, `TunerService` construction |
| `<dtv/userland/host_daemon/card_service.h>` | `<px4/card_service.h>` | `CardServiceBackend`, `CardProtocolSession`, `CardService::status`/`connect`, `CardServiceStatus` fields, `CardService` construction |
| `<dtv/userland/host_runtime/posix_ipc.h>` | `<px4/posix_ipc.h>` | `EndpointConfig` fields, `EndpointAccess`, `SocketStream::connect`/`read_some`/`write_all`/`write_frame`, runtime root/discovery entries, `kStoredPathCapacity` |

Each probe is one translation unit that includes exactly one of those entry
headers before anything else. The canonical and forwarder variant of a header
are separate translation units, so a probe that only succeeds because both
include orders are accepted cannot pass. The expected signature lives in the
paired `expect_*.h`, which is included after the entry header.

The fixture consumes the installed package only:

```cmake
find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_runtime)
target_link_libraries(<probe> PRIVATE dtv::host_runtime)
```

Configure with `-DCMAKE_PREFIX_PATH=<Core install prefix>`. The fixture fails
to configure when `CMAKE_PREFIX_PATH` is empty, when an imported include
directory points into the Core source tree, or when it lies outside every
`CMAKE_PREFIX_PATH` entry. `host_runtime` pulls the installed `host_tuner`,
`host_workers`, and their dependencies, which is why the all-on build is the
supported producer of this fixture.

A separate system-wide installation on the compiler's default include path is
outside that check and can satisfy a header missing from the chosen install
tree. Run the fixture where the default search path does not carry these
headers, as the CI runner does.

This is a Core canonical-header and forwarder contract only. It does not check
product headers of the same name, every production object or archive member,
or a single native link of several real backends. Those remain the final
multi-backend gate. The `px4/*` forwarders stay installed until the product
migrations and the later cleanup remove their remaining callers.
