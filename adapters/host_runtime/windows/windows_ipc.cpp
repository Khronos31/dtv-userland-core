// SPDX-License-Identifier: GPL-2.0-only
// Windows implementation of the local IPC endpoint adapter declared in
// px4/posix_ipc_contract.h. It provides the same operation shape as the POSIX AF_UNIX
// adapter (listener/stream/lease) but uses Windows AF_UNIX sockets, Win32 file
// identity, reparse-point checks, and same-user ownership verification.
//
// Offline/mock behavior is validated on native Windows 11 x64 (report.md,
// validation-results.md 2026-10-09); physical tuner/card/LNB hardware behavior
// remains unverified.
#include "px4/posix_ipc_contract.h"
#include <dtv/userland/identity.h>

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
#include <winioctl.h>
#include <windows.h>
#include "windows/windows_security.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#if !defined(PX4_USERLAND_RUNTIME_PRODUCT)
#error PX4_USERLAND_RUNTIME_PRODUCT must be set on the host runtime target
#endif
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE) && \
    defined(PX4_USERLAND_RUNTIME_INSTANCE_LOWERCASE)
#error runtime instance policies are mutually exclusive
#elif !defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE) && \
    !defined(PX4_USERLAND_RUNTIME_INSTANCE_LOWERCASE)
#error exactly one runtime instance policy must be set on the host runtime target
#endif

namespace px4::userland::ipc::posix {
namespace {

constexpr const char* kProductDirectoryName = PX4_USERLAND_RUNTIME_PRODUCT;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

// AF_UNIX on Windows uses a Win32 path. The complete runtime, product,
// instance, and endpoint path must fit the SOCKADDR_UN sun_path limit.
// The endpoint path must fit the platform AF_UNIX address plus its terminating
// NUL. Derive it from the actual SOCKADDR_UN field rather than a magic number.
constexpr std::size_t kMaxEndpointPathBytes = sizeof(SOCKADDR_UN{}.sun_path);

struct Layout final {
    std::array<wchar_t, kStoredPathCapacity> runtime_directory{};
    std::array<wchar_t, kStoredPathCapacity> product_directory{};
    std::array<wchar_t, kStoredPathCapacity> version_directory{};
    std::array<wchar_t, kStoredPathCapacity> instance_directory{};
    std::array<wchar_t, kStoredPathCapacity> endpoint_path{};
    bool group_access = false;
};

bool ensure_winsock() noexcept
{
    static std::atomic<int> state{0};  // 0=init, 1=initializing, 2=ready, 3=failed
    int current = state.load(std::memory_order_acquire);
    if (current == 2) {
        return true;
    }
    if (current == 3) {
        return false;
    }
    int expected = 0;
    if (state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            state.store(2, std::memory_order_release);
            return true;
        }
        state.store(3, std::memory_order_release);
        return false;
    }
    while ((current = state.load(std::memory_order_acquire)) == 1) {
        ::Sleep(0);
    }
    return current == 2;
}

Error map_socket_error(int error) noexcept
{
    switch (error) {
    case WSAEINVAL:
    case WSAENAMETOOLONG:
        return Error::INVALID_ARGUMENT;
    case WSAEADDRINUSE:
    case WSAEALREADY:
        return Error::BUSY;
    case WSAETIMEDOUT:
        return Error::TIMEOUT;
    case WSAECONNREFUSED:
        return Error::NOT_READY;
    case WSAECONNRESET:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
        return Error::DISCONNECTED;
    default:
        return Error::INTERNAL;
    }
}

std::size_t bounded_length(const char* text, std::size_t limit) noexcept
{
    if (text == nullptr) {
        return limit;
    }
    std::size_t length = 0U;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    return length;
}

bool valid_component(const char* value) noexcept
{
    const std::size_t length = bounded_length(value, kStoredPathCapacity);
    if (length == 0U || length == kStoredPathCapacity ||
        (length == 1U && value[0] == '.') ||
        (length == 2U && value[0] == '.' && value[1] == '.')) {
        return false;
    }
    for (std::size_t index = 0U; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        const bool accepted =
            (character >= static_cast<unsigned char>('a') &&
             character <= static_cast<unsigned char>('z')) ||
            (character >= static_cast<unsigned char>('A') &&
             character <= static_cast<unsigned char>('Z')) ||
            (character >= static_cast<unsigned char>('0') &&
             character <= static_cast<unsigned char>('9')) ||
            character == static_cast<unsigned char>('-') ||
            character == static_cast<unsigned char>('_') ||
            character == static_cast<unsigned char>('.');
        if (!accepted) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
bool append_component(std::array<wchar_t, N>& output, std::size_t& used,
                      const wchar_t* component, bool separator) noexcept
{
    const std::size_t length = std::wcslen(component);
    const std::size_t extra = length + (separator ? 1U : 0U);
    if (used > output.size() || extra > output.size() - used ||
        used + extra >= output.size()) {
        return false;
    }
    if (separator) {
        output[used++] = L'\\';
    }
    if (length != 0U) {
        std::wmemcpy(output.data() + used, component, length);
        used += length;
    }
    output[used] = L'\0';
    return true;
}

bool utf8_to_wide(const char* text, std::size_t length,
                  std::array<wchar_t, kStoredPathCapacity>& output) noexcept
{
    if (length == 0U || length >= output.size()) {
        return false;
    }
    const int converted = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length),
        output.data(), static_cast<int>(output.size() - 1U));
    if (converted <= 0 || static_cast<std::size_t>(converted) >= output.size()) {
        return false;
    }
    output[static_cast<std::size_t>(converted)] = L'\0';
    return true;
}

bool is_local_absolute_path(const wchar_t* path) noexcept
{
    if (path == nullptr) {
        return false;
    }
    // Accept only drive-absolute ("C:\...") or extended drive-absolute
    // ("\\?\C:\...") paths. Reject drive-relative ("C:..."), rooted-relative
    // ("\...") and UNC/remote ("\\server\share", "\\?\UNC\...") paths, because
    // same-host IPC must not resolve differently across processes or hosts.
    if (path[0] == L'\\' && path[1] == L'\\') {
        if (path[2] == L'?' && path[3] == L'\\') {
            return (path[4] >= L'A' && path[4] <= L'Z') ||
                   (path[4] >= L'a' && path[4] <= L'z');
        }
        return false;
    }
    if (path[0] == L'\\') {
        return false;
    }
    const bool letter = (path[0] >= L'A' && path[0] <= L'Z') ||
                        (path[0] >= L'a' && path[0] <= L'z');
    return letter && path[1] == L':' && path[2] == L'\\';
}

bool is_af_unix_reparse_path(const wchar_t* path) noexcept
{
    const HANDLE handle = ::CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<std::uint8_t, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> buffer{};
    DWORD returned = 0U;
    const BOOL ok = ::DeviceIoControl(
        handle, FSCTL_GET_REPARSE_POINT, nullptr, 0U, buffer.data(),
        static_cast<DWORD>(buffer.size()), &returned, nullptr);
    (void)::CloseHandle(handle);
    if (ok == 0 || returned < sizeof(DWORD)) {
        return false;
    }
    const DWORD tag = *reinterpret_cast<const DWORD*>(buffer.data());
    return tag == IO_REPARSE_TAG_AF_UNIX;
}

// Aligned storage for TOKEN_USER/SID queries. A plain byte array does not
// guarantee the pointer alignment that TOKEN_USER and SID require.
struct alignas(16) SecurityBuffer final {
    std::uint8_t bytes[512U];
};

enum class SecurityPolicy : std::uint8_t {
    // Project-owned directories and socket/lease objects: protected DACL that
    // grants the current user only.
    private_user,
    // Caller-owned runtime parent: current user must have write/delete, and no
    // untrusted SID may be able to write or replace entries in it.
    runtime_parent,
    runtime_client_parent,
};

// Copies the current process user SID into caller storage and returns it.
// Returns nullptr if the token cannot be queried.
PSID current_user_sid(SecurityBuffer& storage) noexcept
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return nullptr;
    }
    DWORD returned = 0U;
    const BOOL ok = ::GetTokenInformation(token, TokenUser, storage.bytes,
                                          static_cast<DWORD>(sizeof(storage.bytes)),
                                          &returned);
    (void)::CloseHandle(token);
    if (ok == 0) {
        return nullptr;
    }
    return reinterpret_cast<TOKEN_USER*>(storage.bytes)->User.Sid;
}

