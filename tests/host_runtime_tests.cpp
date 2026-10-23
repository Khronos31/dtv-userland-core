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

bool add_unapproved_allow_ace(const wchar_t* path, BYTE ace_type, DWORD object_flags)
{
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
            &descriptor) != ERROR_SUCCESS || current == nullptr) return false;
    PSID everyone = nullptr;
    if (::ConvertStringSidToSidW(L"S-1-1-0", &everyone) == 0) {
        if (descriptor != nullptr) (void)::LocalFree(descriptor);
        return false;
    }
    const DWORD sid_size = ::GetLengthSid(everyone);
    const bool object_ace = ace_type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
        ace_type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
    const std::size_t guid_count = object_ace ?
        static_cast<std::size_t>((object_flags & ACE_OBJECT_TYPE_PRESENT) != 0U) +
        static_cast<std::size_t>((object_flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0U) : 0U;
    const std::size_t sid_offset = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) +
        (object_ace ? sizeof(DWORD) + guid_count * sizeof(GUID) : 0U);
    const std::size_t ace_size = sid_offset + sid_size;
    std::vector<std::uint8_t> ace(ace_size, 0U);
    auto* header = reinterpret_cast<ACE_HEADER*>(ace.data());
    header->AceType = ace_type;
    header->AceSize = static_cast<WORD>(ace_size);
    *reinterpret_cast<ACCESS_MASK*>(ace.data() + sizeof(ACE_HEADER)) = GENERIC_ALL;
    if (object_ace)
        *reinterpret_cast<DWORD*>(ace.data() + sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) =
            object_flags;
    const bool copied_sid = ::CopySid(sid_size, ace.data() + sid_offset, everyone) != 0;
    (void)::LocalFree(everyone);
    if (!copied_sid) {
        if (descriptor != nullptr) (void)::LocalFree(descriptor);
        return false;
    }
    ACL_SIZE_INFORMATION size_info{};
    const bool read_size = ::GetAclInformation(current, &size_info, sizeof(size_info),
        AclSizeInformation) != 0;
    const std::size_t acl_capacity = read_size ?
        static_cast<std::size_t>(size_info.AclBytesInUse) + ace_size + 16U : 0U;
    std::vector<std::uint8_t> acl_storage(acl_capacity, 0U);
    PACL updated = reinterpret_cast<PACL>(acl_storage.data());
    bool built = read_size && ::InitializeAcl(updated, static_cast<DWORD>(acl_capacity),
        ACL_REVISION_DS) != 0;
    for (DWORD index = 0U; built && index < size_info.AceCount; ++index) {
        void* existing = nullptr;
        built = ::GetAce(current, index, &existing) != 0 && existing != nullptr &&
            ::AddAce(updated, ACL_REVISION_DS, MAXDWORD, existing,
                     static_cast<ACE_HEADER*>(existing)->AceSize) != 0;
    }
    if (built)
        built = ::AddAce(updated, ACL_REVISION_DS, MAXDWORD, ace.data(),
                         static_cast<DWORD>(ace.size())) != 0;
    const DWORD set_result = built ? ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated, nullptr) : ERROR_INVALID_ACL;
    if (descriptor != nullptr) (void)::LocalFree(descriptor);
    return set_result == ERROR_SUCCESS;
}

