// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_PX4D_WINDOWS_STDIN_H
#define PX4_USERLAND_PX4D_WINDOWS_STDIN_H

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

namespace px4::userland::cli {

// Owns a cancellable stdin reader thread used by the cooperative
// `--exit-on-stdin-eof` parent-stop contract. The thread is created with
// CreateThread (error return, never a throwing std::thread) and is always
// cancelled and joined by stop(), including when stdin never closes.
class StdinEofMonitor final {
public:
    using Callback = void (*)(void*) noexcept;

    StdinEofMonitor() noexcept = default;
    ~StdinEofMonitor() noexcept { stop(); }
    StdinEofMonitor(const StdinEofMonitor&) = delete;
    StdinEofMonitor& operator=(const StdinEofMonitor&) = delete;

    // Starts reading `input`. on_eof is invoked once when the input reaches EOF
    // or the read fails. Returns false if the handle is invalid or the thread
    // cannot be created.
    bool start(HANDLE input, Callback on_eof, void* context) noexcept;
    void stop() noexcept;
    bool running() const noexcept { return thread_ != nullptr; }

private:
    static DWORD WINAPI run(LPVOID context) noexcept;

    HANDLE input_ = nullptr;
    HANDLE thread_ = nullptr;
    Callback on_eof_ = nullptr;
    void* context_ = nullptr;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace px4::userland::cli

#endif  // _WIN32

#endif  // PX4_USERLAND_PX4D_WINDOWS_STDIN_H
