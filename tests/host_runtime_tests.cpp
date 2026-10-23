// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_runtime/family.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/host_facade/px4d_signals.h"
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <sddl.h>
#include "px4/windows_tuner_nonce.h"
#include "windows/windows_security.h"
#else
#include "px4/posix_tuner_nonce.h"
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

namespace {

#if defined(_WIN32)
bool utf8(const wchar_t* value, std::string& output)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    const bool converted = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
        output.data(), size, nullptr, nullptr) == size;
    if (converted) output.resize(static_cast<std::size_t>(size - 1));
    return converted;
}

bool add_everyone_allow(const wchar_t* path)
{
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
            &descriptor) != ERROR_SUCCESS) return false;
    PSID everyone = nullptr;
    if (::ConvertStringSidToSidW(L"S-1-1-0", &everyone) == 0) {
        (void)::LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
    PACL updated = nullptr;
    const DWORD acl_result = ::SetEntriesInAclW(1U, &access, current, &updated);
    (void)::LocalFree(everyone);
    (void)::LocalFree(descriptor);
    if (acl_result != ERROR_SUCCESS || updated == nullptr) return false;
    const DWORD set_result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated, nullptr);
    (void)::LocalFree(updated);
    return set_result == ERROR_SUCCESS;
}

bool socket_transport_test()
{
    struct WinsockLifecycle final {
        WSADATA data{};
        bool ready = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        ~WinsockLifecycle() { if (ready) (void)::WSACleanup(); }
    } winsock;
    if (!winsock.ready) return false;
    wchar_t temporary[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, temporary) == 0U) return false;
    wchar_t directory[MAX_PATH]{};
    if (::GetTempFileNameW(temporary, L"dtv", 0U, directory) == 0U) return false;
    (void)::DeleteFileW(directory);
    if (::CreateDirectoryW(directory, nullptr) == 0) return false;
    wchar_t runtime[MAX_PATH]{};
    if (::swprintf_s(runtime, MAX_PATH, L"%ls\\rt", directory) < 0 ||
        ::CreateDirectoryW(runtime, nullptr) == 0) return false;
    wchar_t product[MAX_PATH]{};
    wchar_t version[MAX_PATH]{};
    wchar_t instance[MAX_PATH]{};
    if (::swprintf_s(product, MAX_PATH, L"%ls\\siano-userland", runtime) < 0 ||
        ::CreateDirectoryW(product, nullptr) == 0 ||
        ::swprintf_s(version, MAX_PATH, L"%ls\\v1", product) < 0 ||
        ::CreateDirectoryW(version, nullptr) == 0 ||
        ::swprintf_s(instance, MAX_PATH, L"%ls\\test", version) < 0 ||
        ::CreateDirectoryW(instance, nullptr) == 0) return false;
    std::string runtime_utf8;
    if (!utf8(runtime, runtime_utf8)) return false;
    dtv::userland::host_runtime::EndpointConfig config{runtime_utf8, "siano-userland", "test"};
    const auto path = dtv::userland::host_runtime::control_endpoint_path(config);
    if (!path) return false;
    std::vector<wchar_t> wide_path(static_cast<std::size_t>(path.value().size()) + 1U);
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.value().c_str(), -1,
            wide_path.data(), static_cast<int>(wide_path.size())) <= 0) return false;
    SOCKET server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) return false;
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    if (path.value().size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.value().c_str(), path.value().size() + 1U);
    if (::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(server, 4) != 0 ||
        !px4::userland::windows_security::set_current_user_owner_and_dacl(wide_path.data())) {
        (void)::closesocket(server);
        return false;
    }
    auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
    if (!transport) {
        (void)::closesocket(server);
        return false;
    }
    SOCKET peer = ::accept(server, nullptr, nullptr);
    if (peer == INVALID_SOCKET) return false;
    const std::uint8_t byte = 0x5aU;
    const bool sent = ::send(peer, reinterpret_cast<const char*>(&byte), 1, 0) == 1;
    (void)::closesocket(peer);
    std::array<std::uint8_t, 1U> received{};
    const auto read = transport.value()->read_some(
        dtv::userland::MutableByteView{received.data(), received.size()}, {1000U});
    std::array<std::uint8_t, 1U> after_eof{};
    const auto eof = transport.value()->read_some(
        dtv::userland::MutableByteView{after_eof.data(), after_eof.size()}, {1000U});
    transport.value()->close();
    const bool rejected_acl = add_everyone_allow(wide_path.data()) &&
        !dtv::userland::host_runtime::connect_control(config, {1000U});
    auto shared_config = config;
    shared_config.access = dtv::userland::host_runtime::EndpointAccess::shared_group;
    const bool rejected_group = !dtv::userland::host_runtime::connect_control(
        shared_config, {1000U});
    if (sent && read && received[0] == byte)
        std::puts("windows_current_user_endpoint_connection=PASS");
    if (rejected_acl)
        std::puts("windows_unapproved_everyone_allow_ace_rejected=PASS");
    if (rejected_group)
        std::puts("windows_shared_group_access_rejected=PASS");
    (void)::closesocket(server);
    (void)::DeleteFileW(wide_path.data());
    (void)::RemoveDirectoryW(instance);
    (void)::RemoveDirectoryW(version);
    (void)::RemoveDirectoryW(product);
    (void)::RemoveDirectoryW(runtime);
    (void)::RemoveDirectoryW(directory);
    return sent && read && read.value() == 1U && received[0] == byte &&
           eof && eof.value() == 0U && rejected_acl &&
           rejected_group;
}

