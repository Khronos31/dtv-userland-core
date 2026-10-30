// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_runtime/posix_ipc.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <iostream>
#include <string>

namespace {

using px4::userland::ipc::posix::EndpointAccess;

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
    if (::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) == 0U ||
        ::GetTempFileNameW(temporary.data(), L"dtv", 0U, unique.data()) == 0U)
        return false;
    if (::DeleteFileW(unique.data()) == 0) return false;

    TemporaryTree tree;
    tree.root = std::wstring(unique.data()) + L"-検査";
    tree.product = child_path(tree.root, L"px4-userland");
    tree.version = child_path(tree.product, L"v1");
    tree.valid = child_path(tree.version, L"Live-01");
    tree.invalid = child_path(tree.version, L"invalid name");
    tree.file = child_path(tree.version, L"not-a-directory");
    if (!make_directory(tree.root)) return false;
    tree.root_created = true;

    std::string runtime_directory;
    if (!to_utf8(tree.root, runtime_directory)) return false;
    const auto missing = px4::userland::ipc::posix::discover_runtime_instances(
        runtime_directory.c_str(), EndpointAccess::private_user, "px4-userland");
    if (missing || missing.error() != px4::userland::Error::NOT_FOUND) {
        std::cerr << "missing Windows runtime root was not reported as NOT_FOUND\n";
        return false;
    }

    if (!make_directory(tree.product)) return false;
    tree.product_created = true;
    if (!make_directory(tree.version)) return false;
    tree.version_created = true;
    if (!make_directory(tree.valid)) return false;
    tree.valid_created = true;
    if (!make_directory(tree.invalid)) return false;
    tree.invalid_created = true;
    if (!create_file(tree.file)) return false;
    tree.file_created = true;

    const auto discovered = px4::userland::ipc::posix::discover_runtime_instances(
        runtime_directory.c_str(), EndpointAccess::private_user, "px4-userland");
    if (!discovered || discovered.value().count != 1U ||
        discovered.value().canonical_collision ||
        std::string(discovered.value().instances[0U].data()) != "Live-01") {
        std::cerr << "Windows discovery did not return only the valid live instance\n";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    if (!test_windows_runtime_instance_discovery()) return 1;
    return 0;
}
