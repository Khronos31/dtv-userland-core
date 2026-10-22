# TunerService owner adapter

`dtv::host_tuner` is a default-OFF compatibility adapter around the existing
`px4::userland::TunerService`. Enable it with
`DTV_USERLAND_BUILD_RECEIVER=ON` and
`DTV_USERLAND_BUILD_HOST_TUNER=ON`; installed consumers request
`find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_tuner)` and
link `dtv::host_tuner`.

This target preserves the existing service API and its single private
`ReceiverEngine` owner. The installed `px4/tuner_service.h` exposes the full
legacy service surface, including strict-tune and v1 stream methods; this
increment does not make those methods a stable Core API or claim that every
backend supports every operation. Backend capability and `UNSUPPORTED`
results remain authoritative. The focused fake-backend test establishes only
the owner target's ACQUIRE/TUNE/STATUS/RELEASE and cleanup linkage.

The separate, default-OFF `dtv::host_authority_owner` component adds a narrow
friend-only seam that constructs `ControlAuthority` over this service's one
private Engine. Legacy IDs are restricted to `1..2^63-1`; the host Authority
allocator starts at `2^63` and stops at `UINT64_MAX`. The TunerService entry
points reject high-bit IDs before forwarding to the Engine, including the
ticket and stream attachment entry points. This is ownership and identity
separation only: it adds no worker, server, socket, production dispatch,
capture producer, USB transport, or product connection.

The Core main used for this extraction contains no `ControlServer` source or
its connection counter. `ClientIdAllocator` provides and tests the low-half
terminal boundary, but no absent product server counter is silently imported
or claimed as capped by this component. Existing downstream counters must
adopt that boundary when their host connection source is integrated. This
slice alone does not establish product readiness.

The adapter source and compatibility IPC headers are GPL-2.0-only. They remain
in the separate, default-OFF `dtv::host_tuner` component. Its install places
the license and provenance under
`share/licenses/dtv_userland_core/host_tuner/`; the owner seam installs its
notice and provenance under `host_authority_owner/`. The existing MIT Core
targets remain separately licensed. Consumers that link or redistribute
these GPL components must account for their license obligations. See
`adapters/host_daemon/TUNER_SERVICE_PROVENANCE.json` for source paths and
`adapters/host_daemon/COPYING` for the license text.
