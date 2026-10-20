// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CONTROL_SERVER_H
#define PX4_USERLAND_CONTROL_SERVER_H

#include "dtv/userland/records.h"
#include "px4/card_service.h"
#include "px4/control_execution_layout.h"
#include "px4/posix_ipc.h"
#include "px4/tuner_service.h"

#include <array>
#include <cstdint>
#include <memory>

namespace px4::userland::ipc::posix {

// The one canonical context a daemon serves.  list holds the immutable
// producer, enclosure, topology, function, receiver and reader descriptors;
// its epoch and snapshot are ignored and set by the server.  receiver_index
// maps the i-th list receiver (public u16 ID) to its TunerService receiver.
// The caller builds it once from the resolved product context.
struct ControlContext final {
    dtv::userland::wire::Uuid epoch{};
    dtv::userland::wire::Optional<dtv::userland::wire::WString<64>> instance{};
    dtv::userland::wire::ListResponse list{};
    // Optional, payload-free control request logging for --verbose.
    bool verbose = false;
    std::array<std::uint8_t, dtv::userland::wire::kMaxPublicReceivers> receiver_index{};
    ControlExecutionLayout execution{};
};

// POSIX control-plane engine used by px4d and socket integration tests. The
// poll thread owns sockets, protocol state, responses, and leases; finite
// CardService/TunerService calls run on private topology-specific worker
// lanes. Same-bridge tuner operations remain serialized by the bank.
//
// The control endpoint speaks UDTV 1.0 (HELLO, LIST, STATUS, ACQUIRE, RELEASE,
// TUNE).  Stream, card and event operations are not connected yet and are
// refused with UNSUPPORTED; the data endpoint refuses ATTACH the same way.
class PosixControlServer final {
public:
    // context must pass validate_list_response and map its receivers to
    // distinct TunerService receivers; the server copies it.  TUNE uses only
    // the strict Hz entrance and is UNSUPPORTED unless the tuner service has
    // strict capabilities configured.
    static Result<std::unique_ptr<PosixControlServer>> create(
        const EndpointConfig& endpoint, CardService& card_service,
        TunerService& tuner_service, const ControlContext& context,
        TunerStreamControl* stream_control = nullptr) noexcept;

    ~PosixControlServer() noexcept;
    PosixControlServer(const PosixControlServer&) = delete;
    PosixControlServer& operator=(const PosixControlServer&) = delete;

    // Processes accept/read/dispatch/write work for at most timeout. Repeated
    // calls form the foreground daemon loop and give signal state a bounded
    // observation interval.
    Result<void> poll_once(Timeout timeout) noexcept;
    Result<void> shutdown() noexcept;
    const PathChar* endpoint_path() const noexcept;
    const PathChar* stream_endpoint_path() const noexcept;
    std::size_t connection_count() const noexcept;

private:
    struct Impl;
    explicit PosixControlServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_CONTROL_SERVER_H
