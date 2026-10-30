// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_RUNTIME_POSIX_IPC_H
#define DTV_USERLAND_HOST_RUNTIME_POSIX_IPC_H

#include "px4/ipc.h"
#include <dtv/userland/host_runtime/family.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/wire.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace px4::userland::ipc::posix {

inline constexpr std::size_t kStoredPathCapacity = 512U;

enum class EndpointAccess : std::uint8_t { private_user, shared_group };
using EnclosureFamily = dtv::userland::host_runtime::EnclosureFamily;

struct EndpointConfig final {
    const char* runtime_directory = nullptr;
    const char* instance = nullptr;
    const char* endpoint_name = "control.sock";
    EndpointAccess access = EndpointAccess::private_user;
    const char* product_slug = "px4-userland";
};

struct RuntimeRootCandidates final {
    std::array<std::array<char, kStoredPathCapacity>, 3U> paths{};
    std::size_t count = 0U;
};

struct RuntimeInstanceDiscovery final {
    std::array<std::array<char, 65U>, 256U> instances{};
    std::size_t count = 0U;
    bool canonical_collision = false;
};

Result<RuntimeRootCandidates> client_runtime_roots() noexcept;
Result<std::string> select_publish_runtime_root(const char* product_slug,
                                                const char* instance) noexcept;
Result<RuntimeInstanceDiscovery> discover_runtime_instances(
    const char* runtime_directory, EndpointAccess access,
    const char* product_slug) noexcept;

class SocketStream final {
public:
    SocketStream() noexcept = default;
    SocketStream(SocketStream&&) noexcept = default;
    SocketStream& operator=(SocketStream&&) noexcept = default;
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    static Result<SocketStream> connect(const EndpointConfig& endpoint,
                                        Timeout timeout) noexcept;
    static Result<SocketStream> connect(
        const EndpointConfig& endpoint, Timeout timeout,
        dtv::userland::host_runtime::EndpointConnectDiagnostic* diagnostic) noexcept;
    bool valid() const noexcept { return static_cast<bool>(transport_); }
    void close() noexcept;
    Result<std::size_t> read_some(MutableByteView output, Timeout timeout) noexcept;
    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept;
    Result<void> write_frame(ByteView bytes, Timeout timeout) noexcept
    {
        return write_all(bytes, timeout);
    }

private:
    std::unique_ptr<dtv::userland::wire::TimedClientTransport> transport_;
};

}  // namespace px4::userland::ipc::posix

#endif  // DTV_USERLAND_HOST_RUNTIME_POSIX_IPC_H