bool sid_equal(PSID left, PSID right) noexcept
{
    return left != nullptr && right != nullptr && ::EqualSid(left, right) != 0;
}

bool matches_well_known(PSID candidate, WELL_KNOWN_SID_TYPE type) noexcept
{
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (::CreateWellKnownSid(type, nullptr, buffer.data(), &size) == 0) {
        return false;
    }
    return sid_equal(candidate, reinterpret_cast<PSID>(buffer.data()));
}

// The only privileged SIDs accepted on the caller-owned runtime parent.
// CREATOR OWNER and OWNER RIGHTS resolve to the object owner, which is
// verified to be the current user, so they are safe here as well.
bool sid_is_trusted_for_runtime(PSID candidate) noexcept
{
    return matches_well_known(candidate, WinLocalSystemSid) ||
           matches_well_known(candidate, WinBuiltinAdministratorsSid) ||
           matches_well_known(candidate, WinCreatorOwnerSid) ||
           matches_well_known(candidate, WinCreatorOwnerRightsSid);
}

constexpr DWORD kUntrustedWriteMask =
    GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER |
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    FILE_DELETE_CHILD;

// Builds a security descriptor that sets the owner to the current user and a
// protected DACL granting the current user only. The caller owns acl_out and
// must LocalFree() it.
bool build_current_user_security(SECURITY_ATTRIBUTES& attributes,
                                 SECURITY_DESCRIPTOR& descriptor, PACL& acl_out,
                                 SecurityBuffer& sid_storage) noexcept
{
    PSID sid = current_user_sid(sid_storage);
    if (sid == nullptr) {
        return false;
    }
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL acl = nullptr;
    if (::SetEntriesInAclW(1U, &access, nullptr, &acl) != ERROR_SUCCESS ||
        acl == nullptr) {
        return false;
    }
    if (::InitializeSecurityDescriptor(&descriptor,
                                       SECURITY_DESCRIPTOR_REVISION) == 0 ||
        ::SetSecurityDescriptorOwner(&descriptor, sid, FALSE) == 0 ||
        ::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == 0 ||
        ::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                       SE_DACL_PROTECTED) == 0) {
        ::LocalFree(acl);
        return false;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    attributes.bInheritHandle = FALSE;
    acl_out = acl;
    return true;
}

// Iterates every ACE. For private_user every allow ACE must name the current
// user. For runtime_parent the current user must be granted write/delete and
// no untrusted SID may hold write/delete. Null/malformed ACLs and unknown ACE
// types fail closed.
bool acl_satisfies(const ACL* dacl, PSID current, SecurityPolicy policy) noexcept
{
    if (dacl == nullptr) {
        return false;
    }
    ACL_SIZE_INFORMATION information{};
    if (::GetAclInformation(const_cast<ACL*>(dacl), &information,
                            sizeof(information), AclSizeInformation) == 0) {
        return false;
    }
    bool current_user_can_write = false;
    for (DWORD index = 0U; index < information.AceCount; ++index) {
        void* raw = nullptr;
        if (::GetAce(const_cast<ACL*>(dacl), index, &raw) == 0 || raw == nullptr) {
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            // Deny/audit/object/unknown ACEs are outside the accepted shape.
            return false;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID ace_sid = reinterpret_cast<PSID>(
            const_cast<DWORD*>(&ace->SidStart));
        const DWORD mask = ace->Mask;
        if (sid_equal(ace_sid, current)) {
            if ((mask & kUntrustedWriteMask) != 0U) {
                current_user_can_write = true;
            }
            continue;
        }
        if (policy == SecurityPolicy::private_user) {
            return false;
        }
        if (sid_is_trusted_for_runtime(ace_sid)) {
            continue;
        }
        if ((mask & kUntrustedWriteMask) != 0U) {
            return false;
        }
    }
    if (policy == SecurityPolicy::private_user) {
        return true;
    }
    return policy == SecurityPolicy::runtime_parent ? current_user_can_write : true;
}

bool verify_security(const wchar_t* path, SecurityPolicy policy) noexcept
{
    SecurityBuffer storage{};
    PSID current = current_user_sid(storage);
    if (current == nullptr) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    bool ok = sid_equal(owner, current) && acl_satisfies(dacl, current, policy);
    if (ok && policy == SecurityPolicy::private_user) {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        ok = ::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
             (control & SE_DACL_PROTECTED) != 0;
    }
    (void)::LocalFree(descriptor);
    return ok;
}

bool verify_existing_security(const wchar_t* path) noexcept
{
    return verify_security(path, SecurityPolicy::private_user);
}

bool verify_runtime_security(const wchar_t* path) noexcept
{
    // Reuse the shared runtime-parent verifier so the wake directory setup and
    // the IPC endpoint agree on the same same-user policy.
    return windows_security::verify_runtime_parent(path);
}

// Replaces the endpoint DACL with a protected current-user-only DACL so the
// AF_UNIX socket cannot be reached under an inherited broad parent access.
bool set_endpoint_security(const wchar_t* path) noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    SecurityBuffer sid_storage{};
    if (!build_current_user_security(attributes, descriptor, acl, sid_storage)) {
        return false;
    }
    PSID owner = reinterpret_cast<TOKEN_USER*>(sid_storage.bytes)->User.Sid;
    if (owner == nullptr) {
        ::LocalFree(acl);
        return false;
    }
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, acl, nullptr);
    ::LocalFree(acl);
    return status == ERROR_SUCCESS && verify_existing_security(path);
}

