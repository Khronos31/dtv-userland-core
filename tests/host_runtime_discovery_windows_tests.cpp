// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_runtime/posix_ipc.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#include <array>
#include <iostream>
#include <string>
#include "windows/windows_security.h"

namespace {

using px4::userland::ipc::posix::EndpointAccess;

const char* runtime_parent_status_name(
    px4::userland::windows_security::RuntimeParentStatus status)
{
    using Status = px4::userland::windows_security::RuntimeParentStatus;
    switch (status) {
    case Status::accepted: return "accepted";
    case Status::invalid_attributes: return "invalid_attributes";
    case Status::current_user_sid_unavailable: return "current_user_sid_unavailable";
    case Status::security_query_failed: return "security_query_failed";
    case Status::owner_mismatch: return "owner_mismatch";
    case Status::dacl_rejected: return "dacl_rejected";
    }
    return "unknown";
}

void print_default_localappdata_diagnostic(const wchar_t* path)
{
    const DWORD attributes = ::GetFileAttributesW(path);
    px4::userland::windows_security::SidBuffer current_storage{};
    PSID current = px4::userland::windows_security::current_user_sid(current_storage);
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(const_cast<wchar_t*>(path), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &descriptor);
    LPWSTR current_text = nullptr;
    LPWSTR owner_text = nullptr;
    if (current != nullptr) (void)::ConvertSidToStringSidW(current, &current_text);
    if (status == ERROR_SUCCESS && owner != nullptr)
        (void)::ConvertSidToStringSidW(owner, &owner_text);
    std::wcerr << L"Windows default LOCALAPPDATA diagnostic: path=" << path
        << L" attributes=" << attributes
        << L" directory=" << ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
        << L" reparse=" << ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        << L" owner=" << (owner_text == nullptr ? L"unavailable" : owner_text)
        << L" token_user=" << (current_text == nullptr ? L"unavailable" : current_text)
        << L" security_query=" << status
        << L" verifier=" << runtime_parent_status_name(
            px4::userland::windows_security::runtime_parent_status(path)) << L"\n";
    if (status == ERROR_SUCCESS && dacl != nullptr) {
        ACL_SIZE_INFORMATION information{};
        if (::GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) {
            for (DWORD index = 0U; index < information.AceCount; ++index) {
                void* raw = nullptr;
                if (::GetAce(dacl, index, &raw) == 0 || raw == nullptr) continue;
                const auto* header = static_cast<const ACE_HEADER*>(raw);
                if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                    PSID sid = reinterpret_cast<PSID>(
                        const_cast<DWORD*>(&ace->SidStart));
                    LPWSTR sid_text = nullptr;
                    (void)::ConvertSidToStringSidW(sid, &sid_text);
                    std::wcerr << L"  DACL ACE type=" << header->AceType
                        << L" mask=0x" << std::hex << ace->Mask << std::dec
                        << L" sid=" << (sid_text == nullptr ? L"unavailable" : sid_text)
                        << L"\n";
                    if (sid_text != nullptr) (void)::LocalFree(sid_text);
                } else {
                    std::wcerr << L"  DACL ACE type=" << header->AceType << L"\n";
                }
            }
        }
    }
    if (current_text != nullptr) (void)::LocalFree(current_text);
    if (owner_text != nullptr) (void)::LocalFree(owner_text);
    if (descriptor != nullptr) (void)::LocalFree(descriptor);
}

std::wstring child_path(const std::wstring& parent, const wchar_t* child)
{
    std::wstring result = parent;
    result += L'\\';
    result += child;
    return result;
}

bool make_directory(const std::wstring& path)
{
    return ::CreateDirectoryW(path.c_str(), nullptr) != 0;
}

bool create_file(const std::wstring& path)
{
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    return ::CloseHandle(file) != 0;
}

bool to_utf8(const std::wstring& value, std::string& output)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return false;
    output.assign(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1,
            output.data(), size, nullptr, nullptr) != size) return false;
    output.resize(static_cast<std::size_t>(size - 1));
    return true;
}

struct TemporaryTree final {
    std::wstring root;
    std::wstring product;
    std::wstring version;
    std::wstring valid;
    std::wstring invalid;
    std::wstring file;
    bool root_created = false;
    bool product_created = false;
    bool version_created = false;
    bool valid_created = false;
    bool invalid_created = false;
    bool file_created = false;

    ~TemporaryTree()
    {
        if (file_created) (void)::DeleteFileW(file.c_str());
        if (invalid_created) (void)::RemoveDirectoryW(invalid.c_str());
        if (valid_created) (void)::RemoveDirectoryW(valid.c_str());
        if (version_created) (void)::RemoveDirectoryW(version.c_str());
        if (product_created) (void)::RemoveDirectoryW(product.c_str());
        if (root_created) (void)::RemoveDirectoryW(root.c_str());
    }
};

