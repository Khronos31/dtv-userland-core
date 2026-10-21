// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_JSON_H
#define DTV_USERLAND_JSON_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

// Both functions validate the complete response against its canonical context.
// Keep value, context, canonical list and their referenced storage unchanged for
// the entire call. Encode output must not overlap any of those inputs.
Result<std::size_t> status_json_size(const StatusResponse& value,
                                     const StatusContext& context) noexcept;

// Writes one compact UTF-8 JSON object with a trailing newline and no NUL.
// Validation and capacity failures leave output unchanged.
Result<std::size_t> encode_status_json(const StatusResponse& value,
                                       const StatusContext& context,
                                       MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_JSON_H
