// SPDX-License-Identifier: GPL-2.0-only
#include "detail/listener_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace dtv::userland::host_listener_context::detail {
namespace {

constexpr std::size_t kConnectionCount = 64U;
constexpr std::uint64_t kIndexMask = 0xffU;

Error posix_error(int value) noexcept
{
    if (value == EACCES || value == EPERM) return Error::UNSUPPORTED;
    if (value == EADDRINUSE || value == EBUSY) return Error::BUSY;
    if (value == ENOENT || value == ECONNREFUSED) return Error::NOT_READY;
    if (value == EINVAL || value == ENAMETOOLONG) return Error::INVALID_ARGUMENT;
    if (value == EAGAIN || value == EWOULDBLOCK || value == ETIMEDOUT) return Error::TIMEOUT;
    if (value == ECONNRESET || value == EPIPE || value == ENOTCONN) return Error::DISCONNECTED;
    return Error::INTERNAL;
}

bool set_descriptor_flags(int descriptor) noexcept
{
    const int status = ::fcntl(descriptor, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (status < 0 || descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) != 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) return false;
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return false;
#endif
    return true;
}

struct PathIdentity final {
    dev_t device = 0;
    ino_t inode = 0;
    uid_t owner = 0;
    bool valid = false;
};

PathIdentity identity_of(const struct stat& info) noexcept
{
    return PathIdentity{info.st_dev, info.st_ino, info.st_uid, true};
}

bool same_identity(const PathIdentity& expected, const struct stat& actual) noexcept
{
    return expected.valid && expected.device == actual.st_dev &&
           expected.inode == actual.st_ino && expected.owner == actual.st_uid;
}

bool secure_directory(const std::string& path, host_runtime::EndpointAccess access,
                      uid_t owner) noexcept
{
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        S_ISLNK(info.st_mode) || info.st_uid != owner) {
        return false;
    }
    if (access == host_runtime::EndpointAccess::private_user) return (info.st_mode & 0077) == 0;
    return (info.st_mode & 0007) == 0 && (info.st_mode & 0070) == 0070;
}

bool ensure_directory(const std::string& path, host_runtime::EndpointAccess access,
                      uid_t owner) noexcept
{
    const bool created = ::mkdir(path.c_str(),
        access == host_runtime::EndpointAccess::private_user ? 0700 : 0770) == 0;
    if (!created && errno != EEXIST) return false;
    if (created && access == host_runtime::EndpointAccess::shared_group &&
        ::chmod(path.c_str(), 0770) != 0) {
        return false;
    }
    return secure_directory(path, access, owner);
}

bool stale_socket(const std::string& path, uid_t owner) noexcept
{
    struct stat before{};
    if (::lstat(path.c_str(), &before) != 0) return errno == ENOENT;
    if (!S_ISSOCK(before.st_mode) || S_ISLNK(before.st_mode) || before.st_uid != owner) {
        errno = EACCES;
        return false;
    }

    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return false;
    if (!set_descriptor_flags(probe)) {
        const int error = errno;
        (void)::close(probe);
        errno = error;
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        (void)::close(probe);
        errno = ENAMETOOLONG;
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const int connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&address),
                                    sizeof(address));
    const int connect_error = connected == 0 ? 0 : errno;
    (void)::close(probe);
    if (connected == 0 || (connect_error != ECONNREFUSED && connect_error != ENOENT)) {
        errno = connected == 0 ? EADDRINUSE : connect_error;
        return false;
    }

    struct stat after{};
    if (::lstat(path.c_str(), &after) != 0 || !same_identity(identity_of(before), after)) {
        errno = EBUSY;
        return false;
    }
    if (::unlink(path.c_str()) != 0) return false;
    return true;
}

std::uint64_t token_for(std::size_t index, std::uint32_t generation) noexcept
{
    return (static_cast<std::uint64_t>(generation) << 8U) |
           static_cast<std::uint64_t>(index + 1U);
}

