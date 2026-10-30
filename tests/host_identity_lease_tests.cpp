// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/identity_lease.h"
#include "detail/directory_security.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

using dtv::userland::Error;
using dtv::userland::host_listener_context::IdentityLease;
using dtv::userland::host_listener_context::IdentityLeaseConfig;
using dtv::userland::host_listener_context::IdentityLeaseDiagnostic;
using dtv::userland::host_listener_context::IdentityLeaseDiagnosticReason;
using dtv::userland::host_runtime::EndpointAccess;
namespace directory_security =
    dtv::userland::host_listener_context::detail::directory_security;

namespace {

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::fprintf(stderr, "identity lease check failed at line %d: %s\n", \
                         __LINE__, #expression); \
            return false; \
        } \
    } while (false)

constexpr const char* kPrefix = ".asicen-userland-";

bool make_root(std::string& output) noexcept
{
    std::array<char, 64U> pattern{};
    const char* text = "/tmp/dtv-identity-XXXXXX";
    std::memcpy(pattern.data(), text, std::strlen(text) + 1U);
    char* created = ::mkdtemp(pattern.data());
    if (created == nullptr) return false;
    output = created;
    return true;
}

class RootGuard final {
public:
    explicit RootGuard(std::string path) noexcept : path_(std::move(path)) {}
    ~RootGuard() noexcept
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    RootGuard(const RootGuard&) = delete;
    RootGuard& operator=(const RootGuard&) = delete;

private:
    std::string path_;
};

std::string lock_path(const std::string& root, std::string_view identity) noexcept
{
    std::string path = root;
    path.push_back('/');
    path.append(kPrefix);
    path.append(identity.data(), identity.size());
    path.append(".lock");
    return path;
}

dtv::userland::Result<IdentityLease> acquire(
    const std::string& root, std::string_view identity,
    EndpointAccess access = EndpointAccess::private_user,
    IdentityLeaseDiagnostic* diagnostic = nullptr,
    bool require_effective_user_owner = false) noexcept
{
    const IdentityLeaseConfig config{root, kPrefix, access,
                                     require_effective_user_owner};
    return IdentityLease::acquire(config, identity, diagnostic);
}

bool file_exists(const std::string& path) noexcept
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

bool test_busy_and_independence()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    auto first = acquire(root, "identity-a");
    CHECK(first);
    struct stat status {};
    CHECK(::stat(lock_path(root, "identity-a").c_str(), &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    CHECK((status.st_mode & 07777) == 0600);
    CHECK(status.st_uid == ::geteuid());
    CHECK(status.st_nlink == 1);

    IdentityLeaseDiagnostic diagnostic;
    auto duplicate = acquire(root, "identity-a", EndpointAccess::private_user, &diagnostic);
    CHECK(!duplicate && duplicate.error() == Error::BUSY);
    CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::none);

    auto other = acquire(root, "identity-b");
    CHECK(other);
    CHECK(file_exists(lock_path(root, "identity-a")));
    CHECK(file_exists(lock_path(root, "identity-b")));

    first.value().close();
    CHECK(!file_exists(lock_path(root, "identity-a")));
    auto reacquired = acquire(root, "identity-a");
    CHECK(reacquired);
    return true;
}

bool test_raii_and_move()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    {
        auto lease = acquire(root, "raii");
        CHECK(lease);
        CHECK(file_exists(lock_path(root, "raii")));
    }
    CHECK(!file_exists(lock_path(root, "raii")));

    auto lease = acquire(root, "move-source");
    CHECK(lease);
    IdentityLease moved(std::move(lease.value()));
    CHECK(!lease.value().valid());
    CHECK(moved.valid());
    auto blocked = acquire(root, "move-source");
    CHECK(!blocked && blocked.error() == Error::BUSY);
    moved.close();
    CHECK(!file_exists(lock_path(root, "move-source")));
    auto reacquired = acquire(root, "move-source");
    CHECK(reacquired);

    auto assigned = acquire(root, "move-assign-source");
    CHECK(assigned);
    IdentityLease target;
    target = std::move(assigned.value());
    CHECK(target.valid());
    CHECK(!assigned.value().valid());
    auto assigned_blocked = acquire(root, "move-assign-source");
    CHECK(!assigned_blocked && assigned_blocked.error() == Error::BUSY);
    return true;
}