bool test_windows_runtime_instance_discovery()
{
    std::array<wchar_t, MAX_PATH> temporary{};
    std::array<wchar_t, MAX_PATH> unique{};
    if (::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) == 0U) {
        std::cerr << "Windows test GetTempPathW failed: " << ::GetLastError() << "\n";
        return false;
    }
    if (::GetTempFileNameW(temporary.data(), L"dtv", 0U, unique.data()) == 0U) {
        std::cerr << "Windows test GetTempFileNameW failed: " << ::GetLastError() << "\n";
        return false;
    }
    if (::DeleteFileW(unique.data()) == 0) {
        std::cerr << "Windows test DeleteFileW failed: " << ::GetLastError() << "\n";
        return false;
    }

    TemporaryTree tree;
    tree.root = std::wstring(unique.data()) + L"-検査";
    tree.product = child_path(tree.root, L"px4-userland");
    tree.version = child_path(tree.product, L"v1");
    tree.valid = child_path(tree.version, L"Live-01");
    tree.invalid = child_path(tree.version, L"invalid name");
    tree.file = child_path(tree.version, L"not-a-directory");
    if (!make_directory(tree.root)) {
        std::cerr << "Windows test CreateDirectoryW(root) failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.root_created = true;

    std::string runtime_directory;
    if (!to_utf8(tree.root, runtime_directory)) {
        std::cerr << "Windows test root UTF-8 conversion failed\n";
        return false;
    }
    const auto missing = px4::userland::ipc::posix::discover_runtime_instances(
        runtime_directory.c_str(), EndpointAccess::private_user, "px4-userland");
    if (missing || missing.error() != px4::userland::Error::NOT_FOUND) {
        std::cerr << "missing Windows runtime root was not reported as NOT_FOUND\n";
        return false;
    }

    if (!make_directory(tree.product)) {
        std::cerr << "Windows test CreateDirectoryW(product) failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.product_created = true;
    if (!make_directory(tree.version)) {
        std::cerr << "Windows test CreateDirectoryW(version) failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.version_created = true;
    if (!make_directory(tree.valid)) {
        std::cerr << "Windows test CreateDirectoryW(valid) failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.valid_created = true;
    if (!make_directory(tree.invalid)) {
        std::cerr << "Windows test CreateDirectoryW(invalid) failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.invalid_created = true;
    if (!create_file(tree.file)) {
        std::cerr << "Windows test CreateFileW failed: " << ::GetLastError() << "\n";
        return false;
    }
    tree.file_created = true;

    const auto discovered = px4::userland::ipc::posix::discover_runtime_instances(
        runtime_directory.c_str(), EndpointAccess::private_user, "px4-userland");
    if (!discovered || discovered.value().count != 1U ||
        discovered.value().canonical_collision ||
        std::string(discovered.value().instances[0U].data()) != "Live-01") {
        std::cerr << "Windows discovery did not return only the valid live instance: success="
                  << static_cast<bool>(discovered) << " count="
                  << (discovered ? discovered.value().count : 0U) << "\n";
        return false;
    }
    return true;
}

bool test_windows_default_runtime_root()
{
    std::array<wchar_t, MAX_PATH> temporary{};
    std::array<wchar_t, MAX_PATH> unique{};
    if (::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) == 0U ||
        ::GetTempFileNameW(temporary.data(), L"dtv", 0U, unique.data()) == 0U)
        return false;
    if (::DeleteFileW(unique.data()) == 0) return false;

    TemporaryTree tree;
    tree.root = std::wstring(unique.data()) + L"-runtime";
    if (!make_directory(tree.root)) return false;
    tree.root_created = true;
    if (!px4::userland::windows_security::set_current_user_owner_and_dacl(
            tree.root.c_str())) return false;

    std::array<wchar_t, 32768U> original{};
    const DWORD count = ::GetEnvironmentVariableW(L"LOCALAPPDATA", original.data(),
        static_cast<DWORD>(original.size()));
    const bool had_value = count > 0U && count < original.size();
    if (had_value) print_default_localappdata_diagnostic(original.data());
    const auto default_roots = px4::userland::ipc::posix::client_runtime_roots();
    const auto default_publish = px4::userland::ipc::posix::select_publish_runtime_root(
        "px4-userland", "windows-default");
    const auto default_lease = px4::userland::ipc::posix::select_lease_runtime_root();
    std::cerr << "Windows default LOCALAPPDATA use: roots="
        << static_cast<bool>(default_roots) << " publish="
        << static_cast<bool>(default_publish) << " lease="
        << static_cast<bool>(default_lease) << "\n";
    std::string expected;
    if (!to_utf8(tree.root, expected) ||
        ::SetEnvironmentVariableW(L"LOCALAPPDATA", tree.root.c_str()) == 0)
        return false;

    const auto roots = px4::userland::ipc::posix::client_runtime_roots();
    const auto publish = px4::userland::ipc::posix::select_publish_runtime_root(
        "px4-userland", "windows-default");
    const auto lease = px4::userland::ipc::posix::select_lease_runtime_root();
    bool ok = roots && roots.value().count == 1U &&
        std::string(roots.value().paths[0U].data()) == expected && publish && lease &&
        publish.value() == expected && lease.value() == expected;
    if (!ok) std::cerr << "Windows LOCALAPPDATA default mismatch: roots="
        << static_cast<bool>(roots) << " count=" << (roots ? roots.value().count : 0U)
        << " publish=" << static_cast<bool>(publish) << " lease=" << static_cast<bool>(lease)
        << " expected=" << expected << "\n";

    (void)::SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Windows\\Temp");
    const auto unsafe = px4::userland::ipc::posix::client_runtime_roots();
    ok = ok && !unsafe && unsafe.error() == px4::userland::Error::NOT_FOUND;
    if (unsafe) std::cerr << "unsafe LOCALAPPDATA was accepted\n";
    (void)::SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    const auto missing = px4::userland::ipc::posix::client_runtime_roots();
    ok = ok && !missing && missing.error() == px4::userland::Error::NOT_FOUND;
    if (missing) std::cerr << "missing LOCALAPPDATA produced a runtime root\n";
    (void)::SetEnvironmentVariableW(L"LOCALAPPDATA",
        had_value ? original.data() : nullptr);
    if (ok) std::cout << "windows_LOCALAPPDATA_default_unsafe_missing=PASS\n";
    return ok;
}

}  // namespace

int main()
{
    if (!test_windows_runtime_instance_discovery()) return 1;
    if (!test_windows_default_runtime_root()) return 1;
    return 0;
}