bool decode_token(ConnectionToken token, std::size_t& index,
                  std::uint32_t& generation) noexcept
{
    const std::uint64_t encoded_index = token.value & kIndexMask;
    if (encoded_index == 0U || encoded_index > kConnectionCount) return false;
    index = static_cast<std::size_t>(encoded_index - 1U);
    generation = static_cast<std::uint32_t>(token.value >> 8U);
    return generation != 0U;
}

class PosixListenerIo final : public ListenerIo {
public:
    ~PosixListenerIo() noexcept override
    {
        (void)unpublish_pair(pair_);
        (void)release_namespace();
    }

    Error reserve_pair(const host_runtime::EndpointConfig& control,
                       const host_runtime::EndpointConfig& data) noexcept override
    {
        if (reserved_ || control.endpoint_name != "control.sock" ||
            data.endpoint_name != "stream.sock" ||
            control.runtime_directory != data.runtime_directory ||
            control.product != data.product || control.instance != data.instance ||
            control.access != data.access || control.expected_peer_uid != data.expected_peer_uid) {
            return Error::INVALID_ARGUMENT;
        }
        const auto control_result = host_runtime::endpoint_path(control);
        const auto data_result = host_runtime::endpoint_path(data);
        if (!control_result || !data_result) {
            return !control_result ? control_result.error() : data_result.error();
        }

        const uid_t owner = ::geteuid();
        const std::string runtime(control.runtime_directory);
        if (!secure_directory(runtime, control.access, owner)) return Error::UNSUPPORTED;
        std::string product_directory = runtime;
        product_directory.push_back('/');
        product_directory.append(control.product.data(), control.product.size());
        const std::string version_directory = product_directory + "/v1";
        const std::string instance_directory = version_directory + "/" +
            std::string(control.instance.data(), control.instance.size());
        if (!ensure_directory(product_directory, control.access, owner) ||
            !ensure_directory(version_directory, control.access, owner) ||
            !ensure_directory(instance_directory, control.access, owner)) {
            return errno == EACCES || errno == EPERM ? Error::UNSUPPORTED : Error::INTERNAL;
        }

        const int directory = ::open(instance_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) return posix_error(errno);
        const int lock = ::openat(directory, ".listener.lock",
            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        const int open_error = errno;
        (void)::close(directory);
        if (lock < 0) return posix_error(open_error);
        struct stat lock_info{};
        if (::fstat(lock, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
            lock_info.st_uid != owner || (lock_info.st_mode & 0077) != 0 ||
            ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            (void)::close(lock);
            return error == EWOULDBLOCK || error == EAGAIN ? Error::BUSY : Error::UNSUPPORTED;
        }

        control_path_ = control_result.value();
        data_path_ = data_result.value();
        access_ = control.access;
        owner_ = owner;
        lease_fd_ = lock;
        reserved_ = true;
        return Error::OK;
    }

    Error publish_pair(ListenerPair& pair) noexcept override
    {
        if (!reserved_ || published_) return Error::NOT_READY;
        if (!stale_socket(control_path_, owner_))
            return errno == EADDRINUSE || errno == EBUSY ? Error::BUSY : posix_error(errno);
        control_listener_ = create_listener(control_path_, control_identity_);
        if (control_listener_ < 0) return posix_error(errno);
        if (!stale_socket(data_path_, owner_)) {
            const int error = errno;
            rollback_listeners();
            return error == EADDRINUSE || error == EBUSY ? Error::BUSY : posix_error(error);
        }
        data_listener_ = create_listener(data_path_, data_identity_);
        if (data_listener_ < 0) {
            const int error = errno;
            rollback_listeners();
            return posix_error(error);
        }
        if (::listen(control_listener_, SOMAXCONN) != 0 ||
            ::listen(data_listener_, SOMAXCONN) != 0) {
            const int error = errno;
            rollback_listeners();
            return posix_error(error);
        }
        pair_ = ListenerPair{ListenerToken{1U}, ListenerToken{2U}};
        pair = pair_;
        published_ = true;
        return Error::OK;
    }

    Error poll(const ::px4::userland::ipc::posix::WakeHandle& worker_wake,
               Timeout timeout, ReadyEvent* events, std::size_t event_capacity,
               std::size_t& event_count) noexcept override
    {
        event_count = 0U;
        if (!published_ || events == nullptr || event_capacity == 0U) return Error::INVALID_ARGUMENT;
        std::array<pollfd, kListenerIoEventCapacity> descriptors{};
        std::array<std::size_t, kListenerIoEventCapacity> slots{};
        std::array<ReadyKind, kListenerIoEventCapacity> kinds{};
        std::array<ListenerKind, kListenerIoEventCapacity> listener_kinds{};
        std::array<ConnectionToken, kListenerIoEventCapacity> connection_tokens{};
        nfds_t count = 0U;
        descriptors[count] = pollfd{control_listener_, POLLIN, 0};
        kinds[count] = ReadyKind::listener;
        listener_kinds[count] = ListenerKind::control;
        ++count;
        descriptors[count] = pollfd{data_listener_, POLLIN, 0};
        kinds[count] = ReadyKind::listener;
        listener_kinds[count] = ListenerKind::data;
        ++count;
        for (std::size_t index = 0U; index < connections_.size(); ++index) {
            const auto& slot = connections_[index];
            if (slot.descriptor < 0) continue;
            short requested = 0;
            if ((slot.interest & ready_read) != 0U) requested |= POLLIN;
            if ((slot.interest & ready_write) != 0U) requested |= POLLOUT;
            descriptors[count] = pollfd{slot.descriptor, requested, 0};
            kinds[count] = ReadyKind::connection;
            slots[count] = index;
            connection_tokens[count] = ConnectionToken{token_for(index, slot.generation)};
            ++count;
        }
        if (worker_wake >= 0) {
            descriptors[count] = pollfd{worker_wake, POLLIN, 0};
            kinds[count] = ReadyKind::worker_wake;
            ++count;
        }
        const int wait_ms = static_cast<int>(std::min<std::uint32_t>(timeout.milliseconds, 0x7fffffffU));
        int result = 0;
        do {
            result = ::poll(descriptors.data(), count, wait_ms);
        } while (result < 0 && errno == EINTR);
        if (result < 0) return posix_error(errno);
        for (nfds_t index = 0U; index < count; ++index) {
            const short flags = descriptors[index].revents;
            if (flags == 0) continue;
            if (event_count >= event_capacity) {
                event_count = 0U;
                return Error::RESOURCE_LIMIT;
            }
            std::uint8_t mapped = 0U;
            if ((flags & (POLLIN | POLLPRI)) != 0) mapped |= ready_read;
            if ((flags & POLLOUT) != 0) mapped |= ready_write;
            if ((flags & (POLLERR | POLLNVAL)) != 0) mapped |= ready_error;
            if ((flags & POLLHUP) != 0) mapped |= ready_hangup;
            events[event_count++] = ReadyEvent{kinds[index], listener_kinds[index],
                index < 2U ? ListenerToken{index == 0U ? 1U : 2U} : ListenerToken{},
                connection_tokens[index], mapped};
            if ((flags & POLLNVAL) != 0) return Error::INTERNAL;
            (void)slots[index];
        }
        return Error::OK;
    }

    AcceptResult accept(ListenerToken listener) noexcept override
    {
        const int source = listener.value == 1U ? control_listener_ :
                           listener.value == 2U ? data_listener_ : -1;
        if (source < 0) return AcceptResult{IoStatus::failure, {}, Error::INVALID_ARGUMENT};
        const int descriptor = ::accept(source, nullptr, nullptr);
        if (descriptor < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return AcceptResult{IoStatus::would_block, {}, Error::OK};
            return AcceptResult{IoStatus::failure, {}, posix_error(errno)};
        }
        if (!set_descriptor_flags(descriptor)) {
            const int error = errno;
            (void)::close(descriptor);
            return AcceptResult{IoStatus::failure, {}, posix_error(error)};
        }
        for (std::size_t index = 0U; index < connections_.size(); ++index) {
            auto& slot = connections_[index];
            if (slot.descriptor >= 0) continue;
            if (slot.generation == 0U) slot.generation = 1U;
            slot.descriptor = descriptor;
            slot.interest = ready_read;
            return AcceptResult{IoStatus::progress,
                ConnectionToken{token_for(index, slot.generation)}, Error::OK};
        }
        (void)::close(descriptor);
        return AcceptResult{IoStatus::failure, {}, Error::RESOURCE_LIMIT};
    }

    Error set_connection_interest(ConnectionToken connection,
                                  std::uint8_t flags) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if ((flags & static_cast<std::uint8_t>(~(ready_read | ready_write))) != 0U ||
            !connection_slot(connection, index, generation)) return Error::INVALID_ARGUMENT;
        connections_[index].interest = flags;
        return Error::OK;
    }

    IoResult read_some(ConnectionToken connection, MutableByteView output) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if (output.data == nullptr || output.size == 0U || !connection_slot(connection, index, generation))
            return IoResult{IoStatus::failure, 0U, Error::INVALID_ARGUMENT};
        auto& slot = connections_[index];
        const ssize_t count = ::recv(slot.descriptor, output.data, output.size, 0);
        if (count > 0) return IoResult{IoStatus::progress, static_cast<std::size_t>(count), Error::OK};
        if (count == 0) return IoResult{IoStatus::eof, 0U, Error::OK};
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return IoResult{IoStatus::would_block, 0U, Error::OK};
        return IoResult{IoStatus::failure, 0U, posix_error(errno)};
    }

