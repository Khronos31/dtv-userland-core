// SPDX-License-Identifier: GPL-2.0-only
// Windows high-resolution pacing sleep for the portable core.
#include "px4/platform_sleep.h"

#if defined(_WIN32)

// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION and CreateWaitableTimerExW require the
// Windows 10 SDK declarations even when the toolchain defaults to an older
// target. The runtime paths used here exist on Windows 11 x64.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <chrono>
#include <thread>
#include <windows.h>

namespace px4::userland::platform {
namespace {

void monotonic_fallback(std::uint32_t milliseconds) noexcept
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

DWORD bounded_wait_timeout(std::uint32_t milliseconds) noexcept
{
    // Allow a full scheduler quantum of slack beyond the requested interval,
    // without wrapping DWORD near INFINITE.
    if (milliseconds > INFINITE - 1000U) {
        return INFINITE;
    }
    return static_cast<DWORD>(milliseconds) + 1000U;
}

// Arms and awaits a caller-owned timer, then closes it. Returns true only when
// the requested high-resolution wait actually completed.
bool wait_on_timer(HANDLE timer, std::uint32_t milliseconds) noexcept
{
    // Relative due time is expressed in signed 100ns units. uint32 milliseconds
    // times 10000 never overflows LONGLONG.
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10000LL;
    const BOOL armed =
        ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0;
    if (!armed) {
        (void)::CloseHandle(timer);
        monotonic_fallback(milliseconds);
        return false;
    }
    const DWORD wait = ::WaitForSingleObject(timer, bounded_wait_timeout(milliseconds));
    (void)::CloseHandle(timer);
    if (wait != WAIT_OBJECT_0) {
        monotonic_fallback(milliseconds);
        return false;
    }
    return true;
}

}  // namespace

bool sleep_milliseconds(std::uint32_t milliseconds) noexcept
{
    if (milliseconds == 0U) {
        return true;
    }
    // Each call owns its timer so concurrent bridge/card sleeps never reset a
    // shared due time or steal another waiter's signal. No process-global
    // timer state or handle is retained or leaked.
    const HANDLE timer = ::CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        return wait_on_timer(timer, milliseconds);
    }
    const HANDLE legacy = ::CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (legacy == nullptr) {
        monotonic_fallback(milliseconds);
        return false;
    }
    // The legacy waitable timer still sleeps but is quantized to the scheduler
    // tick, so report that the high-resolution contract was not met.
    (void)wait_on_timer(legacy, milliseconds);
    return false;
}

}  // namespace px4::userland::platform

#endif
