// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_TS_PX4_TS_WINDOWS_OUTPUT_H
#define DTV_HOST_TS_PX4_TS_WINDOWS_OUTPUT_H

#include <dtv/host_ts/px4_ts_core.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>

namespace px4::userland::cli {

// Windows binary TS sink with an owned writer thread.
//
// - The writer is created with CreateThread (error return), never a throwing
//   std::thread inside a noexcept context.
// - write() enqueues a whole 188-byte packet.
// - finish() waits for the worker to actually finish the in-flight WriteFile
//   and drain the queue before cancelling/joining, so a normally successful
//   final packet is never truncated. Cancellation is used only on a stalled
//   consumer or a write failure.
// - completed_packets() counts packets actually written; final_error() reports
//   the drain result.
// - The worker shares a reference-counted state so a thread that cannot be
//   joined within the hard bound cannot cause use-after-free; the state (and
//   the blocked thread) is intentionally leaked instead of deadlocking.
class Px4TsWindowsOutput final : public Px4TsOutput {
public:
    static Result<std::unique_ptr<Px4TsWindowsOutput>> create(HANDLE handle,
                                                              bool close_handle) noexcept;
    ~Px4TsWindowsOutput() noexcept override;

    Px4TsWindowsOutput(const Px4TsWindowsOutput&) = delete;
    Px4TsWindowsOutput& operator=(const Px4TsWindowsOutput&) = delete;

    Result<void> write(ByteView bytes, const Px4TsSignal& signal) noexcept override;
    Result<void> finish() noexcept;

    std::uint64_t completed_packets() const noexcept;
    Error final_error() const noexcept;
    bool valid() const noexcept;
    // True only after the worker has actually stopped and closed its handle.
    bool worker_stopped() const noexcept;

private:
    struct State;
    explicit Px4TsWindowsOutput(State* state) noexcept;
    static DWORD WINAPI thread_entry(LPVOID context) noexcept;
    static void run(State* state) noexcept;

    State* state_ = nullptr;
};

}  // namespace px4::userland::cli

#endif  // _WIN32

#endif  // DTV_HOST_TS_PX4_TS_WINDOWS_OUTPUT_H