bool test_invalid_identity_and_prefix()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    for (const std::string_view identity : {"", "a/b", "a\\b", ".", ".."}) {
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, identity, EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::identity);
    }

    const std::string long_identity(80U, 'a');
    IdentityLeaseDiagnostic long_diagnostic;
    auto long_lease = acquire(root, long_identity, EndpointAccess::private_user,
                              &long_diagnostic);
    CHECK(!long_lease && long_lease.error() == Error::INVALID_ARGUMENT);
    CHECK(long_diagnostic.reason == IdentityLeaseDiagnosticReason::identity);

    for (const std::string_view prefix : {"", "a/b", "a\\b", "."}) {
        const IdentityLeaseConfig config{root, prefix, EndpointAccess::private_user};
        IdentityLeaseDiagnostic diagnostic;
        auto lease = IdentityLease::acquire(config, "identity", &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lock_prefix);
    }
    return true;
}

bool test_runtime_directory_diagnostics()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    {
        const std::string missing = root + "/missing";
        const IdentityLeaseConfig config{missing, kPrefix, EndpointAccess::private_user};
        IdentityLeaseDiagnostic diagnostic;
        auto lease = IdentityLease::acquire(config, "identity", &diagnostic);
        CHECK(!lease && lease.error() == Error::NOT_FOUND);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_missing);
        CHECK(!std::filesystem::exists(missing));
    }
    {
        const std::string file = root + "/file";
        const int descriptor = ::open(file.c_str(), O_CREAT | O_WRONLY, 0600);
        CHECK(descriptor >= 0);
        (void)::close(descriptor);
        const IdentityLeaseConfig config{file, kPrefix, EndpointAccess::private_user};
        IdentityLeaseDiagnostic diagnostic;
        auto lease = IdentityLease::acquire(config, "identity", &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_type);
    }
    {
        const std::string link = root + "/link";
        CHECK(::symlink(root.c_str(), link.c_str()) == 0);
        const IdentityLeaseConfig config{link, kPrefix, EndpointAccess::private_user};
        IdentityLeaseDiagnostic diagnostic;
        auto lease = IdentityLease::acquire(config, "identity", &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_type);
    }
    {
        CHECK(::chmod(root.c_str(), 0755) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "identity", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_mode);
        CHECK(::chmod(root.c_str(), 0700) == 0);
    }
    {
        CHECK(::chmod(root.c_str(), 0500) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "identity", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_mode);
        CHECK(::chmod(root.c_str(), 0700) == 0);
    }
    {
        CHECK(::chmod(root.c_str(), 0750) == 0);
        auto lease = acquire(root, "shared", EndpointAccess::shared_group);
        CHECK(lease);
        lease.value().close();
        auto strict_owner = acquire(root, "shared-strict", EndpointAccess::shared_group,
                                    nullptr, true);
        CHECK(strict_owner);
        strict_owner.value().close();
        CHECK(::chmod(root.c_str(), 0755) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto rejected = acquire(root, "shared", EndpointAccess::shared_group, &diagnostic);
        CHECK(!rejected && rejected.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_mode);
        CHECK(::chmod(root.c_str(), 0700) == 0);
    }
    if (::geteuid() == 0) {
        const std::string shared_owned = root + "/shared-owned";
        CHECK(::mkdir(shared_owned.c_str(), 0750) == 0);
        CHECK(::chown(shared_owned.c_str(), 65534, ::getegid()) == 0);
        CHECK(::chmod(shared_owned.c_str(), 0750) == 0);
        auto current_shared = acquire(shared_owned, "shared-current",
                                      EndpointAccess::shared_group);
        CHECK(current_shared);
        current_shared.value().close();
        IdentityLeaseDiagnostic shared_diagnostic;
        auto strict_shared = acquire(shared_owned, "shared-strict",
                                     EndpointAccess::shared_group,
                                     &shared_diagnostic, true);
        CHECK(!strict_shared && strict_shared.error() == Error::INVALID_ARGUMENT);
        CHECK(shared_diagnostic.reason ==
              IdentityLeaseDiagnosticReason::runtime_directory_owner);

        const std::string owned = root + "/owned";
        CHECK(::mkdir(owned.c_str(), 0700) == 0);
        CHECK(::chown(owned.c_str(), 65534, 65534) == 0);
        const IdentityLeaseConfig config{owned, kPrefix, EndpointAccess::private_user};
        IdentityLeaseDiagnostic diagnostic;
        auto lease = IdentityLease::acquire(config, "identity", &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::runtime_directory_owner);
        CHECK(::chown(owned.c_str(), ::geteuid(), ::getegid()) == 0);
    }
    return true;
}