bool windows_control_signal_test()
{
    if (!px4::userland::px4d::install_signal_handlers()) return false;
    const bool handled = px4::userland::px4d::dispatch_console_control_for_test(
        CTRL_BREAK_EVENT);
    const bool stopped = px4::userland::px4d::stop_requested();
    if (handled && stopped) std::puts("windows_ctrl_break_handler_lifecycle=PASS");
    return handled && stopped;
}
#else
bool make_directory(const std::string& path)
{
    return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), 0700) == 0;
}

bool socket_transport_test()
{
    std::array<char, 64U> temporary{};
    const char pattern[] = "/tmp/dtv-client-runtime-XXXXXX";
    std::memcpy(temporary.data(), pattern, sizeof(pattern));
    char* root_value = ::mkdtemp(temporary.data());
    if (root_value == nullptr) return false;
    const std::string root(root_value);
    const std::string product = root + "/siano-userland";
    const std::string version = product + "/v1";
    const std::string instance = version + "/test";
    bool ok = make_directory(product) && make_directory(version) && make_directory(instance);
    dtv::userland::host_runtime::EndpointConfig config{root, "siano-userland", "test"};
    const auto path = dtv::userland::host_runtime::control_endpoint_path(config);
    int server = -1;
    if (!ok || !path) ok = false;
    else {
        server = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (server < 0 || path.value().size() >= sizeof(address.sun_path)) ok = false;
        else {
            std::memcpy(address.sun_path, path.value().c_str(), path.value().size() + 1U);
            ok = ::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                 ::chmod(path.value().c_str(), 0600) == 0 && ::listen(server, 4) == 0;
        }
    }
    if (ok) {
        auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
        if (!transport) ok = false;
        else {
            const int peer = ::accept(server, nullptr, nullptr);
            const std::uint8_t byte = 0x5aU;
            ok = peer >= 0 && ::send(peer, &byte, 1U, 0) == 1;
            if (peer >= 0) (void)::close(peer);
            std::array<std::uint8_t, 1U> received{};
            const auto read = transport.value()->read_some(
                dtv::userland::MutableByteView{received.data(), received.size()}, {1000U});
            std::array<std::uint8_t, 1U> after_eof{};
            const auto eof = transport.value()->read_some(
                dtv::userland::MutableByteView{after_eof.data(), after_eof.size()}, {1000U});
            ok = ok && read && read.value() == 1U && received[0] == byte &&
                 eof && eof.value() == 0U;
            if (ok) std::puts("posix_buffered_hup_payload_then_eof=PASS");
            transport.value()->close();
        }
    }
    if (ok) {
        const auto owner = static_cast<std::int64_t>(::geteuid());
        config.expected_peer_uid = owner == 0 ? 1 : 0;
        ok = !dtv::userland::host_runtime::connect_control(config, {1000U});
        config.expected_peer_uid = -1;
        if (::geteuid() == 0) {
            ok = ok && ::chown(path.value().c_str(), 1, static_cast<gid_t>(-1)) == 0 &&
                 !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
                 ::chown(path.value().c_str(), 0, static_cast<gid_t>(-1)) == 0;
        }
        ok = ok && ::chmod(path.value().c_str(), 0666) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U});
        if (ok) std::puts("posix_peer_owner_mode_rejections=PASS");
    }
    if (server >= 0) (void)::close(server);
    if (path) (void)::unlink(path.value().c_str());
    (void)::rmdir(instance.c_str());
    (void)::rmdir(version.c_str());
    (void)::rmdir(product.c_str());
    (void)::rmdir(root.c_str());
    return ok;
}
#endif

}  // namespace

int main()
{
#if defined(_WIN32)
    if (!windows_control_signal_test()) {
        std::fputs("Windows CTRL_BREAK handler fixture failed\n", stderr);
        return 1;
    }
    px4::userland::ipc::windows::WindowsTunerNonceSource source;
#else
    px4::userland::ipc::posix::PosixTunerNonceSource source;
#endif
    const auto first = source.generate();
    const auto second = source.generate();
    if (!first || !second || first.value() == second.value()) {
        std::fputs("platform nonce lifecycle failed\n", stderr);
        return 1;
    }
    if (!px4::userland::px4d::install_signal_handlers()) {
        std::fputs("signal handler install failed\n", stderr);
        return 1;
    }
#if defined(_WIN32)
    px4::userland::px4d::request_stop();
#else
    if (std::raise(SIGTERM) != 0) return 1;
#endif
    if (!px4::userland::px4d::stop_requested()) {
        std::fputs("stop signal was not observed\n", stderr);
        return 1;
    }
    if (!socket_transport_test()) {
        std::fputs("control socket security/transport fixture failed\n", stderr);
        return 1;
    }
    px4::userland::px4d::notify_cleanup_complete();
#if defined(_WIN32)
    if (!px4::userland::px4d::cleanup_complete()) return 1;
#else
    if (px4::userland::px4d::cleanup_complete()) return 1;
#endif
    return 0;
}
