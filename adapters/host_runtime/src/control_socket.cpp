// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_runtime/control_socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <aclapi.h>
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#endif
#endif

namespace dtv::userland::host_runtime {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ms() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

bool valid_component(std::string_view value) noexcept
{
    if (value.empty() || value == "." || value == "..") return false;
    for (const unsigned char character : value) {
        const bool accepted = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_' || character == '.';
        if (!accepted) return false;
    }
    return true;
}

#if defined(_WIN32)

using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

bool ensure_winsock() noexcept
{
    static bool ready = []() noexcept {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

int remaining_ms(Clock::time_point deadline) noexcept
{
    const auto now = Clock::now();
    if (now >= deadline) return 0;
    const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::min<std::int64_t>(remain.count() + 1, 0x7fffffff));
}

bool wait_socket(NativeSocket socket, bool writing, Clock::time_point deadline) noexcept
{
    const int timeout = remaining_ms(deadline);
    if (timeout == 0) return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval value{timeout / 1000, (timeout % 1000) * 1000};
    const int selected = ::select(0, writing ? nullptr : &set,
                                  writing ? &set : nullptr, nullptr, &value);
    return selected > 0;
}

bool current_sid(PSID& sid, HANDLE& token, std::array<std::uint8_t, 512U>& storage) noexcept
{
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return false;
    DWORD returned = 0U;
    if (::GetTokenInformation(token, TokenUser, storage.data(),
            static_cast<DWORD>(storage.size()), &returned) == 0) {
        (void)::CloseHandle(token);
        token = nullptr;
        return false;
    }
    sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
    return sid != nullptr;
}

Error endpoint_security(const std::string& path) noexcept
{
    const int wide_length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path.c_str(), -1, nullptr, 0);
    if (wide_length <= 0) return Error::UNSUPPORTED;
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
            wide.data(), wide_length) != wide_length) return Error::UNSUPPORTED;

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = ::GetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
        return Error::NOT_FOUND;
    if (result != ERROR_SUCCESS || owner == nullptr || dacl == nullptr || descriptor == nullptr)
        return Error::UNSUPPORTED;

    SECURITY_DESCRIPTOR_CONTROL control = 0U;
    DWORD revision = 0U;
    if (::GetSecurityDescriptorControl(descriptor, &control, &revision) == 0 ||
        (control & SE_DACL_PROTECTED) == 0U) {
        (void)::LocalFree(descriptor);
        return Error::UNSUPPORTED;
    }

    std::array<std::uint8_t, 512U> storage{};
    HANDLE token = nullptr;
    PSID user = nullptr;
    const bool have_user = current_sid(user, token, storage);
    bool accepted = have_user && ::EqualSid(owner, user) != 0;
    if (accepted) {
        for (DWORD index = 0U; index < dacl->AceCount; ++index) {
            void* raw = nullptr;
            if (::GetAce(dacl, index, &raw) == 0 || raw == nullptr) {
                accepted = false;
                break;
            }
            const auto* header = static_cast<ACE_HEADER*>(raw);
            PSID trustee = nullptr;
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                trustee = const_cast<DWORD*>(
                    &static_cast<ACCESS_ALLOWED_ACE*>(raw)->SidStart);
            } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
                trustee = const_cast<DWORD*>(
                    &static_cast<ACCESS_ALLOWED_OBJECT_ACE*>(raw)->SidStart);
            } else if (header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
                trustee = const_cast<DWORD*>(
                    &static_cast<ACCESS_ALLOWED_CALLBACK_ACE*>(raw)->SidStart);
            } else if (header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
                trustee = const_cast<DWORD*>(
                    &static_cast<ACCESS_ALLOWED_CALLBACK_OBJECT_ACE*>(raw)->SidStart);
            } else {
                continue;
            }
            if (::EqualSid(trustee, user) == 0) {
                accepted = false;
                break;
            }
        }
    }
    if (token != nullptr) (void)::CloseHandle(token);
    if (descriptor != nullptr) (void)::LocalFree(descriptor);
    return accepted ? Error::OK : Error::UNSUPPORTED;
}

#else

using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

