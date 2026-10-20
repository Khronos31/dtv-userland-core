// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_facade/px4d_windows_stdin.h"

#if defined(_WIN32)

namespace px4::userland::cli {

bool StdinEofMonitor::start(HANDLE input, Callback on_eof, void* context) noexcept
{
    if (thread_ != nullptr || input == nullptr || input == INVALID_HANDLE_VALUE) {
        return false;
    }
    input_ = input;
    on_eof_ = on_eof;
    context_ = context;
    stop_requested_.store(false, std::memory_order_release);
    thread_ = ::CreateThread(nullptr, 0U, &StdinEofMonitor::run, this, 0U, nullptr);
    return thread_ != nullptr;
}

void StdinEofMonitor::stop() noexcept
{
    if (thread_ == nullptr) {
        return;
    }
    // Request a cooperative stop, then cancel the blocking ReadFile. The wait
    // checks thread completion first, so an already-exited thread (for example
    // after ordinary EOF) is joined immediately instead of spinning on
    // CancelSynchronousIo's ERROR_NOT_FOUND.
    stop_requested_.store(true, std::memory_order_release);
    for (;;) {
        if (::WaitForSingleObject(thread_, 0U) == WAIT_OBJECT_0) {
            break;
        }
        (void)::CancelSynchronousIo(thread_);
        ::Sleep(1U);
    }
    (void)::CloseHandle(thread_);
    thread_ = nullptr;
}

DWORD WINAPI StdinEofMonitor::run(LPVOID context) noexcept
{
    auto* self = static_cast<StdinEofMonitor*>(context);
    char buffer[1];
    DWORD read = 0U;
    bool eof = false;
    while (!self->stop_requested_.load(std::memory_order_acquire)) {
        if (::ReadFile(self->input_, buffer, sizeof(buffer), &read, nullptr) == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_OPERATION_ABORTED) {
                return 0U;  // cancelled by stop()
            }
            // Any other read failure is treated as a stop condition.
            eof = true;
            break;
        }
        if (read == 0U) {
            eof = true;
            break;
        }
    }
    if (eof && self->on_eof_ != nullptr) {
        self->on_eof_(self->context_);
    }
    return 0U;
}

}  // namespace px4::userland::cli

#endif  // _WIN32