Result<Layout> make_layout(const EndpointConfig& config) noexcept
{
    if (config.access == EndpointAccess::shared_group) {
        // shared_group has no tested Windows equivalent; fail argument
        // validation instead of silently inheriting broad access.
        return Result<Layout>::failure(Error::UNSUPPORTED);
    }
    const std::string_view instance_input = config.instance == nullptr ?
        std::string_view{} : std::string_view(config.instance);
    if (!valid_component(config.endpoint_name)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    std::array<char, 81U> instance_utf8{};
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE)
    if (!valid_component(config.instance) || instance_input.size() > 80U) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    std::memcpy(instance_utf8.data(), instance_input.data(), instance_input.size());
    instance_utf8[instance_input.size()] = '\0';
#else
    const auto canonical_instance =
        dtv::userland::identity::canonicalize_instance(instance_input);
    if (!canonical_instance) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    std::memcpy(instance_utf8.data(), canonical_instance.value().bytes.data(),
                canonical_instance.value().length);
    instance_utf8[canonical_instance.value().length] = '\0';
#endif

    Layout layout{};
    if (config.runtime_directory == nullptr) {
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", layout.runtime_directory.data(),
            static_cast<DWORD>(layout.runtime_directory.size()));
        if (length == 0U ||
            length >= layout.runtime_directory.size()) {
            return Result<Layout>::failure(Error::INVALID_ARGUMENT);
        }
    } else {
        const std::size_t runtime_length =
            bounded_length(config.runtime_directory, kStoredPathCapacity);
        if (runtime_length == 0U || runtime_length == kStoredPathCapacity ||
            !utf8_to_wide(config.runtime_directory, runtime_length,
                          layout.runtime_directory)) {
            return Result<Layout>::failure(Error::INVALID_ARGUMENT);
        }
    }
    if (!is_local_absolute_path(layout.runtime_directory.data())) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    std::size_t used = 0U;
    std::array<wchar_t, kStoredPathCapacity> product_wide{};
    if (!utf8_to_wide(kProductDirectoryName, std::strlen(kProductDirectoryName),
                      product_wide) ||
        !append_component(layout.product_directory, used,
                          layout.runtime_directory.data(), false) ||
        !append_component(layout.product_directory, used, product_wide.data(), true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    const std::size_t product_length = used;
    std::wmemcpy(layout.version_directory.data(), layout.product_directory.data(),
                 product_length + 1U);
    if (!append_component(layout.version_directory, used, L"v1", true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    const std::size_t version_length = used;
    std::wmemcpy(layout.instance_directory.data(), layout.version_directory.data(),
                 version_length + 1U);
    used = version_length;
    std::array<wchar_t, kStoredPathCapacity> instance_wide{};
    if (!utf8_to_wide(instance_utf8.data(), std::strlen(instance_utf8.data()),
                      instance_wide) ||
        !append_component(layout.instance_directory, used, instance_wide.data(),
                          true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    const std::size_t directory_length = used;
    std::wmemcpy(layout.endpoint_path.data(), layout.instance_directory.data(),
                 directory_length + 1U);
    used = directory_length;
    std::array<wchar_t, kStoredPathCapacity> endpoint_wide{};
    if (!utf8_to_wide(config.endpoint_name, std::strlen(config.endpoint_name),
                      endpoint_wide) ||
        !append_component(layout.endpoint_path, used, endpoint_wide.data(), true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    const int endpoint_bytes = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, layout.endpoint_path.data(), -1, nullptr,
        0, nullptr, nullptr);
    if (endpoint_bytes <= 0 ||
        endpoint_bytes > static_cast<int>(kMaxEndpointPathBytes)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<Layout>::success(std::move(layout));
}

void close_socket(NativeHandle handle) noexcept
{
    if (handle != kInvalidHandle) {
        (void)::closesocket(static_cast<SOCKET>(handle));
    }
}

void close_handle(NativeHandle handle) noexcept
{
    if (handle != kInvalidHandle) {
        (void)::CloseHandle(reinterpret_cast<HANDLE>(handle));
    }
}

bool set_nonblocking(NativeHandle handle) noexcept
{
    u_long enabled = 1UL;
    return ::ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &enabled) == 0;
}

bool clear_inherit(HANDLE handle) noexcept
{
    return ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0) != 0;
}

bool valid_file_identity(const BY_HANDLE_FILE_INFORMATION& info) noexcept
{
    return info.dwVolumeSerialNumber != 0U ||
           info.nFileIndexHigh != 0U || info.nFileIndexLow != 0U;
}

struct FileIdentity final {
    std::uint64_t device = 0U;
    std::uint64_t index = 0U;
    bool valid = false;
};

FileIdentity identity_of(const BY_HANDLE_FILE_INFORMATION& info) noexcept
{
    return FileIdentity{
        static_cast<std::uint64_t>(info.dwVolumeSerialNumber),
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
            static_cast<std::uint64_t>(info.nFileIndexLow),
        true};
}

Result<FileIdentity> identity_of_path(const std::array<wchar_t, kStoredPathCapacity>& path,
                                      DWORD flags) noexcept
{
    const HANDLE handle = ::CreateFileW(
        path.data(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<FileIdentity>::failure(Error::NOT_FOUND);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    (void)::CloseHandle(handle);
    if (ok == 0 || !valid_file_identity(info)) {
        return Result<FileIdentity>::failure(Error::INTERNAL);
    }
    return Result<FileIdentity>::success(identity_of(info));
}

// True only when the current object at path still has the saved identity. This
// prevents cleanup from deleting a replacement created by another process.
bool identity_matches_path(std::uint64_t device, std::uint64_t index,
                           const std::array<wchar_t, kStoredPathCapacity>& path,
                           DWORD flags) noexcept
{
    const auto current = identity_of_path(path, flags);
    return current && current.value().device == device &&
           current.value().index == index;
}

// POSIX-equivalent last-holder cleanup of the serial lease lock file.
// posix_ipc.cpp's SerialEndpointLease::close() upgrades its flock to
// exclusive and unlinks the lock file when the closing holder is the last
// one, so a cleanly stopped daemon leaves no lock file behind.  Windows
// byte-range locks cannot upgrade in place: re-issuing LockFileEx
// exclusively on the handle that already holds the range fails with
// ERROR_LOCK_VIOLATION even when no other holder exists (measured on
// Windows 11 real hardware), so the last-holder test happens on a fresh
// probe handle after this lease fully releases its own lock and handle:
//   * the path must still resolve to the exact inode this lease held, so a
//     lock file replaced by another process is never deleted;
//   * the probe handle's exclusive LockFileEx succeeds only when no other
//     holder still locks the range; those holders keep the lock file and
//     their own close performs the cleanup, exactly like a failed flock
//     upgrade on POSIX;
//   * DeleteFileW can only succeed while no other process holds the file
//     open (lock handles deliberately omit FILE_SHARE_DELETE), and a
//     failure is reported on stderr instead of being silently swallowed;
//     ERROR_SHARING_VIOLATION here is the rare takeover race with a
//     starting daemon, which then owns the lock file and its own cleanup.
void remove_lock_if_last_holder(
    const FileIdentity& identity,
    const std::array<wchar_t, kStoredPathCapacity>& path) noexcept
{
    if (path[0] == L'\0' || !identity.valid) return;
    if (!identity_matches_path(identity.device, identity.index, path,
                               FILE_FLAG_OPEN_REPARSE_POINT)) {
        return;
    }
    const HANDLE probe = ::CreateFileW(
        path.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            std::fprintf(stderr, "px4 windows ipc: lease lock probe error=%lu\n",
                         static_cast<unsigned long>(error));
        }
        return;
    }
    OVERLAPPED overlapped{};
    if (::LockFileEx(probe, LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK,
                     0, 1UL, 0UL, &overlapped) == 0) {
        // Another holder still locks the range; its own close removes it.
        (void)::CloseHandle(probe);
        return;
    }
    OVERLAPPED unlock{};
    (void)::UnlockFileEx(probe, 0, 1UL, 0UL, &unlock);
    (void)::CloseHandle(probe);
    if (::DeleteFileW(path.data()) == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            std::fprintf(stderr, "px4 windows ipc: lease lock cleanup error=%lu\n",
                         static_cast<unsigned long>(error));
        }
    }
}

bool is_current_user_owner(const std::array<wchar_t, kStoredPathCapacity>& path) noexcept
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    SecurityBuffer buffer{};
    DWORD returned = 0U;
    const BOOL ok = ::GetTokenInformation(token, TokenUser, buffer.bytes,
                                          static_cast<DWORD>(sizeof(buffer.bytes)),
                                          &returned);
    (void)::CloseHandle(token);
    if (ok == 0) {
        return false;
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.bytes);

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.data()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    const bool same = owner != nullptr && ::EqualSid(owner, user->User.Sid) != 0;
    ::LocalFree(descriptor);
    return same;
}

Result<FileIdentity> validate_directory(const std::array<wchar_t, kStoredPathCapacity>& path,
                                        bool require_existing,
                                        SecurityPolicy policy) noexcept
{
    const DWORD attributes = ::GetFileAttributesW(path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return require_existing ? Result<FileIdentity>::failure(Error::NOT_FOUND)
                                : Result<FileIdentity>::success(FileIdentity{});
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return Result<FileIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    const bool secure = policy == SecurityPolicy::private_user ?
                            verify_existing_security(path.data()) :
                            verify_runtime_security(path.data());
    if (!secure) {
        return Result<FileIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    return identity_of_path(path, FILE_FLAG_BACKUP_SEMANTICS);
}

Result<FileIdentity> ensure_directory(std::array<wchar_t, kStoredPathCapacity>& path,
                                      bool& created) noexcept
{
    created = false;
    const auto existing = validate_directory(path, false, SecurityPolicy::private_user);
    if (!existing) {
        return existing;
    }
    if (existing.value().valid) {
        return existing;
    }

    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    SecurityBuffer sid_storage{};
    if (!build_current_user_security(attributes, descriptor, acl, sid_storage)) {
        return Result<FileIdentity>::failure(Error::INTERNAL);
    }
    const BOOL made = ::CreateDirectoryW(path.data(), &attributes);
    ::LocalFree(acl);
    if (made == 0) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return Result<FileIdentity>::failure(Error::INTERNAL);
        }
        // Raced with another creator; fall through to validation.
    } else {
        created = true;
    }
    return validate_directory(path, true, SecurityPolicy::private_user);
}

Result<void> remove_stale_endpoint(
    const std::array<wchar_t, kStoredPathCapacity>& path) noexcept
{
    const DWORD attributes = ::GetFileAttributesW(path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return Result<void>::success();
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!is_af_unix_reparse_path(path.data())) {
        // Only an AF_UNIX socket endpoint is removable; other reparse points
        // (symlink, mount point, ...) must never be unlinked here.
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!is_current_user_owner(path)) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto identity = identity_of_path(path, FILE_FLAG_OPEN_REPARSE_POINT);
    if (!identity) {
        return Result<void>::success();
    }

    const SOCKET probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe == INVALID_SOCKET) {
        return Result<void>::failure(Error::INTERNAL);
    }
    (void)set_nonblocking(static_cast<NativeHandle>(probe));
    std::array<char, kStoredPathCapacity> utf8{};
    const int converted = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), -1, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, nullptr);
    if (converted <= 0) {
        close_socket(static_cast<NativeHandle>(probe));
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    SOCKADDR_UN address{};
    address.sun_family = AF_UNIX;
    if (static_cast<std::size_t>(converted) > sizeof(address.sun_path)) {
        close_socket(static_cast<NativeHandle>(probe));
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    std::memcpy(address.sun_path, utf8.data(),
                static_cast<std::size_t>(converted));
    const int connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&address),
                                    sizeof(address));
    const int connect_error = connected == 0 ? 0 : ::WSAGetLastError();
    close_socket(static_cast<NativeHandle>(probe));

    if (connected == 0 || connect_error == WSAEWOULDBLOCK ||
        connect_error == WSAEINPROGRESS || connect_error == WSAEALREADY ||
        connect_error == WSAEINTR) {
        // A nonblocking connect that is established or still in progress means a
        // live listener may own the endpoint. Never delete it.
        return Result<void>::failure(Error::BUSY);
    }
    if (connect_error != WSAECONNREFUSED) {
        std::fprintf(stderr, "px4 windows ipc: stale probe error=%d\n", connect_error);
        return Result<void>::failure(map_socket_error(connect_error));
    }

    const auto current = identity_of_path(path, FILE_FLAG_OPEN_REPARSE_POINT);
    if (!current || current.value().device != identity.value().device ||
        current.value().index != identity.value().index) {
        return Result<void>::failure(Error::BUSY);
    }
    return ::DeleteFileW(path.data()) != 0 ? Result<void>::success()
                                           : Result<void>::failure(Error::BUSY);
}

Deadline deadline_after(Timeout timeout) noexcept
{
    return Clock::now() + std::chrono::milliseconds(timeout.milliseconds);
}

bool deadline_expired(Deadline deadline) noexcept
{
    return Clock::now() >= deadline;
}

int remaining_poll_milliseconds(Deadline deadline) noexcept
{
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = deadline - now;
    const auto whole = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    auto milliseconds = whole.count();
    if (whole < remaining) {
        ++milliseconds;
    }
    if (milliseconds > INT_MAX) {
        return INT_MAX;
    }
    return static_cast<int>(milliseconds);
}

Result<void> wait_socket(NativeHandle handle, short events, Deadline deadline) noexcept
{
    while (true) {
        const int milliseconds = remaining_poll_milliseconds(deadline);
        if (milliseconds == 0) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        WSAPOLLFD descriptor{};
        descriptor.fd = static_cast<SOCKET>(handle);
        descriptor.events = events;
        const int result = ::WSAPoll(&descriptor, 1UL, milliseconds);
        if (result > 0) {
            if ((descriptor.revents & POLLNVAL) != 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
            return Result<void>::success();
        }
        if (result == 0) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        if (::WSAGetLastError() != WSAEINTR) {
            return Result<void>::failure(map_socket_error(::WSAGetLastError()));
        }
    }
}

bool make_address(const std::array<wchar_t, kStoredPathCapacity>& path,
                  SOCKADDR_UN& address,
                  std::array<char, kStoredPathCapacity>& scratch) noexcept
{
    const int converted = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), -1, scratch.data(),
        static_cast<int>(scratch.size()), nullptr, nullptr);
    if (converted <= 0) {
        return false;
    }
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (static_cast<std::size_t>(converted) > sizeof(address.sun_path)) {
        return false;
    }
    std::memcpy(address.sun_path, scratch.data(),
                static_cast<std::size_t>(converted));
    return true;
}

}  // namespace

