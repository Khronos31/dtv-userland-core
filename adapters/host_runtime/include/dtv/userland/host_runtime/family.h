// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_RUNTIME_FAMILY_H
#define DTV_USERLAND_HOST_RUNTIME_FAMILY_H

#include "px4/card_service.h"
#include "px4/tuner_service.h"

namespace dtv::userland::host_runtime {

// One enclosure's product-owned hardware and clock dependencies. A later
// listener Context owns this interface and constructs the services once.
class EnclosureFamily {
public:
    virtual ~EnclosureFamily() noexcept = default;
    virtual px4::userland::Result<void> shutdown() noexcept
    {
        return px4::userland::Result<void>::success();
    }
    virtual px4::userland::TunerServiceBackend& tuner_backend() noexcept = 0;
    virtual px4::userland::CardServiceBackend& card_backend() noexcept = 0;
    virtual px4::userland::CardProtocolSession& card_protocol() noexcept = 0;
    virtual px4::userland::TunerServiceTime& time() noexcept = 0;
    virtual px4::userland::TunerStreamControl* stream_control() noexcept = 0;
};

using TunerNonceSource = px4::userland::TunerNonceSource;

}  // namespace dtv::userland::host_runtime

#endif  // DTV_USERLAND_HOST_RUNTIME_FAMILY_H