    IoResult write_some(ConnectionToken connection, ByteView input) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if (input.data == nullptr || input.size == 0U || !connection_slot(connection, index, generation))
            return IoResult{IoStatus::failure, 0U, Error::INVALID_ARGUMENT};
        const ssize_t count = ::send(connections_[index].descriptor, input.data, input.size,
#if defined(MSG_NOSIGNAL)
                                     MSG_NOSIGNAL
#else
                                     0
#endif
        );
        if (count > 0) return IoResult{IoStatus::progress, static_cast<std::size_t>(count), Error::OK};
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            return IoResult{IoStatus::would_block, 0U, Error::OK};
        return IoResult{IoStatus::failure, 0U, count == 0 ? Error::DISCONNECTED : posix_error(errno)};
    }

    Error close_connection(ConnectionToken connection) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if (!connection_slot(connection, index, generation)) return Error::NOT_FOUND;
        auto& slot = connections_[index];
        (void)::shutdown(slot.descriptor, SHUT_RDWR);
        if (::close(slot.descriptor) != 0) {
            const Error error = posix_error(errno);
            slot.descriptor = -1;
            advance_generation(slot);
            return error;
        }
        slot.descriptor = -1;
        advance_generation(slot);
        return Error::OK;
    }

    Error unpublish_pair(ListenerPair pair) noexcept override
    {
        if (pair.control.value != 0U && pair.control.value != 1U) return Error::INVALID_ARGUMENT;
        if (pair.data.value != 0U && pair.data.value != 2U) return Error::INVALID_ARGUMENT;
        for (auto& slot : connections_) {
            if (slot.descriptor >= 0) {
                (void)::shutdown(slot.descriptor, SHUT_RDWR);
                (void)::close(slot.descriptor);
                slot.descriptor = -1;
                advance_generation(slot);
            }
        }
        const int listener_error = close_listener(control_listener_);
        const int data_error = close_listener(data_listener_);
        remove_owned_path(control_path_, control_identity_);
        remove_owned_path(data_path_, data_identity_);
        control_identity_ = {};
        data_identity_ = {};
        published_ = false;
        return listener_error != 0 ? posix_error(listener_error) :
               data_error != 0 ? posix_error(data_error) : Error::OK;
    }

    Error release_namespace() noexcept override
    {
        if (published_) return Error::BUSY;
        if (lease_fd_ < 0) {
            reserved_ = false;
            return Error::OK;
        }
        (void)::flock(lease_fd_, LOCK_UN);
        const int result = ::close(lease_fd_);
        const int error = errno;
        lease_fd_ = -1;
        reserved_ = false;
        control_path_.clear();
        data_path_.clear();
        return result == 0 ? Error::OK : posix_error(error);
    }