Result<RuntimeRootCandidates> client_runtime_roots() noexcept
{
    const EndpointConfig endpoint{nullptr, "probe", kControlEndpointName,
                                  EndpointAccess::private_user};
    const auto layout = make_layout(endpoint);
    if (!layout) return Result<RuntimeRootCandidates>::failure(layout.error());
    const auto runtime = validate_directory(layout.value().runtime_directory, true,
                                            SecurityPolicy::runtime_client_parent);
    if (!runtime) return Result<RuntimeRootCandidates>::failure(runtime.error());
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        layout.value().runtime_directory.data(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0 || static_cast<std::size_t>(length) >= kStoredPathCapacity)
        return Result<RuntimeRootCandidates>::failure(Error::INVALID_ARGUMENT);
    RuntimeRootCandidates roots{};
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            layout.value().runtime_directory.data(), -1, roots.paths[0].data(),
            static_cast<int>(roots.paths[0].size()), nullptr, nullptr) != length)
        return Result<RuntimeRootCandidates>::failure(Error::INTERNAL);
    roots.count = 1U;
    return Result<RuntimeRootCandidates>::success(std::move(roots));
}

Result<RuntimeInstanceDiscovery> discover_runtime_instances(
    const char* runtime_directory, EndpointAccess access) noexcept
{
    if (access == EndpointAccess::shared_group)
        return Result<RuntimeInstanceDiscovery>::failure(Error::UNSUPPORTED);
    const EndpointConfig probe{runtime_directory, "probe", kControlEndpointName,
                               EndpointAccess::private_user};
    const auto layout = make_layout(probe);
    if (!layout) return Result<RuntimeInstanceDiscovery>::failure(layout.error());
    const auto runtime = validate_directory(layout.value().runtime_directory, true,
                                            SecurityPolicy::runtime_client_parent);
    if (!runtime) return Result<RuntimeInstanceDiscovery>::failure(runtime.error());
    const auto product = validate_directory(layout.value().product_directory, true,
                                             SecurityPolicy::private_user);
    const auto version = validate_directory(layout.value().version_directory, true,
                                             SecurityPolicy::private_user);
    if (!product || !version)
        return Result<RuntimeInstanceDiscovery>::failure(
            !product ? product.error() : version.error());

    std::array<wchar_t, kStoredPathCapacity> search_path =
        layout.value().version_directory;
    std::size_t search_length = std::wcslen(search_path.data());
    if (!append_component(search_path, search_length, L"*", true))
        return Result<RuntimeInstanceDiscovery>::failure(Error::INVALID_ARGUMENT);
    WIN32_FIND_DATAW entry{};
    const HANDLE search = ::FindFirstFileW(search_path.data(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return Result<RuntimeInstanceDiscovery>::success(RuntimeInstanceDiscovery{});

    RuntimeInstanceDiscovery discovery{};
    std::array<std::array<char, 65U>, 256U> seen{};
    std::size_t seen_count = 0U;
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            std::wcscmp(entry.cFileName, L".") == 0 ||
            std::wcscmp(entry.cFileName, L"..") == 0)
            continue;
        std::array<char, 65U> name{};
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName,
                -1, name.data(), static_cast<int>(name.size()), nullptr, nullptr) <= 0)
            continue;
        const std::string_view name_view(name.data());
        const auto canonical = dtv::userland::identity::canonicalize_instance(name_view);
        if (!canonical) continue;
        std::array<char, 65U> token{};
        std::memcpy(token.data(), canonical.value().bytes.data(),
                    canonical.value().length);
        token[canonical.value().length] = '\0';
        for (std::size_t index = 0U; index < seen_count; ++index) {
            if (std::strcmp(seen[index].data(), token.data()) == 0) {
                discovery.canonical_collision = true;
                (void)::FindClose(search);
                return Result<RuntimeInstanceDiscovery>::success(discovery);
            }
        }
        if (seen_count >= seen.size()) {
            (void)::FindClose(search);
            return Result<RuntimeInstanceDiscovery>::failure(Error::RESOURCE_LIMIT);
        }
        seen[seen_count++] = token;
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE)
        const char* endpoint_instance = name.data();
