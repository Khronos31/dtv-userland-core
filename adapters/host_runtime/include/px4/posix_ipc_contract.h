// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_POSIX_IPC_CONTRACT_H
#define PX4_USERLAND_POSIX_IPC_CONTRACT_H

#include "px4/ipc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace px4::userland::ipc::posix {

inline constexpr const char* kControlEndpointName = "control.sock";
inline constexpr const char* kStreamEndpointName = "stream.sock";
inline constexpr std::size_t kStoredPathCapacity = 512U;

// POSIX descriptors are int; a Windows SOCKET is a pointer-width handle. The
// platform adapter behind this API stores the native value, so callers must
// treat it as opaque and only pass it back to the adapter or an OS wait. The
// POSIX build keeps the exact int representation.
#if defined(_WIN32)
using NativeHandle = std::uintptr_t;
inline constexpr NativeHandle kInvalidHandle =
    static_cast<NativeHandle>(~static_cast<std::uintptr_t>(0U));
// AF_UNIX and the Win32 file APIs used by the adapter take wide paths.
using PathChar = wchar_t;
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
using PathChar = char;
#endif

enum class EndpointAccess : std::uint8_t {
    private_user,
    shared_group,
};

// A null runtime_directory selects $XDG_RUNTIME_DIR. instance and endpoint_name
// are single path components. The product namespace is a private target input;
// the resulting endpoint is:
//   <runtime_directory>/<product-userland>/v1/<instance>/<endpoint_name>
// This adapter selects permissions only. Group identity selection/chown remains
// the responsibility of the process launcher.
struct EndpointConfig final {
    const char* runtime_directory;
    const char* instance;
    const char* endpoint_name;
    EndpointAccess access = EndpointAccess::private_user;
};

struct RuntimeRootCandidates final {
    std::array<std::array<char, kStoredPathCapacity>, 3U> paths{};
    std::array<bool, 3U> create_if_missing{};
    std::size_t count = 0U;
};

struct RuntimeInstanceDiscovery final {
    std::array<std::array<char, 65U>, 256U> instances{};
    std::size_t count = 0U;
    bool canonical_collision = false;
};

Result<RuntimeRootCandidates> client_runtime_roots() noexcept;
Result<RuntimeInstanceDiscovery> discover_runtime_instances(
    const char* runtime_directory, EndpointAccess access) noexcept;
#if !defined(_WIN32)
Result<std::array<char, kStoredPathCapacity>> select_publish_runtime_root() noexcept;
#endif

// Holds the serial endpoint namespace while a daemon publishes its sockets.
// A serial-named endpoint is exclusive; custom instances of that observed
// serial share the lease. Close the sockets before releasing this lease.
class SerialEndpointLease final {
public:
    SerialEndpointLease() noexcept = default;
    ~SerialEndpointLease() noexcept;
    SerialEndpointLease(SerialEndpointLease&& other) noexcept;
    SerialEndpointLease& operator=(SerialEndpointLease&& other) noexcept;
    SerialEndpointLease(const SerialEndpointLease&) = delete;
    SerialEndpointLease& operator=(const SerialEndpointLease&) = delete;

    static Result<SerialEndpointLease> acquire(
        const EndpointConfig& endpoint, std::string_view observed_serial) noexcept;
    // A serial-free product identity always obtains an exclusive lease.
    static Result<SerialEndpointLease> acquire_identity(
        const EndpointConfig& endpoint, std::string_view observed_identity) noexcept;
    bool valid() const noexcept { return lock_fd_ != kInvalidHandle; }
    void close() noexcept;

private:
    static Result<SerialEndpointLease> acquire_validated(
        const EndpointConfig& endpoint, std::string_view observed_identity,
        bool force_exclusive) noexcept;

    NativeHandle directory_fd_ = kInvalidHandle;
    NativeHandle lock_fd_ = kInvalidHandle;
    std::array<char, 64U> filename_{};
#if defined(_WIN32)
    // Full lock path captured at acquire time. The Windows close() performs
    // the POSIX-equivalent last-holder lock-file cleanup and needs the path;
    // the POSIX build derives it from directory_fd_ + filename_ instead.
    std::array<PathChar, kStoredPathCapacity> lock_path_{};
#endif
};

