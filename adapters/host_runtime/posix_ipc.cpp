// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_ipc_contract.h"

#include <dtv/userland/identity.h>

#if defined(PX4_POSIX_IPC_TEST_ACCESS)
#include "posix_ipc_test_access.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

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

constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateSocketMode = 0600;
constexpr mode_t kGroupDirectoryMode = 0750;
constexpr mode_t kGroupSocketMode = 0660;
constexpr const char* kProductDirectoryName = PX4_USERLAND_RUNTIME_PRODUCT;
constexpr const char* kRuntimeNamespaceVersion = "v1";
// Keep credential queries bounded even if a platform reports an unexpectedly
// large supplementary-group count.  This covers the supported POSIX targets;
// an over-limit or allocation failure deliberately fails closed.
constexpr std::size_t kMaxSupplementaryGroups = 65536U;

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using GroupQuery = int (*)(int, gid_t*) noexcept;

struct NativeIdentity final {
    std::uint64_t device;
    std::uint64_t inode;
};

struct Layout final {
    std::array<char, kStoredPathCapacity> runtime_directory{};
    std::array<char, kStoredPathCapacity> product_directory{};
    std::array<char, kStoredPathCapacity> version_directory{};
    std::array<char, kStoredPathCapacity> instance_directory{};
    std::array<char, kStoredPathCapacity> endpoint_path{};
    mode_t directory_mode = 0;
    mode_t socket_mode = 0;
    bool group_access = false;
};

Error map_errno(int error) noexcept
{
    switch (error) {
    case EINVAL:
    case ENAMETOOLONG:
    case ENOTDIR:
    case ELOOP:
        return Error::INVALID_ARGUMENT;
    case ENOENT:
        return Error::NOT_FOUND;
    case EADDRINUSE:
    case EALREADY:
        return Error::BUSY;
    case ETIMEDOUT:
        return Error::TIMEOUT;
    case ECONNREFUSED:
        return Error::NOT_READY;
    case ECONNRESET:
    case ENOTCONN:
    case EPIPE:
        return Error::DISCONNECTED;
    default:
        return Error::INTERNAL;
    }
}

