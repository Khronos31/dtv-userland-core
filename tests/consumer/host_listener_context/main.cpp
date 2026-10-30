// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_listener_context/enclosure_context.h>
#include <dtv/userland/host_listener_context/identity_lease.h>

int main()
{
    dtv::userland::host_listener_context::IdentityLeaseConfig lease_config{};
    lease_config.access = dtv::userland::host_runtime::EndpointAccess::shared_group;
    lease_config.require_effective_user_owner = true;
    if (!lease_config.require_effective_user_owner) return 1;

    const auto assembled =
        dtv::userland::host_listener_context::EnclosureContext::assemble(
            nullptr, nullptr, {});
    return !assembled && assembled.error() == dtv::userland::Error::INVALID_ARGUMENT
        ? 0 : 1;
}