#else
        if (name_view.size() != canonical.value().length ||
            std::memcmp(name_view.data(), token.data(), name_view.size()) != 0)
            continue;
        const char* endpoint_instance = token.data();
#endif

        EndpointConfig endpoint{runtime_directory, endpoint_instance, kControlEndpointName,
                                EndpointAccess::private_user};
        const auto candidate = make_layout(endpoint);
        if (!candidate) continue;
        const auto instance = validate_directory(candidate.value().instance_directory,
                                                 true, SecurityPolicy::private_user);
        if (!instance) continue;
        const DWORD attributes = ::GetFileAttributesW(candidate.value().endpoint_path.data());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            !is_af_unix_reparse_path(candidate.value().endpoint_path.data()) ||
            !verify_existing_security(candidate.value().endpoint_path.data()))
            continue;
        if (discovery.count >= discovery.instances.size()) {
            (void)::FindClose(search);
            return Result<RuntimeInstanceDiscovery>::failure(Error::RESOURCE_LIMIT);
        }
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE)
        discovery.instances[discovery.count++] = name;
#else
        discovery.instances[discovery.count++] = token;
#endif
    } while (::FindNextFileW(search, &entry) != 0);
    (void)::FindClose(search);
    return Result<RuntimeInstanceDiscovery>::success(discovery);
}

SerialEndpointLease::~SerialEndpointLease() noexcept
{
    close();
}

SerialEndpointLease::SerialEndpointLease(SerialEndpointLease&& other) noexcept
    : directory_fd_(other.directory_fd_), lock_fd_(other.lock_fd_),
      filename_(other.filename_), lock_path_(other.lock_path_)
{
    other.directory_fd_ = kInvalidHandle;
    other.lock_fd_ = kInvalidHandle;
    other.lock_path_.fill(L'\0');
}

SerialEndpointLease& SerialEndpointLease::operator=(SerialEndpointLease&& other) noexcept
{
    if (this != &other) {
        close();
        directory_fd_ = other.directory_fd_;
        lock_fd_ = other.lock_fd_;
        filename_ = other.filename_;
        lock_path_ = other.lock_path_;
        other.directory_fd_ = kInvalidHandle;
        other.lock_fd_ = kInvalidHandle;
        other.lock_path_.fill(L'\0');
    }
    return *this;
}

Result<SerialEndpointLease> SerialEndpointLease::acquire(
    const EndpointConfig& endpoint, std::string_view observed_serial) noexcept
{
    if (!ensure_winsock()) {
        return Result<SerialEndpointLease>::failure(Error::INTERNAL);
    }
    const bool serial_identity =
        (observed_serial.size() == 14U || observed_serial.size() == 15U) &&
        std::all_of(observed_serial.begin(), observed_serial.end(),
                    [](char character) { return character >= '0' && character <= '9'; });
    const bool topology_identity = observed_serial.size() == 37U &&
        observed_serial.substr(0U, 5U) == "auto-" &&
        std::all_of(observed_serial.begin() + 5, observed_serial.end(),
                    [](char character) {
                        return (character >= '0' && character <= '9') ||
                               (character >= 'a' && character <= 'f');
                    });
    if (!serial_identity && !topology_identity) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    return acquire_validated(endpoint, observed_serial, false);
}

