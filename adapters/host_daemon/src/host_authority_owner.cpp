// SPDX-License-Identifier: GPL-2.0-only
#include "px4/host_authority_owner.h"

namespace px4::userland {
namespace {

}  // namespace

HostAuthorityOwner::HostAuthorityOwner(
    TunerService& service, const dtv::userland::wire::ServerHello& server,
    const dtv::userland::wire::ListResponse& canonical,
    dtv::userland::detail::ControlAuthority::TuneValidator validate_tune) noexcept
    : authority_(server, canonical, service.engine_, validate_tune)
{
}

}  // namespace px4::userland
