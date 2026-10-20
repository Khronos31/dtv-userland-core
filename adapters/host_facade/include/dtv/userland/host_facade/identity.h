// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_FACADE_IDENTITY_H
#define DTV_USERLAND_HOST_FACADE_IDENTITY_H

#include <string_view>

namespace dtv::userland::host_facade {

bool valid_runtime_instance(std::string_view value) noexcept;

}  // namespace dtv::userland::host_facade

#endif  // DTV_USERLAND_HOST_FACADE_IDENTITY_H
