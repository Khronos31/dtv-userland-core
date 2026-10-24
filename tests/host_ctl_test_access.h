// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_CTL_TEST_ACCESS_H
#define DTV_HOST_CTL_TEST_ACCESS_H

#include "host_ctl.h"
#include "px4/posix_ipc.h"

namespace dtv::userland::host_ctl::test_access {

int run_control_cli_with_roots(
    int argc, const char* const* argv, const Profile& profile,
    const px4::userland::ipc::posix::RuntimeRootCandidates& roots) noexcept;

}  // namespace dtv::userland::host_ctl::test_access

#endif  // DTV_HOST_CTL_TEST_ACCESS_H
