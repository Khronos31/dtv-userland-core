// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/control_workers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#endif

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 3U; }

    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        if (receiver == 0U && block_receiver0.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            receiver0_entered_ = true;
            condition_.notify_all();
            const bool released = condition_.wait_for(lock, std::chrono::seconds(5),
                [this] { return !block_receiver0.load(std::memory_order_acquire); });
            if (!released) return Result<void>::failure(Error::TIMEOUT);
        }
        if (receiver == 2U) {
            std::lock_guard<std::mutex> lock(mutex_);
            receiver2_entered_ = true;
            condition_.notify_all();
        }
        return Result<void>::success();
    }

    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }

    bool wait_receiver0()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(3),
                                   [this] { return receiver0_entered_; });
    }

    bool wait_receiver2()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(3),
                                   [this] { return receiver2_entered_; });
    }

    void release_receiver0()
    {
        block_receiver0.store(false, std::memory_order_release);
        condition_.notify_all();
    }

    std::atomic<bool> block_receiver0{true};

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool receiver0_entered_ = false;
    bool receiver2_entered_ = false;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> nonce{};
        nonce[0U] = 1U;
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(nonce);
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    bool initialized() const noexcept override { return false; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}

private:
    CardAtrBytes atr_{};
};

class UnblockGuard final {
public:
    explicit UnblockGuard(Backend& backend) : backend_(backend) {}
    ~UnblockGuard() { backend_.release_receiver0(); }

private:
    Backend& backend_;
};

std::size_t process_thread_count()
{
#if defined(_WIN32)
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U);
    if (snapshot == INVALID_HANDLE_VALUE) return 0U;
    THREADENTRY32 entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    std::size_t count = 0U;
    if (::Thread32First(snapshot, &entry) != 0) {
        do {
            if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) ++count;
            entry.dwSize = static_cast<DWORD>(sizeof(entry));
        } while (::Thread32Next(snapshot, &entry) != 0);
    }
    (void)::CloseHandle(snapshot);
    return count;
#else
    DIR* directory = ::opendir("/proc/self/task");
    if (directory == nullptr) return 0U;
    std::size_t count = 0U;
    while (const dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    (void)::closedir(directory);
    return count;
#endif
}

bool wait_for_thread_count(std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (process_thread_count() == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return process_thread_count() == expected;
}

bool collect_completion(ControlWorkerLanes& workers, ControlWorkerLane lane,
                        std::uint32_t request_id, bool& found)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !found) {
        ControlWorkerCompletion completion{};
        while (workers.try_pop(lane, completion)) {
            if (completion.request_id == request_id) {
                found = true;
                if (completion.error != Error::OK) return false;
            }
        }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return found;
}

bool collect_pair(ControlWorkerLanes& workers, ControlWorkerLane lane,
                  std::uint32_t first_id, bool& first_found,
                  std::uint32_t sibling_id, bool& sibling_found)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline &&
           (!first_found || !sibling_found)) {
        ControlWorkerCompletion completion{};
        bool drained = false;
        while (workers.try_pop(lane, completion)) {
            drained = true;
            if (completion.error != Error::OK) return false;
            if (completion.request_id == first_id) first_found = true;
            if (completion.request_id == sibling_id) sibling_found = true;
        }
        if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return first_found && sibling_found;
}

int fail(const char* message)
{
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

int test_sibling_progress(bool expect_blocked)
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlExecutionLayout execution{};
    execution.receiver_count = 3U;
    execution.receiver_lane[0U] = ControlWorkerLane::tuner_dev1;
    execution.receiver_lane[1U] = ControlWorkerLane::tuner_dev2;
    execution.receiver_lane[2U] = ControlWorkerLane::tuner_dev1;
    auto created = ControlWorkerLanes::create(card, tuner, nullptr, execution);
    if (!created) return fail("worker creation failed");
    auto workers = std::move(created.value());
    UnblockGuard unblock(backend);

    const auto lane0 = workers->authority_lane(0U, false);
    const auto other_group = workers->authority_lane(1U, false);
    const auto sibling_lane = workers->authority_lane(2U, false);
    if (!lane0 || !other_group || !sibling_lane ||
        lane0.value() == other_group.value() || lane0.value() != sibling_lane.value())
        return fail("noncontiguous receiver group mapping changed");

    ControlWorkerTask first{};
    first.operation = ControlWorkerOperation::tuner_acquire;
    first.client_id = 101U;
    first.connection_id = 1001U;
    first.request_id = 1001U;
    first.receiver = 0U;
    if (!workers->submit(lane0.value(), first) || !backend.wait_receiver0())
        return fail("receiver 0 did not enter the blocked backend operation");

    ControlWorkerTask sibling{};
    sibling.operation = ControlWorkerOperation::tuner_acquire;
    sibling.client_id = 102U;
    sibling.connection_id = 1002U;
    sibling.request_id = 1002U;
    sibling.receiver = 2U;
    if (!workers->submit(sibling_lane.value(), sibling))
        return fail("same-group sibling task was rejected");

    bool sibling_completed = false;
    if (expect_blocked) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < deadline) {
            ControlWorkerCompletion completion{};
            while (workers->try_pop(sibling_lane.value(), completion)) {
                if (completion.request_id == sibling.request_id)
                    sibling_completed = true;
            }
            if (sibling_completed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        if (!backend.wait_receiver2() ||
            !collect_completion(*workers, sibling_lane.value(),
                                sibling.request_id, sibling_completed)) {
            return fail("same-group sibling did not progress independently");
        }
    }
    const bool outcome_matches = expect_blocked ? !sibling_completed : sibling_completed;
    backend.release_receiver0();
    bool first_completed = false;
    if (!collect_pair(*workers, lane0.value(), first.request_id, first_completed,
                      sibling.request_id, sibling_completed))
        return fail("receiver completions were not observed after unblocking");
    if (!outcome_matches || !workers->stop_and_join() ||
        workers->live_thread_count() != 0U)
        return fail("completion outcome or normal stop/join failed");
    std::puts(expect_blocked ? "baseline same-group blocking reproduced"
                             : "same-group sibling progress and stop/join: PASS");
    return 0;
}

