// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_ipc.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <cwchar>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace px4::userland::ipc::posix {
namespace {

bool put_root(RuntimeRootCandidates& roots, const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0' || roots.count == roots.paths.size()) return false;
    const std::size_t length = std::strlen(path);
    if (length >= roots.paths[0].size()) return false;
    for (std::size_t index = 0U; index < roots.count; ++index) {
        if (std::strcmp(roots.paths[index].data(), path) == 0) return true;
    }
    std::memcpy(roots.paths[roots.count].data(), path, length + 1U);
    ++roots.count;
    return true;
}

bool valid_instance(const char* value) noexcept
{
    if (value == nullptr || value[0] == '\0') return false;
    const std::size_t length = std::strlen(value);
    if (length > 64U) return false;
    for (std::size_t index = 0U; index < length; ++index) {
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        const bool accepted = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
        if (!accepted) return false;
    }
    return true;
}

void add_instance(RuntimeInstanceDiscovery& result, const char* name) noexcept
{
    if (!valid_instance(name) || result.count == result.instances.size()) return;
    for (std::size_t index = 0U; index < result.count; ++index) {
        if (std::strcmp(result.instances[index].data(), name) == 0) return;
        std::string left(result.instances[index].data());
        std::string right(name);
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char ch) {
            return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char ch) {
            return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        });
        if (left == right) result.canonical_collision = true;
    }
    const std::size_t length = std::strlen(name);
    std::memcpy(result.instances[result.count].data(), name, length + 1U);
    ++result.count;
}

}  // namespace

Result<RuntimeRootCandidates> client_runtime_roots() noexcept
{
    RuntimeRootCandidates roots{};
#if defined(_WIN32)
    std::array<wchar_t, 512U> wide{};
    const DWORD count = ::GetEnvironmentVariableW(L"LOCALAPPDATA", wide.data(),
                                                   static_cast<DWORD>(wide.size()));
    if (count > 0U && count < wide.size()) {
        const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide.data(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 0 && bytes < static_cast<int>(kStoredPathCapacity)) {
            std::array<char, kStoredPathCapacity> path{};
            if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), -1,
                    path.data(), static_cast<int>(path.size()), nullptr, nullptr) == bytes)
                (void)put_root(roots, path.data());
        }
    }
    (void)put_root(roots, "C:\\Windows\\Temp");
#else
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] == '/') (void)put_root(roots, xdg);
    std::array<char, kStoredPathCapacity> user_runtime{};
    const int length = std::snprintf(user_runtime.data(), user_runtime.size(),
        "/run/user/%lu", static_cast<unsigned long>(::geteuid()));
    if (length > 0 && static_cast<std::size_t>(length) < user_runtime.size())
        (void)put_root(roots, user_runtime.data());
#endif
    if (roots.count == 0U) return Result<RuntimeRootCandidates>::failure(Error::NOT_FOUND);
    return Result<RuntimeRootCandidates>::success(roots);
}

Result<RuntimeInstanceDiscovery> discover_runtime_instances(
    const char* runtime_directory, EndpointAccess,
    const char* product_slug) noexcept
{
    if (runtime_directory == nullptr || product_slug == nullptr || product_slug[0] == '\0')
        return Result<RuntimeInstanceDiscovery>::failure(Error::INVALID_ARGUMENT);
    RuntimeInstanceDiscovery result{};
#if defined(_WIN32)
    const int root_size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        runtime_directory, -1, nullptr, 0);
    const int slug_size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        product_slug, -1, nullptr, 0);
    if (root_size <= 0 || slug_size <= 0) return Result<RuntimeInstanceDiscovery>::failure(Error::INVALID_ARGUMENT);
    std::wstring root(static_cast<std::size_t>(root_size), L'\0');
    std::wstring slug(static_cast<std::size_t>(slug_size), L'\0');
    (void)::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, runtime_directory, -1,
        root.data(), root_size);
    (void)::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, product_slug, -1,
        slug.data(), slug_size);
    std::wstring pattern = root + L"\\" + slug + L"\\v1\\*";
    WIN32_FIND_DATAW data{};
    HANDLE search = ::FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE)
        return Result<RuntimeInstanceDiscovery>::failure(Error::NOT_FOUND);
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) continue;
        const int name_size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            data.cFileName, -1, nullptr, 0, nullptr, nullptr);
        if (name_size <= 0 || name_size > 65) continue;
        std::array<char, 65U> name{};
        if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1,
                name.data(), static_cast<int>(name.size()), nullptr, nullptr) == name_size)
            add_instance(result, name.data());
    } while (::FindNextFileW(search, &data) != 0);
    (void)::FindClose(search);
#else
    const std::string product_path = std::string(runtime_directory) + "/" +
        product_slug + "/v1";
    DIR* directory = ::opendir(product_path.c_str());
    if (directory == nullptr) return Result<RuntimeInstanceDiscovery>::failure(Error::NOT_FOUND);
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        struct stat info{};
        const std::string path = product_path + "/" + entry->d_name;
        if (::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
            !S_ISLNK(info.st_mode) && info.st_uid == ::geteuid())
            add_instance(result, entry->d_name);
    }
    (void)::closedir(directory);
#endif
    return Result<RuntimeInstanceDiscovery>::success(result);
}

Result<SocketStream> SocketStream::connect(const EndpointConfig& endpoint,
                                          Timeout timeout) noexcept
{
    const auto roots = client_runtime_roots();
    const char* root = endpoint.runtime_directory;
    if (root == nullptr) {
        if (!roots) return Result<SocketStream>::failure(roots.error());
        root = roots.value().paths[0].data();
    }
    if (endpoint.instance == nullptr || endpoint.product_slug == nullptr ||
        endpoint.endpoint_name == nullptr)
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    dtv::userland::host_runtime::EndpointConfig config{
        root, endpoint.product_slug, endpoint.instance,
        endpoint.access == EndpointAccess::shared_group ?
            dtv::userland::host_runtime::EndpointAccess::shared_group :
            dtv::userland::host_runtime::EndpointAccess::private_user};
    config.endpoint_name = endpoint.endpoint_name;
    auto connected = dtv::userland::host_runtime::connect_endpoint(config, {timeout.milliseconds});
    if (!connected) return Result<SocketStream>::failure(connected.error());
    SocketStream stream;
    stream.transport_ = std::move(connected.value());
    return Result<SocketStream>::success(std::move(stream));
}

void SocketStream::close() noexcept
{
    if (transport_) transport_->close();
    transport_.reset();
}

Result<std::size_t> SocketStream::read_some(MutableByteView output, Timeout timeout) noexcept
{
    if (!transport_) return Result<std::size_t>::failure(Error::NOT_READY);
    return transport_->read_some(output, {timeout.milliseconds});
}

Result<void> SocketStream::write_all(ByteView bytes, Timeout timeout) noexcept
{
    if (!transport_) return Result<void>::failure(Error::NOT_READY);
    return transport_->write_all(bytes, {timeout.milliseconds});
}

}  // namespace px4::userland::ipc::posix