void close_fd(int fd) noexcept
{
    if (fd >= 0) {
        // Retrying close after EINTR can close an unrelated reused descriptor on
        // platforms where the first close already consumed it.
        (void)::close(fd);
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

bool append_path(std::array<char, kStoredPathCapacity>& output,
                 std::size_t& used, const char* component,
                 std::size_t component_length, bool separator) noexcept
{
    const std::size_t extra = component_length + (separator ? 1U : 0U);
    if (used > output.size() || extra > output.size() - used ||
        used + extra >= output.size()) {
        return false;
    }
    if (separator) {
        output[used++] = '/';
    }
    if (component_length != 0U) {
        std::memcpy(output.data() + used, component, component_length);
        used += component_length;
    }
    output[used] = '\0';
    return true;
}

bool make_instance_tokens(const char* instance, std::array<char, 65U>& display,
                          std::array<char, 65U>& canonical) noexcept
{
    const std::size_t length = bounded_length(instance, display.size());
    if (length == 0U || length >= display.size()) return false;
    const auto normalized = dtv::userland::identity::canonicalize_instance(
        std::string_view(instance, length));
    if (!normalized || normalized.value().length != length) return false;
    std::memcpy(canonical.data(), normalized.value().bytes.data(), length);
    canonical[length] = '\0';
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE)
    std::memcpy(display.data(), instance, length);
    display[length] = '\0';
#else
    std::memcpy(display.data(), canonical.data(), length + 1U);
#endif
    return true;
}

Result<Layout> make_layout(const EndpointConfig& config) noexcept
{
    std::array<char, 65U> instance_name{};
    std::array<char, 65U> canonical_instance{};
    if (!make_instance_tokens(config.instance, instance_name, canonical_instance))
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    const char* runtime = config.runtime_directory;
    if (runtime == nullptr) {
        runtime = std::getenv("XDG_RUNTIME_DIR");
    }
    const std::size_t runtime_length = bounded_length(runtime, kStoredPathCapacity);
    if (runtime_length == 0U || runtime_length == kStoredPathCapacity ||
        runtime[0] != '/' ||
        !valid_component(config.endpoint_name)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    std::size_t normalized_runtime_length = runtime_length;
    while (normalized_runtime_length > 1U &&
           runtime[normalized_runtime_length - 1U] == '/') {
        --normalized_runtime_length;
    }

    Layout layout{};
    layout.directory_mode = config.access == EndpointAccess::shared_group ?
                                kGroupDirectoryMode : kPrivateDirectoryMode;
    layout.socket_mode = config.access == EndpointAccess::shared_group ?
                             kGroupSocketMode : kPrivateSocketMode;
    layout.group_access = config.access == EndpointAccess::shared_group;

    std::memcpy(layout.runtime_directory.data(), runtime,
                normalized_runtime_length);
    layout.runtime_directory[normalized_runtime_length] = '\0';

    std::size_t used = 0U;
    if (!append_path(layout.product_directory, used, runtime,
                     normalized_runtime_length, false) ||
        !append_path(layout.product_directory, used, kProductDirectoryName,
                     std::strlen(kProductDirectoryName),
                     normalized_runtime_length != 1U)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    const std::size_t product_length = used;
    std::memcpy(layout.version_directory.data(), layout.product_directory.data(),
                product_length + 1U);
    used = product_length;
    if (!append_path(layout.version_directory, used, kRuntimeNamespaceVersion,
                     std::strlen(kRuntimeNamespaceVersion), true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    const std::size_t version_length = used;
    std::memcpy(layout.instance_directory.data(), layout.version_directory.data(),
                version_length + 1U);
    used = version_length;
    const std::size_t instance_length = std::strlen(instance_name.data());
    if (!append_path(layout.instance_directory, used, instance_name.data(),
                     instance_length, true)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }

    const std::size_t directory_length = used;
    std::memcpy(layout.endpoint_path.data(), layout.instance_directory.data(),
                directory_length + 1U);
    used = directory_length;
    const std::size_t endpoint_length = std::strlen(config.endpoint_name);
    if (!append_path(layout.endpoint_path, used, config.endpoint_name,
                     endpoint_length, true) ||
        used >= sizeof(sockaddr_un{}.sun_path)) {
        return Result<Layout>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<Layout>::success(std::move(layout));
}

bool same_identity(const struct stat& status, const NativeIdentity& identity) noexcept
{
    return identity.device == static_cast<std::uint64_t>(status.st_dev) &&
           identity.inode == static_cast<std::uint64_t>(status.st_ino);
}

NativeIdentity identity_of(const struct stat& status) noexcept
{
    return NativeIdentity{static_cast<std::uint64_t>(status.st_dev),
                          static_cast<std::uint64_t>(status.st_ino)};
}

int native_getgroups(int size, gid_t* groups) noexcept
{
    return ::getgroups(size, groups);
}

bool supplementary_group_contains(gid_t group, GroupQuery getgroups) noexcept
{
    if (getgroups == nullptr) {
        return false;
    }
    const int group_count = getgroups(0, nullptr);
    if (group_count <= 0 ||
        static_cast<std::size_t>(group_count) > kMaxSupplementaryGroups) {
        return false;
    }

    const std::size_t count = static_cast<std::size_t>(group_count);
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(gid_t)) {
        return false;
    }
    auto* groups = static_cast<gid_t*>(std::malloc(count * sizeof(gid_t)));
    if (groups == nullptr) {
        return false;
    }

    const int actual_count = getgroups(group_count, groups);
    bool found = false;
    if (actual_count == group_count) {
        for (int index = 0; index < actual_count; ++index) {
            if (groups[index] == group) {
                found = true;
                break;
            }
        }
    }
    std::free(groups);
    return found;
}

bool ownership_allowed_for_ids(uid_t owner_uid, gid_t owner_gid,
                               uid_t effective_uid, gid_t effective_gid,
                               bool group_access, GroupQuery getgroups) noexcept
{
    return owner_uid == effective_uid ||
           (group_access && (owner_gid == effective_gid ||
                             supplementary_group_contains(owner_gid, getgroups)));
}

bool ownership_allowed(const struct stat& status, bool group_access) noexcept
{
    return ownership_allowed_for_ids(status.st_uid, status.st_gid, ::geteuid(),
                                     ::getegid(), group_access,
                                     native_getgroups);
}

bool safe_group_ancestor(const struct stat& status, uid_t owner) noexcept
{
    return S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) &&
           (status.st_uid == 0 || status.st_uid == owner) &&
           (status.st_mode & 0022) == 0;
}

Result<struct stat> validate_group_runtime(const char* path, mode_t mode) noexcept
{
    if (path == nullptr || path[0] != '/')
        return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
    struct stat anchor {};
    if (::lstat(path, &anchor) != 0)
        return Result<struct stat>::failure(map_errno(errno));
    if (!S_ISDIR(anchor.st_mode) || S_ISLNK(anchor.st_mode) ||
        (anchor.st_mode & 07777) != mode)
        return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return Result<struct stat>::failure(map_errno(errno));
    struct stat current {};
    if (::fstat(directory, &current) != 0 ||
        !safe_group_ancestor(current, anchor.st_uid)) {
        close_fd(directory);
        return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
    }
    const char* cursor = path + 1;
    while (*cursor == '/') ++cursor;
    if (*cursor == '\0') {
        close_fd(directory);
        return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
    }
    while (*cursor != '\0') {
        const char* end = cursor;
        while (*end != '\0' && *end != '/') ++end;
        const std::size_t length = static_cast<std::size_t>(end - cursor);
        if (length == 0U || length > NAME_MAX ||
            (length == 1U && cursor[0] == '.') ||
            (length == 2U && cursor[0] == '.' && cursor[1] == '.')) {
            close_fd(directory);
            return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
        }
        char component[NAME_MAX + 1U]{};
        std::memcpy(component, cursor, length);
        const int next = ::openat(directory, component,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            const Error error = map_errno(errno);
            close_fd(directory);
            return Result<struct stat>::failure(error);
        }
        struct stat status {};
        if (::fstat(next, &status) != 0) {
            const Error error = map_errno(errno);
            close_fd(next);
            close_fd(directory);
            return Result<struct stat>::failure(error);
        }
        const char* remainder = end;
        while (*remainder == '/') ++remainder;
        const bool final_component = *remainder == '\0';
        if ((final_component &&
             (!S_ISDIR(status.st_mode) || (status.st_mode & 07777) != mode ||
              !same_identity(status, identity_of(anchor)))) ||
            (!final_component && !safe_group_ancestor(status, anchor.st_uid))) {
            close_fd(next);
            close_fd(directory);
            return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
        }
        close_fd(directory);
        directory = next;
        current = status;
        cursor = remainder;
        if (final_component) {
            close_fd(directory);
            return Result<struct stat>::success(anchor);
        }
    }
    close_fd(directory);
    return Result<struct stat>::failure(Error::INVALID_ARGUMENT);
}

bool matches_group_anchor(const struct stat& status, const struct stat& root,
                          mode_t mode, bool socket) noexcept
{
    const bool correct_type = socket ? S_ISSOCK(status.st_mode) : S_ISDIR(status.st_mode);
    return correct_type && !S_ISLNK(status.st_mode) &&
           status.st_uid == root.st_uid && status.st_gid == root.st_gid &&
           (status.st_mode & 07777) == mode;
}

Result<NativeIdentity> validate_group_entry(const char* path, const struct stat& root,
                                            mode_t mode, bool socket) noexcept
{
    struct stat status {};
    if (::lstat(path, &status) != 0)
        return Result<NativeIdentity>::failure(map_errno(errno));
    if (!matches_group_anchor(status, root, mode, socket))
        return Result<NativeIdentity>::failure(Error::INVALID_ARGUMENT);
    return Result<NativeIdentity>::success(identity_of(status));
}

Result<void> validate_peer_uid(int fd, uid_t expected_uid) noexcept
{
#if defined(SO_PEERCRED)
    struct PeerCredential final {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } credential{};
    socklen_t length = static_cast<socklen_t>(sizeof(credential));
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) != 0)
        return Result<void>::failure(map_errno(errno));
    if (length != sizeof(credential) || credential.uid != expected_uid)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return Result<void>::success();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    uid_t peer_uid = static_cast<uid_t>(-1);
    gid_t peer_gid = static_cast<gid_t>(-1);
    if (::getpeereid(fd, &peer_uid, &peer_gid) != 0)
        return Result<void>::failure(map_errno(errno));
    (void)peer_gid;
    if (peer_uid != expected_uid)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return Result<void>::success();
#else
    (void)fd;
    (void)expected_uid;
    return Result<void>::failure(Error::UNSUPPORTED);
#endif
}

Result<NativeIdentity> validate_directory(const char* path, mode_t expected_mode,
                                          bool group_access = false) noexcept
{
    struct stat status {};
    if (::lstat(path, &status) != 0) {
        return Result<NativeIdentity>::failure(map_errno(errno));
    }
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
        !ownership_allowed(status, group_access) ||
        (status.st_mode & 07777) != expected_mode) {
        return Result<NativeIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<NativeIdentity>::success(identity_of(status));
}

Result<NativeIdentity> ensure_directory(const char* path, mode_t mode,
                                        bool& created) noexcept
{
    created = false;
    if (::mkdir(path, mode) == 0) {
        created = true;
        if (::chmod(path, mode) != 0) {
            return Result<NativeIdentity>::failure(map_errno(errno));
        }
    } else if (errno != EEXIST) {
        return Result<NativeIdentity>::failure(map_errno(errno));
    }
    return validate_directory(path, mode);
}

bool safe_private_runtime_root(const char* path) noexcept
{
    struct stat status {};
    return path != nullptr && ::lstat(path, &status) == 0 &&
           S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) &&
           status.st_uid == ::geteuid() && (status.st_mode & 07777) == 0700;
}

bool safe_private_runtime_parent(const char* path) noexcept
{
    const std::size_t length = bounded_length(path, kStoredPathCapacity);
    if (length == 0U || length == kStoredPathCapacity || path[0] != '/') return false;
    char parent[kStoredPathCapacity]{};
    std::memcpy(parent, path, length);
    std::size_t parent_length = length;
    while (parent_length > 1U && parent[parent_length - 1U] == '/')
        parent[--parent_length] = '\0';
    while (parent_length > 1U && parent[parent_length - 1U] != '/')
        parent[--parent_length] = '\0';
    if (parent_length > 1U) parent[--parent_length] = '\0';
    if (parent_length == 0U) {
        parent[0] = '/';
        parent[1] = '\0';
        parent_length = 1U;
    }

    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return false;
    struct stat status {};
    if (::fstat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
        close_fd(directory);
        return false;
    }
    const char* cursor = parent + 1;
    while (*cursor == '/') ++cursor;
    while (*cursor != '\0') {
        const char* end = cursor;
        while (*end != '\0' && *end != '/') ++end;
        const std::size_t component_length = static_cast<std::size_t>(end - cursor);
        if (component_length == 0U || component_length > NAME_MAX ||
            (component_length == 1U && cursor[0] == '.') ||
            (component_length == 2U && cursor[0] == '.' && cursor[1] == '.')) {
            close_fd(directory);
            return false;
        }
        char component[NAME_MAX + 1U]{};
        std::memcpy(component, cursor, component_length);
        const int next = ::openat(directory, component,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 || ::fstat(next, &status) != 0) {
            if (next >= 0) close_fd(next);
            close_fd(directory);
            return false;
        }
        const mode_t writable = status.st_mode & 0022;
        const bool safe_owner = status.st_uid == 0 || status.st_uid == ::geteuid();
        const bool safe_writable = writable == 0 ||
                                   (status.st_uid == 0 &&
                                    (status.st_mode & S_ISVTX) != 0);
        if (!S_ISDIR(status.st_mode) || !safe_owner || !safe_writable) {
            close_fd(next);
            close_fd(directory);
            return false;
        }
        close_fd(directory);
        directory = next;
        cursor = end;
        while (*cursor == '/') ++cursor;
    }
    close_fd(directory);
    return true;
}

bool add_runtime_root(RuntimeRootCandidates& candidates, const char* path,
                     bool create_if_missing = false) noexcept
{
    const std::size_t length = bounded_length(path, kStoredPathCapacity);
    if (length == 0U || length == kStoredPathCapacity || path[0] != '/') return false;
    std::size_t normalized = length;
    while (normalized > 1U && path[normalized - 1U] == '/') --normalized;
    for (std::size_t index = 0U; index < candidates.count; ++index) {
        if (std::strlen(candidates.paths[index].data()) == normalized &&
            std::memcmp(candidates.paths[index].data(), path, normalized) == 0)
            return true;
    }
    if (candidates.count >= candidates.paths.size() || normalized >= kStoredPathCapacity)
        return false;
    std::memcpy(candidates.paths[candidates.count].data(), path, normalized);
    candidates.paths[candidates.count][normalized] = '\0';
    candidates.create_if_missing[candidates.count] = create_if_missing;
    ++candidates.count;
    return true;
}

RuntimeRootCandidates make_runtime_root_candidates(
    const char* default_temp_root = nullptr) noexcept
{
    RuntimeRootCandidates candidates{};
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] != '\0') (void)add_runtime_root(candidates, xdg);
    const char* temp = std::getenv("TMPDIR");
    if (temp == nullptr || temp[0] != '/')
        temp = default_temp_root != nullptr ? default_temp_root : "/tmp";
    char user_root[kStoredPathCapacity]{};
    const int count = std::snprintf(
        user_root, sizeof(user_root), "%s%sdtv-userland-%llu", temp,
        temp[std::strlen(temp) - 1U] == '/' ? "" : "/",
        static_cast<unsigned long long>(::geteuid()));
    if (count > 0 && static_cast<std::size_t>(count) < sizeof(user_root))
        (void)add_runtime_root(candidates, user_root, true);
    char fallback[kStoredPathCapacity]{};
    const int fallback_count = std::snprintf(
        fallback, sizeof(fallback), "/tmp/dtv-userland-%llu",
        static_cast<unsigned long long>(::geteuid()));
    if (fallback_count > 0 &&
        static_cast<std::size_t>(fallback_count) < sizeof(fallback))
        (void)add_runtime_root(candidates, fallback, true);
    return candidates;
}

