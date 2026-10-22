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

The target does not add a worker, server, socket, production dispatch, capture
producer, USB transport, or product connection. It is not connected to
`ControlAuthority`; this extraction alone does not establish product
readiness.

The adapter source and compatibility IPC headers are GPL-2.0-only. They remain
in the separate, default-OFF `dtv::host_tuner` component. Its install places
the license and provenance under
`share/licenses/dtv_userland_core/host_tuner/`. The existing MIT Core targets
are unchanged. Consumers that link or redistribute this GPL component must
account for the component's license obligations. See
`adapters/host_daemon/TUNER_SERVICE_PROVENANCE.json` for source paths and
`adapters/host_daemon/COPYING` for the license text.
