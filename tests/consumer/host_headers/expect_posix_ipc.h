// SPDX-License-Identifier: MIT
#ifndef DTV_CONSUMER_HOST_HEADERS_EXPECT_POSIX_IPC_H
#define DTV_CONSUMER_HOST_HEADERS_EXPECT_POSIX_IPC_H

// Compile-time contract for the installed POSIX host runtime entrance.  The
// including probe TU must include the one header under test before this file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace host_headers_probe {

namespace posix = px4::userland::ipc::posix;

using posix::EndpointAccess;
using posix::EndpointConfig;
using posix::RuntimeInstanceDiscovery;
using posix::RuntimeRootCandidates;
using posix::SocketStream;

[[maybe_unused]] constexpr px4::userland::Result<SocketStream> (
    *kConnect)(const EndpointConfig&, px4::userland::Timeout) noexcept =
    &SocketStream::connect;
[[maybe_unused]] constexpr px4::userland::Result<SocketStream> (
    *kConnectWithDiagnostic)(
    const EndpointConfig&, px4::userland::Timeout,
    dtv::userland::host_runtime::EndpointConnectDiagnostic*) noexcept =
    &SocketStream::connect;
[[maybe_unused]] constexpr px4::userland::Result<std::size_t> (
    SocketStream::*kReadSome)(
    px4::userland::MutableByteView, px4::userland::Timeout) noexcept =
    &SocketStream::read_some;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    SocketStream::*kWriteAll)(
    px4::userland::ByteView, px4::userland::Timeout) noexcept =
    &SocketStream::write_all;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    SocketStream::*kWriteFrame)(
    px4::userland::ByteView, px4::userland::Timeout) noexcept =
    &SocketStream::write_frame;

[[maybe_unused]] constexpr px4::userland::Result<RuntimeRootCandidates> (
    *kClientRuntimeRoots)() noexcept = &posix::client_runtime_roots;
[[maybe_unused]] constexpr px4::userland::Result<std::string> (
    *kSelectPublishRuntimeRoot)(const char*, const char*) noexcept =
    &posix::select_publish_runtime_root;
[[maybe_unused]] constexpr px4::userland::Result<RuntimeInstanceDiscovery> (
    *kDiscoverRuntimeInstances)(
    const char*, EndpointAccess, const char*) noexcept =
    &posix::discover_runtime_instances;

static_assert(std::is_same_v<decltype(EndpointConfig::runtime_directory),
                             const char*>,
              "EndpointConfig::runtime_directory changed type");
static_assert(std::is_same_v<decltype(EndpointConfig::instance), const char*>,
              "EndpointConfig::instance changed type");
static_assert(std::is_same_v<decltype(EndpointConfig::endpoint_name),
                             const char*>,
              "EndpointConfig::endpoint_name changed type");
static_assert(std::is_same_v<decltype(EndpointConfig::access), EndpointAccess>,
              "EndpointConfig::access changed type");
static_assert(std::is_same_v<decltype(EndpointConfig::product_slug),
                             const char*>,
              "EndpointConfig::product_slug changed type");
static_assert(std::is_same_v<std::underlying_type_t<EndpointAccess>,
                             std::uint8_t>,
              "EndpointAccess is no longer a byte-sized enum");
static_assert(std::is_same_v<decltype(RuntimeRootCandidates::count),
                             std::size_t>,
              "RuntimeRootCandidates::count changed type");
static_assert(std::is_same_v<decltype(RuntimeInstanceDiscovery::count),
                             std::size_t>,
              "RuntimeInstanceDiscovery::count changed type");
static_assert(posix::kStoredPathCapacity == 512U,
              "POSIX endpoint path capacity changed");

}  // namespace host_headers_probe

#endif  // DTV_CONSUMER_HOST_HEADERS_EXPECT_POSIX_IPC_H