Result<std::array<char, kStoredPathCapacity>> select_publish_runtime_root_from(
    const char* default_temp_root) noexcept
{
    const RuntimeRootCandidates candidates =
        make_runtime_root_candidates(default_temp_root);
    for (std::size_t index = 0U; index < candidates.count; ++index) {
        const char* path = candidates.paths[index].data();
        if (safe_private_runtime_root(path))
            return Result<std::array<char, kStoredPathCapacity>>::success(
                candidates.paths[index]);
        if (!candidates.create_if_missing[index] || !safe_private_runtime_parent(path)) continue;
        if (::mkdir(path, 0700) == 0) {
            if (::chmod(path, 0700) != 0) continue;
        } else if (errno != EEXIST) {
            continue;
        }
        if (safe_private_runtime_root(path))
            return Result<std::array<char, kStoredPathCapacity>>::success(
                candidates.paths[index]);
    }
    return Result<std::array<char, kStoredPathCapacity>>::failure(Error::INTERNAL);
}

Result<void> configure_socket_fd(int fd) noexcept
{
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return Result<void>::failure(map_errno(errno));
    }
    const int status_flags = ::fcntl(fd, F_GETFL);
    if (status_flags < 0 ||
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return Result<void>::failure(map_errno(errno));
    }
#if !defined(MSG_NOSIGNAL)
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                     static_cast<socklen_t>(sizeof(enabled))) != 0) {
        return Result<void>::failure(map_errno(errno));
    }
