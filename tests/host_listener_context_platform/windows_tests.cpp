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

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace dtv::userland;
using namespace dtv::userland::host_listener_context::detail;

namespace dtv::userland::host_listener_context::detail {
Error set_connection_send_buffer_for_test(ListenerIo& io, ConnectionToken connection,
                                          int bytes) noexcept;
}

namespace {

namespace windows_security = px4::userland::windows_security;

bool check(bool value, const char* label) noexcept
{
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", label);
    return false;
}

bool to_utf8(const std::wstring& input, std::string& output) noexcept
{
    const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    std::vector<char> buffer(static_cast<std::size_t>(count));
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.c_str(), -1,
            buffer.data(), count, nullptr, nullptr) != count) return false;
    output.assign(buffer.data());
    return true;
}

bool to_wide(const std::string& input, std::wstring& output) noexcept
{
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.c_str(), -1, nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            input.c_str(), -1, output.data(), count) != count) return false;
    output.resize(static_cast<std::size_t>(count - 1));
    return true;
}

bool make_runtime(std::wstring& output, std::string& utf8) noexcept
{
    std::array<wchar_t, MAX_PATH> temp{};
    std::array<wchar_t, MAX_PATH> candidate{};
    if (::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) == 0U ||
        ::GetTempFileNameW(temp.data(), L"lcb", 0U, candidate.data()) == 0U) return false;
    (void)::DeleteFileW(candidate.data());
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    windows_security::SidBuffer sid_storage{};
    if (!windows_security::build_current_user_security(attributes, descriptor, acl,
                                                        sid_storage)) return false;
    const BOOL created = ::CreateDirectoryW(candidate.data(), &attributes);
    (void)::LocalFree(acl);
    if (created == 0 || !windows_security::set_current_user_owner_and_dacl(candidate.data()) ||
        !windows_security::verify_runtime_parent(candidate.data())) return false;
    output = candidate.data();
    return to_utf8(output, utf8);
}

host_runtime::EndpointConfig config(const std::string& root, const char* name,
                                   host_runtime::EndpointAccess access =
                                       host_runtime::EndpointAccess::private_user) noexcept
{
    return host_runtime::EndpointConfig{root, "listener-test", "instance", access, -1, name};
}

bool path_for(const host_runtime::EndpointConfig& endpoint, std::string& utf8,
              std::wstring& wide) noexcept
{
    const auto result = host_runtime::endpoint_path(endpoint);
    if (!result) return false;
    utf8 = result.value();
    return to_wide(utf8, wide);
}

bool endpoint_dacl_is_current_user_only(const wchar_t* path) noexcept
{
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS ||
        owner == nullptr || dacl == nullptr || descriptor == nullptr) return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0U;
    DWORD revision = 0U;
    windows_security::SidBuffer sid_storage{};
    PSID current = windows_security::current_user_sid(sid_storage);
    bool valid = current != nullptr && windows_security::sid_equal(owner, current) &&
        ::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
        (control & SE_DACL_PROTECTED) != 0U;
    ACL_SIZE_INFORMATION information{};
    if (valid) valid = ::GetAclInformation(dacl, &information, sizeof(information),
                                           AclSizeInformation) != 0;
    for (DWORD index = 0U; valid && index < information.AceCount; ++index) {
        void* raw = nullptr;
        if (::GetAce(dacl, index, &raw) == 0 || raw == nullptr) {
            valid = false;
            break;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            PSID trustee = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
            valid = windows_security::sid_equal(trustee, current);
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE &&
                   header->AceType != SYSTEM_AUDIT_ACE_TYPE) {
            valid = false;
        }
    }
    (void)::LocalFree(descriptor);
    return valid;
}

