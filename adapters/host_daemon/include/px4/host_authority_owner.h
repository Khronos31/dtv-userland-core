// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_HOST_AUTHORITY_OWNER_H
#define PX4_HOST_AUTHORITY_OWNER_H

#include "px4/tuner_service.h"

#include <dtv/userland/detail/control_authority.h>

namespace px4::userland {

class HostAuthorityOwner final {
public:
    HostAuthorityOwner(TunerService& service,
                       const dtv::userland::wire::ServerHello& server,
                       const dtv::userland::wire::ListResponse& canonical,
                       dtv::userland::detail::ControlAuthority::TuneValidator
                           validate_tune = nullptr) noexcept;

    dtv::userland::detail::ControlAuthority& authority() noexcept { return authority_; }

private:
    dtv::userland::detail::ControlAuthority authority_;
};

}  // namespace px4::userland

#endif  // PX4_HOST_AUTHORITY_OWNER_H
