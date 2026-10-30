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
#include "dtv/userland/host_runtime/posix_ipc.h"
#include "px4/posix_ipc.h"
#include "px4/posix_tuner_nonce.h"
#include <csignal>
#include <cstdlib>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#endif

#include <array>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>
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
    const bool observed_os_registration =
        px4::userland::px4d::unregister_signal_handler_for_test();
    if (observed_os_registration)
        std::puts("windows_signal_os_registration_observed=PASS");
    else
        std::fputs("windows_signal_os_registration_observed=FAIL\n", stderr);
    const bool handled = px4::userland::px4d::dispatch_console_control_for_test(
        CTRL_BREAK_EVENT);
    const bool stopped = px4::userland::px4d::stop_requested();
    if (handled && stopped) std::puts("windows_ctrl_break_handler_lifecycle=PASS");
    if (rejected_registration)
        std::puts("windows_console_handler_registration_failure_rejected=PASS");
    return observed_os_registration && handled && stopped;
}
#else
bool make_directory(const std::string& path)
{
    return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), 0700) == 0;
}

class EnvironmentValue final {
public:
    explicit EnvironmentValue(const char* name) : name_(name)
    {
        const char* value = std::getenv(name);
        had_value_ = value != nullptr;
        if (had_value_) value_ = value;
    }
    ~EnvironmentValue()
    {
        if (had_value_) (void)::setenv(name_.c_str(), value_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }
    bool set(const char* value) const { return ::setenv(name_.c_str(), value, 1) == 0; }
private:
    std::string name_;
    std::string value_;
    bool had_value_ = false;
};

bool default_runtime_roots_test()
{
    std::array<char, 64U> temporary{};
    const char pattern[] = "/tmp/dtv-runtime-roots-XXXXXX";
    std::memcpy(temporary.data(), pattern, sizeof(pattern));
    char* base_value = ::mkdtemp(temporary.data());
    if (base_value == nullptr) return false;
    const std::string base(base_value);
    const std::string unsafe_xdg = base + "/unsafe-xdg";
    const std::string temporary_parent = base + "/tmp";
    bool ok = make_directory(unsafe_xdg) && make_directory(temporary_parent) &&
        ::chmod(unsafe_xdg.c_str(), 0777) == 0;
    EnvironmentValue saved_xdg("XDG_RUNTIME_DIR");
    EnvironmentValue saved_tmp("TMPDIR");
    ok = ok && saved_xdg.set(unsafe_xdg.c_str()) &&
        saved_tmp.set(temporary_parent.c_str());

    const auto roots = px4::userland::ipc::posix::client_runtime_roots();
    const std::string selected_path = temporary_parent + "/dtv-userland-" +
        std::to_string(static_cast<unsigned long>(::geteuid()));
    struct stat unsafe_info_before{};
    const bool unsafe_stat_before = ::lstat(unsafe_xdg.c_str(), &unsafe_info_before) == 0;
    const auto selected = px4::userland::ipc::posix::select_publish_runtime_root(
        "siano-userland", "test");
    struct stat unsafe_info{};
    struct stat selected_info{};
    ok = ok && unsafe_stat_before && roots && roots.value().count == 3U &&
        std::strcmp(roots.value().paths[0].data(), unsafe_xdg.c_str()) == 0 &&
        std::strcmp(roots.value().paths[1].data(), selected_path.c_str()) == 0 &&
        selected && selected.value() == selected_path &&
        ::lstat(unsafe_xdg.c_str(), &unsafe_info) == 0 &&
        (unsafe_info.st_mode & 0777) == 0777 &&
        unsafe_info.st_dev == unsafe_info_before.st_dev &&
        unsafe_info.st_ino == unsafe_info_before.st_ino &&
        unsafe_info.st_uid == unsafe_info_before.st_uid &&
        unsafe_info.st_mode == unsafe_info_before.st_mode &&
        ::lstat(selected_path.c_str(), &selected_info) == 0 &&
        selected_info.st_uid == ::geteuid() && (selected_info.st_mode & 0077) == 0;

    const bool removed_selected = ::rmdir(selected_path.c_str()) == 0;
    const bool duplicate_xdg_set = saved_xdg.set(selected_path.c_str());
    const auto duplicate_roots =
        px4::userland::ipc::posix::client_runtime_roots();
    const auto duplicate_selected = duplicate_xdg_set
        ? px4::userland::ipc::posix::select_publish_runtime_root(
            "siano-userland", "test")
        : px4::userland::Result<std::string>::failure(px4::userland::Error::INTERNAL);
    ok = ok && removed_selected && duplicate_xdg_set && duplicate_roots &&
        duplicate_roots.value().count == 2U &&
        std::strcmp(duplicate_roots.value().paths[0U].data(), selected_path.c_str()) == 0 &&
        duplicate_selected &&
        duplicate_selected.value() == selected_path &&
        saved_xdg.set(unsafe_xdg.c_str());

    const std::string long_xdg = base + "/" + std::string(90U, 'x');
    ok = ok && make_directory(long_xdg) && saved_xdg.set(long_xdg.c_str());
    dtv::userland::host_runtime::EndpointConfig long_endpoint{
        long_xdg, "siano-userland", "test"};
    dtv::userland::host_runtime::EndpointDiagnostic long_path_diagnostic{};
    const auto long_endpoint_path = dtv::userland::host_runtime::control_endpoint_path(
        long_endpoint, &long_path_diagnostic);
    dtv::userland::host_runtime::EndpointConnectDiagnostic long_connect_diagnostic{};
    const auto long_connect = dtv::userland::host_runtime::connect_control(
        {long_xdg, "siano-userland", "test"}, {1000U}, &long_connect_diagnostic);
    const auto long_path_fallback = ok
        ? px4::userland::ipc::posix::select_publish_runtime_root(
            "siano-userland", "test")
        : px4::userland::Result<std::string>::failure(px4::userland::Error::INTERNAL);
    ok = ok && !long_endpoint_path &&
        long_endpoint_path.error() == px4::userland::Error::INVALID_ARGUMENT &&
        long_path_diagnostic.reason == dtv::userland::host_runtime::EndpointDiagnosticReason::path_too_long &&
        long_path_diagnostic.path_bytes + 1U == long_path_diagnostic.required_bytes &&
        long_path_diagnostic.required_bytes > long_path_diagnostic.native_capacity &&
        long_path_diagnostic.native_capacity == sizeof(sockaddr_un{}.sun_path) &&
        !long_connect && long_connect.error() ==
            px4::userland::Error::INVALID_ARGUMENT &&
        long_connect_diagnostic.stage ==
            dtv::userland::host_runtime::EndpointConnectStage::endpoint_path &&
        long_connect_diagnostic.endpoint.path_bytes == long_path_diagnostic.path_bytes &&
        long_path_fallback && long_path_fallback.value() == selected_path &&
        saved_xdg.set(unsafe_xdg.c_str());

    dtv::userland::host_runtime::EndpointConfig invalid_instance{
        selected_path, "siano-userland", ""};
    dtv::userland::host_runtime::EndpointDiagnostic invalid_diagnostic{};
    const auto invalid_endpoint = dtv::userland::host_runtime::control_endpoint_path(
        invalid_instance, &invalid_diagnostic);
    ok = ok && !invalid_endpoint &&
        invalid_endpoint.error() == px4::userland::Error::INVALID_ARGUMENT &&
        invalid_diagnostic.reason ==
            dtv::userland::host_runtime::EndpointDiagnosticReason::instance;

    const std::size_t fixed_bytes = 1U + std::strlen("siano-userland") + 1U + 2U +
        1U + std::strlen("test") + 1U + std::strlen("control.sock");
    const std::size_t capacity = sizeof(sockaddr_un{}.sun_path);
    const std::string boundary_root = "/r" +
        std::string(capacity - fixed_bytes - 3U, 'b');
    dtv::userland::host_runtime::EndpointConfig boundary_endpoint{
        boundary_root, "siano-userland", "test"};
    dtv::userland::host_runtime::EndpointDiagnostic boundary_diagnostic{};
    const auto boundary_path = dtv::userland::host_runtime::control_endpoint_path(
        boundary_endpoint, &boundary_diagnostic);
    ok = ok && boundary_path && boundary_diagnostic.path_bytes == capacity - 1U &&
        boundary_diagnostic.required_bytes == capacity;

    const std::string over_boundary_root = boundary_root + "b";
    dtv::userland::host_runtime::EndpointConfig over_boundary_endpoint{
        over_boundary_root, "siano-userland", "test"};
    dtv::userland::host_runtime::EndpointDiagnostic over_boundary_diagnostic{};
    const auto over_boundary_path = dtv::userland::host_runtime::control_endpoint_path(
        over_boundary_endpoint, &over_boundary_diagnostic);
    ok = ok && !over_boundary_path &&
        over_boundary_path.error() == px4::userland::Error::INVALID_ARGUMENT &&
        over_boundary_diagnostic.reason ==
            dtv::userland::host_runtime::EndpointDiagnosticReason::path_too_long &&
        over_boundary_diagnostic.path_bytes == capacity &&
        over_boundary_diagnostic.required_bytes == capacity + 1U;

    const std::string product = selected_path + "/siano-userland";
    const std::string version = product + "/v1";
    const std::string instance = version + "/test";
    ok = ok && make_directory(product) && make_directory(version) &&
        make_directory(instance);
    dtv::userland::host_runtime::EndpointConfig server_config{
        selected_path, "siano-userland", "test"};
    const auto path = dtv::userland::host_runtime::control_endpoint_path(server_config);
    int server = -1;
    if (ok && path && path.value().size() < sizeof(sockaddr_un{}.sun_path)) {
        server = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.value().c_str(), path.value().size() + 1U);
        ok = server >= 0 &&
            ::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            ::chmod(path.value().c_str(), 0600) == 0 && ::listen(server, 4) == 0;
    } else {
        ok = false;
    }

