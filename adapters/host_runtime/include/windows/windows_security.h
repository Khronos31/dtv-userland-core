// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINDOWS_SECURITY_H
#define PX4_USERLAND_WINDOWS_SECURITY_H

// Shared, header-only Windows security helpers for the platform adapters and
// the Windows tests. They create objects owned by the current user with a
// protected current-user-only DACL, which the token-default owner
// (BUILTIN\Administrators under an elevated token) would not provide.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <bcrypt.h>
#include <windows.h>

#include <array>
#include <cstdint>

namespace px4::userland::windows_security {

// Fills out with a lowercase hex token derived from the system preferred RNG.
// Returns false on failure or insufficient capacity.
inline bool random_hex(std::uint8_t* bytes, std::size_t count,
                       wchar_t* out, std::size_t capacity) noexcept
{
    if (count == 0U || capacity < count * 2U + 1U) {
        return false;
    }
    if (::BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(count),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return false;
    }
    static const wchar_t digits[] = L"0123456789abcdef";
    for (std::size_t index = 0U; index < count; ++index) {
        out[index * 2U] = digits[bytes[index] >> 4U];
        out[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    out[count * 2U] = L'\0';
    return true;
}

// Aligned storage for TOKEN_USER/SID queries.
struct alignas(16) SidBuffer final {
    std::uint8_t bytes[512U];
};

inline PSID current_user_sid(SidBuffer& storage) noexcept
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

// Builds a security descriptor whose owner is the current user and whose DACL
// grants the current user only, protected from inheritance. The caller owns
// acl_out and must LocalFree() it.
inline bool build_current_user_security(SECURITY_ATTRIBUTES& attributes,
                                        SECURITY_DESCRIPTOR& descriptor,
                                        PACL& acl_out,
                                        SidBuffer& sid_storage) noexcept
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

// Replaces the owner and DACL of an existing object with the current user and a
// protected current-user-only DACL. Used by test fixtures that std::filesystem
// creates with the token-default owner.
inline bool set_current_user_owner_and_dacl(const wchar_t* path) noexcept
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    SidBuffer sid_storage{};
    if (!build_current_user_security(attributes, descriptor, acl, sid_storage)) {
        return false;
    }
    PSID owner = current_user_sid(sid_storage);
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
    return status == ERROR_SUCCESS;
}

inline bool sid_equal(PSID left, PSID right) noexcept
{
    return left != nullptr && right != nullptr && ::EqualSid(left, right) != 0;
}

inline bool matches_well_known(PSID candidate, WELL_KNOWN_SID_TYPE type) noexcept
{
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (::CreateWellKnownSid(type, nullptr, buffer.data(), &size) == 0) {
        return false;
    }
    return sid_equal(candidate, reinterpret_cast<PSID>(buffer.data()));
}

// Privileged SIDs accepted on a caller-owned runtime parent. CREATOR OWNER and
// OWNER RIGHTS resolve to the verified owner and are safe here.
inline bool sid_is_trusted_for_runtime(PSID candidate) noexcept
{
    return matches_well_known(candidate, WinLocalSystemSid) ||
           matches_well_known(candidate, WinBuiltinAdministratorsSid) ||
           matches_well_known(candidate, WinCreatorOwnerSid) ||
           matches_well_known(candidate, WinCreatorOwnerRightsSid);
}

inline constexpr DWORD kUntrustedWriteMask =
    GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER |
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    FILE_DELETE_CHILD;

// True when the current user can write to the ACL and no untrusted SID can
// write or replace entries in it. Null/malformed ACLs and unknown ACE types
// fail closed.
inline bool acl_allows_only_trusted_writers(const ACL* dacl, PSID current) noexcept
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
            return false;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        PSID ace_sid =
            reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
        const DWORD mask = ace->Mask;
        if (sid_equal(ace_sid, current)) {
            if ((mask & kUntrustedWriteMask) != 0U) {
                current_user_can_write = true;
            }
            continue;
        }
        if (sid_is_trusted_for_runtime(ace_sid)) {
            continue;
        }
        if ((mask & kUntrustedWriteMask) != 0U) {
            return false;
        }
    }
    return current_user_can_write;
}

// Verifies a caller-owned runtime parent: a directory (not a reparse point)
// owned by the current user, with the current user able to write and no
// untrusted SID able to write/delete children.
inline bool verify_runtime_parent(const wchar_t* path) noexcept
{
    const DWORD attributes = ::GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }
    SidBuffer storage{};
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
    const bool ok =
        sid_equal(owner, current) && acl_allows_only_trusted_writers(dacl, current);
    (void)::LocalFree(descriptor);
    return ok;
}

}  // namespace px4::userland::windows_security

#endif  // _WIN32

#endif  // PX4_USERLAND_WINDOWS_SECURITY_H
