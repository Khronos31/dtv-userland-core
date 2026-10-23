// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_FACADE_PX4D_SIGNALS_H
#define DTV_USERLAND_HOST_FACADE_PX4D_SIGNALS_H

#if defined(_WIN32) && defined(DTV_USERLAND_SIGNAL_TESTING)
#include <cstdint>
#endif

namespace px4::userland::px4d {

bool install_signal_handlers() noexcept;
bool stop_requested() noexcept;
void request_stop() noexcept;
void notify_cleanup_complete() noexcept;
bool cleanup_complete() noexcept;
[[noreturn]] void exit_after_close_deadline(int status) noexcept;
#if defined(_WIN32) && defined(DTV_USERLAND_SIGNAL_TESTING)
bool dispatch_console_control_for_test(std::uint32_t control_type) noexcept;
#endif

}  // namespace px4::userland::px4d

#endif  // DTV_USERLAND_HOST_FACADE_PX4D_SIGNALS_H