#else
    return Result<void>::failure(Error::UNSUPPORTED);
#endif
#endif
    return Result<void>::success();
}

socklen_t make_address(const char* path, sockaddr_un& address) noexcept
{
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const std::size_t length = std::strlen(path);
    std::memcpy(address.sun_path, path, length + 1U);
    const std::size_t native_length = offsetof(sockaddr_un, sun_path) + length + 1U;
#if defined(__APPLE__)
    address.sun_len = static_cast<decltype(address.sun_len)>(native_length);
#endif
    return static_cast<socklen_t>(native_length);
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

Result<void> wait_fd(int fd, short events, Deadline deadline) noexcept
{
    while (true) {
        const int milliseconds = remaining_poll_milliseconds(deadline);
        if (milliseconds == 0) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1U, milliseconds);
        if (result > 0) {
            if ((descriptor.revents & POLLNVAL) != 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
            return Result<void>::success();
        }
        if (result == 0) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        if (errno != EINTR) {
            return Result<void>::failure(map_errno(errno));
        }
    }
}

Result<NativeIdentity> validate_socket_path(const char* path, mode_t mode,
                                            bool group_access = false) noexcept
{
    struct stat status {};
    if (::lstat(path, &status) != 0) {
        return Result<NativeIdentity>::failure(map_errno(errno));
    }
    if (!S_ISSOCK(status.st_mode) || S_ISLNK(status.st_mode) ||
        !ownership_allowed(status, group_access) ||
        (status.st_mode & 07777) != mode) {
        return Result<NativeIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<NativeIdentity>::success(identity_of(status));
}

Result<void> remove_stale_socket(const char* path, mode_t mode) noexcept
{
    struct stat initial_status {};
    if (::lstat(path, &initial_status) != 0) {
        return errno == ENOENT ? Result<void>::success() :
                                Result<void>::failure(map_errno(errno));
    }
    if (!S_ISSOCK(initial_status.st_mode) || S_ISLNK(initial_status.st_mode) ||
        initial_status.st_uid != ::geteuid() ||
        (initial_status.st_mode & 07777) != mode) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const NativeIdentity initial_identity = identity_of(initial_status);

    const int probe_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe_fd < 0) {
        return Result<void>::failure(map_errno(errno));
    }
    const auto configured = configure_socket_fd(probe_fd);
    if (!configured) {
        close_fd(probe_fd);
        return configured;
    }
    sockaddr_un address{};
    const socklen_t address_length = make_address(path, address);
    const int connect_result =
        ::connect(probe_fd, reinterpret_cast<const sockaddr*>(&address), address_length);
    const int connect_error = connect_result == 0 ? 0 : errno;
    close_fd(probe_fd);
    if (connect_result == 0 || connect_error == EINPROGRESS ||
        connect_error == EAGAIN || connect_error == EALREADY) {
        return Result<void>::failure(Error::BUSY);
    }
    if (connect_error != ECONNREFUSED) {
        return Result<void>::failure(map_errno(connect_error));
    }

    struct stat current_status {};
    if (::lstat(path, &current_status) != 0 ||
        !S_ISSOCK(current_status.st_mode) ||
        current_status.st_uid != ::geteuid() ||
        (current_status.st_mode & 07777) != mode ||
        !same_identity(current_status, initial_identity)) {
        return Result<void>::failure(Error::BUSY);
    }
    if (::unlink(path) != 0) {
        return Result<void>::failure(map_errno(errno));
    }
    return Result<void>::success();
}

void remove_owned_socket(const char* path, const NativeIdentity& identity,
                         mode_t mode) noexcept
{
    struct stat status {};
    if (::lstat(path, &status) == 0 && S_ISSOCK(status.st_mode) &&
        status.st_uid == ::geteuid() && (status.st_mode & 07777) == mode &&
        same_identity(status, identity)) {
        (void)::unlink(path);
    }
}

void remove_owned_directory(const char* path, const NativeIdentity& identity,
                            mode_t mode) noexcept
{
    struct stat status {};
    if (::lstat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
        status.st_uid == ::geteuid() && (status.st_mode & 07777) == mode &&
        same_identity(status, identity)) {
        // rmdir is intentionally used: unrelated contents make cleanup fail
        // safely instead of being recursively deleted.
        (void)::rmdir(path);
    }
}

}  // namespace

Result<RuntimeRootCandidates> client_runtime_roots() noexcept
{
    const RuntimeRootCandidates candidates = make_runtime_root_candidates();
    RuntimeRootCandidates safe{};
    for (std::size_t index = 0U; index < candidates.count; ++index) {
        if (safe_private_runtime_root(candidates.paths[index].data()))
            (void)add_runtime_root(safe, candidates.paths[index].data());
    }
    return safe.count == 0U ?
        Result<RuntimeRootCandidates>::failure(Error::NOT_FOUND) :
        Result<RuntimeRootCandidates>::success(std::move(safe));
}

Result<std::array<char, kStoredPathCapacity>> select_publish_runtime_root() noexcept
{
    return select_publish_runtime_root_from(nullptr);
}