bool test_owner_policy_predicate()
{
    constexpr uid_t effective_user = 1000U;
    constexpr gid_t effective_group = 100U;
    constexpr uid_t foreign_user = 2000U;

    CHECK(directory_security::ownership_allowed(
        foreign_user, effective_group, effective_user, effective_group,
        true, true, false, nullptr));
    CHECK(!directory_security::ownership_allowed(
        foreign_user, effective_group, effective_user, effective_group,
        true, true, true, nullptr));
    CHECK(!directory_security::ownership_allowed(
        foreign_user, 300U, effective_user, effective_group,
        true, true, false, nullptr));
    CHECK(!directory_security::ownership_allowed(
        foreign_user, 300U, effective_user, effective_group,
        true, true, true, nullptr));
    CHECK(directory_security::ownership_allowed(
        effective_user, 300U, effective_user, effective_group,
        true, true, true, nullptr));
    CHECK(directory_security::ownership_allowed(
        effective_user, 300U, effective_user, effective_group,
        false, false, false, nullptr));
    return true;
}

bool test_lock_file_diagnostics()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    {
        const std::string path = lock_path(root, "mode");
        const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
        CHECK(descriptor >= 0);
        (void)::close(descriptor);
        CHECK(::chmod(path.c_str(), 0644) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "mode", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lease_file_mode);
        CHECK(::unlink(path.c_str()) == 0);
    }
    {
        const std::string path = lock_path(root, "directory");
        CHECK(::mkdir(path.c_str(), 0700) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "directory", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lease_file_type);
        CHECK(::rmdir(path.c_str()) == 0);
    }
    {
        const std::string path = lock_path(root, "symlink");
        CHECK(::symlink("/tmp/identity-lease-target", path.c_str()) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "symlink", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lease_file_type);
        CHECK(::unlink(path.c_str()) == 0);
    }
    {
        const std::string path = lock_path(root, "links");
        const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
        CHECK(descriptor >= 0);
        (void)::close(descriptor);
        const std::string alias = root + "/links-alias";
        CHECK(::link(path.c_str(), alias.c_str()) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "links", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lease_file_links);
        CHECK(::unlink(alias.c_str()) == 0);
        CHECK(::unlink(path.c_str()) == 0);
    }
    if (::geteuid() == 0) {
        const std::string path = lock_path(root, "owner");
        const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY, 0600);
        CHECK(descriptor >= 0);
        (void)::close(descriptor);
        CHECK(::chown(path.c_str(), 65534, 65534) == 0);
        IdentityLeaseDiagnostic diagnostic;
        auto lease = acquire(root, "owner", EndpointAccess::private_user, &diagnostic);
        CHECK(!lease && lease.error() == Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason == IdentityLeaseDiagnosticReason::lease_file_owner);
        CHECK(::unlink(path.c_str()) == 0);
    }
    return true;
}

bool test_cross_process_busy()
{
    std::string root;
    CHECK(make_root(root));
    RootGuard guard(root);

    int ready[2] = {-1, -1};
    int release[2] = {-1, -1};
    CHECK(::pipe(ready) == 0);
    CHECK(::pipe(release) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready[0]);
        (void)::close(release[1]);
        auto lease = acquire(root, "cross-process");
        char status = lease ? '1' : '0';
        (void)::write(ready[1], &status, 1U);
        if (lease) {
            char byte = 0;
            (void)::read(release[0], &byte, 1U);
            lease.value().close();
        }
        _exit(0);
    }
    (void)::close(ready[1]);
    (void)::close(release[0]);
    char status = 0;
    CHECK(::read(ready[0], &status, 1U) == 1U);
    if (status != '1') {
        (void)::waitpid(child, nullptr, 0);
        return false;
    }
    IdentityLeaseDiagnostic diagnostic;
    auto blocked = acquire(root, "cross-process", EndpointAccess::private_user, &diagnostic);
    CHECK(!blocked && blocked.error() == Error::BUSY);
    const char byte = 'x';
    CHECK(::write(release[1], &byte, 1U) == 1U);
    int child_status = 0;
    CHECK(::waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);
    auto reacquired = acquire(root, "cross-process");
    CHECK(reacquired);
    return true;
}

}  // namespace

int main()
{
    if (!test_busy_and_independence() || !test_raii_and_move() ||
        !test_invalid_identity_and_prefix() ||
        !test_runtime_directory_diagnostics() ||
        !test_lock_file_diagnostics() || !test_cross_process_busy() ||
        !test_owner_policy_predicate()) {
        return 1;
    }
    std::printf("identity lease regression fixtures passed\n");
    return 0;
}
