// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_facade/identity.h>

#include <dtv/userland/identity.h>

namespace dtv::userland::host_facade {

bool valid_runtime_instance(std::string_view value) noexcept
{
    return static_cast<bool>(dtv::userland::identity::canonicalize_instance(value));
}

}  // namespace dtv::userland::host_facade
