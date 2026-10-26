// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_ipc.h"

#include <algorithm>
#include <cerrno>
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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

#if !defined(_WIN32)
bool valid_absolute_path(const char* path) noexcept
{
    if (path == nullptr || path[0] != '/') return false;
    const char* cursor = path + 1;
    while (*cursor != '\0') {
        while (*cursor == '/') ++cursor;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') ++cursor;
        const std::size_t length = static_cast<std::size_t>(cursor - start);
        if ((length == 1U && start[0] == '.') ||
            (length == 2U && start[0] == '.' && start[1] == '.')) return false;
    }
    return true;
}

bool safe_parent_directory(const struct stat& info, uid_t owner) noexcept
{
    if (!S_ISDIR(info.st_mode) || (info.st_uid != owner && info.st_uid != 0U))
        return false;
    if ((info.st_mode & 0022) == 0) return true;
    return info.st_uid == 0U && (info.st_mode & S_ISVTX) != 0 &&
        (info.st_mode & 0002) != 0;
}

bool safe_private_directory(const struct stat& info, uid_t owner) noexcept
{
    return S_ISDIR(info.st_mode) && info.st_uid == owner &&
        (info.st_mode & 0077) == 0;
}

int open_directory_path(const char* path, bool require_private_final,
                        uid_t owner) noexcept
{
    if (!valid_absolute_path(path)) return -1;
    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) return -1;
    const char* cursor = path + 1;
    bool saw_component = false;
    while (*cursor != '\0') {
        while (*cursor == '/') ++cursor;
        if (*cursor == '\0') break;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '/') ++cursor;
        const std::string component(start, static_cast<std::size_t>(cursor - start));
        const int next = ::openat(current, component.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            const int error = errno;
            (void)::close(current);
            errno = error;
            return -1;
        }
        struct stat info{};
        const char* after = cursor;
        while (*after == '/') ++after;
        if (::fstat(next, &info) != 0) {
            const int error = errno;
            (void)::close(next);
            (void)::close(current);
            errno = error;
            return -1;
        }
        if ((require_private_final && *after == '\0'
                ? !safe_private_directory(info, owner)
                : !safe_parent_directory(info, owner))) {
            (void)::close(next);
            (void)::close(current);
            errno = EPERM;
            return -1;
        }
        (void)::close(current);
        current = next;
        saw_component = true;
    }
    if (!saw_component && require_private_final) {
        (void)::close(current);
        errno = EPERM;
        return -1;
    }
    return current;
}

bool validate_private_root(const char* path, uid_t owner) noexcept
{
    const int root = open_directory_path(path, true, owner);
    if (root < 0) return false;
    (void)::close(root);
    return true;
}

enum class PrivateRootStatus : std::uint8_t { valid, missing, overlong, refused };

PrivateRootStatus check_private_root(const char* path, uid_t owner) noexcept
{
    const int root = open_directory_path(path, true, owner);
    if (root >= 0) {
        (void)::close(root);
        return PrivateRootStatus::valid;
    }
    if (errno == ENOENT) return PrivateRootStatus::missing;
    if (errno == ENAMETOOLONG) return PrivateRootStatus::overlong;
    return PrivateRootStatus::refused;
}

