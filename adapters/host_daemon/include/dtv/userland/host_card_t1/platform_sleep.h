// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/include/px4/platform_sleep.h at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_CARD_T1_PLATFORM_SLEEP_H
#define DTV_USERLAND_HOST_CARD_T1_PLATFORM_SLEEP_H

#if defined(_WIN32)

#include <cstdint>

namespace dtv::userland::host_card_t1::platform {

// Cooperatively sleeps for the requested whole milliseconds using a
// high-resolution waitable timer so IT930x 1ms pacing and card deadlines are
// not quantized to the default ~15.6ms scheduler tick. Returns false when the
// high-resolution timer could not be used (legacy-timer or monotonic
// fallback), so callers do not silently lose the precision contract.
[[nodiscard]] bool sleep_milliseconds(std::uint32_t milliseconds) noexcept;

}  // namespace dtv::userland::host_card_t1::platform

#endif  // _WIN32

#endif  // DTV_USERLAND_HOST_CARD_T1_PLATFORM_SLEEP_H