    const auto connect_default = []() {
        return px4::userland::ipc::posix::SocketStream::connect(
            {nullptr, "test", "control.sock",
             px4::userland::ipc::posix::EndpointAccess::private_user,
             "siano-userland"}, px4::userland::Timeout{1000U});
    };
    auto refused_unsafe = connect_default();
    pollfd unsafe_pending{server, POLLIN, 0};
    ok = ok && !refused_unsafe && refused_unsafe.error() ==
        px4::userland::Error::UNSUPPORTED && ::poll(&unsafe_pending, 1U, 0) == 0;
    for (unsigned attempt = 0U; attempt < 2U && ok; ++attempt) {
        if (attempt == 0U) {
            ok = saved_xdg.set(long_xdg.c_str());
        } else {
            const std::string missing_xdg = base + "/missing-xdg";
            ok = saved_xdg.set(missing_xdg.c_str());
        }
        auto stream = connect_default();
        const int peer = stream ? ::accept(server, nullptr, nullptr) : -1;
        ok = ok && stream && stream.value().valid() && peer >= 0;
        if (peer >= 0) (void)::close(peer);
        if (stream) stream.value().close();
    }
    if (ok) {
        const std::string missing_xdg = base + "/missing-xdg";
        px4::userland::ipc::posix::EndpointConfig explicit_endpoint{
            missing_xdg.c_str(), "test", "control.sock",
            px4::userland::ipc::posix::EndpointAccess::private_user,
            "siano-userland"};
        const auto explicit_stream = px4::userland::ipc::posix::SocketStream::connect(
            explicit_endpoint, px4::userland::Timeout{1000U});
        pollfd pending{server, POLLIN, 0};
        ok = !explicit_stream && explicit_stream.error() ==
            px4::userland::Error::NOT_READY &&
            ::poll(&pending, 1U, 0) == 0;
    }
    const std::string missing_xdg = base + "/missing-xdg";
    const std::string temporary_link = base + "/tmp-link";
    const std::string os_root = "/tmp/dtv-userland-" +
        std::to_string(static_cast<unsigned long>(::geteuid()));
    struct stat os_root_before{};
    const bool os_root_existed = ::lstat(os_root.c_str(), &os_root_before) == 0;
    const bool os_root_was_safe = os_root_existed &&
        S_ISDIR(os_root_before.st_mode) && os_root_before.st_uid == ::geteuid() &&
        (os_root_before.st_mode & 0077) == 0;
    const bool symlink_created = ::symlink(temporary_parent.c_str(),
                                            temporary_link.c_str()) == 0;
    ok = ok && symlink_created && saved_xdg.set(missing_xdg.c_str()) &&
        saved_tmp.set(temporary_link.c_str());
    const auto os_fallback = ok
        ? px4::userland::ipc::posix::select_publish_runtime_root(
            "siano-userland", "test")
        : px4::userland::Result<std::string>::failure(px4::userland::Error::INTERNAL);
    struct stat os_root_after{};
    if (os_root_existed && !os_root_was_safe) {
        ok = ok && !os_fallback && os_fallback.error() ==
            px4::userland::Error::UNSUPPORTED &&
            ::lstat(os_root.c_str(), &os_root_after) == 0;
    } else {
        ok = ok && os_fallback && os_fallback.value() == os_root &&
            ::lstat(os_root.c_str(), &os_root_after) == 0 &&
            os_root_after.st_uid == ::geteuid() && (os_root_after.st_mode & 0077) == 0;
    }
    if (os_root_existed && os_root_after.st_ino != 0)
        ok = ok && os_root_before.st_dev == os_root_after.st_dev &&
            os_root_before.st_ino == os_root_after.st_ino &&
            os_root_before.st_mode == os_root_after.st_mode &&
            os_root_before.st_uid == os_root_after.st_uid;
    if (server >= 0) (void)::close(server);
    if (path) (void)::unlink(path.value().c_str());
    (void)::rmdir(instance.c_str());
    (void)::rmdir(version.c_str());
    (void)::rmdir(product.c_str());
    (void)::rmdir(selected_path.c_str());
    (void)::rmdir(temporary_parent.c_str());
    (void)::rmdir(long_xdg.c_str());
    (void)::rmdir(unsafe_xdg.c_str());
    (void)::unlink(temporary_link.c_str());
    (void)::rmdir(base.c_str());
    if (!os_root_existed) (void)::rmdir(os_root.c_str());
    if (ok) std::puts("posix_default_runtime_root_order_security_and_client_match=PASS");
    return ok;
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
    dtv::userland::host_runtime::EndpointConnectDiagnostic missing_diagnostic{};
    const auto missing_endpoint = dtv::userland::host_runtime::connect_control(
        config, {1000U}, &missing_diagnostic);
    ok = ok && !missing_endpoint && missing_endpoint.error() ==
        dtv::userland::Error::NOT_FOUND && missing_diagnostic.stage ==
        dtv::userland::host_runtime::EndpointConnectStage::endpoint;
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
                 ::chmod(path.value().c_str(), 0600) == 0;
            if (ok) {
                dtv::userland::host_runtime::EndpointConnectDiagnostic refused_diagnostic{};
                const auto refused = dtv::userland::host_runtime::connect_control(
                    config, {1000U}, &refused_diagnostic);
                ok = !refused && refused.error() == dtv::userland::Error::NOT_READY &&
                     refused_diagnostic.stage ==
                         dtv::userland::host_runtime::EndpointConnectStage::connect;
            }
            ok = ok && ::listen(server, 4) == 0;
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

bool shared_group_distinct_uid_test()
{
    if (::geteuid() != 0) {
        std::puts("posix_shared_group_distinct_uid=UNVERIFIED (requires root test process)");
        return true;
    }
    std::array<char, 64U> temporary{};
    const char pattern[] = "/run/dtv-shared-group-XXXXXX";
    std::memcpy(temporary.data(), pattern, sizeof(pattern));
    char* root_value = ::mkdtemp(temporary.data());
    if (root_value == nullptr) return false;
    const std::string root(root_value);
    const std::string product = root + "/px4-userland";
    const std::string version = product + "/v1";
    const std::string instance = version + "/fixture";
    bool ok = ::chmod(root.c_str(), 0750) == 0 &&
        ::mkdir(product.c_str(), 0750) == 0 && ::chmod(product.c_str(), 0750) == 0 &&
        ::mkdir(version.c_str(), 0750) == 0 && ::chmod(version.c_str(), 0750) == 0 &&
        ::mkdir(instance.c_str(), 0750) == 0 && ::chmod(instance.c_str(), 0750) == 0;
    dtv::userland::host_runtime::EndpointConfig config{
        root, "px4-userland", "fixture",
        dtv::userland::host_runtime::EndpointAccess::shared_group};
    const auto path = dtv::userland::host_runtime::control_endpoint_path(config);
    int server = -1;
    if (ok && path) {
        server = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        ok = server >= 0 && path.value().size() < sizeof(address.sun_path);
        if (ok) {
            std::memcpy(address.sun_path, path.value().c_str(), path.value().size() + 1U);
            ok = ::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                 ::chmod(path.value().c_str(), 0660) == 0 && ::listen(server, 4) == 0;
        }
    } else {
        ok = false;
    }

    if (ok) {
        const pid_t child = ::fork();
        if (child == 0) {
            const gid_t group = ::getegid();
            if (::setgroups(1U, &group) != 0 || ::setgid(65534U) != 0 ||
                ::setuid(65534U) != 0) _exit(10);
            auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
            if (!transport) _exit(11);
            const std::uint8_t byte = 0x5aU;
            const auto written = transport.value()->write_all(
                dtv::userland::ByteView{&byte, 1U}, {1000U});
            _exit(written ? 0 : 12);
        }
        int peer = -1;
        if (child > 0) peer = ::accept(server, nullptr, nullptr);
        bool accepted_peer = false;
#if defined(SO_PEERCRED)
        struct ucred credentials{};
        socklen_t credentials_length = sizeof(credentials);
        accepted_peer = peer >= 0 &&
            ::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials,
                         &credentials_length) == 0 &&
            credentials_length == sizeof(credentials) && credentials.uid == 65534U;
#elif defined(__APPLE__) || defined(__FreeBSD__)
        uid_t peer_uid = 0;
        gid_t peer_gid = 0;
        accepted_peer = peer >= 0 && ::getpeereid(peer, &peer_uid, &peer_gid) == 0 &&
                        peer_uid == 65534U;
#endif
        std::uint8_t byte = 0U;
        const bool received = peer >= 0 && ::recv(peer, &byte, sizeof(byte), 0) == 1 &&
                              byte == 0x5aU;
        if (peer >= 0) (void)::close(peer);
        int status = 0;
        const bool exited = child > 0 && ::waitpid(child, &status, 0) == child;
        ok = accepted_peer && received && exited && WIFEXITED(status) &&
             WEXITSTATUS(status) == 0;
        if (ok) std::puts("posix_shared_group_real_uid_supplementary_group_peer_uid=PASS");
    }
    if (ok) {
        const pid_t child = ::fork();
        if (child == 0) {
            if (::setgroups(0U, nullptr) != 0 || ::setgid(65534U) != 0 ||
                ::setuid(65534U) != 0) _exit(20);
            const auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
            _exit(transport ? 21 : 0);
        }
        int status = 0;
        ok = child > 0 && ::waitpid(child, &status, 0) == child &&
             WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (ok) std::puts("posix_shared_group_unrelated_uid_rejected=PASS");
    }
    if (ok) {
        const std::string symlink_path = root + "-symlink";
        ok = ::symlink(root.c_str(), symlink_path.c_str()) == 0;
        auto symlink_config = config;
        symlink_config.runtime_directory = symlink_path;
        ok = ok && !dtv::userland::host_runtime::connect_control(symlink_config, {1000U});
        (void)::unlink(symlink_path.c_str());
        if (ok) std::puts("posix_shared_group_symlink_root_rejected=PASS");
    }
    if (ok && path) {
        const std::string saved_path = path.value() + ".saved";
        ok = ::chown(path.value().c_str(), 65534U, ::getegid()) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
             ::chown(path.value().c_str(), ::geteuid(), ::getegid()) == 0 &&
             ::chown(path.value().c_str(), ::geteuid(), 65534U) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
             ::chown(path.value().c_str(), ::geteuid(), ::getegid()) == 0;
        if (ok) std::puts("posix_shared_group_endpoint_owner_gid_rejected=PASS");
        ok = ok && ::chmod(path.value().c_str(), 0666) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
             ::chmod(path.value().c_str(), 0660) == 0;
        if (ok) std::puts("posix_shared_group_endpoint_world_access_rejected=PASS");
        ok = ok && ::rename(path.value().c_str(), saved_path.c_str()) == 0 &&
             ::symlink(saved_path.c_str(), path.value().c_str()) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
             ::unlink(path.value().c_str()) == 0 &&
             ::rename(saved_path.c_str(), path.value().c_str()) == 0;
        if (ok) std::puts("posix_shared_group_endpoint_replacement_rejected=PASS");
        ok = ok && ::chmod(root.c_str(), 0770) == 0 &&
             !dtv::userland::host_runtime::connect_control(config, {1000U}) &&
             ::chmod(root.c_str(), 0750) == 0;
        if (ok) std::puts("posix_shared_group_writable_root_rejected=PASS");
    }
    if (server >= 0) (void)::close(server);
    if (path) (void)::unlink(path.value().c_str());
    (void)::rmdir(instance.c_str());
    (void)::rmdir(version.c_str());
    (void)::rmdir(product.c_str());
    (void)::rmdir(root.c_str());
    return ok;
}

