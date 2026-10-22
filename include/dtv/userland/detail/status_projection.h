// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_STATUS_PROJECTION_H
#define DTV_USERLAND_DETAIL_STATUS_PROJECTION_H

#include "dtv/userland/detail/receiver_engine.h"
#include "dtv/userland/records.h"

namespace dtv::userland::detail {

// Projects engine-owned receiver state into a shell built from the same
// canonical LIST. Caller-observed fields in status are preserved.
Error project_receiver_engine_status(const wire::ListResponse& canonical,
                                     const ReceiverEngineStatus& engine,
                                     wire::StatusResponse& status) noexcept;

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_STATUS_PROJECTION_H