bool add_sid_start_mismatch_object_allow(const wchar_t* path)
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return false;
    DWORD token_size = 0U;
    (void)::GetTokenInformation(token, TokenUser, nullptr, 0U, &token_size);
    std::vector<std::uint8_t> token_storage(token_size, 0U);
    const bool have_user = token_size != 0U &&
        ::GetTokenInformation(token, TokenUser, token_storage.data(), token_size,
                              &token_size) != 0;
    const PSID user = have_user ?
        reinterpret_cast<TOKEN_USER*>(token_storage.data())->User.Sid : nullptr;
    const DWORD user_size = user != nullptr ? ::GetLengthSid(user) : 0U;
    (void)::CloseHandle(token);

    PSID everyone = nullptr;
    if (user == nullptr || ::ConvertStringSidToSidW(L"S-1-1-0", &everyone) == 0)
        return false;
    constexpr DWORD object_flags = ACE_OBJECT_TYPE_PRESENT;
    const std::size_t fixed_sid_offset = offsetof(ACCESS_ALLOWED_OBJECT_ACE, SidStart);
    const std::size_t actual_sid_offset = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) +
        sizeof(DWORD) + sizeof(GUID);
    const DWORD everyone_size = ::GetLengthSid(everyone);
    const bool layout_fits = user_size != 0U && actual_sid_offset + everyone_size <=
        fixed_sid_offset;
    const std::size_t ace_size = fixed_sid_offset + user_size;
    std::vector<std::uint8_t> ace(layout_fits ? ace_size : 0U, 0U);
    bool built = layout_fits;
    if (built) {
        auto* header = reinterpret_cast<ACE_HEADER*>(ace.data());
        header->AceType = ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
        header->AceSize = static_cast<WORD>(ace.size());
        *reinterpret_cast<ACCESS_MASK*>(ace.data() + sizeof(ACE_HEADER)) = GENERIC_ALL;
        *reinterpret_cast<DWORD*>(ace.data() + sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) =
            object_flags;
        built = ::CopySid(everyone_size, ace.data() + actual_sid_offset, everyone) != 0 &&
            ::CopySid(user_size, ace.data() + fixed_sid_offset,
                      const_cast<PSID>(user)) != 0 &&
            ::EqualSid(ace.data() + actual_sid_offset, everyone) != 0 &&
            ::EqualSid(ace.data() + fixed_sid_offset, user) != 0;
    }
    if (!built) {
        (void)::LocalFree(everyone);
        return false;
    }

    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
            &descriptor) != ERROR_SUCCESS || current == nullptr) {
        if (descriptor != nullptr) (void)::LocalFree(descriptor);
        return false;
    }
    ACL_SIZE_INFORMATION size_info{};
    const bool read_size = ::GetAclInformation(current, &size_info, sizeof(size_info),
        AclSizeInformation) != 0;
    const std::size_t acl_capacity = read_size ?
        static_cast<std::size_t>(size_info.AclBytesInUse) + ace.size() + 16U : 0U;
    std::vector<std::uint8_t> acl_storage(acl_capacity, 0U);
    PACL updated = reinterpret_cast<PACL>(acl_storage.data());
    built = read_size && ::InitializeAcl(updated, static_cast<DWORD>(acl_capacity),
        ACL_REVISION_DS) != 0;
    for (DWORD index = 0U; built && index < size_info.AceCount; ++index) {
        void* existing = nullptr;
        built = ::GetAce(current, index, &existing) != 0 && existing != nullptr &&
            ::AddAce(updated, ACL_REVISION_DS, MAXDWORD, existing,
                     static_cast<ACE_HEADER*>(existing)->AceSize) != 0;
    }
    if (built)
        built = ::AddAce(updated, ACL_REVISION_DS, MAXDWORD, ace.data(),
                         static_cast<DWORD>(ace.size())) != 0;
    const DWORD set_result = built ? ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated, nullptr) : ERROR_INVALID_ACL;
    (void)::LocalFree(descriptor);
    bool verified_layout = false;
    PACL installed = nullptr;
    PSECURITY_DESCRIPTOR installed_descriptor = nullptr;
    if (set_result == ERROR_SUCCESS && ::GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &installed, nullptr, &installed_descriptor) == ERROR_SUCCESS &&
        installed != nullptr) {
        std::printf("windows_sid_start_fixture_readback_acl_count=%lu\n",
            static_cast<unsigned long>(installed->AceCount));
        for (DWORD index = 0U; index < installed->AceCount; ++index) {
            void* raw = nullptr;
            if (::GetAce(installed, index, &raw) == 0 || raw == nullptr) continue;
            const auto* header = static_cast<ACE_HEADER*>(raw);
            std::printf("windows_sid_start_fixture_readback_ace index=%lu type=%u size=%u\n",
                static_cast<unsigned long>(index), static_cast<unsigned>(header->AceType),
                static_cast<unsigned>(header->AceSize));
            if (header->AceType != ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) continue;
            const auto* bytes = static_cast<const std::uint8_t*>(raw);
            if (header->AceSize < sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) + sizeof(DWORD))
                continue;
            const DWORD installed_flags = *reinterpret_cast<const DWORD*>(
                bytes + sizeof(ACE_HEADER) + sizeof(ACCESS_MASK));
            std::printf("windows_sid_start_fixture_readback_object flags=%lu\n",
                static_cast<unsigned long>(installed_flags));
            if (header->AceSize < actual_sid_offset + everyone_size ||
                header->AceSize < fixed_sid_offset + user_size) continue;
            const bool actual_sid_is_everyone = ::EqualSid(const_cast<std::uint8_t*>(
                bytes + actual_sid_offset), everyone) != 0;
            const bool fixed_sid_is_user = ::EqualSid(const_cast<std::uint8_t*>(
                bytes + fixed_sid_offset), user) != 0;
            std::printf("windows_sid_start_fixture_readback size=%u flags=%lu actual_everyone=%u fixed_user=%u\n",
                static_cast<unsigned>(header->AceSize),
                static_cast<unsigned long>(installed_flags),
                actual_sid_is_everyone ? 1U : 0U,
                fixed_sid_is_user ? 1U : 0U);
            verified_layout = installed_flags == object_flags &&
                actual_sid_is_everyone && fixed_sid_is_user;
            if (verified_layout) break;
        }
    }
    if (installed_descriptor != nullptr) (void)::LocalFree(installed_descriptor);
    (void)::LocalFree(everyone);
    if (!verified_layout)
        std::fputs("windows_sid_start_fixture_readback=NO_MATCH\n", stderr);
    return set_result == ERROR_SUCCESS && verified_layout;
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
    bool rejected_nonstandard_aces = true;
    for (const BYTE type : std::array<BYTE, 3U>{ACCESS_ALLOWED_OBJECT_ACE_TYPE,
            ACCESS_ALLOWED_CALLBACK_ACE_TYPE, ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE}) {
        const DWORD flags = type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
            type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE ?
            ACE_OBJECT_TYPE_PRESENT | ACE_INHERITED_OBJECT_TYPE_PRESENT : 0U;
        rejected_nonstandard_aces = rejected_nonstandard_aces &&
            px4::userland::windows_security::set_current_user_owner_and_dacl(
                wide_path.data()) &&
            add_unapproved_allow_ace(wide_path.data(), type, flags) &&
            !dtv::userland::host_runtime::connect_control(config, {1000U});
    }
    const bool installed_sid_start_mismatch =
        px4::userland::windows_security::set_current_user_owner_and_dacl(
            wide_path.data()) &&
        add_sid_start_mismatch_object_allow(wide_path.data());
    if (!installed_sid_start_mismatch)
        std::fputs("windows_sid_start_fixture_layout=FAIL\n", stderr);
    const bool rejected_sid_start_mismatch = installed_sid_start_mismatch &&
        !dtv::userland::host_runtime::connect_control(config, {1000U});
    if (!rejected_sid_start_mismatch)
        std::fputs("windows_sid_start_everyone_mismatch_rejected=FAIL\n", stderr);
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
    if (rejected_nonstandard_aces)
        std::puts("windows_nonstandard_allow_aces_rejected=PASS");
    if (rejected_sid_start_mismatch)
        std::puts("windows_sid_start_everyone_mismatch_rejected=PASS");
    (void)::closesocket(server);
    (void)::DeleteFileW(wide_path.data());
    (void)::RemoveDirectoryW(instance);
    (void)::RemoveDirectoryW(version);
    (void)::RemoveDirectoryW(product);
    (void)::RemoveDirectoryW(runtime);
    (void)::RemoveDirectoryW(directory);
    return sent && read && read.value() == 1U && received[0] == byte &&
           eof && eof.value() == 0U && rejected_acl &&
           rejected_group && rejected_nonstandard_aces && rejected_sid_start_mismatch;
}

