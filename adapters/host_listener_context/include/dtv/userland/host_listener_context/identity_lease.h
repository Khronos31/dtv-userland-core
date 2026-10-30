// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_IDENTITY_LEASE_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_IDENTITY_LEASE_H

#include "dtv/userland/error.h"
#include "dtv/userland/host_runtime/control_socket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dtv::userland::host_listener_context {

enum class IdentityLeaseDiagnosticReason : std::uint8_t {
    none,
    runtime_directory,
    runtime_directory_missing,
    runtime_directory_lookup,
    runtime_directory_type,
    runtime_directory_owner,
    runtime_directory_mode,
    runtime_directory_security,
    identity,
    lock_prefix,
    lease_file_type,
    lease_file_owner,
    lease_file_mode,
    lease_file_links,
};

struct IdentityLeaseDiagnostic final {
    IdentityLeaseDiagnosticReason reason = IdentityLeaseDiagnosticReason::none;
};

// The product owns the runtime directory, the lock filename prefix (for
// example ".asicen-userland-") and when to acquire the lease. Core owns only
// the safe directory/lock checks and the last-holder cleanup.
struct IdentityLeaseConfig final {
    // Empty selects $XDG_RUNTIME_DIR on POSIX. Windows requires an explicit
    // absolute UTF-8 runtime directory.
    std::string_view runtime_directory;
    std::string_view lock_prefix;
    host_runtime::EndpointAccess access = host_runtime::EndpointAccess::private_user;
    // POSIX only: with shared_group, require the runtime directory owner to
    // match the effective user.
    bool require_effective_user_owner = false;
};

namespace detail {

#if defined(_WIN32)
using IdentityLeaseHandle = std::uintptr_t;
inline constexpr IdentityLeaseHandle kInvalidIdentityLeaseHandle =
    static_cast<IdentityLeaseHandle>(~static_cast<std::uintptr_t>(0U));
#else
using IdentityLeaseHandle = int;
inline constexpr IdentityLeaseHandle kInvalidIdentityLeaseHandle = -1;
#endif

}  // namespace detail

// Identity-level exclusive lease, independent of the endpoint instance. The
// lock lives directly in the runtime directory as
// <lock_prefix><identity>.lock. Move-only; the destructor releases the lease.
class IdentityLease final {
public:
    IdentityLease() noexcept = default;
    ~IdentityLease() noexcept;
    IdentityLease(IdentityLease&& other) noexcept;
    IdentityLease& operator=(IdentityLease&& other) noexcept;
    IdentityLease(const IdentityLease&) = delete;
    IdentityLease& operator=(const IdentityLease&) = delete;

    static Result<IdentityLease> acquire(
        const IdentityLeaseConfig& config, std::string_view identity) noexcept;
    static Result<IdentityLease> acquire(
        const IdentityLeaseConfig& config, std::string_view identity,
        IdentityLeaseDiagnostic* diagnostic) noexcept;

    bool valid() const noexcept
    {
        return lock_handle_ != detail::kInvalidIdentityLeaseHandle;
    }
    void close() noexcept;

private:
    detail::IdentityLeaseHandle lock_handle_ =
        detail::kInvalidIdentityLeaseHandle;
    detail::IdentityLeaseHandle directory_handle_ =
        detail::kInvalidIdentityLeaseHandle;
    std::array<char, 64U> filename_{};
#if defined(_WIN32)
    std::array<wchar_t, 512U> lock_path_{};
#endif
};

}  // namespace dtv::userland::host_listener_context

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_IDENTITY_LEASE_H
