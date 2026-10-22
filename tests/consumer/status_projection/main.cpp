// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/status_projection.h"

int main()
{
    dtv::userland::wire::ListResponse canonical;
    dtv::userland::wire::StatusResponse status;
    dtv::userland::detail::ReceiverEngineStatus engine;
    return dtv::userland::detail::project_receiver_engine_status(canonical, engine, status) ==
                   dtv::userland::Error::INVALID_ARGUMENT
               ? 0
               : 1;
}