bool endpoint_is_af_unix_socket(const wchar_t* path) noexcept
{
    const HANDLE file = ::CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO information{};
    const BOOL queried = ::GetFileInformationByHandleEx(file, FileAttributeTagInfo,
        &information, sizeof(information));
    (void)::CloseHandle(file);
    return queried != 0 &&
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U &&
        (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
        information.ReparseTag == IO_REPARSE_TAG_AF_UNIX;
}

SOCKET connect_peer(const std::wstring& path) noexcept
{
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        path.c_str(), -1, address.sun_path, static_cast<int>(sizeof(address.sun_path)), nullptr, nullptr);
    if (count <= 0) return INVALID_SOCKET;
    const SOCKET socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == INVALID_SOCKET) return INVALID_SOCKET;
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        (void)::closesocket(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

bool test_pair_io_and_dacl() noexcept
{
    std::wstring runtime;
    std::string root;
    if (!check(make_runtime(runtime, root), "private runtime directory")) return false;
    auto control = config(root, "control.sock");
    auto data = config(root, "stream.sock");
    std::string control_utf8;
    std::string data_utf8;
    std::wstring control_path;
    std::wstring data_path;
    if (!check(path_for(control, control_utf8, control_path) &&
               path_for(data, data_utf8, data_path), "endpoint paths")) return false;
    auto io = create_listener_io();
    if (!check(io != nullptr, "Windows listener factory")) return false;
    const Error reserved = io->reserve_pair(control, data);
    if (reserved != Error::OK) {
        std::fprintf(stderr, "Windows reserve error=%u win32=%lu winsock=%d\n",
            static_cast<unsigned>(reserved), static_cast<unsigned long>(::GetLastError()),
            ::WSAGetLastError());
        const auto report_directory = [](const char* label, const std::wstring& path) noexcept {
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            const DWORD attribute_error = attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
            const bool secure = attributes != INVALID_FILE_ATTRIBUTES &&
                windows_security::verify_runtime_parent(path.c_str());
            const DWORD verify_error = ::GetLastError();
            std::fprintf(stderr, "Windows reserve path %s attrs=%lu attr_error=%lu secure=%u verify_error=%lu\n",
                label, static_cast<unsigned long>(attributes),
                static_cast<unsigned long>(attribute_error), secure ? 1U : 0U,
                static_cast<unsigned long>(verify_error));
        };
        std::wstring root_wide;
        if (to_wide(root, root_wide)) {
            report_directory("runtime", root_wide);
            report_directory("product", root_wide + L"\\listener-test");
            report_directory("version", root_wide + L"\\listener-test\\v1");
            report_directory("instance", root_wide + L"\\listener-test\\v1\\instance");
            const std::wstring lock_path = root_wide + L"\\listener-test\\v1\\instance\\.listener.lock";
            const DWORD lock_attributes = ::GetFileAttributesW(lock_path.c_str());
            std::fprintf(stderr, "Windows reserve lock attrs=%lu error=%lu\n",
                static_cast<unsigned long>(lock_attributes),
                static_cast<unsigned long>(lock_attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS));
        }
    }
    if (!check(reserved == Error::OK, "reserve Windows private pair")) return false;
    std::wstring root_wide;
    if (!check(to_wide(root, root_wide), "runtime path conversion")) return false;
    const std::wstring product_path = root_wide + L"\\listener-test";
    const std::wstring version_path = product_path + L"\\v1";
    const std::wstring instance_path = version_path + L"\\instance";
    const auto secure_directory = [](const std::wstring& path) noexcept {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
            windows_security::verify_runtime_parent(path.c_str());
    };
    if (!check(secure_directory(product_path) && secure_directory(version_path) &&
               secure_directory(instance_path),
               "private product, version, and instance directory paths")) return false;
    const std::wstring lock_path = instance_path + L"\\.listener.lock";
    const DWORD lock_attributes = ::GetFileAttributesW(lock_path.c_str());
    if (!check(lock_attributes != INVALID_FILE_ATTRIBUTES &&
               (lock_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U &&
               endpoint_dacl_is_current_user_only(lock_path.c_str()),
               "private regular lock file at instance path")) return false;
    auto competitor = create_listener_io();
    if (!check(competitor->reserve_pair(control, data) == Error::BUSY,
               "exclusive namespace lease")) return false;
    ListenerPair pair{};
    if (!check(io->publish_pair(pair) == Error::OK, "publish Windows pair")) return false;
    const DWORD control_attributes = ::GetFileAttributesW(control_path.c_str());
    const DWORD data_attributes = ::GetFileAttributesW(data_path.c_str());
    if (!check(control_attributes != INVALID_FILE_ATTRIBUTES &&
               data_attributes != INVALID_FILE_ATTRIBUTES &&
               endpoint_is_af_unix_socket(control_path.c_str()) &&
               endpoint_is_af_unix_socket(data_path.c_str()),
               "control and data AF_UNIX socket paths under instance directory")) return false;
    if (!check(io->release_namespace() == Error::BUSY &&
               competitor->reserve_pair(control, data) == Error::BUSY,
               "published pair retains exclusive namespace lease")) return false;
    if (!check(endpoint_dacl_is_current_user_only(control_path.c_str()) &&
               endpoint_dacl_is_current_user_only(data_path.c_str()),
               "protected current-user endpoint DACL readback")) return false;

    const SOCKET peer = ::socket(AF_UNIX, SOCK_STREAM, 0);
    int receive_buffer = 4096;
    if (!check(peer != INVALID_SOCKET &&
               ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer)) == 0,
               "limit Windows peer receive buffer")) return false;
    SOCKADDR_UN peer_address{};
    peer_address.sun_family = AF_UNIX;
    if (!check(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            control_path.c_str(), -1, peer_address.sun_path,
            static_cast<int>(sizeof(peer_address.sun_path)), nullptr, nullptr) > 0 &&
               ::connect(peer, reinterpret_cast<const sockaddr*>(&peer_address),
                   sizeof(peer_address)) == 0,
               "connect Windows control peer")) {
        (void)::closesocket(peer);
        return false;
    }
    std::array<ReadyEvent, kListenerIoEventCapacity> events{};
    std::size_t count = 0U;
    if (!check(io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
            events.data(), events.size(), count) == Error::OK && count == 1U &&
            events[0].kind == ReadyKind::listener && events[0].listener_kind == ListenerKind::control,
            "control readiness")) return false;
    const AcceptResult accepted = io->accept(events[0].listener);
    if (!check(accepted.status == IoStatus::progress, "accept Windows peer")) return false;
    std::array<std::uint8_t, 16U> input{};
    IoResult read = io->read_some(accepted.connection, MutableByteView{input.data(), input.size()});
    if (!check(read.status == IoStatus::would_block, "Windows would-block mapping")) return false;
    const ULONGLONG before = ::GetTickCount64();
    if (!check(io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{25U},
            events.data(), events.size(), count) == Error::OK && count == 0U &&
            ::GetTickCount64() - before >= 15U,
            "idle read-only poll does not spin on writable peer")) return false;
    if (!check(io->set_connection_interest(accepted.connection, ready_write) == Error::OK &&
            io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{100U},
                events.data(), events.size(), count) == Error::OK && count == 1U &&
            (events[0].flags & ready_write) != 0U,
            "requested Windows write readiness")) return false;
    if (!check(io->set_connection_interest(accepted.connection, ready_read) == Error::OK,
               "restore Windows read interest")) return false;

    const char fragment = 'x';
    if (!check(::send(peer, &fragment, 1, 0) == 1, "send Windows fragment")) return false;
    read = io->read_some(accepted.connection, MutableByteView{input.data(), input.size()});
    if (!check(read.status == IoStatus::progress && read.transferred == 1U && input[0] == 'x',
               "read Windows fragment")) return false;
    if (!check(set_connection_send_buffer_for_test(*io, accepted.connection, 4096) == Error::OK,
               "constrain Windows server send buffer")) return false;
    std::vector<std::uint8_t> large(12U * 1024U * 1024U, 0x5aU);
    constexpr std::size_t prefill_size = 8U * 1024U * 1024U;
    const IoResult prefill = io->write_some(accepted.connection,
        ByteView{large.data(), prefill_size});
    if (!check(prefill.status == IoStatus::progress && prefill.transferred == prefill_size,
               "prefill Windows peer receive queue")) return false;
    const IoResult written = io->write_some(accepted.connection,
        ByteView{large.data(), large.size()});
    const bool partial_write = written.status == IoStatus::progress &&
        written.transferred > 0U && written.transferred < large.size();
    if (!partial_write) {
        std::fprintf(stderr, "Windows write prefill=%zu status=%u transferred=%zu error=%u\n",
            prefill.transferred, static_cast<unsigned>(written.status), written.transferred,
            static_cast<unsigned>(written.error));
    }
    if (!check(partial_write, "partial Windows write")) return false;
    (void)::shutdown(peer, SD_SEND);
    read = io->read_some(accepted.connection, MutableByteView{input.data(), input.size()});
    if (!check(read.status == IoStatus::eof, "clean Windows EOF")) return false;
    (void)io->close_connection(accepted.connection);
    (void)::closesocket(peer);

    const SOCKET reset_peer = connect_peer(control_path);
    if (!check(reset_peer != INVALID_SOCKET &&
            io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
                events.data(), events.size(), count) == Error::OK && count == 1U,
            "connect reset peer")) return false;
    const AcceptResult reset_connection = io->accept(events[0].listener);
    if (!check(reset_connection.status == IoStatus::progress, "accept reset peer")) return false;
    linger reset{1, 0};
    (void)::setsockopt(reset_peer, SOL_SOCKET, SO_LINGER,
        reinterpret_cast<const char*>(&reset), sizeof(reset));
    (void)::closesocket(reset_peer);
    (void)io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
        events.data(), events.size(), count);
    read = io->read_some(reset_connection.connection, MutableByteView{input.data(), input.size()});
    if (!check(read.status == IoStatus::failure && read.error == Error::DISCONNECTED,
               "reset maps to typed disconnect")) return false;
    (void)io->close_connection(reset_connection.connection);
    (void)io->unpublish_pair(pair);
    const Error released = io->release_namespace();
    std::filesystem::remove_all(runtime);
    return check(released == Error::OK, "release Windows namespace");
}