bool endpoint_connect_diagnostic_text_test()
{
    FILE* output = std::tmpfile();
    if (output == nullptr) return false;
    dtv::userland::host_runtime::EndpointConnectDiagnostic diagnostic{};
    diagnostic.stage = dtv::userland::host_runtime::EndpointConnectStage::endpoint_path;
    diagnostic.endpoint.reason =
        dtv::userland::host_runtime::EndpointDiagnosticReason::path_too_long;
    diagnostic.endpoint.path_bytes = 113U;
    diagnostic.endpoint.required_bytes = 114U;
    diagnostic.endpoint.native_capacity = 108U;
    dtv::userland::host_runtime::print_endpoint_connect_diagnostic(
        output, "test-client", dtv::userland::Error::INVALID_ARGUMENT, diagnostic);
    if (std::fflush(output) != 0) {
        (void)std::fclose(output);
        return false;
    }
    std::rewind(output);
    std::array<char, 512U> message{};
    const std::size_t size = std::fread(message.data(), 1U, message.size() - 1U, output);
    const bool closed = std::fclose(output) == 0;
    const std::string_view text(message.data(), size);
    return closed && text.find("113 UTF-8 bytes") != std::string_view::npos &&
           text.find("114 bytes including NUL required") != std::string_view::npos &&
           text.find("108 bytes including NUL") != std::string_view::npos &&
           text.find("--runtime-dir or --instance") != std::string_view::npos;
}
#endif

