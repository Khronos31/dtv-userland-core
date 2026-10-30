// SPDX-License-Identifier: GPL-2.0-only
//
// ASICEN SerialEndpointLease::acquire_identity() Windows semantics
// (e2a86ca1) with the Core host_listener_context runtime/DACL checks reused.
// The product keeps the acquisition point, USB identity and lock prefix
// policy.
#include "dtv/userland/host_listener_context/identity_lease.h"

#include "detail/windows_path.h"
#include "detail/windows_security_checks.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>

#include <windows/windows_security.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace dtv::userland::host_listener_context {
namespace {

namespace windows_security = px4::userland::windows_security;
namespace windows_security_checks = detail::windows_security_checks;
namespace windows_path = detail::windows_path;

constexpr std::size_t kStoredPathCapacity = 512U;

bool valid_component(std::string_view value) noexcept
{
    if (value.empty() || value == "." || value == "..") return false;
    for (const char character : value) {
        const bool accepted =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.';
        if (!accepted) return false;
    }
    return true;
}

bool build_lock_filename(std::string_view prefix, std::string_view identity,
                         std::array<char, 64U>& filename) noexcept
{
    const int written = std::snprintf(filename.data(), filename.size(),
                                      "%.*s%.*s.lock",
                                      static_cast<int>(prefix.size()), prefix.data(),
                                      static_cast<int>(identity.size()), identity.data());
    return written > 0 && static_cast<std::size_t>(written) < filename.size();
}

Error win32_error(DWORD error) noexcept
{
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return Error::NOT_FOUND;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
        return Error::UNSUPPORTED;
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
        error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        return Error::BUSY;
    if (error == ERROR_INVALID_NAME || error == ERROR_INVALID_PARAMETER ||
        error == ERROR_FILENAME_EXCED_RANGE)
        return Error::INVALID_ARGUMENT;
    return Error::INTERNAL;
}

bool clear_inherit(HANDLE handle) noexcept
{
    return ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0U) != 0;
}

struct FileIdentity final {
    DWORD volume = 0U;
    std::uint64_t index = 0U;
    bool valid = false;
};

bool file_identity(HANDLE file, FileIdentity& output, bool require_regular) noexcept
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileInformationByHandle(file, &info) == 0) return false;
    if (require_regular &&
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return false;
    }
    output = FileIdentity{info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
            info.nFileIndexLow,
        true};
    return true;
}

bool identity_of_path(const std::wstring& path, FileIdentity& output) noexcept
{
    const HANDLE file = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = file_identity(file, output, true);
    (void)::CloseHandle(file);
    return ok;
}

bool same_identity(const FileIdentity& left, const FileIdentity& right) noexcept
{
    return left.valid && right.valid && left.volume == right.volume &&
           left.index == right.index;
}

void remove_lock_if_last_holder(const FileIdentity& identity,
                                const std::wstring& path) noexcept
{
    if (!identity.valid || path.empty()) return;
    FileIdentity current;
    if (!identity_of_path(path, current) || !same_identity(identity, current)) return;

    const HANDLE probe = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return;
    OVERLAPPED overlapped{};
    if (::LockFileEx(probe, LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK,
                     0, 1UL, 0UL, &overlapped) == 0) {
        (void)::CloseHandle(probe);
        return;
    }
    OVERLAPPED unlock{};
    (void)::UnlockFileEx(probe, 0, 1UL, 0UL, &unlock);
    (void)::CloseHandle(probe);
    (void)::DeleteFileW(path.c_str());
}

bool build_lock_path(const std::wstring& runtime, const std::wstring& filename,
                     std::wstring& output) noexcept
{
    std::wstring base = runtime;
    while (base.size() > 1U && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    output = base;
    if (!output.empty() && output.back() != L'\\' && output.back() != L'/') {
        output.push_back(L'\\');
    }
    output.append(filename);
    return output.size() < kStoredPathCapacity;
}

}  // namespace

IdentityLease::~IdentityLease() noexcept
{
    close();
}

IdentityLease::IdentityLease(IdentityLease&& other) noexcept
    : lock_handle_(other.lock_handle_), lock_path_(other.lock_path_)
{
    other.lock_handle_ = detail::kInvalidIdentityLeaseHandle;
}