Result<SerialEndpointLease> SerialEndpointLease::acquire_identity(
    const EndpointConfig& endpoint, std::string_view observed_identity) noexcept
{
    if (!ensure_winsock()) {
        return Result<SerialEndpointLease>::failure(Error::INTERNAL);
    }
    std::array<char, kStoredPathCapacity> identity{};
    if (observed_identity.empty() || observed_identity.size() >= identity.size()) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::copy(observed_identity.begin(), observed_identity.end(), identity.begin());
    if (!valid_component(identity.data()) ||
        std::strlen(identity.data()) != observed_identity.size()) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    return acquire_validated(endpoint, observed_identity, true);
}

Result<SerialEndpointLease> SerialEndpointLease::acquire_validated(
    const EndpointConfig& endpoint, std::string_view observed_serial,
    bool force_exclusive) noexcept
{
    const auto layout_result = make_layout(endpoint);
    if (!layout_result) {
        return Result<SerialEndpointLease>::failure(layout_result.error());
    }
    const Layout& layout = layout_result.value();
    const auto runtime =
        validate_directory(layout.runtime_directory, true, SecurityPolicy::runtime_parent);
    if (!runtime) {
        return Result<SerialEndpointLease>::failure(runtime.error());
    }

    SerialEndpointLease lease;
    const int length = std::snprintf(lease.filename_.data(), lease.filename_.size(),
                                     ".%s-%.*s.lock", kProductDirectoryName,
                                     static_cast<int>(observed_serial.size()),
                                     observed_serial.data());
    if (length <= 0 || static_cast<std::size_t>(length) >= lease.filename_.size()) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }

    std::array<wchar_t, kStoredPathCapacity> filename_wide{};
    if (!utf8_to_wide(lease.filename_.data(), std::strlen(lease.filename_.data()),
                      filename_wide)) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::array<wchar_t, kStoredPathCapacity> lock_path{};
    std::size_t used = 0U;
    if (!append_component(lock_path, used, layout.runtime_directory.data(), false) ||
        !append_component(lock_path, used, filename_wide.data(), true)) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }

    SecurityBuffer lease_sid_storage{};
    PSID current_sid = current_user_sid(lease_sid_storage);
    if (current_sid == nullptr) {
        return Result<SerialEndpointLease>::failure(Error::INTERNAL);
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR owner_descriptor = nullptr;

    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    SecurityBuffer sid_storage{};
    if (!build_current_user_security(attributes, descriptor, acl, sid_storage)) {
        return Result<SerialEndpointLease>::failure(Error::INTERNAL);
    }
    // FILE_SHARE_READ | FILE_SHARE_WRITE permits concurrent shared holders but
    // deliberately omits FILE_SHARE_DELETE, so the lock file cannot be renamed
    // or deleted while any daemon holds it. That prevents a lock split in which
    // a replacement inode would bypass the byte-range lock.
    const HANDLE file = ::CreateFileW(
        lock_path.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ::LocalFree(acl);
    if (file == INVALID_HANDLE_VALUE) {
        return Result<SerialEndpointLease>::failure(
            ::GetLastError() == ERROR_SHARING_VIOLATION ? Error::BUSY
                                                        : Error::INTERNAL);
    }
    if (!clear_inherit(file)) {
        (void)::CloseHandle(file);
        return Result<SerialEndpointLease>::failure(Error::INTERNAL);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(file, &info) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        (void)::CloseHandle(file);
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (::GetNamedSecurityInfoW(lock_path.data(), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION, &owner, nullptr,
                                nullptr, nullptr, &owner_descriptor) !=
            ERROR_SUCCESS ||
        owner == nullptr || ::EqualSid(owner, current_sid) == 0) {
        (void)::LocalFree(owner_descriptor);
        (void)::CloseHandle(file);
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    (void)::LocalFree(owner_descriptor);

    const bool serial_instance = force_exclusive ||
        std::string_view(endpoint.instance) == observed_serial;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (serial_instance) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    OVERLAPPED overlapped{};
    if (::LockFileEx(file, flags, 0, 1UL, 0UL, &overlapped) == 0) {
        const DWORD error = ::GetLastError();
        (void)::CloseHandle(file);
        return Result<SerialEndpointLease>::failure(
            error == ERROR_LOCK_VIOLATION ? Error::BUSY : Error::INTERNAL);
    }
    // Confirm the named path still resolves to the locked inode; otherwise a
    // replacement would split the lock namespace.
    const auto named = identity_of_path(lock_path, FILE_FLAG_OPEN_REPARSE_POINT);
    if (!named || named.value().device !=
                      static_cast<std::uint64_t>(info.dwVolumeSerialNumber) ||
        named.value().index !=
            ((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
             static_cast<std::uint64_t>(info.nFileIndexLow))) {
        OVERLAPPED unlock{};
        (void)::UnlockFileEx(file, 0, 1UL, 0UL, &unlock);
        (void)::CloseHandle(file);
        return Result<SerialEndpointLease>::failure(Error::BUSY);
    }
    lease.lock_fd_ = reinterpret_cast<NativeHandle>(file);
    lease.directory_fd_ = kInvalidHandle;
    lease.lock_path_ = lock_path;
    return Result<SerialEndpointLease>::success(std::move(lease));
}

void SerialEndpointLease::close() noexcept
{
    if (lock_fd_ != kInvalidHandle) {
        // Capture the identity of the inode this lease holds before the
        // release, so the last-holder cleanup below can never delete a
        // lock file that another process replaced after this release.
        const HANDLE file = reinterpret_cast<HANDLE>(lock_fd_);
        BY_HANDLE_FILE_INFORMATION info{};
        const BOOL identity_ok = ::GetFileInformationByHandle(file, &info);
        OVERLAPPED overlapped{};
        (void)::UnlockFileEx(file, 0, 1UL, 0UL, &overlapped);
        close_handle(lock_fd_);
        lock_fd_ = kInvalidHandle;
        if (identity_ok != 0 && valid_file_identity(info)) {
            remove_lock_if_last_holder(identity_of(info), lock_path_);
        }
        lock_path_.fill(L'\0');
    }
    directory_fd_ = kInvalidHandle;
}

SocketStream::~SocketStream() noexcept
{
    close();
}

SocketStream::SocketStream(SocketStream&& other) noexcept : fd_(other.fd_)
{
    other.fd_ = kInvalidHandle;
}

SocketStream& SocketStream::operator=(SocketStream&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = kInvalidHandle;
    }
    return *this;
}

Result<SocketStream> SocketStream::connect(const EndpointConfig& config,
                                           Timeout timeout) noexcept
{
    if (!ensure_winsock()) {
        return Result<SocketStream>::failure(Error::INTERNAL);
    }
    const auto layout_result = make_layout(config);
    if (!layout_result) {
        return Result<SocketStream>::failure(layout_result.error());
    }
    const Layout& layout = layout_result.value();
    const auto runtime =
        validate_directory(layout.runtime_directory, true,
                           SecurityPolicy::runtime_client_parent);
    const auto product = validate_directory(layout.product_directory, true, SecurityPolicy::private_user);
    const auto version = validate_directory(layout.version_directory, true, SecurityPolicy::private_user);
    const auto instance = validate_directory(layout.instance_directory, true, SecurityPolicy::private_user);
    if (!runtime || !product || !version || !instance) {
        const Error error = !runtime ? runtime.error() :
                            (!product ? product.error() :
                             (!version ? version.error() : instance.error()));
        return Result<SocketStream>::failure(error);
    }
    const DWORD attributes = ::GetFileAttributesW(layout.endpoint_path.data());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return Result<SocketStream>::failure(Error::NOT_FOUND);
    }
    if (!is_af_unix_reparse_path(layout.endpoint_path.data()) ||
        !verify_existing_security(layout.endpoint_path.data())) {
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    }

    const SOCKET socket_handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_handle == INVALID_SOCKET) {
        return Result<SocketStream>::failure(map_socket_error(::WSAGetLastError()));
    }
    if (!set_nonblocking(static_cast<NativeHandle>(socket_handle))) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketStream>::failure(Error::INTERNAL);
    }

    SOCKADDR_UN address{};
    std::array<char, kStoredPathCapacity> scratch{};
    if (!make_address(layout.endpoint_path, address, scratch)) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    const int result = ::connect(socket_handle,
                                 reinterpret_cast<const sockaddr*>(&address),
                                 sizeof(address));
    if (result != 0) {
        const int initial_error = ::WSAGetLastError();
        if (initial_error != WSAEWOULDBLOCK && initial_error != WSAEINPROGRESS &&
            initial_error != WSAEALREADY && initial_error != WSAEINTR) {
            close_socket(static_cast<NativeHandle>(socket_handle));
            return Result<SocketStream>::failure(map_socket_error(initial_error));
        }
        const auto ready = wait_socket(static_cast<NativeHandle>(socket_handle),
                                       POLLWRNORM, deadline);
        if (!ready) {
            close_socket(static_cast<NativeHandle>(socket_handle));
            return Result<SocketStream>::failure(ready.error());
        }
        int socket_error = 0;
        int socket_error_size = static_cast<int>(sizeof(socket_error));
        if (::getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&socket_error),
                         &socket_error_size) != 0) {
            const Error error = map_socket_error(::WSAGetLastError());
            close_socket(static_cast<NativeHandle>(socket_handle));
            return Result<SocketStream>::failure(error);
        }
        if (socket_error != 0) {
            close_socket(static_cast<NativeHandle>(socket_handle));
            return Result<SocketStream>::failure(map_socket_error(socket_error));
        }
    }
    return Result<SocketStream>::success(SocketStream(
        static_cast<NativeHandle>(socket_handle)));
}

