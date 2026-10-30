// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_DIRECTORY_SECURITY_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_DIRECTORY_SECURITY_H

#if !defined(_WIN32)

#include "dtv/userland/host_runtime/control_socket.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace dtv::userland::host_listener_context::detail::directory_security {

enum class Status : std::uint8_t {
    ok,
    missing,
    lookup,
    type,
    owner,
    mode,
    security,
};

struct Identity final {
    std::uint64_t device = 0U;
    std::uint64_t inode = 0U;
    bool valid = false;
};

struct Check final {
    Status status = Status::ok;
    int system_error = 0;
    Identity identity{};
};

// exact_mode selects the ASICEN lease rule (0700/0750 exactly) over the
// listener rule (private: no group/other write, shared: exact 0750).
// group_owner_membership selects the ASICEN shared rule (owner user, matching
// primary group, or a supplementary group) over the listener rule (owner user
// and, for shared access, the effective group).
struct Policy final {
    host_runtime::EndpointAccess access = host_runtime::EndpointAccess::private_user;
    bool exact_mode = false;
    bool group_owner_membership = false;
    bool require_effective_user_owner = false;
};

using GroupQuery = int (*)(int, gid_t*) noexcept;

inline Identity identity_of(const struct stat& info) noexcept
{
    return Identity{static_cast<std::uint64_t>(info.st_dev),
                    static_cast<std::uint64_t>(info.st_ino), true};
}

inline bool same_identity(const Identity& expected, const struct stat& actual) noexcept
{
    return expected.valid &&
           expected.device == static_cast<std::uint64_t>(actual.st_dev) &&
           expected.inode == static_cast<std::uint64_t>(actual.st_ino);
}

inline int native_getgroups(int size, gid_t* groups) noexcept
{
    return ::getgroups(size, groups);
}

inline bool supplementary_group_contains(gid_t group, GroupQuery getgroups) noexcept
{
    if (getgroups == nullptr) {
        return false;
    }
    constexpr std::size_t kMaxSupplementaryGroups = 65536U;
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

inline bool ownership_allowed(uid_t owner_uid, gid_t owner_gid,
                              uid_t effective_uid, gid_t effective_gid,
                              bool shared_group, bool group_owner_membership,
                              bool require_effective_user_owner,
                              GroupQuery getgroups) noexcept
{
    if (require_effective_user_owner) {
        return owner_uid == effective_uid;
    }
    if (owner_uid == effective_uid) {
        return !shared_group || group_owner_membership || owner_gid == effective_gid;
    }
    return shared_group && group_owner_membership &&
           (owner_gid == effective_gid ||
            supplementary_group_contains(owner_gid, getgroups));
}

inline Check inspect(const std::string& path, const Policy& policy,
                     uid_t owner, gid_t group,
                     GroupQuery getgroups = nullptr) noexcept
{
    Check check;
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        check.system_error = errno;
        check.status = errno == ENOENT ? Status::missing : Status::lookup;
        return check;
    }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
        check.status = Status::type;
        return check;
    }
    const bool shared = policy.access == host_runtime::EndpointAccess::shared_group;
    if (!ownership_allowed(info.st_uid, info.st_gid, owner, group, shared,
                           policy.group_owner_membership,
                           policy.require_effective_user_owner, getgroups)) {
        check.status = Status::owner;
        return check;
    }
    const mode_t allowed_mode = shared ? 0750 : 0700;
    const bool allowed = policy.exact_mode
        ? (info.st_mode & 07777) == allowed_mode
        : (shared ? (info.st_mode & 07777) == 0750
                  : (info.st_mode & 0077) == 0);
    if (!allowed) {
        check.status = Status::mode;
        return check;
    }
    check.identity = identity_of(info);
    return check;
}

inline Check ensure(const std::string& path, const Policy& policy,
                    uid_t owner, gid_t group) noexcept
{
    const bool created = ::mkdir(path.c_str(),
        policy.access == host_runtime::EndpointAccess::private_user ? 0700 : 0750) == 0;
    if (!created && errno != EEXIST) {
        Check check;
        check.system_error = errno;
        check.status = errno == EACCES || errno == EPERM
            ? Status::security
            : errno == ENOENT ? Status::missing : Status::lookup;
        return check;
    }
    if (created && policy.access == host_runtime::EndpointAccess::shared_group &&
        ::chmod(path.c_str(), 0750) != 0) {
        Check check;
        check.system_error = errno;
        check.status = Status::mode;
        return check;
    }
    return inspect(path, policy, owner, group);
}

inline bool shared_ancestors_secure(const std::string& path, uid_t owner) noexcept
{
    std::string normalized(path);
    while (normalized.size() > 1U && normalized.back() == '/') normalized.pop_back();
    if (normalized.empty() || normalized.front() != '/') {
        return false;
    }
    std::string current;
    std::size_t offset = 1U;
    while (offset < normalized.size()) {
        const std::size_t separator = normalized.find('/', offset);
        const std::size_t end = separator == std::string::npos ? normalized.size() : separator;
        const std::string_view component(normalized.data() + offset, end - offset);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        current.push_back('/');
        current.append(component.data(), component.size());
        struct stat info{};
        if (::lstat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
            S_ISLNK(info.st_mode) || (info.st_mode & 0022) != 0 ||
            (info.st_uid != 0 && info.st_uid != owner)) {
            return false;
        }
        if (separator == std::string::npos) break;
        offset = separator + 1U;
    }
    return true;
}

}  // namespace dtv::userland::host_listener_context::detail::directory_security

#endif  // !defined(_WIN32)

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_DIRECTORY_SECURITY_H
