// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_ENCLOSURE_CONTEXT_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_ENCLOSURE_CONTEXT_H

#include "dtv/userland/detail/control_authority.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/host_runtime/family.h"

#include <cstdint>
#include <memory>
#include <string>

namespace dtv::userland::host_listener_context {

struct ContextConfig final {
    // These values are copied into Context-owned configuration during assemble.
    std::string runtime_directory;
    std::string product;
    std::string instance;
    host_runtime::EndpointAccess access =
        host_runtime::EndpointAccess::private_user;
    std::int64_t expected_peer_uid = -1;
    wire::ServerHello server;
    wire::ListResponse canonical_list;
    wire::Context context;
    ::dtv::userland::detail::ControlAuthority::TuneValidator validate_tune = nullptr;
    std::uint64_t stream_engine_epoch = 0U;
    std::uint64_t stream_arm_budget_ms = 0U;
};

enum class CloseState : std::uint8_t {
    quiesced,
    pending,
};

class EnclosureContext final {
public:
    // Ownership of family and nonce_source transfers to the returned Context.
    static Result<std::unique_ptr<EnclosureContext>> assemble(
        std::unique_ptr<host_runtime::EnclosureFamily> family,
        std::unique_ptr<host_runtime::TunerNonceSource> nonce_source,
        const ContextConfig& config) noexcept;

    ~EnclosureContext() noexcept;

    EnclosureContext(const EnclosureContext&) = delete;
    EnclosureContext& operator=(const EnclosureContext&) = delete;

    Result<void> reserve_namespace() noexcept;
    Result<void> publish() noexcept;
    Result<void> poll_once(Timeout timeout) noexcept;
    // pending keeps the Context owner graph alive; call close() again to
    // observe the same cleanup operation's eventual result.
    Result<CloseState> close(Timeout timeout) noexcept;

private:
    struct Impl;
    explicit EnclosureContext(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace dtv::userland::host_listener_context

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_ENCLOSURE_CONTEXT_H