bool create_private_root(const char* path, uid_t owner) noexcept
{
    if (!valid_absolute_path(path)) return false;
    const std::string full(path);
    const std::size_t slash = full.rfind('/');
    if (slash == std::string::npos || slash + 1U == full.size()) return false;
    const std::string parent = slash == 0U ? "/" : full.substr(0U, slash);
    const std::string leaf = full.substr(slash + 1U);
    const int parent_fd = open_directory_path(parent.c_str(), false, owner);
    if (parent_fd < 0) return false;
    if (::mkdirat(parent_fd, leaf.c_str(), 0700) != 0 && errno != EEXIST) {
        (void)::close(parent_fd);
        return false;
    }
    const int root_fd = ::openat(parent_fd, leaf.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    (void)::close(parent_fd);
    if (root_fd < 0) return false;
    struct stat info{};
    const bool safe = ::fstat(root_fd, &info) == 0 &&
        safe_private_directory(info, owner);
    (void)::close(root_fd);
    return safe;
}

bool put_private_candidate(RuntimeRootCandidates& roots, const char* base,
                           uid_t owner) noexcept
{
    if (!valid_absolute_path(base)) return false;
    std::string path(base);
    while (path.size() > 1U && path.back() == '/') path.pop_back();
    path += "/dtv-userland-";
    path += std::to_string(static_cast<unsigned long>(owner));
    if (path.size() >= kStoredPathCapacity) return false;
    return put_root(roots, path.c_str());
}

bool is_private_uid_root_candidate(const char* path, uid_t owner) noexcept
{
    if (path == nullptr) return false;
    const auto matches = [path, owner](const char* base) {
        if (!valid_absolute_path(base)) return false;
        std::string candidate(base);
        while (candidate.size() > 1U && candidate.back() == '/') candidate.pop_back();
        candidate += "/dtv-userland-";
        candidate += std::to_string(static_cast<unsigned long>(owner));
        return candidate == path;
    };
    const char* temporary = std::getenv("TMPDIR");
    if (matches(temporary)) return true;
    return matches("/tmp");
}
#endif

bool valid_instance(const char* value) noexcept
{
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, ".") == 0 ||
        std::strcmp(value, "..") == 0) return false;
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
    if (valid_absolute_path(xdg) && std::strlen(xdg) < kStoredPathCapacity)
        (void)put_root(roots, xdg);
    const uid_t owner = ::geteuid();
    const char* temporary = std::getenv("TMPDIR");
    if (valid_absolute_path(temporary))
        (void)put_private_candidate(roots, temporary, owner);
    std::array<char, kStoredPathCapacity> os_temporary{};
    const int length = std::snprintf(os_temporary.data(), os_temporary.size(),
        "/tmp/dtv-userland-%lu", static_cast<unsigned long>(owner));
    if (length > 0 && static_cast<std::size_t>(length) < os_temporary.size())
        (void)put_root(roots, os_temporary.data());
#endif
    if (roots.count == 0U) return Result<RuntimeRootCandidates>::failure(Error::NOT_FOUND);
    return Result<RuntimeRootCandidates>::success(roots);
}

Result<std::string> select_publish_runtime_root(const char* product_slug,
                                                const char* instance) noexcept
{
#if defined(_WIN32)
    (void)product_slug;
    (void)instance;
    return Result<std::string>::failure(Error::UNSUPPORTED);
#else
    if (product_slug == nullptr || instance == nullptr)
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    const dtv::userland::host_runtime::EndpointConfig minimum_control{
        "/", product_slug, instance};
    const auto minimum_control_path =
        dtv::userland::host_runtime::control_endpoint_path(minimum_control);
    if (!minimum_control_path)
        return Result<std::string>::failure(minimum_control_path.error());
    auto minimum_stream = minimum_control;
    minimum_stream.endpoint_name = "stream.sock";
    const auto minimum_stream_path =
        dtv::userland::host_runtime::endpoint_path(minimum_stream);
    if (!minimum_stream_path)
        return Result<std::string>::failure(minimum_stream_path.error());
    const auto roots = client_runtime_roots();
    if (!roots) return Result<std::string>::failure(roots.error());
    const uid_t owner = ::geteuid();
    bool path_usable = false;
    for (std::size_t index = 0U; index < roots.value().count; ++index) {
        const char* path = roots.value().paths[index].data();
        const dtv::userland::host_runtime::EndpointConfig control{
            path, product_slug, instance};
        const auto control_path =
            dtv::userland::host_runtime::control_endpoint_path(control);
        auto stream = control;
        stream.endpoint_name = "stream.sock";
        const auto stream_path = dtv::userland::host_runtime::endpoint_path(stream);
        if (!control_path || !stream_path) continue;
        path_usable = true;
        const bool ready = is_private_uid_root_candidate(path, owner)
            ? create_private_root(path, owner) : validate_private_root(path, owner);
        if (ready) return Result<std::string>::success(path);
    }
    if (!path_usable) return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    return Result<std::string>::failure(Error::UNSUPPORTED);
#endif
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
    root.resize(static_cast<std::size_t>(root_size - 1));
    slug.resize(static_cast<std::size_t>(slug_size - 1));
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
    return connect(endpoint, timeout, nullptr);
}

