// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_SECURITY_CHECKS_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_SECURITY_CHECKS_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>

#include <windows/windows_security.h>

#include <climits>
#include <cstddef>
#include <string>

namespace dtv::userland::host_listener_context::detail::windows_security_checks {

inline bool utf8_to_wide(const std::string& input, std::wstring& output) noexcept
{
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1,
                                             nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1,
                              output.data(), count) != count) return false;
    output.resize(static_cast<std::size_t>(count - 1));
    return true;
}

inline bool verify_current_user_security(const wchar_t* path) noexcept
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
    px4::userland::windows_security::SidBuffer sid_storage{};
    PSID current = px4::userland::windows_security::current_user_sid(sid_storage);
    bool accepted = current != nullptr &&
        px4::userland::windows_security::sid_equal(owner, current) &&
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
                accepted = px4::userland::windows_security::sid_equal(trustee, current);
            } else if (header->AceType != ACCESS_DENIED_ACE_TYPE &&
                       header->AceType != SYSTEM_AUDIT_ACE_TYPE) {
                accepted = false;
            }
        }
    }
    (void)::LocalFree(descriptor);
    return accepted;
}

}  // namespace dtv::userland::host_listener_context::detail::windows_security_checks

#endif  // _WIN32

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_WINDOWS_SECURITY_CHECKS_H