Error socket_error(int value) noexcept
{
    if (value == ETIMEDOUT) return Error::TIMEOUT;
    if (value == EACCES || value == EPERM) return Error::UNSUPPORTED;
    if (value == ENOENT || value == ECONNREFUSED) return Error::NOT_READY;
    if (value == EINVAL || value == ENAMETOOLONG) return Error::INVALID_ARGUMENT;
    if (value == EAGAIN || value == EWOULDBLOCK) return Error::TIMEOUT;
    if (value == ECONNRESET || value == EPIPE || value == ENOTCONN) return Error::DISCONNECTED;
    return Error::INTERNAL;
}

bool secure_directory(const std::string& path, EndpointAccess access, uid_t owner) noexcept
{
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != owner || S_ISLNK(info.st_mode)) return false;
    const mode_t forbidden = access == EndpointAccess::private_user ? 0077 : 0007;
    return (info.st_mode & forbidden) == 0;
}

enum class EndpointCheck : std::uint8_t { ready, missing, refused };

EndpointCheck secure_endpoint(const std::string& path, EndpointAccess access, uid_t owner,
                              struct stat& identity) noexcept
{
    if (::lstat(path.c_str(), &identity) != 0)
        return errno == ENOENT ? EndpointCheck::missing : EndpointCheck::refused;
    if (!S_ISSOCK(identity.st_mode) || identity.st_uid != owner)
        return EndpointCheck::refused;
    if (access == EndpointAccess::private_user)
        return (identity.st_mode & 0077) == 0 ? EndpointCheck::ready :
                                               EndpointCheck::refused;
    return (identity.st_mode & 0007) == 0 && (identity.st_mode & 0060) == 0060
        ? EndpointCheck::ready : EndpointCheck::refused;
}

bool wait_socket(NativeSocket socket, short events, Clock::time_point deadline) noexcept
{
    while (true) {
        const auto now = Clock::now();
        if (now >= deadline) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto milliseconds = std::min<std::int64_t>(remaining.count() + 1, 0x7fffffff);
        pollfd descriptor{socket, events, 0};
        const int result = ::poll(&descriptor, 1U, static_cast<int>(milliseconds));
        if (result > 0) {
            if ((descriptor.revents & POLLNVAL) != 0) return false;
            if ((descriptor.revents & events) != 0) return true;
            return (events & POLLIN) != 0 &&
                   (descriptor.revents & (POLLERR | POLLHUP)) != 0;
        }
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

#endif

class SocketTransport final : public wire::TimedClientTransport {
public:
    explicit SocketTransport(NativeSocket socket) noexcept : socket_(socket) {}
    ~SocketTransport() noexcept override { close(); }

    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept override
    {
        if (socket_ == kInvalidSocket || (bytes.size != 0U && bytes.data == nullptr))
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout.milliseconds);
        std::size_t offset = 0U;
        while (offset < bytes.size) {
#if defined(_WIN32)
            if (!wait_socket(socket_, true, deadline)) return Result<void>::failure(Error::TIMEOUT);
            const int count = ::send(socket_, reinterpret_cast<const char*>(bytes.data + offset),
                static_cast<int>(std::min<std::size_t>(bytes.size - offset, INT_MAX)), 0);
            if (count == SOCKET_ERROR) {
                const int error = ::WSAGetLastError();
                if (error == WSAEINTR || error == WSAEWOULDBLOCK) continue;
                return Result<void>::failure(error == WSAECONNRESET ? Error::DISCONNECTED : Error::INTERNAL);
            }
#else
            if (!wait_socket(socket_, POLLOUT, deadline)) return Result<void>::failure(Error::TIMEOUT);
            const ssize_t count = ::send(socket_, bytes.data + offset, bytes.size - offset,
#if defined(MSG_NOSIGNAL)
                                         MSG_NOSIGNAL
#else
                                         0
#endif
            );
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return Result<void>::failure(socket_error(errno));
            }
#endif
            if (count <= 0) return Result<void>::failure(Error::DISCONNECTED);
            offset += static_cast<std::size_t>(count);
        }
        return Result<void>::success();
    }

    Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept override
    {
        if (socket_ == kInvalidSocket || bytes.data == nullptr || bytes.size == 0U)
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout.milliseconds);
        while (true) {
#if defined(_WIN32)
            if (!wait_socket(socket_, false, deadline)) return Result<std::size_t>::failure(Error::TIMEOUT);
            const int count = ::recv(socket_, reinterpret_cast<char*>(bytes.data),
                static_cast<int>(std::min<std::size_t>(bytes.size, INT_MAX)), 0);
            if (count == SOCKET_ERROR) {
                const int error = ::WSAGetLastError();
                if (error == WSAEINTR || error == WSAEWOULDBLOCK) continue;
                return Result<std::size_t>::failure(error == WSAECONNRESET ? Error::DISCONNECTED : Error::INTERNAL);
            }
#else
            if (!wait_socket(socket_, POLLIN, deadline)) return Result<std::size_t>::failure(Error::TIMEOUT);
            const ssize_t count = ::recv(socket_, bytes.data, bytes.size, 0);
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return Result<std::size_t>::failure(socket_error(errno));
            }
#endif
            if (count == 0) return Result<std::size_t>::success(0U);
            return Result<std::size_t>::success(static_cast<std::size_t>(count));
        }
    }

    std::uint64_t monotonic_ms() noexcept override { return now_ms(); }

    void close() noexcept override
    {
        if (socket_ == kInvalidSocket) return;
#if defined(_WIN32)
        (void)::shutdown(socket_, SD_BOTH);
        (void)::closesocket(socket_);
#else
        (void)::shutdown(socket_, SHUT_RDWR);
        (void)::close(socket_);
#endif
        socket_ = kInvalidSocket;
    }