class SocketListener;

// Move-only ownership of one connected nonblocking AF_UNIX SOCK_STREAM fd.
class SocketStream final {
public:
    SocketStream() noexcept = default;
    ~SocketStream() noexcept;

    SocketStream(SocketStream&& other) noexcept;
    SocketStream& operator=(SocketStream&& other) noexcept;
    SocketStream(const SocketStream&) = delete;
    SocketStream& operator=(const SocketStream&) = delete;

    static Result<SocketStream> connect(const EndpointConfig& config,
                                        Timeout timeout) noexcept;

    bool valid() const noexcept { return fd_ != kInvalidHandle; }
    // Borrowed descriptor for integration with an outer event loop. The
    // caller must not close it or transfer ownership.
    NativeHandle native_handle() const noexcept { return fd_; }
    void close() noexcept;

    // A successful read always returns at least one byte. EOF and reset are
    // reported as DISCONNECTED; no native errno escapes this boundary.
    Result<std::size_t> read_some(MutableByteView output, Timeout timeout) noexcept;

    // Reads one transport chunk into caller-owned storage and passes it to the
    // allocation-free StreamFramer. The returned count is the number of frames
    // delivered by this read; zero is valid for a partial frame.
    Result<std::size_t> read_frames(MutableByteView read_buffer,
                                    StreamFramer& framer,
                                    FrameConsumer& consumer,
                                    Timeout timeout) noexcept;

    // Validates that frame is one complete IPC frame, then sends every byte
    // within one monotonic absolute deadline.
    Result<void> write_frame(ByteView frame, Timeout timeout) noexcept;

    // Sends every byte within one monotonic absolute deadline, without
    // interpreting them.  This is the byte transport under write_frame for
    // callers that frame with another codec.
    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept;

private:
    explicit SocketStream(NativeHandle fd) noexcept : fd_(fd) {}
    friend class SocketListener;

    NativeHandle fd_ = kInvalidHandle;
};

// Move-only listener ownership. Destruction closes the fd and unlinks only the
// socket inode created by this instance. Newly-created empty endpoint
// directories are removed only when their inode still matches.
class SocketListener final {
public:
    SocketListener() noexcept = default;
    ~SocketListener() noexcept;

    SocketListener(SocketListener&& other) noexcept;
    SocketListener& operator=(SocketListener&& other) noexcept;
    SocketListener(const SocketListener&) = delete;
    SocketListener& operator=(const SocketListener&) = delete;

    static Result<SocketListener> listen(const EndpointConfig& config,
                                         int backlog = 16) noexcept;

    bool valid() const noexcept { return fd_ != kInvalidHandle; }
    // Borrowed descriptor for integration with an outer event loop. The
    // caller must not close it or transfer ownership.
    NativeHandle native_handle() const noexcept { return fd_; }
    const PathChar* endpoint_path() const noexcept { return endpoint_path_.data(); }
    Result<SocketStream> accept(Timeout timeout) noexcept;
    void close() noexcept;

private:
    struct FileIdentity final {
        std::uint64_t device = 0U;
        std::uint64_t inode = 0U;
        bool valid = false;
    };

    void move_from(SocketListener& other) noexcept;
    void cleanup_paths() noexcept;

    NativeHandle fd_ = kInvalidHandle;
    std::array<PathChar, kStoredPathCapacity> product_directory_{};
    std::array<PathChar, kStoredPathCapacity> version_directory_{};
    std::array<PathChar, kStoredPathCapacity> instance_directory_{};
    std::array<PathChar, kStoredPathCapacity> endpoint_path_{};
    FileIdentity product_identity_{};
    FileIdentity version_identity_{};
    FileIdentity instance_identity_{};
    FileIdentity endpoint_identity_{};
    std::uint16_t directory_mode_ = 0U;
    std::uint16_t socket_mode_ = 0U;
    bool created_product_directory_ = false;
    bool created_version_directory_ = false;
    bool created_instance_directory_ = false;
    bool created_endpoint_ = false;
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_POSIX_IPC_CONTRACT_H
