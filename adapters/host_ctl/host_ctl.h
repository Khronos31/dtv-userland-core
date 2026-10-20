// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_CTL_H
#define DTV_USERLAND_HOST_CTL_H

#include <dtv/userland/identity.h>
#include <dtv/userland/records.h>

#include <string_view>

namespace dtv::userland::host_ctl {

using InstanceNormalizer = bool (*)(std::string_view,
                                    identity::InstanceText&) noexcept;
using SerialMatcher = bool (*)(const wire::ListResponse&,
                               const identity::ResolvedIdentity&) noexcept;

struct Profile final {
    const char* tool_name = nullptr;
    const char* product_slug = nullptr;
    const char* product_version = nullptr;
    identity::Product product = identity::Product::px4;
    bool card_supported = false;
    bool serial_selector_supported = false;
    InstanceNormalizer normalize_instance = nullptr;
    SerialMatcher matches_serial = nullptr;
    bool lnb_power_supported = false;
    bool internal_card_status_supported = false;
};

int run_control_cli(int argc, const char* const* argv,
                    const Profile& profile) noexcept;

}  // namespace dtv::userland::host_ctl

#endif  // DTV_USERLAND_HOST_CTL_H