bool windows_control_signal_test()
{
    const std::uint32_t before =
        px4::userland::px4d::signal_registration_attempts_for_test();
    const std::uint32_t successful_before =
        px4::userland::px4d::successful_signal_registrations_for_test();
    px4::userland::px4d::fail_next_signal_registration_for_test();
    const bool rejected_registration =
        !px4::userland::px4d::install_signal_handlers();
    const std::uint32_t after_failure =
        px4::userland::px4d::signal_registration_attempts_for_test();
    const std::uint32_t successful_after_failure =
        px4::userland::px4d::successful_signal_registrations_for_test();
    if (successful_after_failure != successful_before) {
        std::fputs("windows_signal_success_counter_after_failure=FAIL\n", stderr);
        return false;
    }
    if (!rejected_registration || after_failure != before + 1U ||
        px4::userland::px4d::stop_requested()) return false;
    if (!px4::userland::px4d::install_signal_handlers() ||
        px4::userland::px4d::signal_registration_attempts_for_test() != before + 2U ||
        px4::userland::px4d::successful_signal_registrations_for_test() !=
            successful_before + 1U) {
        std::fputs("windows_signal_success_counter_after_api_success=FAIL\n", stderr);
        return false;
    }
    const bool handled = px4::userland::px4d::dispatch_console_control_for_test(
        CTRL_BREAK_EVENT);
    const bool stopped = px4::userland::px4d::stop_requested();
    if (handled && stopped) std::puts("windows_ctrl_break_handler_lifecycle=PASS");
    if (rejected_registration)
        std::puts("windows_console_handler_registration_failure_rejected=PASS");
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
