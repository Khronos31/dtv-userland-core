// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_RUNTIME_CONTROL_SOCKET_H
#define DTV_USERLAND_HOST_RUNTIME_CONTROL_SOCKET_H

#include <dtv/userland/control_client.h>
#include <dtv/userland/error.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace dtv::userland::host_runtime {

enum class EndpointAccess : std::uint8_t {
    private_user,
    shared_group,
};

struct EndpointConfig final {
    std::string_view runtime_directory;
    std::string_view product;
    std::string_view instance;
    EndpointAccess access = EndpointAccess::private_user;
    // Tests and tightly managed deployments may pin the expected server uid.
    // A negative value selects the current effective uid.
    std::int64_t expected_peer_uid = -1;
    std::string_view endpoint_name = "control.sock";
};

enum class EndpointDiagnosticReason : std::uint8_t {
    none,
    runtime_directory,
    product,
    instance,
    endpoint_name,
    path_too_long,
};

struct EndpointDiagnostic final {
    EndpointDiagnosticReason reason = EndpointDiagnosticReason::none;
    std::size_t path_bytes = 0U;
    std::size_t native_capacity = 0U;
    std::size_t required_bytes = 0U;
};

enum class EndpointConnectStage : std::uint8_t {
    none,
    endpoint_path,
    runtime_namespace,
    endpoint,
    connect,
    connect_timeout,
    peer_identity,
    transport,
};

struct EndpointConnectDiagnostic final {
    EndpointConnectStage stage = EndpointConnectStage::none;
    EndpointDiagnostic endpoint{};
};

Result<std::string> control_endpoint_path(const EndpointConfig& config) noexcept;
Result<std::string> control_endpoint_path(
    const EndpointConfig& config, EndpointDiagnostic* diagnostic) noexcept;
Result<std::string> endpoint_path(const EndpointConfig& config) noexcept;
Result<std::string> endpoint_path(
    const EndpointConfig& config, EndpointDiagnostic* diagnostic) noexcept;

// Connects to the local control endpoint after checking its namespace owner,
// permissions, and peer identity. The returned transport owns its socket.
Result<std::unique_ptr<wire::TimedClientTransport>> connect_control(
    const EndpointConfig& config, Timeout timeout) noexcept;
Result<std::unique_ptr<wire::TimedClientTransport>> connect_control(
    const EndpointConfig& config, Timeout timeout,
    EndpointConnectDiagnostic* diagnostic) noexcept;
Result<std::unique_ptr<wire::TimedClientTransport>> connect_endpoint(
    const EndpointConfig& config, Timeout timeout) noexcept;
Result<std::unique_ptr<wire::TimedClientTransport>> connect_endpoint(
    const EndpointConfig& config, Timeout timeout,
    EndpointConnectDiagnostic* diagnostic) noexcept;
void print_endpoint_connect_diagnostic(
    std::FILE* output, const char* tool_name, Error error,
    const EndpointConnectDiagnostic& diagnostic) noexcept;

}  // namespace dtv::userland::host_runtime

#endif  // DTV_USERLAND_HOST_RUNTIME_CONTROL_SOCKET_H