Result<RuntimeInstanceDiscovery> discover_runtime_instances(
    const char* runtime_directory, EndpointAccess access) noexcept
{
    EndpointConfig probe{runtime_directory, "probe", kControlEndpointName, access};
    const auto probe_layout = make_layout(probe);
    if (!probe_layout)
        return Result<RuntimeInstanceDiscovery>::failure(probe_layout.error());
    const Layout& layout = probe_layout.value();
    Result<NativeIdentity> product = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    Result<NativeIdentity> version = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    struct stat group_root {};
    if (layout.group_access) {
        const auto root = validate_group_runtime(layout.runtime_directory.data(),
                                                 layout.directory_mode);
        if (!root) return Result<RuntimeInstanceDiscovery>::failure(root.error());
        group_root = root.value();
        if (::geteuid() != group_root.st_uid &&
            group_root.st_gid != ::getegid() &&
            !supplementary_group_contains(group_root.st_gid, native_getgroups))
            return Result<RuntimeInstanceDiscovery>::failure(Error::INTERNAL);
        product = validate_group_entry(layout.product_directory.data(), group_root,
                                       layout.directory_mode, false);
        version = validate_group_entry(layout.version_directory.data(), group_root,
                                       layout.directory_mode, false);
    } else {
        product = validate_directory(layout.product_directory.data(),
                                     layout.directory_mode);
        version = validate_directory(layout.version_directory.data(),
                                     layout.directory_mode);
    }
    if (!product || !version)
        return Result<RuntimeInstanceDiscovery>::failure(
            !product ? product.error() : version.error());

    const int version_fd = ::open(layout.version_directory.data(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (version_fd < 0)
        return Result<RuntimeInstanceDiscovery>::failure(map_errno(errno));
    struct stat opened_version {};
    if (::fstat(version_fd, &opened_version) != 0 ||
        !same_identity(opened_version, version.value())) {
        close_fd(version_fd);
        return Result<RuntimeInstanceDiscovery>::failure(Error::BUSY);
    }
    DIR* directories = ::fdopendir(version_fd);
    if (directories == nullptr) {
        const Error error = map_errno(errno);
        close_fd(version_fd);
        return Result<RuntimeInstanceDiscovery>::failure(error);
    }

    RuntimeInstanceDiscovery discovery{};
    std::array<std::array<char, 65U>, 256U> seen{};
    std::size_t seen_count = 0U;
    while (dirent* entry = ::readdir(directories)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;
        const std::string_view name(entry->d_name);
        const auto canonical = dtv::userland::identity::canonicalize_instance(name);
        if (!canonical) continue;
        std::array<char, 65U> token{};
        std::memcpy(token.data(), canonical.value().bytes.data(),
                    canonical.value().length);
        token[canonical.value().length] = '\0';
        for (std::size_t index = 0U; index < seen_count; ++index) {
            if (std::strcmp(seen[index].data(), token.data()) == 0) {
                discovery.canonical_collision = true;
                (void)::closedir(directories);
                return Result<RuntimeInstanceDiscovery>::success(discovery);
            }
        }
        if (seen_count >= seen.size()) {
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(Error::RESOURCE_LIMIT);
        }
        seen[seen_count++] = token;
#if defined(PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE)
        std::array<char, 65U> instance_name{};
        if (name.size() >= instance_name.size()) continue;
        std::memcpy(instance_name.data(), name.data(), name.size());
        instance_name[name.size()] = '\0';
#else
        if (name.size() != canonical.value().length ||
            std::memcmp(name.data(), token.data(), name.size()) != 0)
            continue;
        const auto instance_name = token;
#endif

        struct stat instance_status {};
        if (::fstatat(::dirfd(directories), entry->d_name, &instance_status,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            const Error error = map_errno(errno);
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(error);
        }
        const bool safe_instance = layout.group_access ?
            matches_group_anchor(instance_status, group_root,
                                 layout.directory_mode, false) :
            (S_ISDIR(instance_status.st_mode) && !S_ISLNK(instance_status.st_mode) &&
             instance_status.st_uid == ::geteuid() &&
             (instance_status.st_mode & 07777) == layout.directory_mode);
        if (!safe_instance) {
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(Error::INVALID_ARGUMENT);
        }

        EndpointConfig endpoint{layout.runtime_directory.data(), instance_name.data(),
                                kControlEndpointName, access};
        const auto endpoint_layout = make_layout(endpoint);
        if (!endpoint_layout) {
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(endpoint_layout.error());
        }
        Result<NativeIdentity> socket = Result<NativeIdentity>::failure(
            Error::INVALID_ARGUMENT);
        if (layout.group_access) {
            socket = validate_group_entry(endpoint_layout.value().endpoint_path.data(),
                                          group_root, layout.socket_mode, true);
        } else {
            socket = validate_socket_path(endpoint_layout.value().endpoint_path.data(),
                                          layout.socket_mode);
        }
        if (!socket) {
            if (socket.error() == Error::NOT_FOUND) continue;
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(socket.error());
        }
        if (discovery.count >= discovery.instances.size()) {
            (void)::closedir(directories);
            return Result<RuntimeInstanceDiscovery>::failure(Error::RESOURCE_LIMIT);
        }
        discovery.instances[discovery.count++] = instance_name;
    }
    (void)::closedir(directories);
    return Result<RuntimeInstanceDiscovery>::success(discovery);
}

#if defined(PX4_POSIX_IPC_TEST_ACCESS)
bool PosixIpcTestAccess::ownership_allowed(uid_t owner_uid, gid_t owner_gid,
                                            uid_t effective_uid,
                                            gid_t effective_gid,
                                            bool group_access,
                                            PosixIpcGetGroups getgroups) noexcept
{
    return ownership_allowed_for_ids(owner_uid, owner_gid, effective_uid,
                                     effective_gid, group_access, getgroups);
}

Result<std::array<char, kStoredPathCapacity>>
PosixIpcTestAccess::select_publish_runtime_root_with_default_temp_for_test(
    const char* default_temp_root) noexcept
{
    return select_publish_runtime_root_from(default_temp_root);
}
#endif

SerialEndpointLease::~SerialEndpointLease() noexcept
{
    close();
}

SerialEndpointLease::SerialEndpointLease(SerialEndpointLease&& other) noexcept
    : directory_fd_(other.directory_fd_), lock_fd_(other.lock_fd_),
      filename_(other.filename_)
{
    other.directory_fd_ = -1;
    other.lock_fd_ = -1;
}

SerialEndpointLease& SerialEndpointLease::operator=(SerialEndpointLease&& other) noexcept
{
    if (this != &other) {
        close();
        directory_fd_ = other.directory_fd_;
        lock_fd_ = other.lock_fd_;
        filename_ = other.filename_;
        other.directory_fd_ = -1;
        other.lock_fd_ = -1;
    }
    return *this;
}

Result<SerialEndpointLease> SerialEndpointLease::acquire(
    const EndpointConfig& endpoint, std::string_view observed_serial) noexcept
{
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
    std::array<char, kStoredPathCapacity> identity{};
    if (observed_identity.empty() || observed_identity.size() >= identity.size()) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::memcpy(identity.data(), observed_identity.data(), observed_identity.size());
    identity[observed_identity.size()] = '\0';
    if (!valid_component(identity.data()) ||
        std::strlen(identity.data()) != observed_identity.size()) {
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    }
    return acquire_validated(endpoint, observed_identity, true);
}

Result<SerialEndpointLease> SerialEndpointLease::acquire_validated(
    const EndpointConfig& endpoint, std::string_view observed_identity,
    bool force_exclusive) noexcept
{
    const auto layout_result = make_layout(endpoint);
    if (!layout_result) return Result<SerialEndpointLease>::failure(layout_result.error());
    const Layout& layout = layout_result.value();
    Result<NativeIdentity> runtime = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    if (layout.group_access) {
        const auto group_root = validate_group_runtime(layout.runtime_directory.data(),
                                                      layout.directory_mode);
        if (!group_root || group_root.value().st_uid != ::geteuid() ||
            group_root.value().st_gid != ::getegid())
            return Result<SerialEndpointLease>::failure(
                group_root ? Error::INVALID_ARGUMENT : group_root.error());
        runtime = Result<NativeIdentity>::success(identity_of(group_root.value()));
    } else {
        runtime = validate_directory(layout.runtime_directory.data(),
                                     layout.directory_mode);
    }
    if (!runtime) return Result<SerialEndpointLease>::failure(runtime.error());

    SerialEndpointLease lease;
    const int length = std::snprintf(lease.filename_.data(), lease.filename_.size(),
                                     "." PX4_USERLAND_RUNTIME_PRODUCT "-%.*s.lock",
                                     static_cast<int>(observed_identity.size()),
                                     observed_identity.data());
    if (length <= 0 || static_cast<std::size_t>(length) >= lease.filename_.size())
        return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
    lease.directory_fd_ = ::open(layout.runtime_directory.data(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (lease.directory_fd_ < 0)
        return Result<SerialEndpointLease>::failure(map_errno(errno));
    struct stat directory_status {};
    if (::fstat(lease.directory_fd_, &directory_status) != 0 ||
        !same_identity(directory_status, runtime.value()))
        return Result<SerialEndpointLease>::failure(Error::BUSY);

    const bool serial_instance =
        force_exclusive || std::string_view(endpoint.instance) == observed_identity;
    const int operation = (serial_instance ? LOCK_EX : LOCK_SH) | LOCK_NB;
    constexpr std::size_t kMaxStaleInodeRetries = 8U;
    for (std::size_t attempt = 0U; attempt < kMaxStaleInodeRetries; ++attempt) {
        int fd = ::openat(lease.directory_fd_, lease.filename_.data(),
                          O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (::fchmod(fd, 0600) != 0) {
                close_fd(fd);
                return Result<SerialEndpointLease>::failure(map_errno(errno));
            }
        } else if (errno == EEXIST) {
            fd = ::openat(lease.directory_fd_, lease.filename_.data(),
                          O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0 && errno == ENOENT) continue;
        }
        if (fd < 0) return Result<SerialEndpointLease>::failure(map_errno(errno));

        struct stat lock_status {};
        if (::fstat(fd, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) ||
            lock_status.st_uid != ::geteuid() ||
            (lock_status.st_mode & 07777) != 0600 || lock_status.st_nlink != 1) {
            close_fd(fd);
            return Result<SerialEndpointLease>::failure(Error::INVALID_ARGUMENT);
        }
        if (::flock(fd, operation) != 0) {
            const int lock_error = errno;
            struct stat named_status {};
            const bool stale = ::fstatat(lease.directory_fd_, lease.filename_.data(),
                                          &named_status, AT_SYMLINK_NOFOLLOW) != 0 ||
                               !same_identity(named_status, identity_of(lock_status));
            close_fd(fd);
            if (stale) continue;
            return Result<SerialEndpointLease>::failure(
                lock_error == EWOULDBLOCK || lock_error == EAGAIN ?
                    Error::BUSY : map_errno(lock_error));
        }
        struct stat named_status {};
        if (::fstatat(lease.directory_fd_, lease.filename_.data(), &named_status,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(named_status.st_mode) &&
            same_identity(named_status, identity_of(lock_status))) {
            lease.lock_fd_ = fd;
            return Result<SerialEndpointLease>::success(std::move(lease));
        }
        close_fd(fd);
    }
    return Result<SerialEndpointLease>::failure(Error::BUSY);
}

void SerialEndpointLease::close() noexcept
{
    if (lock_fd_ >= 0) {
        // Only the last holder can upgrade to exclusive. Never unlink a lock
        // inode while another custom-instance daemon still holds it shared.
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) {
            struct stat held_status {};
            struct stat named_status {};
            if (::fstat(lock_fd_, &held_status) == 0 &&
                ::fstatat(directory_fd_, filename_.data(), &named_status,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(named_status.st_mode) &&
                same_identity(named_status, identity_of(held_status))) {
                (void)::unlinkat(directory_fd_, filename_.data(), 0);
            }
        }
        close_fd(lock_fd_);
        lock_fd_ = -1;
    }
    close_fd(directory_fd_);
    directory_fd_ = -1;
}

SocketStream::~SocketStream() noexcept
{
    close();
}

SocketStream::SocketStream(SocketStream&& other) noexcept : fd_(other.fd_)
{
    other.fd_ = -1;
}

SocketStream& SocketStream::operator=(SocketStream&& other) noexcept
{
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Result<SocketStream> SocketStream::connect(const EndpointConfig& config,
                                           Timeout timeout) noexcept
{
    const auto layout_result = make_layout(config);
    if (!layout_result) {
        return Result<SocketStream>::failure(layout_result.error());
    }
    const Layout& layout = layout_result.value();

    // Every traversed project-owned directory and the endpoint itself must
    // already satisfy the selected ownership/mode contract.
    Result<NativeIdentity> runtime = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    Result<NativeIdentity> product = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    Result<NativeIdentity> version = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    Result<NativeIdentity> instance = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    Result<NativeIdentity> endpoint = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    struct stat group_root {};
    if (layout.group_access) {
        const auto root = validate_group_runtime(layout.runtime_directory.data(),
                                                 layout.directory_mode);
        if (!root) return Result<SocketStream>::failure(root.error());
        group_root = root.value();
        if (::geteuid() != group_root.st_uid &&
            group_root.st_gid != ::getegid() &&
            !supplementary_group_contains(group_root.st_gid, native_getgroups))
            return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
        runtime = Result<NativeIdentity>::success(identity_of(group_root));
        product = validate_group_entry(layout.product_directory.data(), group_root,
                                       layout.directory_mode, false);
        version = validate_group_entry(layout.version_directory.data(), group_root,
                                       layout.directory_mode, false);
        instance = validate_group_entry(layout.instance_directory.data(), group_root,
                                        layout.directory_mode, false);
        endpoint = validate_group_entry(layout.endpoint_path.data(), group_root,
                                        layout.socket_mode, true);
    } else {
        runtime = validate_directory(layout.runtime_directory.data(),
                                     layout.directory_mode);
        product = validate_directory(layout.product_directory.data(),
                                     layout.directory_mode);
        version = validate_directory(layout.version_directory.data(),
                                     layout.directory_mode);
        instance = validate_directory(layout.instance_directory.data(),
                                      layout.directory_mode);
        endpoint = validate_socket_path(layout.endpoint_path.data(),
                                        layout.socket_mode);
    }
    if (!runtime || !product || !version || !instance || !endpoint) {
        const Error error = !runtime ? runtime.error() :
                            (!product ? product.error() :
                             (!version ? version.error() :
                              (!instance ? instance.error() : endpoint.error())));
        return Result<SocketStream>::failure(error);
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<SocketStream>::failure(map_errno(errno));
    }
    const auto configured = configure_socket_fd(fd);
    if (!configured) {
        close_fd(fd);
        return Result<SocketStream>::failure(configured.error());
    }

    sockaddr_un address{};
    const socklen_t address_length = make_address(layout.endpoint_path.data(), address);
    const Deadline deadline = deadline_after(timeout);
    const int result =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length);
    if (result != 0) {
        const int initial_error = errno;
        if (initial_error != EINPROGRESS && initial_error != EAGAIN &&
            initial_error != EALREADY && initial_error != EINTR) {
            close_fd(fd);
            return Result<SocketStream>::failure(map_errno(initial_error));
        }
        const auto ready = wait_fd(fd, POLLOUT, deadline);
        if (!ready) {
            close_fd(fd);
            return Result<SocketStream>::failure(ready.error());
        }
        int socket_error = 0;
        socklen_t socket_error_size = static_cast<socklen_t>(sizeof(socket_error));
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                         &socket_error_size) != 0) {
            const Error error = map_errno(errno);
            close_fd(fd);
            return Result<SocketStream>::failure(error);
        }
        if (socket_error != 0) {
            close_fd(fd);
            return Result<SocketStream>::failure(map_errno(socket_error));
        }
    }
    struct stat connected_endpoint {};
    if (::lstat(layout.endpoint_path.data(), &connected_endpoint) != 0 ||
        !same_identity(connected_endpoint, endpoint.value()) ||
        !S_ISSOCK(connected_endpoint.st_mode)) {
        close_fd(fd);
        return Result<SocketStream>::failure(Error::BUSY);
    }
    const auto peer = validate_peer_uid(fd, connected_endpoint.st_uid);
    if (!peer) {
        close_fd(fd);
        return Result<SocketStream>::failure(peer.error());
    }
    return Result<SocketStream>::success(SocketStream(fd));
}

void SocketStream::close() noexcept
{
    const int fd = fd_;
    fd_ = -1;
    close_fd(fd);
}

Result<std::size_t> SocketStream::read_some(MutableByteView output,
                                            Timeout timeout) noexcept
{
    if (!valid() || output.data == nullptr || output.size == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    const std::size_t maximum = static_cast<std::size_t>(
        std::numeric_limits<ssize_t>::max());
    const std::size_t size = std::min(output.size, maximum);
    while (true) {
        const ssize_t received = ::recv(fd_, output.data, size, 0);
        if (received > 0) {
            return Result<std::size_t>::success(static_cast<std::size_t>(received));
        }
        if (received == 0) {
            return Result<std::size_t>::failure(Error::DISCONNECTED);
        }
        const int error = errno;
        if (error == EINTR) {
            if (deadline_expired(deadline)) {
                return Result<std::size_t>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return Result<std::size_t>::failure(map_errno(error));
        }
        const auto ready = wait_fd(fd_, POLLIN, deadline);
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
    const std::size_t maximum = static_cast<std::size_t>(
        std::numeric_limits<ssize_t>::max());
    while (offset < bytes.size) {
        const std::size_t amount = std::min(bytes.size - offset, maximum);
#if defined(MSG_NOSIGNAL)
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const ssize_t sent = ::send(fd_, bytes.data + offset, amount, send_flags);
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
        const int error = errno;
        if (error == EINTR) {
            if (deadline_expired(deadline)) {
                return Result<void>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return Result<void>::failure(map_errno(error));
        }
        const auto ready = wait_fd(fd_, POLLOUT, deadline);
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

    other.fd_ = -1;
    other.created_product_directory_ = false;
    other.created_version_directory_ = false;
    other.created_instance_directory_ = false;
    other.created_endpoint_ = false;
}

Result<SocketListener> SocketListener::listen(const EndpointConfig& config,
                                              int backlog) noexcept
{
    if (backlog <= 0) {
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
    listener.directory_mode_ = static_cast<std::uint16_t>(layout.directory_mode);
    listener.socket_mode_ = static_cast<std::uint16_t>(layout.socket_mode);

    // XDG_RUNTIME_DIR (or the explicit replacement) is supplied by the caller
    // and must already be a safe directory. The adapter owns only descendants.
    Result<NativeIdentity> runtime_check = Result<NativeIdentity>::failure(
        Error::INVALID_ARGUMENT);
    struct stat group_root {};
    if (layout.group_access) {
        const auto root = validate_group_runtime(layout.runtime_directory.data(),
                                                 layout.directory_mode);
        if (!root || root.value().st_uid != ::geteuid() ||
            root.value().st_gid != ::getegid())
            return Result<SocketListener>::failure(
                root ? Error::INVALID_ARGUMENT : root.error());
        group_root = root.value();
        runtime_check = Result<NativeIdentity>::success(identity_of(group_root));
    } else {
        runtime_check = validate_directory(layout.runtime_directory.data(),
                                           layout.directory_mode);
    }
    if (!runtime_check)
        return Result<SocketListener>::failure(runtime_check.error());

    bool created = false;
    const auto product = ensure_directory(listener.product_directory_.data(),
                                          layout.directory_mode, created);
    listener.created_product_directory_ = created;
    if (!product) {
        return Result<SocketListener>::failure(product.error());
    }
    if (layout.group_access) {
        const auto anchored = validate_group_entry(listener.product_directory_.data(),
                                                   group_root,
                                                   layout.directory_mode, false);
        if (!anchored) return Result<SocketListener>::failure(anchored.error());
    }
    listener.product_identity_ = FileIdentity{
        product.value().device, product.value().inode, true};

    const auto version = ensure_directory(listener.version_directory_.data(),
                                          layout.directory_mode, created);
    listener.created_version_directory_ = created;
    if (!version) {
        return Result<SocketListener>::failure(version.error());
    }
    if (layout.group_access) {
        const auto anchored = validate_group_entry(listener.version_directory_.data(),
                                                   group_root,
                                                   layout.directory_mode, false);
        if (!anchored) return Result<SocketListener>::failure(anchored.error());
    }
    listener.version_identity_ = FileIdentity{
        version.value().device, version.value().inode, true};

    const auto instance = ensure_directory(listener.instance_directory_.data(),
                                           layout.directory_mode, created);
    listener.created_instance_directory_ = created;
    if (!instance) {
        return Result<SocketListener>::failure(instance.error());
    }
    if (layout.group_access) {
        const auto anchored = validate_group_entry(listener.instance_directory_.data(),
                                                   group_root,
                                                   layout.directory_mode, false);
        if (!anchored) return Result<SocketListener>::failure(anchored.error());
    }
    listener.instance_identity_ = FileIdentity{
        instance.value().device, instance.value().inode, true};

    const auto stale = remove_stale_socket(listener.endpoint_path_.data(),
                                           layout.socket_mode);
    if (!stale) {
        return Result<SocketListener>::failure(stale.error());
    }

    listener.fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener.fd_ < 0) {
        return Result<SocketListener>::failure(map_errno(errno));
    }
    const auto configured = configure_socket_fd(listener.fd_);
    if (!configured) {
        return Result<SocketListener>::failure(configured.error());
    }

    sockaddr_un address{};
    const socklen_t address_length = make_address(listener.endpoint_path_.data(), address);
    if (::bind(listener.fd_, reinterpret_cast<const sockaddr*>(&address),
               address_length) != 0) {
        return Result<SocketListener>::failure(map_errno(errno));
    }
    struct stat endpoint_status {};
    if (::lstat(listener.endpoint_path_.data(), &endpoint_status) != 0 ||
        !S_ISSOCK(endpoint_status.st_mode) ||
        endpoint_status.st_uid != ::geteuid()) {
        return Result<SocketListener>::failure(Error::INTERNAL);
    }
    listener.endpoint_identity_ = FileIdentity{
        static_cast<std::uint64_t>(endpoint_status.st_dev),
        static_cast<std::uint64_t>(endpoint_status.st_ino), true};
    listener.created_endpoint_ = true;

    if (::chmod(listener.endpoint_path_.data(), layout.socket_mode) != 0) {
        return Result<SocketListener>::failure(map_errno(errno));
    }
    if (layout.group_access) {
        const auto anchored = validate_group_entry(listener.endpoint_path_.data(),
                                                   group_root,
                                                   layout.socket_mode, true);
        if (!anchored) return Result<SocketListener>::failure(anchored.error());
    }
    const auto endpoint = validate_socket_path(listener.endpoint_path_.data(),
                                               layout.socket_mode);
    if (!endpoint || endpoint.value().device != listener.endpoint_identity_.device ||
        endpoint.value().inode != listener.endpoint_identity_.inode) {
        return Result<SocketListener>::failure(
            endpoint ? Error::BUSY : endpoint.error());
    }
    if (::listen(listener.fd_, backlog) != 0) {
        return Result<SocketListener>::failure(map_errno(errno));
    }
    return Result<SocketListener>::success(std::move(listener));
}

Result<SocketStream> SocketListener::accept(Timeout timeout) noexcept
{
    if (!valid()) {
        return Result<SocketStream>::failure(Error::INVALID_ARGUMENT);
    }
    const Deadline deadline = deadline_after(timeout);
    while (true) {
        const int accepted = ::accept(fd_, nullptr, nullptr);
        if (accepted >= 0) {
            const auto configured = configure_socket_fd(accepted);
            if (!configured) {
                close_fd(accepted);
                return Result<SocketStream>::failure(configured.error());
            }
            return Result<SocketStream>::success(SocketStream(accepted));
        }
        const int error = errno;
        if (error == EINTR) {
            if (deadline_expired(deadline)) {
                return Result<SocketStream>::failure(Error::TIMEOUT);
            }
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            return Result<SocketStream>::failure(map_errno(error));
        }
        const auto ready = wait_fd(fd_, POLLIN, deadline);
        if (!ready) {
            return Result<SocketStream>::failure(ready.error());
        }
    }
}

void SocketListener::cleanup_paths() noexcept
{
    const mode_t directory_mode = static_cast<mode_t>(directory_mode_);
    const mode_t socket_mode = static_cast<mode_t>(socket_mode_);
    if (created_endpoint_ && endpoint_identity_.valid) {
        remove_owned_socket(endpoint_path_.data(),
                            NativeIdentity{endpoint_identity_.device,
                                           endpoint_identity_.inode},
                            socket_mode);
    }
    if (created_instance_directory_ && instance_identity_.valid) {
        remove_owned_directory(instance_directory_.data(),
                               NativeIdentity{instance_identity_.device,
                                              instance_identity_.inode},
                               directory_mode);
    }
    if (created_version_directory_ && version_identity_.valid) {
        remove_owned_directory(version_directory_.data(),
                               NativeIdentity{version_identity_.device,
                                              version_identity_.inode},
                               directory_mode);
    }
    if (created_product_directory_ && product_identity_.valid) {
        remove_owned_directory(product_directory_.data(),
                               NativeIdentity{product_identity_.device,
                                              product_identity_.inode},
                               directory_mode);
    }
    created_endpoint_ = false;
    created_instance_directory_ = false;
    created_product_directory_ = false;
    created_version_directory_ = false;
}

void SocketListener::close() noexcept
{
    const int fd = fd_;
    fd_ = -1;
    close_fd(fd);
    cleanup_paths();
}

}  // namespace px4::userland::ipc::posix
