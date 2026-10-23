// SPDX-License-Identifier: GPL-2.0-only
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

#include "detail/listener_io.h"
#include <windows/windows_security.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace dtv::userland::host_listener_context::detail {
namespace {

namespace windows_security = px4::userland::windows_security;

constexpr std::size_t kConnectionCount = 64U;
constexpr std::uint64_t kIndexMask = 0xffU;

bool ensure_winsock() noexcept
{
    static const bool ready = []() noexcept {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

Error socket_error(int value) noexcept
{
    if (value == WSAEACCES) return Error::UNSUPPORTED;
    if (value == WSAEADDRINUSE) return Error::BUSY;
    if (value == WSAECONNREFUSED) return Error::NOT_READY;
    if (value == WSAEINVAL || value == WSAENAMETOOLONG) return Error::INVALID_ARGUMENT;
    if (value == WSAEWOULDBLOCK || value == WSAETIMEDOUT) return Error::TIMEOUT;
    if (value == WSAECONNRESET || value == WSAECONNABORTED || value == WSAENOTCONN)
        return Error::DISCONNECTED;
    return Error::INTERNAL;
}

bool utf8_to_wide(const std::string& input, std::wstring& output) noexcept
{
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1,
                                             nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1,
                                 output.data(), count) == count;
}

bool wide_to_utf8(const std::wstring& input, char* output, std::size_t capacity) noexcept
{
    if (input.empty() || output == nullptr || capacity == 0U ||
        capacity > static_cast<std::size_t>(INT_MAX)) return false;
    const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.c_str(), -1, output, static_cast<int>(capacity), nullptr, nullptr);
    return count > 0;
}

bool clear_socket_inherit(SOCKET socket) noexcept
{
    return ::SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0U) != 0;
}

bool set_nonblocking(SOCKET socket) noexcept
{
    u_long enabled = 1U;
    return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

struct PathIdentity final {
    DWORD volume = 0U;
    std::uint64_t index = 0U;
    bool valid = false;
};

bool file_identity(const wchar_t* path, PathIdentity& output,
                   bool require_file = true) noexcept
{
    const HANDLE file = ::CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (require_file ? 0U : FILE_FLAG_BACKUP_SEMANTICS),
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(file, &info);
    (void)::CloseHandle(file);
    if (ok == 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (require_file && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)) return false;
    output = PathIdentity{info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow, true};
    return true;
}

bool same_identity(const PathIdentity& expected, const PathIdentity& actual) noexcept
{
    return expected.valid && actual.valid && expected.volume == actual.volume &&
           expected.index == actual.index;
}

bool verify_current_user_security(const wchar_t* path) noexcept
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || owner == nullptr || dacl == nullptr || descriptor == nullptr)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0U;
    DWORD revision = 0U;
    windows_security::SidBuffer sid_storage{};
    PSID current = windows_security::current_user_sid(sid_storage);
    bool accepted = current != nullptr && windows_security::sid_equal(owner, current) &&
        ::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
        (control & SE_DACL_PROTECTED) != 0U;
    if (accepted) {
        ACL_SIZE_INFORMATION information{};
        accepted = ::GetAclInformation(dacl, &information, sizeof(information),
                                       AclSizeInformation) != 0;
        for (DWORD index = 0U; accepted && index < information.AceCount; ++index) {
            void* raw = nullptr;
            if (::GetAce(dacl, index, &raw) == 0 || raw == nullptr) {
                accepted = false;
                break;
            }
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                PSID trustee = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
                accepted = windows_security::sid_equal(trustee, current);
            } else if (header->AceType != ACCESS_DENIED_ACE_TYPE &&
                       header->AceType != SYSTEM_AUDIT_ACE_TYPE) {
                accepted = false;
            }
        }
    }
    (void)::LocalFree(descriptor);
    return accepted;
}