void SocketStream::close() noexcept
{
    const NativeHandle handle = fd_;
    fd_ = kInvalidHandle;
    close_socket(handle);
}

Result<std::size_t> SocketStream::read_some(MutableByteView output,
                                            Timeout timeout) noexcept
{
    if (!valid() || output.data == nullptr || output.size == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    const std::size_t maximum = static_cast<std::size_t>(INT_MAX);
    const int size = static_cast<int>(std::min(output.size, maximum));
    while (true) {
        const int received = ::recv(static_cast<SOCKET>(fd_),
                                    reinterpret_cast<char*>(output.data), size, 0);
        if (received > 0) {
            return Result<std::size_t>::success(static_cast<std::size_t>(received));
        }
        if (received == 0) {
            return Result<std::size_t>::failure(Error::DISCONNECTED);
        }
        const int error = ::WSAGetLastError();
        if (error == WSAEINTR) {
            if (deadline_expired(deadline)) {
                return Result<std::size_t>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != WSAEWOULDBLOCK) {
            return Result<std::size_t>::failure(map_socket_error(error));
        }
        const auto ready = wait_socket(fd_, POLLRDNORM, deadline);
        if (!ready) {
            return Result<std::size_t>::failure(ready.error());
        }
    }
}

Result<std::size_t> SocketStream::read_frames(MutableByteView read_buffer,
                                              StreamFramer& framer,
                                              FrameConsumer& consumer,
                                              Timeout timeout) noexcept
{
    const auto received = read_some(read_buffer, timeout);
    if (!received) {
        return Result<std::size_t>::failure(received.error());
    }
    return framer.feed(ByteView{read_buffer.data, received.value()}, consumer);
}

Result<void> SocketStream::write_frame(ByteView frame, Timeout timeout) noexcept
{
    if (!valid()) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto decoded = decode_frame(frame);
    if (!decoded) {
        return Result<void>::failure(decoded.error());
    }
    return write_all(frame, timeout);
}

Result<void> SocketStream::write_all(ByteView bytes, Timeout timeout) noexcept
{
    if (!valid() || (bytes.data == nullptr && bytes.size != 0U)) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    std::size_t offset = 0U;
    const std::size_t maximum = static_cast<std::size_t>(INT_MAX);
    while (offset < bytes.size) {
        const int amount = static_cast<int>(std::min(bytes.size - offset, maximum));
        const int sent = ::send(static_cast<SOCKET>(fd_),
                                reinterpret_cast<const char*>(bytes.data + offset),
                                amount, 0);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            if (offset < bytes.size && deadline_expired(deadline)) {
                return Result<void>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (sent == 0) {
            return Result<void>::failure(Error::DISCONNECTED);
        }
        const int error = ::WSAGetLastError();
        if (error == WSAEINTR) {
            if (deadline_expired(deadline)) {
                return Result<void>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != WSAEWOULDBLOCK) {
            return Result<void>::failure(map_socket_error(error));
        }
        const auto ready = wait_socket(fd_, POLLWRNORM, deadline);
        if (!ready) {
            return ready;
        }
    }
    return Result<void>::success();
}

SocketListener::~SocketListener() noexcept
{
    close();
}

SocketListener::SocketListener(SocketListener&& other) noexcept
{
    move_from(other);
}

SocketListener& SocketListener::operator=(SocketListener&& other) noexcept
{
    if (this != &other) {
        close();
        move_from(other);
    }
    return *this;
}

void SocketListener::move_from(SocketListener& other) noexcept
{
    fd_ = other.fd_;
    product_directory_ = other.product_directory_;
    version_directory_ = other.version_directory_;
    instance_directory_ = other.instance_directory_;
    endpoint_path_ = other.endpoint_path_;
    product_identity_ = other.product_identity_;
    version_identity_ = other.version_identity_;
    instance_identity_ = other.instance_identity_;
    endpoint_identity_ = other.endpoint_identity_;
    directory_mode_ = other.directory_mode_;
    socket_mode_ = other.socket_mode_;
    created_product_directory_ = other.created_product_directory_;
    created_version_directory_ = other.created_version_directory_;
    created_instance_directory_ = other.created_instance_directory_;
    created_endpoint_ = other.created_endpoint_;

    other.fd_ = kInvalidHandle;
    other.created_product_directory_ = false;
    other.created_version_directory_ = false;
    other.created_instance_directory_ = false;
    other.created_endpoint_ = false;
}

Result<SocketListener> SocketListener::listen(const EndpointConfig& config,
                                              int backlog) noexcept
{
    if (backlog <= 0 || !ensure_winsock()) {
        return Result<SocketListener>::failure(Error::INVALID_ARGUMENT);
    }
    const auto layout_result = make_layout(config);
    if (!layout_result) {
        return Result<SocketListener>::failure(layout_result.error());
    }
    const Layout& layout = layout_result.value();

    SocketListener listener;
    listener.product_directory_ = layout.product_directory;
    listener.version_directory_ = layout.version_directory;
    listener.instance_directory_ = layout.instance_directory;
    listener.endpoint_path_ = layout.endpoint_path;
    listener.directory_mode_ = config.access == EndpointAccess::shared_group ?
                                   1U : 0U;
    listener.socket_mode_ = listener.directory_mode_;

    const auto runtime_check =
        validate_directory(layout.runtime_directory, true, SecurityPolicy::runtime_parent);
    if (!runtime_check) {
        return Result<SocketListener>::failure(runtime_check.error());
    }

    bool created = false;
    const auto product = ensure_directory(listener.product_directory_, created);
    listener.created_product_directory_ = created;
    if (!product) {
        return Result<SocketListener>::failure(product.error());
    }
    listener.product_identity_ = FileIdentity{
        product.value().device, product.value().index, product.value().valid};

    const auto version = ensure_directory(listener.version_directory_, created);
    listener.created_version_directory_ = created;
    if (!version) {
        return Result<SocketListener>::failure(version.error());
    }
    listener.version_identity_ = FileIdentity{
        version.value().device, version.value().index, version.value().valid};

    const auto instance = ensure_directory(listener.instance_directory_, created);
    listener.created_instance_directory_ = created;
    if (!instance) {
        return Result<SocketListener>::failure(instance.error());
    }
    listener.instance_identity_ = FileIdentity{
        instance.value().device, instance.value().index, instance.value().valid};

    const SOCKET socket_handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_handle == INVALID_SOCKET) {
        return Result<SocketListener>::failure(map_socket_error(::WSAGetLastError()));
    }
    if (!set_nonblocking(static_cast<NativeHandle>(socket_handle)) ||
        !clear_inherit(reinterpret_cast<HANDLE>(socket_handle))) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketListener>::failure(Error::INTERNAL);
    }

    SOCKADDR_UN address{};
    std::array<char, kStoredPathCapacity> scratch{};
    if (!make_address(listener.endpoint_path_, address, scratch)) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketListener>::failure(Error::INVALID_ARGUMENT);
    }

    const auto stale = remove_stale_endpoint(listener.endpoint_path_);
    if (!stale) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketListener>::failure(stale.error());
    }

    if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        const Error error = map_socket_error(::WSAGetLastError());
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketListener>::failure(error);
    }
    if (!is_af_unix_reparse_path(listener.endpoint_path_.data()) ||
        !set_endpoint_security(listener.endpoint_path_.data())) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        (void)::DeleteFileW(listener.endpoint_path_.data());
        return Result<SocketListener>::failure(Error::INTERNAL);
    }
    const auto endpoint_identity =
        identity_of_path(listener.endpoint_path_, FILE_FLAG_OPEN_REPARSE_POINT);
    if (!endpoint_identity) {
        close_socket(static_cast<NativeHandle>(socket_handle));
        (void)::DeleteFileW(listener.endpoint_path_.data());
        return Result<SocketListener>::failure(Error::INTERNAL);
    }
    listener.endpoint_identity_ = FileIdentity{
        endpoint_identity.value().device, endpoint_identity.value().index, true};
    listener.created_endpoint_ = true;

    if (::listen(socket_handle, backlog) != 0) {
        const Error error = map_socket_error(::WSAGetLastError());
        close_socket(static_cast<NativeHandle>(socket_handle));
        return Result<SocketListener>::failure(error);
    }
    listener.fd_ = static_cast<NativeHandle>(socket_handle);
    return Result<SocketListener>::success(std::move(listener));
}