private:
    NativeSocket socket_ = kInvalidSocket;
};

}  // namespace

Result<std::string> endpoint_path(const EndpointConfig& config) noexcept
{
    if (config.runtime_directory.empty() || config.product.empty() ||
        config.instance.empty() || !valid_component(config.product) ||
        !valid_component(config.instance) || !valid_component(config.endpoint_name))
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
#if defined(_WIN32)
    if (config.runtime_directory.size() < 3U ||
        !((config.runtime_directory[0] >= 'A' && config.runtime_directory[0] <= 'Z') ||
          (config.runtime_directory[0] >= 'a' && config.runtime_directory[0] <= 'z')) ||
        config.runtime_directory[1] != ':' ||
        (config.runtime_directory[2] != '\\' && config.runtime_directory[2] != '/'))
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    constexpr char separator = '\\';
#else
    if (config.runtime_directory.front() != '/')
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    constexpr char separator = '/';
#endif
    std::string path(config.runtime_directory);
    while (path.size() > 1U && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    path.push_back(separator);
    path.append(config.product.data(), config.product.size());
    path.push_back(separator);
    path.append("v1");
    path.push_back(separator);
    path.append(config.instance.data(), config.instance.size());
    path.push_back(separator);
    path.append(config.endpoint_name.data(), config.endpoint_name.size());
#if defined(_WIN32)
    if (path.size() >= sizeof(SOCKADDR_UN{}.sun_path))
#else
    if (path.size() >= sizeof(sockaddr_un{}.sun_path))
#endif
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    return Result<std::string>::success(std::move(path));
}

Result<std::string> control_endpoint_path(const EndpointConfig& config) noexcept
{
    EndpointConfig control = config;
    control.endpoint_name = "control.sock";
    return endpoint_path(control);
}

Result<std::unique_ptr<wire::TimedClientTransport>> connect_endpoint(
    const EndpointConfig& config, Timeout timeout) noexcept
{
    const auto path_result = endpoint_path(config);
    if (!path_result) return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(path_result.error());
    const std::string& path = path_result.value();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout.milliseconds);

#if defined(_WIN32)
    if (config.access != EndpointAccess::private_user)
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    if (!ensure_winsock())
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    const Error security = endpoint_security(path);
    if (security != Error::OK)
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(security);
    NativeSocket socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == kInvalidSocket) return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::INTERNAL);
    u_long nonblocking = 1U;
    if (::ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        (void)::closesocket(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::INTERNAL);
    }
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const int connected = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected == SOCKET_ERROR) {
        const int error = ::WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEALREADY) {
            (void)::closesocket(socket);
            return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::NOT_READY);
        }
        if (!wait_socket(socket, true, deadline)) {
            (void)::closesocket(socket);
            return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::TIMEOUT);
        }
        int socket_error_value = 0;
        int error_length = sizeof(socket_error_value);
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&socket_error_value), &error_length) != 0 || socket_error_value != 0) {
            (void)::closesocket(socket);
            return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::NOT_READY);
        }
    }