int test_worker_resource_bounds(bool baseline)
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlExecutionLayout execution{};
    execution.receiver_count = 1U;
    execution.receiver_lane[0U] = ControlWorkerLane::tuner_dev2;
    const std::size_t initial_threads = process_thread_count();
    if (initial_threads == 0U) return fail("process thread count unavailable");

    ControlWorkerStartupOptions options;
    auto created = ControlWorkerLanes::create_for_test(
        card, tuner, options, nullptr, execution);
    const std::size_t expected_workers = baseline ? 3U : 4U;
    if (!created || created.value()->live_thread_count() != expected_workers ||
        !wait_for_thread_count(initial_threads + expected_workers))
        return fail("single receiver worker count did not match its bound");
    if (!created.value()->stop_and_join() ||
        created.value()->live_thread_count() != 0U ||
        !wait_for_thread_count(initial_threads))
        return fail("single receiver workers did not stop cleanly");
    created.value().reset();

    if (baseline) {
        std::puts("baseline shared worker count recorded");
        return 0;
    }

    ControlExecutionLayout too_many{};
    too_many.receiver_count = 9U;
    const auto rejected = ControlWorkerLanes::create_for_test(
        card, tuner, options, nullptr, too_many);
    if (rejected || rejected.error() != Error::INVALID_ARGUMENT ||
        !wait_for_thread_count(initial_threads))
        return fail("receiver count above the fixed bound was not rejected");

    options.fail_lane = 4;
    ControlExecutionLayout two_receivers{};
    two_receivers.receiver_count = 2U;
    two_receivers.receiver_lane[0U] = ControlWorkerLane::tuner_dev1;
    two_receivers.receiver_lane[1U] = ControlWorkerLane::tuner_dev2;
    const auto failed = ControlWorkerLanes::create_for_test(
        card, tuner, options, nullptr, two_receivers);
    if (failed || !wait_for_thread_count(initial_threads))
        return fail("receiver lane startup failure leaked worker threads");
    std::puts("receiver worker bound, invalid count, startup cleanup: PASS");
    return 0;
}

int test_startup_failure_errors()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlExecutionLayout execution{};
    execution.receiver_count = 0U;
    const std::size_t initial_threads = process_thread_count();
    if (initial_threads == 0U) return fail("process thread count unavailable");

    ControlWorkerStartupOptions options;
    for (int lane = 0; lane < 3; ++lane) {
        options.fail_lane = lane;
        const auto failed = ControlWorkerLanes::create_for_test(
            card, tuner, options, nullptr, execution);
        if (failed || failed.error() != Error::INTERNAL) {
            std::fprintf(stderr, "initial lane %d did not return INTERNAL\n", lane);
            return 1;
        }
        if (!wait_for_thread_count(initial_threads)) {
            std::fprintf(stderr, "initial lane %d leaked a worker thread\n", lane);
            return 1;
        }
    }

    options.fail_lane = -1;
    options.fail_wake_pipe = true;
    const auto wake_failed = ControlWorkerLanes::create_for_test(
        card, tuner, options, nullptr, execution);
    if (wake_failed || wake_failed.error() != Error::INTERNAL)
        return fail("wake creation failure did not return INTERNAL");
    if (!wait_for_thread_count(initial_threads))
        return fail("wake creation failure leaked a worker thread");
    std::puts("lane 0/1/2 and wake startup failures return INTERNAL: PASS");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const bool baseline = argc == 2 && std::strcmp(argv[1], "--baseline") == 0;
    if (argc > 2 || (argc == 2 && !baseline)) return 2;
    if (test_sibling_progress(baseline) != 0) return 1;
    if (test_worker_resource_bounds(baseline) != 0) return 1;
    if (baseline) return 0;
    return test_startup_failure_errors();
}
