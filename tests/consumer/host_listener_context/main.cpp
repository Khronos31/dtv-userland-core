// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_listener_context/enclosure_context.h>

int main()
{
    const auto assembled =
        dtv::userland::host_listener_context::EnclosureContext::assemble(
            nullptr, nullptr, {});
    return !assembled && assembled.error() == dtv::userland::Error::INVALID_ARGUMENT
        ? 0 : 1;
}
