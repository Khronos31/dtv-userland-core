// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_facade/px4d_signals.h"

#include <csignal>
#include <cstdlib>

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <atomic>
#include <windows.h>

namespace {
std::atomic<bool> stop_requested_flag{false};
std::atomic<bool> cleanup_complete_flag{false};
HANDLE cleanup_event = nullptr;
constexpr DWORD kTerminationGraceMilliseconds = 5000U;

BOOL WINAPI console_control_handler(DWORD control_type) noexcept
{
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        stop_requested_flag.store(true, std::memory_order_release);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        stop_requested_flag.store(true, std::memory_order_release);
        if (cleanup_event != nullptr)
            (void)::WaitForSingleObject(cleanup_event, kTerminationGraceMilliseconds);
        return TRUE;
    default:
        return FALSE;
    }
}
}

namespace px4::userland::px4d {

bool install_signal_handlers() noexcept
{
    if (cleanup_event == nullptr) cleanup_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return cleanup_event != nullptr && ::SetConsoleCtrlHandler(console_control_handler, TRUE) != 0;
}

bool stop_requested() noexcept
{
    return stop_requested_flag.load(std::memory_order_acquire);
}

void request_stop() noexcept
{
    stop_requested_flag.store(true, std::memory_order_release);
}

void notify_cleanup_complete() noexcept
{
    cleanup_complete_flag.store(true, std::memory_order_release);
    if (cleanup_event != nullptr) (void)::SetEvent(cleanup_event);
}

bool cleanup_complete() noexcept
{
    return cleanup_complete_flag.load(std::memory_order_acquire);
}

[[noreturn]] void exit_after_close_deadline(int status) noexcept
{
    (void)::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(status));
    std::_Exit(status);
}

}  // namespace px4::userland::px4d

#else

namespace {
volatile std::sig_atomic_t stop_requested_flag = 0;

void stop_signal_handler(int) noexcept
{
    stop_requested_flag = 1;
}
}

namespace px4::userland::px4d {

bool install_signal_handlers() noexcept
{
    struct sigaction action {};
    action.sa_handler = stop_signal_handler;
    if (::sigemptyset(&action.sa_mask) != 0) return false;
    action.sa_flags = 0;
    struct sigaction previous_int {};
    if (::sigaction(SIGINT, &action, &previous_int) != 0) return false;
    struct sigaction previous_term {};
    if (::sigaction(SIGTERM, &action, &previous_term) != 0) {
        (void)::sigaction(SIGINT, &previous_int, nullptr);
        return false;
    }
    struct sigaction previous_hup {};
    if (::sigaction(SIGHUP, &action, &previous_hup) != 0) {
        (void)::sigaction(SIGTERM, &previous_term, nullptr);
        (void)::sigaction(SIGINT, &previous_int, nullptr);
        return false;
    }
    return true;
}

bool stop_requested() noexcept { return stop_requested_flag != 0; }
void request_stop() noexcept { stop_requested_flag = 1; }
void notify_cleanup_complete() noexcept {}
bool cleanup_complete() noexcept { return false; }
[[noreturn]] void exit_after_close_deadline(int status) noexcept { std::_Exit(status); }

}  // namespace px4::userland::px4d

#endif
