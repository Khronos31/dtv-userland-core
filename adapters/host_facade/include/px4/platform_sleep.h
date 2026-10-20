// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_PLATFORM_SLEEP_H
#define PX4_USERLAND_PLATFORM_SLEEP_H

#if defined(_WIN32)

#include <cstdint>

namespace px4::userland::platform {

// Cooperatively sleeps for the requested whole milliseconds using a
// high-resolution waitable timer so IT930x 1ms pacing and card deadlines are
// not quantized to the default ~15.6ms scheduler tick. Returns false when the
// high-resolution timer could not be used (legacy-timer or monotonic
// fallback), so callers do not silently lose the precision contract.
[[nodiscard]] bool sleep_milliseconds(std::uint32_t milliseconds) noexcept;

}  // namespace px4::userland::platform

#endif  // _WIN32

#endif  // PX4_USERLAND_PLATFORM_SLEEP_H