#else
    const uid_t owner = ::geteuid();
    std::string parent(path);
    const std::size_t final_separator = parent.rfind('/');
    const std::string instance_dir = parent.substr(0U, final_separator);
    const std::size_t instance_separator = parent.rfind('/', final_separator - 1U);
    const std::string version_dir = parent.substr(0U, instance_separator);
    const std::size_t version_separator = parent.rfind('/', instance_separator - 1U);
    const std::string product_dir = parent.substr(0U, version_separator);
    const std::string runtime_directory(config.runtime_directory);
    if (!secure_directory(runtime_directory, config.access, owner) ||
        !secure_directory(product_dir, config.access, owner) ||
        !secure_directory(version_dir, config.access, owner) ||
        !secure_directory(instance_dir, config.access, owner))
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    struct stat endpoint_before{};
    const EndpointCheck endpoint_check = secure_endpoint(
        path, config.access, owner, endpoint_before);
    if (endpoint_check != EndpointCheck::ready)
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(
            endpoint_check == EndpointCheck::missing ? Error::NOT_FOUND : Error::UNSUPPORTED);
    NativeSocket socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::INTERNAL);
    const int original = ::fcntl(socket, F_GETFL, 0);
    if (original < 0 || ::fcntl(socket, F_SETFL, original | O_NONBLOCK) != 0) {
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::INTERNAL);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const int connected = ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected != 0 && errno != EINPROGRESS) {
        const Error error = socket_error(errno);
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(error);
    }
    if (connected != 0) {
        if (!wait_socket(socket, POLLOUT, deadline)) {
            (void)::close(socket);
            return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::TIMEOUT);
        }
        int socket_error_value = 0;
        socklen_t error_length = sizeof(socket_error_value);
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error_value, &error_length) != 0 ||
            socket_error_value != 0) {
            const Error error = socket_error(socket_error_value == 0 ? errno : socket_error_value);
            (void)::close(socket);
            return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(error);
        }
    }
    struct stat endpoint_after{};
    if (secure_endpoint(path, config.access, owner, endpoint_after) != EndpointCheck::ready ||
        endpoint_before.st_dev != endpoint_after.st_dev || endpoint_before.st_ino != endpoint_after.st_ino) {
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    }
#if defined(SO_PEERCRED)
    struct ucred peer{};
    socklen_t peer_length = sizeof(peer);
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) != 0 ||
        peer_length != sizeof(peer)) {
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    }
    const std::int64_t expected_uid = config.expected_peer_uid < 0 ?
        static_cast<std::int64_t>(owner) : config.expected_peer_uid;
    if (expected_uid < 0 || static_cast<std::uint64_t>(peer.uid) !=
            static_cast<std::uint64_t>(expected_uid)) {
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    uid_t peer_uid = 0;
    gid_t peer_gid = 0;
    if (::getpeereid(socket, &peer_uid, &peer_gid) != 0 ||
        static_cast<std::int64_t>(peer_uid) != (config.expected_peer_uid < 0 ?
            static_cast<std::int64_t>(owner) : config.expected_peer_uid)) {
        (void)::close(socket);
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
    }
    (void)peer_gid;
#else
    (void)config.expected_peer_uid;
    (void)::close(socket);
    return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::UNSUPPORTED);
#endif
#endif
    std::unique_ptr<wire::TimedClientTransport> transport(new (std::nothrow) SocketTransport(socket));
    if (!transport) {
#if defined(_WIN32)
        (void)::closesocket(socket);
#else
        (void)::close(socket);
#endif
        return Result<std::unique_ptr<wire::TimedClientTransport>>::failure(Error::RESOURCE_LIMIT);
    }
    return Result<std::unique_ptr<wire::TimedClientTransport>>::success(std::move(transport));
}

Result<std::unique_ptr<wire::TimedClientTransport>> connect_control(
    const EndpointConfig& config, Timeout timeout) noexcept
{
    EndpointConfig control = config;
    control.endpoint_name = "control.sock";
    return connect_endpoint(control, timeout);
}

}  // namespace dtv::userland::host_runtime
