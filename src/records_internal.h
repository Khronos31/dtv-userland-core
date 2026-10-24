// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_RECORDS_INTERNAL_H
#define DTV_USERLAND_RECORDS_INTERNAL_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

Error decode_status_response_into(ByteView input, StatusResponse& value) noexcept;
Error decode_list_response_into(ByteView input, ListResponse& value) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_RECORDS_INTERNAL_H