Result<SocketStream> SocketStream::connect(
    const EndpointConfig& endpoint, Timeout timeout,
    dtv::userland::host_runtime::EndpointConnectDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr)
        *diagnostic = dtv::userland::host_runtime::EndpointConnectDiagnostic{};
    if (endpoint.instance == nullptr || endpoint.product_slug == nullptr ||
        endpoint.endpoint_name == nullptr)
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    const auto connect_root = [&endpoint, timeout, diagnostic](const char* root) noexcept {
        dtv::userland::host_runtime::EndpointConfig config{
            root, endpoint.product_slug, endpoint.instance,
            endpoint.access == EndpointAccess::shared_group ?
                dtv::userland::host_runtime::EndpointAccess::shared_group :
                dtv::userland::host_runtime::EndpointAccess::private_user};
        config.endpoint_name = endpoint.endpoint_name;
        return dtv::userland::host_runtime::connect_endpoint(
            config, {timeout.milliseconds}, diagnostic);
    };
    if (endpoint.runtime_directory != nullptr) {
        auto connected = connect_root(endpoint.runtime_directory);
        if (!connected) return Result<SocketStream>::failure(connected.error());
        SocketStream stream;
        stream.transport_ = std::move(connected.value());
        return Result<SocketStream>::success(std::move(stream));
    }
    const auto roots = client_runtime_roots();
    if (!roots) return Result<SocketStream>::failure(roots.error());
#if defined(_WIN32)
    auto connected = connect_root(roots.value().paths[0].data());
    if (!connected) return Result<SocketStream>::failure(connected.error());
    SocketStream stream;
    stream.transport_ = std::move(connected.value());
    return Result<SocketStream>::success(std::move(stream));
#else
    const dtv::userland::host_runtime::EndpointConfig minimum_endpoint{
        "/", endpoint.product_slug, endpoint.instance,
        endpoint.access == EndpointAccess::shared_group ?
            dtv::userland::host_runtime::EndpointAccess::shared_group :
            dtv::userland::host_runtime::EndpointAccess::private_user};
    auto minimum_path_config = minimum_endpoint;
    minimum_path_config.endpoint_name = endpoint.endpoint_name;
    const auto minimum_path = dtv::userland::host_runtime::endpoint_path(
        minimum_path_config);
    if (!minimum_path) return Result<SocketStream>::failure(minimum_path.error());
    bool path_usable = false;
    dtv::userland::host_runtime::EndpointDiagnostic path_diagnostic{};
    for (std::size_t index = 0U; index < roots.value().count; ++index) {
        const char* root = roots.value().paths[index].data();
        const dtv::userland::host_runtime::EndpointConfig candidate{
            root, endpoint.product_slug, endpoint.instance,
            endpoint.access == EndpointAccess::shared_group ?
                dtv::userland::host_runtime::EndpointAccess::shared_group :
                dtv::userland::host_runtime::EndpointAccess::private_user};
        auto candidate_path_config = candidate;
        candidate_path_config.endpoint_name = endpoint.endpoint_name;
        const auto candidate_path = dtv::userland::host_runtime::endpoint_path(
            candidate_path_config, &path_diagnostic);
        if (!candidate_path)
            continue;
        path_usable = true;
        const PrivateRootStatus root_status = check_private_root(root, ::geteuid());
        if (root_status == PrivateRootStatus::missing ||
            root_status == PrivateRootStatus::overlong) continue;
        if (root_status == PrivateRootStatus::refused) {
            if (diagnostic != nullptr)
                diagnostic->stage = dtv::userland::host_runtime::EndpointConnectStage::runtime_namespace;
            return Result<SocketStream>::failure(Error::UNSUPPORTED);
        }
        auto connected = connect_root(root);
        if (!connected) {
            if (connected.error() == Error::NOT_FOUND ||
                connected.error() == Error::NOT_READY) continue;
            return Result<SocketStream>::failure(connected.error());
        }
        SocketStream stream;
        stream.transport_ = std::move(connected.value());
        return Result<SocketStream>::success(std::move(stream));
    }
    if (!path_usable && diagnostic != nullptr) {
        diagnostic->stage = dtv::userland::host_runtime::EndpointConnectStage::endpoint_path;
        diagnostic->endpoint = path_diagnostic;
    }
    if (diagnostic != nullptr && diagnostic->stage ==
            dtv::userland::host_runtime::EndpointConnectStage::none)
        diagnostic->stage = dtv::userland::host_runtime::EndpointConnectStage::endpoint;
    return Result<SocketStream>::failure(
        path_usable ? Error::NOT_FOUND : Error::INVALID_ARGUMENT);
#endif
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