Result<SocketStream> SocketListener::accept(Timeout timeout) noexcept
{
    if (!valid()) {
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    while (true) {
        const SOCKET accepted = ::accept(static_cast<SOCKET>(fd_), nullptr, nullptr);
        if (accepted != INVALID_SOCKET) {
            if (!set_nonblocking(static_cast<NativeHandle>(accepted)) ||
                !clear_inherit(reinterpret_cast<HANDLE>(accepted))) {
                close_socket(static_cast<NativeHandle>(accepted));
                return Result<SocketStream>::failure(Error::INTERNAL);
            }
            return Result<SocketStream>::success(
                SocketStream(static_cast<NativeHandle>(accepted)));
        }
        const int error = ::WSAGetLastError();
        if (error == WSAEINTR) {
            if (deadline_expired(deadline)) {
                return Result<SocketStream>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != WSAEWOULDBLOCK) {
            return Result<SocketStream>::failure(map_socket_error(error));
        }
        const auto ready = wait_socket(fd_, POLLRDNORM, deadline);
        if (!ready) {
            return Result<SocketStream>::failure(ready.error());
        }
    }
}

void SocketListener::cleanup_paths() noexcept
{
    if (created_endpoint_ && endpoint_identity_.valid &&
        is_af_unix_reparse_path(endpoint_path_.data()) &&
        identity_matches_path(endpoint_identity_.device,
                              endpoint_identity_.inode, endpoint_path_,
                              FILE_FLAG_OPEN_REPARSE_POINT)) {
        (void)::DeleteFileW(endpoint_path_.data());
    }
    if (created_instance_directory_ && instance_identity_.valid &&
        identity_matches_path(instance_identity_.device,
                              instance_identity_.inode, instance_directory_,
                              FILE_FLAG_BACKUP_SEMANTICS)) {
        (void)::RemoveDirectoryW(instance_directory_.data());
    }
    if (created_version_directory_ && version_identity_.valid &&
        identity_matches_path(version_identity_.device, version_identity_.inode,
                              version_directory_, FILE_FLAG_BACKUP_SEMANTICS)) {
        (void)::RemoveDirectoryW(version_directory_.data());
    }
    if (created_product_directory_ && product_identity_.valid &&
        identity_matches_path(product_identity_.device, product_identity_.inode,
                              product_directory_, FILE_FLAG_BACKUP_SEMANTICS)) {
        (void)::RemoveDirectoryW(product_directory_.data());
    }
    created_endpoint_ = false;
    created_instance_directory_ = false;
    created_product_directory_ = false;
    created_version_directory_ = false;
}

void SocketListener::close() noexcept
{
    const NativeHandle handle = fd_;
    fd_ = kInvalidHandle;
    close_socket(handle);
    cleanup_paths();
}

}  // namespace px4::userland::ipc::posix

#endif  // _WIN32