#if !defined(_WIN32)
bool write_all_boundary_test() noexcept
{
    std::array<char, 64U> temporary{};
    const char pattern[] = "/tmp/dtv-write-all-XXXXXX";
    std::memcpy(temporary.data(), pattern, sizeof(pattern));
    char* root_value = ::mkdtemp(temporary.data());
    if (root_value == nullptr) return false;
    const std::string root(root_value);
    const std::string product = root + "/siano-userland";
    const std::string version = product + "/v1";
    const std::string instance = version + "/test";
    bool ok = make_directory(product) && make_directory(version) &&
        make_directory(instance);
    dtv::userland::host_runtime::EndpointConfig config{root, "siano-userland", "test"};
    const auto path = dtv::userland::host_runtime::control_endpoint_path(config);
    int server = -1;
    if (!ok || !path || path.value().size() >= sizeof(sockaddr_un{}.sun_path)) {
        ok = false;
    } else {
        server = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.value().c_str(), path.value().size() + 1U);
        ok = server >= 0 &&
            ::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            ::chmod(path.value().c_str(), 0600) == 0 && ::listen(server, 4) == 0;
    }

    if (ok) {
        auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
        int peer = -1;
        if (transport) peer = ::accept(server, nullptr, nullptr);
        if (!transport || peer < 0) ok = false;
        else {
            int receive_buffer = 4096;
            ok = ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                              static_cast<socklen_t>(sizeof(receive_buffer))) == 0;
            std::vector<std::uint8_t> payload(1024U * 1024U);
            for (std::size_t index = 0U; index < payload.size(); ++index)
                payload[index] = static_cast<std::uint8_t>(index);
            std::vector<std::uint8_t> received(payload.size());
            std::atomic<bool> reader_ok{true};
            std::thread reader([&]() {
                std::size_t offset = 0U;
                while (offset < received.size()) {
                    const ssize_t count = ::recv(peer, received.data() + offset,
                                                 received.size() - offset, 0);
                    if (count <= 0) {
                        reader_ok.store(false);
                        return;
                    }
                    offset += static_cast<std::size_t>(count);
                }
            });
            const auto written = transport.value()->write_all(
                dtv::userland::ByteView{payload.data(), payload.size()}, {5000U});
            reader.join();
            ok = ok && written && reader_ok.load() && received == payload;
            transport.value()->close();
            (void)::close(peer);
        }
    }

    if (ok) {
        auto transport = dtv::userland::host_runtime::connect_control(config, {1000U});
        int peer = -1;
        if (transport) peer = ::accept(server, nullptr, nullptr);
        if (!transport || peer < 0) ok = false;
        else {
            int receive_buffer = 4096;
            ok = ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                              static_cast<socklen_t>(sizeof(receive_buffer))) == 0;
            std::vector<std::uint8_t> payload(16U * 1024U * 1024U, 0x5aU);
            const auto written = transport.value()->write_all(
                dtv::userland::ByteView{payload.data(), payload.size()}, {200U});
            ok = ok && !written && written.error() == dtv::userland::Error::TIMEOUT;
            transport.value()->close();
            (void)::close(peer);
        }
    }

    if (server >= 0) (void)::close(server);
    std::filesystem::remove_all(root);
    if (ok) std::puts("posix_write_all_large_frame_and_blocked_timeout=PASS");
    return ok;
}
#endif  // !defined(_WIN32)

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
#if !defined(_WIN32)
    if (!write_all_boundary_test()) {
        std::fputs("POSIX write_all boundary fixture failed\n", stderr);
        return 1;
    }
    if (!shared_group_distinct_uid_test()) {
        std::fputs("POSIX shared-group distinct-UID fixture failed\n", stderr);
        return 1;
    }
    if (!endpoint_connect_diagnostic_text_test()) {
        std::fputs("endpoint connect diagnostic text fixture failed\n", stderr);
        return 1;
    }
#endif
#if !defined(_WIN32)
    if (!default_runtime_roots_test()) {
        std::fputs("POSIX default runtime-root fixture failed\n", stderr);
        return 1;
    }
#endif
    px4::userland::px4d::notify_cleanup_complete();
#if defined(_WIN32)
    if (!px4::userland::px4d::cleanup_complete()) return 1;
#else
    if (px4::userland::px4d::cleanup_complete()) return 1;
#endif
    return 0;
}