bool apply_current_user_security(const wchar_t* path) noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    windows_security::SidBuffer sid_storage{};
    if (!windows_security::build_current_user_security(attributes, descriptor, acl,
                                                        sid_storage)) return false;
    PSID owner = windows_security::current_user_sid(sid_storage);
    const DWORD status = owner == nullptr ? ERROR_INVALID_OWNER :
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                PROTECTED_DACL_SECURITY_INFORMATION,
            owner, nullptr, acl, nullptr);
    (void)::LocalFree(acl);
    return status == ERROR_SUCCESS && verify_current_user_security(path);
}

bool ensure_private_directory(const std::wstring& path) noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    windows_security::SidBuffer sid_storage{};
    if (!windows_security::build_current_user_security(attributes, descriptor, acl,
                                                        sid_storage)) return false;
    const BOOL created = ::CreateDirectoryW(path.c_str(), &attributes);
    const DWORD error = created != 0 ? ERROR_SUCCESS : ::GetLastError();
    (void)::LocalFree(acl);
    if (created == 0 && error != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes_value = ::GetFileAttributesW(path.c_str());
    return attributes_value != INVALID_FILE_ATTRIBUTES &&
           (attributes_value & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes_value & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
           windows_security::verify_runtime_parent(path.c_str());
}

bool delete_if_identity(const wchar_t* path, const PathIdentity& expected) noexcept
{
    if (!expected.valid) return true;
    const HANDLE file = ::CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_FILE_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL queried = ::GetFileInformationByHandle(file, &info);
    const PathIdentity current{info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow,
        queried != 0};
    bool removed = false;
    if (queried != 0 && same_identity(expected, current) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        removed = ::SetFileInformationByHandle(file, FileDispositionInfo,
            &disposition, sizeof(disposition)) != 0;
    }
    (void)::CloseHandle(file);
    return removed;
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

class WindowsListenerIo final : public ListenerIo {
public:
    ~WindowsListenerIo() noexcept override
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
            control.access != data.access || control.expected_peer_uid != data.expected_peer_uid)
            return Error::INVALID_ARGUMENT;
        if (control.access != host_runtime::EndpointAccess::private_user) return Error::UNSUPPORTED;
        if (!ensure_winsock()) return Error::INTERNAL;
        const auto control_result = host_runtime::endpoint_path(control);
        const auto data_result = host_runtime::endpoint_path(data);
        if (!control_result || !data_result)
            return !control_result ? control_result.error() : data_result.error();
        std::wstring runtime;
        if (!utf8_to_wide(std::string(control.runtime_directory), runtime)) {
            return Error::INVALID_ARGUMENT;
        }
        const std::wstring separator = L"\\";
        std::wstring product_path = runtime + separator;
        product_path.append(control.product.begin(), control.product.end());
        std::wstring version_path = product_path + separator + L"v1";
        std::wstring instance_path = version_path + separator;
        instance_path.append(control.instance.begin(), control.instance.end());
        if (!windows_security::verify_runtime_parent(runtime.c_str()) ||
            !ensure_private_directory(product_path) || !ensure_private_directory(version_path) ||
            !ensure_private_directory(instance_path)) return Error::UNSUPPORTED;
        control_path_ = control_result.value();
        data_path_ = data_result.value();
        if (!utf8_to_wide(control_path_, control_wide_) ||
            !utf8_to_wide(data_path_, data_wide_)) return Error::INVALID_ARGUMENT;
        lock_path_ = instance_path + separator + L".listener.lock";

        SECURITY_ATTRIBUTES attributes{};
        SECURITY_DESCRIPTOR descriptor{};
        PACL acl = nullptr;
        windows_security::SidBuffer sid_storage{};
        if (!windows_security::build_current_user_security(attributes, descriptor, acl,
                                                            sid_storage)) return Error::UNSUPPORTED;
        lock_handle_ = ::CreateFileW(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lock_handle_ == INVALID_HANDLE_VALUE &&
            (::GetLastError() == ERROR_FILE_EXISTS || ::GetLastError() == ERROR_ALREADY_EXISTS)) {
            lock_handle_ = ::CreateFileW(lock_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        }
        const DWORD open_error = lock_handle_ == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        (void)::LocalFree(acl);
        if (lock_handle_ == INVALID_HANDLE_VALUE) return open_error == ERROR_SHARING_VIOLATION ?
            Error::BUSY : Error::UNSUPPORTED;
        BY_HANDLE_FILE_INFORMATION lock_info{};
        if (::GetFileInformationByHandle(lock_handle_, &lock_info) == 0 ||
            (lock_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
            !verify_current_user_security(lock_path_.c_str())) {
            (void)::CloseHandle(lock_handle_); lock_handle_ = INVALID_HANDLE_VALUE;
            return Error::UNSUPPORTED;
        }
        if (::LockFileEx(lock_handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0U, MAXDWORD, MAXDWORD, &lock_overlap_) == 0) {
            const DWORD error = ::GetLastError();
            (void)::CloseHandle(lock_handle_); lock_handle_ = INVALID_HANDLE_VALUE;
            return error == ERROR_LOCK_VIOLATION ? Error::BUSY : Error::UNSUPPORTED;
        }
        reserved_ = true;
        return Error::OK;
    }

    Error publish_pair(ListenerPair& pair) noexcept override
    {
        if (!reserved_ || published_) return Error::NOT_READY;
        if (!remove_stale(control_wide_))
            return socket_error(::WSAGetLastError());
        control_listener_ = create_listener(control_wide_, control_identity_);
        if (control_listener_ == INVALID_SOCKET) return socket_error(::WSAGetLastError());
        if (!remove_stale(data_wide_)) {
            const int error = ::WSAGetLastError();
            rollback_listeners();
            return socket_error(error);
        }
        data_listener_ = create_listener(data_wide_, data_identity_);
        if (data_listener_ == INVALID_SOCKET) {
            const int error = ::WSAGetLastError();
            rollback_listeners();
            return socket_error(error);
        }
        if (::listen(control_listener_, SOMAXCONN) != 0 || ::listen(data_listener_, SOMAXCONN) != 0) {
            const int error = ::WSAGetLastError();
            rollback_listeners();
            return socket_error(error);
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
        std::array<WSAPOLLFD, kListenerIoEventCapacity> descriptors{};
        std::array<ReadyKind, kListenerIoEventCapacity> kinds{};
        std::array<ListenerKind, kListenerIoEventCapacity> listener_kinds{};
        std::array<ConnectionToken, kListenerIoEventCapacity> connection_tokens{};
        ULONG count = 0U;
        descriptors[count] = WSAPOLLFD{control_listener_, POLLRDNORM, 0};
        kinds[count] = ReadyKind::listener; listener_kinds[count] = ListenerKind::control; ++count;
        descriptors[count] = WSAPOLLFD{data_listener_, POLLRDNORM, 0};
        kinds[count] = ReadyKind::listener; listener_kinds[count] = ListenerKind::data; ++count;
        for (std::size_t index = 0U; index < connections_.size(); ++index) {
            const auto& slot = connections_[index];
            if (slot.socket == INVALID_SOCKET) continue;
            short requested = 0;
            if ((slot.interest & ready_read) != 0U) requested |= POLLRDNORM;
            if ((slot.interest & ready_write) != 0U) requested |= POLLWRNORM;
            descriptors[count] = WSAPOLLFD{slot.socket, requested, 0};
            kinds[count] = ReadyKind::connection;
            connection_tokens[count] = ConnectionToken{token_for(index, slot.generation)};
            ++count;
        }
        const SOCKET wake_socket = static_cast<SOCKET>(worker_wake);
        if (worker_wake != ::px4::userland::ipc::posix::kInvalidWakeHandle) {
            descriptors[count] = WSAPOLLFD{wake_socket, POLLRDNORM, 0};
            kinds[count] = ReadyKind::worker_wake;
            ++count;
        }
        const int wait_ms = static_cast<int>(std::min<std::uint32_t>(timeout.milliseconds, 0x7fffffffU));
        int result = 0;
        do {
            result = ::WSAPoll(descriptors.data(), count, wait_ms);
        } while (result == SOCKET_ERROR && ::WSAGetLastError() == WSAEINTR);
        if (result == SOCKET_ERROR) return socket_error(::WSAGetLastError());
        for (ULONG index = 0U; index < count; ++index) {
            const short flags = descriptors[index].revents;
            if (flags == 0) continue;
            if (event_count >= event_capacity) { event_count = 0U; return Error::RESOURCE_LIMIT; }
            std::uint8_t mapped = 0U;
            if ((flags & (POLLRDNORM | POLLRDBAND)) != 0) mapped |= ready_read;
            if ((flags & (POLLWRNORM | POLLWRBAND)) != 0) mapped |= ready_write;
            if ((flags & (POLLERR | POLLNVAL)) != 0) mapped |= ready_error;
            if ((flags & POLLHUP) != 0) mapped |= ready_hangup;
            events[event_count++] = ReadyEvent{kinds[index], listener_kinds[index],
                index < 2U ? ListenerToken{index == 0U ? 1U : 2U} : ListenerToken{},
                connection_tokens[index], mapped};
            if ((flags & POLLNVAL) != 0) return Error::INTERNAL;
        }
        return Error::OK;
    }

    AcceptResult accept(ListenerToken listener) noexcept override
    {
        const SOCKET source = listener.value == 1U ? control_listener_ :
                              listener.value == 2U ? data_listener_ : INVALID_SOCKET;
        if (source == INVALID_SOCKET) return AcceptResult{IoStatus::failure, {}, Error::INVALID_ARGUMENT};
        const SOCKET accepted = ::accept(source, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            const int error = ::WSAGetLastError();
            if (error == WSAEINTR || error == WSAEWOULDBLOCK)
                return AcceptResult{IoStatus::would_block, {}, Error::OK};
            return AcceptResult{IoStatus::failure, {}, socket_error(error)};
        }
        if (!clear_socket_inherit(accepted) || !set_nonblocking(accepted)) {
            const int error = ::WSAGetLastError();
            (void)::closesocket(accepted);
            return AcceptResult{IoStatus::failure, {}, socket_error(error)};
        }
        for (std::size_t index = 0U; index < connections_.size(); ++index) {
            auto& slot = connections_[index];
            if (slot.socket != INVALID_SOCKET) continue;
            if (slot.generation == 0U) slot.generation = 1U;
            slot.socket = accepted;
            slot.interest = ready_read;
            return AcceptResult{IoStatus::progress,
                ConnectionToken{token_for(index, slot.generation)}, Error::OK};
        }
        (void)::closesocket(accepted);
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
        if (output.data == nullptr || output.size == 0U || !connection_slot(connection,index,generation))
            return IoResult{IoStatus::failure, 0U, Error::INVALID_ARGUMENT};
        const int count = ::recv(connections_[index].socket, reinterpret_cast<char*>(output.data),
            static_cast<int>(std::min<std::size_t>(output.size, INT_MAX)), 0);
        if (count > 0) return IoResult{IoStatus::progress, static_cast<std::size_t>(count), Error::OK};
        if (count == 0) return IoResult{IoStatus::eof, 0U, Error::OK};
        const int error = ::WSAGetLastError();
        if (error == WSAEINTR || error == WSAEWOULDBLOCK)
            return IoResult{IoStatus::would_block, 0U, Error::OK};
        return IoResult{IoStatus::failure, 0U, socket_error(error)};
    }

    IoResult write_some(ConnectionToken connection, ByteView input) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if (input.data == nullptr || input.size == 0U || !connection_slot(connection,index,generation))
            return IoResult{IoStatus::failure, 0U, Error::INVALID_ARGUMENT};
        const int count = ::send(connections_[index].socket,
            reinterpret_cast<const char*>(input.data),
            static_cast<int>(std::min<std::size_t>(input.size, INT_MAX)), 0);
        if (count > 0) return IoResult{IoStatus::progress, static_cast<std::size_t>(count), Error::OK};
        const int error = ::WSAGetLastError();
        if (count == SOCKET_ERROR && (error == WSAEINTR || error == WSAEWOULDBLOCK))
            return IoResult{IoStatus::would_block, 0U, Error::OK};
        return IoResult{IoStatus::failure, 0U,
            count == 0 ? Error::DISCONNECTED : socket_error(error)};
    }

    Error close_connection(ConnectionToken connection) noexcept override
    {
        std::size_t index = 0U;
        std::uint32_t generation = 0U;
        if (!connection_slot(connection,index,generation)) return Error::NOT_FOUND;
        auto& slot = connections_[index];
        (void)::shutdown(slot.socket, SD_BOTH);
        if (::closesocket(slot.socket) != 0) {
            const int error = ::WSAGetLastError();
            slot.socket = INVALID_SOCKET; advance_generation(slot);
            return socket_error(error);
        }
        slot.socket = INVALID_SOCKET; advance_generation(slot);
        return Error::OK;
    }

    Error unpublish_pair(ListenerPair pair) noexcept override
    {
        if ((pair.control.value != 0U && pair.control.value != 1U) ||
            (pair.data.value != 0U && pair.data.value != 2U)) return Error::INVALID_ARGUMENT;
        for (auto& slot : connections_) {
            if (slot.socket != INVALID_SOCKET) {
                (void)::shutdown(slot.socket, SD_BOTH);
                (void)::closesocket(slot.socket);
                slot.socket = INVALID_SOCKET;
                advance_generation(slot);
            }
        }
        const int control_error = close_listener(control_listener_);
        const int data_error = close_listener(data_listener_);
        (void)delete_if_identity(control_wide_.c_str(), control_identity_);
        (void)delete_if_identity(data_wide_.c_str(), data_identity_);
        control_identity_ = {}; data_identity_ = {}; published_ = false;
        return control_error != 0 ? socket_error(control_error) :
               data_error != 0 ? socket_error(data_error) : Error::OK;
    }

    Error release_namespace() noexcept override
    {
        if (lock_handle_ == INVALID_HANDLE_VALUE) { reserved_ = false; return Error::OK; }
        (void)::UnlockFileEx(lock_handle_, 0U, MAXDWORD, MAXDWORD, &lock_overlap_);
        const BOOL result = ::CloseHandle(lock_handle_);
        const DWORD error = ::GetLastError();
        lock_handle_ = INVALID_HANDLE_VALUE; reserved_ = false;
        return result != 0 ? Error::OK : (error == ERROR_SUCCESS ? Error::INTERNAL : Error::UNSUPPORTED);
    }

private:
    struct ConnectionSlot final {
        SOCKET socket = INVALID_SOCKET;
        std::uint32_t generation = 1U;
        std::uint8_t interest = ready_read;
    };

    bool connection_slot(ConnectionToken token, std::size_t& index,
                         std::uint32_t& generation) noexcept
    {
        return decode_token(token,index,generation) &&
            connections_[index].socket != INVALID_SOCKET &&
            connections_[index].generation == generation;
    }

    static void advance_generation(ConnectionSlot& slot) noexcept
    {
        ++slot.generation;
        if (slot.generation == 0U) slot.generation = 1U;
    }

    static int close_listener(SOCKET& socket) noexcept
    {
        if (socket == INVALID_SOCKET) return 0;
        const SOCKET current = socket; socket = INVALID_SOCKET;
        return ::closesocket(current) == 0 ? 0 : ::WSAGetLastError();
    }

    static bool remove_stale(const std::wstring& path) noexcept
    {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
            ::WSASetLastError(WSAEACCES);
            return false;
        }
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
            !verify_current_user_security(path.c_str())) {
            ::WSASetLastError(WSAEACCES);
            return false;
        }
        PathIdentity before{};
        if (!file_identity(path.c_str(), before)) {
            ::WSASetLastError(WSAEACCES);
            return false;
        }
        SOCKADDR_UN address{}; address.sun_family = AF_UNIX;
        if (!wide_to_utf8(path, address.sun_path, sizeof(address.sun_path))) {
            ::WSASetLastError(WSAEINVAL);
            return false;
        }
        const SOCKET probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe == INVALID_SOCKET) return false;
        (void)set_nonblocking(probe);
        const int connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        int error = connected == 0 ? 0 : ::WSAGetLastError();
        if (connected != 0 && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS)) {
            WSAPOLLFD descriptor{probe, POLLWRNORM, 0};
            if (::WSAPoll(&descriptor, 1U, 100) > 0) {
                int socket_status = 0; int size = sizeof(socket_status);
                if (::getsockopt(probe, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&socket_status), &size) == 0) error = socket_status;
            }
        }
        (void)::closesocket(probe);
        if (connected == 0 || error != WSAECONNREFUSED) {
            ::WSASetLastError(WSAEADDRINUSE);
            return false;
        }
        PathIdentity after{};
        if (!file_identity(path.c_str(), after) || !same_identity(before, after)) {
            ::WSASetLastError(WSAEACCES);
            return false;
        }
        if (!delete_if_identity(path.c_str(), before)) {
            ::WSASetLastError(WSAEACCES);
            return false;
        }
        return true;
    }

    SOCKET create_listener(const std::wstring& path, PathIdentity& identity) noexcept
    {
        SOCKADDR_UN address{}; address.sun_family = AF_UNIX;
        if (!wide_to_utf8(path, address.sun_path, sizeof(address.sun_path))) {
            ::WSASetLastError(WSAEINVAL); return INVALID_SOCKET;
        }
        const SOCKET listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener == INVALID_SOCKET) return INVALID_SOCKET;
        if (!clear_socket_inherit(listener) || !set_nonblocking(listener) ||
            ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const int error = ::WSAGetLastError();
            (void)::closesocket(listener); ::WSASetLastError(error); return INVALID_SOCKET;
        }
        if (!file_identity(path.c_str(), identity) ||
            !apply_current_user_security(path.c_str()) ||
            !verify_current_user_security(path.c_str())) {
            const int error = ::WSAGetLastError();
            (void)::closesocket(listener);
            (void)delete_if_identity(path.c_str(), identity);
            identity = {}; ::WSASetLastError(error == 0 ? WSAEACCES : error);
            return INVALID_SOCKET;
        }
        return listener;
    }

    void rollback_listeners() noexcept
    {
        (void)close_listener(control_listener_); (void)close_listener(data_listener_);
        (void)delete_if_identity(control_wide_.c_str(), control_identity_);
        (void)delete_if_identity(data_wide_.c_str(), data_identity_);
        control_identity_ = {}; data_identity_ = {};
    }

    std::array<ConnectionSlot, kConnectionCount> connections_{};
    std::string control_path_;
    std::string data_path_;
    std::wstring control_wide_;
    std::wstring data_wide_;
    std::wstring lock_path_;
    PathIdentity control_identity_{};
    PathIdentity data_identity_{};
    ListenerPair pair_{};
    HANDLE lock_handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED lock_overlap_{};
    SOCKET control_listener_ = INVALID_SOCKET;
    SOCKET data_listener_ = INVALID_SOCKET;
    bool reserved_ = false;
    bool published_ = false;
};

}  // namespace

std::unique_ptr<ListenerIo> create_listener_io() noexcept
{
    return std::unique_ptr<ListenerIo>(new (std::nothrow) WindowsListenerIo());
}

}  // namespace dtv::userland::host_listener_context::detail
