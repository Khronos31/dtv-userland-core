// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_windows_output.h>

#if defined(_WIN32)

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace px4::userland::cli {
namespace {

constexpr std::size_t kPacketSize = 188U;
constexpr std::size_t kSlotCount = 512U;
constexpr std::uint32_t kDrainTimeoutMs = 5000U;
constexpr std::uint32_t kJoinPollMs = 25U;
constexpr std::uint32_t kHardJoinTimeoutMs = 15000U;

Error classify_write_error(DWORD error) noexcept
{
    switch (error) {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
        return Error::DISCONNECTED;
    case ERROR_OPERATION_ABORTED:
        return Error::TIMEOUT;
    default:
        return Error::INTERNAL;
    }
}

}  // namespace

struct Px4TsWindowsOutput::State final {
    std::atomic<int> refs{1};  // worker ref is added only once the thread starts
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool close_handle = false;
    std::atomic<bool> handle_valid{true};
    std::atomic<bool> worker_done{false};
    HANDLE thread = nullptr;
    std::mutex mutex;
    std::condition_variable data_ready;
    std::condition_variable space_available;
    std::condition_variable drain_done;
    std::array<std::array<std::uint8_t, kPacketSize>, kSlotCount> slots{};
    std::size_t read_head = 0U;
    std::size_t write_head = 0U;
    std::size_t slot_count = 0U;
    bool in_flight = false;
    bool draining = false;
    bool stopping = false;
    bool failed = false;
    bool finalized = false;
    Error write_error = Error::OK;
    std::uint64_t completed = 0U;

    void release() noexcept
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

// The worker is the only code that performs I/O, and it owns closing the
// output handle on exit. The object never mutates the handle while the worker
// may be running.
void Px4TsWindowsOutput::run(State* state) noexcept
{
    std::unique_lock<std::mutex> lock(state->mutex);
    while (true) {
        while (state->slot_count == 0U && !state->stopping && !state->failed) {
            state->data_ready.wait(lock);
        }
        if (state->stopping || state->failed) {
            // Stop or flush-abandon: discard any queued work instead of
            // writing packets after a failure/stop has been reported.
            state->slot_count = 0U;
            state->read_head = state->write_head;
            state->space_available.notify_all();
            state->drain_done.notify_all();
            break;
        }
        std::array<std::uint8_t, kPacketSize> packet = state->slots[state->read_head];
        state->read_head = (state->read_head + 1U) % kSlotCount;
        --state->slot_count;
        state->in_flight = true;
        state->space_available.notify_all();
        lock.unlock();

        std::size_t offset = 0U;
        Error error = Error::OK;
        while (offset < packet.size()) {
            DWORD written = 0U;
            const DWORD amount = static_cast<DWORD>(packet.size() - offset);
            if (::WriteFile(state->handle, packet.data() + offset, amount, &written,
                            nullptr) == 0) {
                error = classify_write_error(::GetLastError());
                break;
            }
            if (written == 0U) {
                error = Error::DISCONNECTED;
                break;
            }
            offset += written;
        }

        lock.lock();
        state->in_flight = false;
        if (error != Error::OK) {
            state->failed = true;
            if (state->write_error == Error::OK) {
                state->write_error = error;
            }
            state->slot_count = 0U;
            state->read_head = state->write_head;
            state->space_available.notify_all();
            state->drain_done.notify_all();
            break;
        }
        ++state->completed;
        state->drain_done.notify_all();
    }
    state->in_flight = false;
    state->drain_done.notify_all();
    lock.unlock();

    // The worker owns closing the output handle; it is the only I/O user.
    if (state->close_handle && state->handle != INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(state->handle);
    }
    state->handle_valid.store(false, std::memory_order_release);
    // The worker has actually finished only after it closed its handle; the
    // frontend must not advertise cleanup completion before this point.
    state->worker_done.store(true, std::memory_order_release);
}

Px4TsWindowsOutput::Px4TsWindowsOutput(State* state) noexcept : state_(state)
{
}

Result<std::unique_ptr<Px4TsWindowsOutput>> Px4TsWindowsOutput::create(
    HANDLE handle, bool close_handle) noexcept
{
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<Px4TsWindowsOutput>>::failure(
            Error::INVALID_ARGUMENT);
    }
    // Allocate the state and the object BEFORE starting the worker, so no
    // failure path can leave a worker waiting on a condition variable that is
    // never signalled or double-release the worker reference.
    State* state = new (std::nothrow) State();
    if (state == nullptr) {
        if (close_handle) {
            (void)::CloseHandle(handle);
        }
        return Result<std::unique_ptr<Px4TsWindowsOutput>>::failure(Error::INTERNAL);
    }
    state->handle = handle;
    state->close_handle = close_handle;
    state->refs.store(1, std::memory_order_release);
    std::unique_ptr<Px4TsWindowsOutput> output(
        new (std::nothrow) Px4TsWindowsOutput(state));
    if (output == nullptr) {
        if (close_handle) {
            (void)::CloseHandle(handle);
        }
        state->handle = INVALID_HANDLE_VALUE;
        state->handle_valid.store(false, std::memory_order_release);
        state->release();
        return Result<std::unique_ptr<Px4TsWindowsOutput>>::failure(Error::INTERNAL);
    }
    state->refs.store(2, std::memory_order_release);  // object + worker
    state->thread = ::CreateThread(nullptr, 0U, &Px4TsWindowsOutput::thread_entry,
                                   state, 0U, nullptr);
    if (state->thread == nullptr) {
        // No worker ref is held. The output destructor's finish() closes the
        // owned handle and releases the object reference.
        state->refs.store(1, std::memory_order_release);
        output.reset();
        return Result<std::unique_ptr<Px4TsWindowsOutput>>::failure(Error::INTERNAL);
    }
    return Result<std::unique_ptr<Px4TsWindowsOutput>>::success(std::move(output));
}

Px4TsWindowsOutput::~Px4TsWindowsOutput() noexcept
{
    (void)finish();
    if (state_ != nullptr) {
        state_->release();
        state_ = nullptr;
    }
}

DWORD WINAPI Px4TsWindowsOutput::thread_entry(LPVOID context) noexcept
{
    auto* state = static_cast<State*>(context);
    Px4TsWindowsOutput::run(state);
    state->release();
    return 0U;
}

std::uint64_t Px4TsWindowsOutput::completed_packets() const noexcept
{
    if (state_ == nullptr) {
        return 0U;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->completed;
}

Error Px4TsWindowsOutput::final_error() const noexcept
{
    if (state_ == nullptr) {
        return Error::OK;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->write_error;
}

bool Px4TsWindowsOutput::valid() const noexcept
{
    return state_ != nullptr && state_->handle_valid.load(std::memory_order_acquire);
}

bool Px4TsWindowsOutput::worker_stopped() const noexcept
{
    return state_ != nullptr && state_->worker_done.load(std::memory_order_acquire);
}

Result<void> Px4TsWindowsOutput::write(ByteView bytes,
                                       const Px4TsSignal& signal) noexcept
{
    if (state_ == nullptr || !valid() || bytes.data == nullptr ||
        bytes.size != kPacketSize) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    while (state_->slot_count == kSlotCount) {
        if (state_->failed) {
            return Result<void>::failure(state_->write_error);
        }
        if (state_->stopping || state_->draining || signal.stop_requested()) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        state_->space_available.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (state_->failed) {
        return Result<void>::failure(state_->write_error);
    }
    if (state_->stopping || state_->draining) {
        return Result<void>::failure(Error::DISCONNECTED);
    }
    std::memcpy(state_->slots[state_->write_head].data(), bytes.data, kPacketSize);
    state_->write_head = (state_->write_head + 1U) % kSlotCount;
    ++state_->slot_count;
    state_->data_ready.notify_one();
    return Result<void>::success();
}

Result<void> Px4TsWindowsOutput::finish() noexcept
{
    if (state_ == nullptr) {
        return Result<void>::success();
    }
    HANDLE thread = nullptr;
    bool drained = false;
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->finalized) {
            return state_->write_error == Error::OK
                       ? Result<void>::success()
                       : Result<void>::failure(state_->write_error);
        }
        state_->finalized = true;
        if (state_->thread == nullptr) {
            // No worker started. Close the owned handle if still open.
            if (state_->handle_valid.load(std::memory_order_acquire) &&
                state_->close_handle && state_->handle != INVALID_HANDLE_VALUE) {
                (void)::CloseHandle(state_->handle);
            }
            state_->handle_valid.store(false, std::memory_order_release);
            return state_->write_error == Error::OK
                       ? Result<void>::success()
                       : Result<void>::failure(state_->write_error);
        }
        thread = state_->thread;
        state_->thread = nullptr;
        // Drain mode: accept no new packets but let the worker finish the
        // in-flight WriteFile and flush every queued packet before any cancel.
        state_->draining = true;
        state_->data_ready.notify_all();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kDrainTimeoutMs);
        while (!state_->failed &&
               (state_->slot_count != 0U || state_->in_flight) &&
               std::chrono::steady_clock::now() < deadline) {
            state_->drain_done.wait_for(lock,
                                        std::chrono::milliseconds(kJoinPollMs));
        }
        drained = !state_->failed && state_->slot_count == 0U && !state_->in_flight;
        // Stop accepting/queuing further packets after the drain deadline.
        state_->stopping = true;
        state_->data_ready.notify_all();
    }

    const auto hard_deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(kHardJoinTimeoutMs);
    bool exited = false;
    for (;;) {
        if (::WaitForSingleObject(thread, 0U) == WAIT_OBJECT_0) {
            exited = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= hard_deadline) {
            break;
        }
        if (!drained) {
            (void)::CancelSynchronousIo(thread);
        }
        ::Sleep(1U);
    }
    // Closing the thread handle is safe whether or not the thread has exited;
    // it never affects the worker's own execution.
    (void)::CloseHandle(thread);

    if (!exited) {
        // The OS did not release a blocked write within the hard bound. Do not
        // mutate the worker's handle or free its state; the worker keeps a
        // reference and will close the handle and release the state when it
        // eventually returns. Persist the failure so a later finish() cannot
        // report success.
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->failed = true;
        if (state_->write_error == Error::OK) {
            state_->write_error = Error::SLOW_CONSUMER;
        }
        return Result<void>::failure(Error::SLOW_CONSUMER);
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->failed) {
        return Result<void>::failure(state_->write_error);
    }
    if (!drained) {
        state_->failed = true;
        state_->write_error = Error::SLOW_CONSUMER;
        return Result<void>::failure(Error::SLOW_CONSUMER);
    }
    return Result<void>::success();
}

}  // namespace px4::userland::cli

#endif  // _WIN32
