// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_RUNTIME_TEST_ACCESS_H
#define DTV_HOST_RUNTIME_TEST_ACCESS_H

#include <dtv/userland/control_client.h>

namespace dtv::userland::host_runtime::test_access {

bool set_client_send_buffer(wire::TimedClientTransport& transport,
                            int bytes) noexcept;
int client_send_buffer(wire::TimedClientTransport& transport) noexcept;

}  // namespace dtv::userland::host_runtime::test_access

#endif  // DTV_HOST_RUNTIME_TEST_ACCESS_H
