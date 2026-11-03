// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_POSIX_CLIENT_COMPAT_TEST_H
#define DTV_USERLAND_POSIX_CLIENT_COMPAT_TEST_H

#include <cstdint>

namespace px4::userland::ipc::posix {

bool test_auto_root_parent_policy(std::uint32_t directory_owner,
                                  std::uint32_t directory_group,
                                  std::uint32_t mode,
                                  std::uint32_t effective_user,
                                  std::uint32_t effective_group,
                                  bool android_policy) noexcept;

}  // namespace px4::userland::ipc::posix

#endif  // DTV_USERLAND_POSIX_CLIENT_COMPAT_TEST_H