private:
    struct ConnectionSlot final {
        int descriptor = -1;
        std::uint32_t generation = 1U;
        std::uint8_t interest = ready_read;
    };

    int create_listener(const std::string& path, PathIdentity& identity) noexcept
    {
        const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (descriptor < 0) return -1;
        if (!set_descriptor_flags(descriptor)) {
            const int error = errno;
            (void)::close(descriptor);
            errno = error;
            return -1;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
        if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const int error = errno;
            (void)::close(descriptor);
            errno = error;
            return -1;
        }
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode) ||
            info.st_uid != owner_) {
            const int error = errno == 0 ? EACCES : errno;
            (void)::close(descriptor);
            errno = error;
            return -1;
        }
        identity = identity_of(info);
        const mode_t mode = access_ == host_runtime::EndpointAccess::private_user ? 0600 : 0660;
        if (::chmod(path.c_str(), mode) != 0) {
            const int error = errno;
            remove_owned_path(path, identity);
            identity = {};
            (void)::close(descriptor);
            errno = error;
            return -1;
        }
        if (::lstat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode) ||
            info.st_uid != owner_ || (info.st_mode & (access_ == host_runtime::EndpointAccess::private_user ? 0077 : 0007)) != 0) {
            const int error = errno == 0 ? EACCES : errno;
            remove_owned_path(path, identity);
            identity = {};
            (void)::close(descriptor);
            errno = error;
            return -1;
        }
        return descriptor;
    }

    bool connection_slot(ConnectionToken token, std::size_t& index,
                         std::uint32_t& generation) noexcept
    {
        return decode_token(token, index, generation) &&
               connections_[index].descriptor >= 0 &&
               connections_[index].generation == generation;
    }

    static void advance_generation(ConnectionSlot& slot) noexcept
    {
        ++slot.generation;
        if (slot.generation == 0U) slot.generation = 1U;
    }

    static int close_listener(int& descriptor) noexcept
    {
        if (descriptor < 0) return 0;
        const int current = descriptor;
        descriptor = -1;
        return ::close(current) == 0 ? 0 : errno;
    }

    static void remove_owned_path(const std::string& path,
                                  const PathIdentity& identity) noexcept
    {
        if (path.empty() || !identity.valid) return;
        struct stat current{};
        if (::lstat(path.c_str(), &current) == 0 && same_identity(identity, current))
            (void)::unlink(path.c_str());
    }

    void rollback_listeners() noexcept
    {
        (void)close_listener(control_listener_);
        (void)close_listener(data_listener_);
        remove_owned_path(control_path_, control_identity_);
        remove_owned_path(data_path_, data_identity_);
        control_identity_ = {};
        data_identity_ = {};
    }

    std::array<ConnectionSlot, kConnectionCount> connections_{};
    std::string control_path_;
    std::string data_path_;
    PathIdentity control_identity_{};
    PathIdentity data_identity_{};
    ListenerPair pair_{};
    host_runtime::EndpointAccess access_ = host_runtime::EndpointAccess::private_user;
    uid_t owner_ = 0;
    int control_listener_ = -1;
    int data_listener_ = -1;
    int lease_fd_ = -1;
    bool reserved_ = false;
    bool published_ = false;
};

}  // namespace

std::unique_ptr<ListenerIo> create_listener_io() noexcept
{
    return std::unique_ptr<ListenerIo>(new (std::nothrow) PosixListenerIo());
}

}  // namespace dtv::userland::host_listener_context::detail