bool test_stale_foreign_and_shared_boundary() noexcept
{
    std::wstring runtime;
    std::string root;
    if (!check(make_runtime(runtime, root), "stale runtime")) return false;
    auto control = config(root, "control.sock");
    auto data = config(root, "stream.sock");
    auto io = create_listener_io();
    if (!check(io->reserve_pair(control, data) == Error::OK, "reserve stale pair")) return false;
    std::string data_utf8;
    std::wstring data_path;
    if (!check(path_for(data, data_utf8, data_path), "stale data path")) return false;
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    if (!check(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data_path.c_str(), -1,
            address.sun_path, static_cast<int>(sizeof(address.sun_path)), nullptr, nullptr) > 0,
            "convert stale endpoint")) return false;
    const SOCKET stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (!check(stale != INVALID_SOCKET &&
            ::bind(stale, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "create stale AF_UNIX path")) return false;
    (void)::closesocket(stale);
    ListenerPair pair{};
    if (!check(io->publish_pair(pair) == Error::OK, "replace stale owned path")) return false;
    (void)io->unpublish_pair(pair);
    (void)io->release_namespace();
    std::filesystem::remove_all(runtime);

    if (!check(make_runtime(runtime, root), "foreign runtime")) return false;
    control = config(root, "control.sock");
    data = config(root, "stream.sock");
    io = create_listener_io();
    if (!check(io->reserve_pair(control, data) == Error::OK, "reserve foreign pair")) return false;
    std::string control_utf8;
    std::wstring control_path;
    std::wstring foreign_path;
    if (!check(path_for(control, control_utf8, control_path) &&
               path_for(data, data_utf8, foreign_path), "foreign endpoint paths")) return false;
    HANDLE foreign = ::CreateFileW(foreign_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!check(foreign != INVALID_HANDLE_VALUE, "create foreign regular endpoint")) return false;
    const char marker[] = "foreign";
    DWORD written = 0U;
    const BOOL wrote = ::WriteFile(foreign, marker, sizeof(marker), &written, nullptr);
    (void)::CloseHandle(foreign);
    if (!check(wrote != 0 && written == sizeof(marker), "write foreign path marker")) return false;
    const Error publish = io->publish_pair(pair);
    const DWORD control_attributes = ::GetFileAttributesW(control_path.c_str());
    const DWORD foreign_attributes = ::GetFileAttributesW(foreign_path.c_str());
    const bool preserved = publish != Error::OK && control_attributes == INVALID_FILE_ATTRIBUTES &&
        foreign_attributes != INVALID_FILE_ATTRIBUTES &&
        (foreign_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
    (void)io->release_namespace();
    std::filesystem::remove_all(runtime);
    if (!check(preserved, "pair rollback preserves foreign file")) return false;

    if (!check(make_runtime(runtime, root), "shared-group runtime")) return false;
    control = config(root, "control.sock", host_runtime::EndpointAccess::shared_group);
    data = config(root, "stream.sock", host_runtime::EndpointAccess::shared_group);
    io = create_listener_io();
    const Error shared_result = io->reserve_pair(control, data);
    std::filesystem::remove_all(runtime);
    return check(shared_result == Error::UNSUPPORTED, "Windows rejects shared-group access");
}

}  // namespace

int main() noexcept
{
    WSADATA data{};
    if (!check(::WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup")) return 1;
    const bool passed = test_pair_io_and_dacl() && test_stale_foreign_and_shared_boundary();
    (void)::WSACleanup();
    return passed ? 0 : 1;
}