IdentityLease& IdentityLease::operator=(IdentityLease&& other) noexcept
{
    if (this != &other) {
        close();
        lock_handle_ = other.lock_handle_;
        lock_path_ = other.lock_path_;
        other.lock_handle_ = detail::kInvalidIdentityLeaseHandle;
    }
    return *this;
}

Result<IdentityLease> IdentityLease::acquire(
    const IdentityLeaseConfig& config, std::string_view identity) noexcept
{
    return acquire(config, identity, nullptr);
}

Result<IdentityLease> IdentityLease::acquire(
    const IdentityLeaseConfig& config, std::string_view identity,
    IdentityLeaseDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr) *diagnostic = IdentityLeaseDiagnostic{};
    if (!valid_component(config.lock_prefix)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::lock_prefix;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (!valid_component(identity)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::array<char, 64U> filename{};
    if (!build_lock_filename(config.lock_prefix, identity, filename)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (config.runtime_directory.empty()) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    std::wstring runtime;
    if (!windows_security_checks::utf8_to_wide(
            std::string(config.runtime_directory.data(), config.runtime_directory.size()),
            runtime)) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (!windows_path::is_local_absolute(runtime)) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (!windows_security::verify_runtime_parent(runtime.c_str())) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_security;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    std::wstring filename_wide;
    if (!windows_security_checks::utf8_to_wide(
            std::string(filename.data(), std::strlen(filename.data())),
            filename_wide)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::wstring lock_path;
    if (!build_lock_path(runtime, filename_wide, lock_path)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    windows_security::SidBuffer sid_storage{};
    if (!windows_security::build_current_user_security(attributes, descriptor, acl,
                                                       sid_storage)) {
        return Result<IdentityLease>::failure(Error::INTERNAL);
    }
    const HANDLE file = ::CreateFileW(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD open_error =
        file == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    (void)::LocalFree(acl);
    if (file == INVALID_HANDLE_VALUE) {
        return Result<IdentityLease>::failure(
            open_error == ERROR_SHARING_VIOLATION ? Error::BUSY
                                                  : win32_error(open_error));
    }
    if (!clear_inherit(file)) {
        (void)::CloseHandle(file);
        return Result<IdentityLease>::failure(Error::INTERNAL);
    }

    FileIdentity lock_identity;
    if (!file_identity(file, lock_identity, true)) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_type;
        (void)::CloseHandle(file);
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (!windows_security_checks::verify_current_user_security(lock_path.c_str())) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_owner;
        (void)::CloseHandle(file);
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    OVERLAPPED overlapped{};
    if (::LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY | LOCKFILE_EXCLUSIVE_LOCK,
                     0, 1UL, 0UL, &overlapped) == 0) {
        const DWORD lock_error = ::GetLastError();
        (void)::CloseHandle(file);
        return Result<IdentityLease>::failure(
            lock_error == ERROR_LOCK_VIOLATION ? Error::BUSY
                                               : win32_error(lock_error));
    }
    FileIdentity named;
    if (!identity_of_path(lock_path, named) || !same_identity(lock_identity, named)) {
        OVERLAPPED unlock{};
        (void)::UnlockFileEx(file, 0, 1UL, 0UL, &unlock);
        (void)::CloseHandle(file);
        return Result<IdentityLease>::failure(Error::BUSY);
    }

    IdentityLease lease;
    lease.lock_handle_ = reinterpret_cast<detail::IdentityLeaseHandle>(file);
    std::copy(lock_path.begin(), lock_path.end(), lease.lock_path_.begin());
    lease.lock_path_[lock_path.size()] = L'\0';
    return Result<IdentityLease>::success(std::move(lease));
}

void IdentityLease::close() noexcept
{
    if (lock_handle_ == detail::kInvalidIdentityLeaseHandle) return;
    const HANDLE file = reinterpret_cast<HANDLE>(lock_handle_);
    FileIdentity identity;
    const bool identity_ok = file_identity(file, identity, true);
    OVERLAPPED overlapped{};
    (void)::UnlockFileEx(file, 0, 1UL, 0UL, &overlapped);
    (void)::CloseHandle(file);
    lock_handle_ = detail::kInvalidIdentityLeaseHandle;
    if (identity_ok) {
        remove_lock_if_last_holder(identity, std::wstring(lock_path_.data()));
    }
    lock_path_.fill(L'\0');
}

}  // namespace dtv::userland::host_listener_context
