#include "dtv/userland/host_daemon/control_workers.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

class FakeBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return block_tune();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return block_tune();
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
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_receiver = receiver;
        return Result<void>::success();
    }

    void wait_for_tune()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return tune_entered; });
    }

    void release_tune()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release = true;
        condition_.notify_all();
    }

    std::uint8_t closed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_receiver;
    }

private:
    Result<void> block_tune() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        tune_entered = true;
        condition_.notify_all();
        condition_.wait(lock, [this]() { return release; });
        return Result<void>::success();
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool tune_entered = false;
    bool release = false;
    std::uint8_t closed_receiver = 0xffU;
};

class FakeNonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> nonce{};
        nonce[0] = 1U;
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(nonce);
    }
};

class FakeTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class FakeCardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class FakeCardSession final : public CardProtocolSession {
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

bool wait_completion(ControlWorkerLanes& workers, ControlWorkerLane lane,
                     ControlWorkerCompletion& completion)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (workers.try_pop(lane, completion)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService tuner(backend, nonce, time);
    FakeCardBackend card_backend;
    FakeCardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return fail("worker create failed");
    ControlWorkerLanes& workers = *created.value();

    ControlWorkerTask acquire{};
    acquire.operation = ControlWorkerOperation::tuner_acquire;
    acquire.client_id = 10U;
    acquire.receiver = 0U;
    acquire.connection_id = 101U;
    acquire.request_id = 11U;
    if (!workers.submit(ControlWorkerLane::tuner_dev1, acquire))
        return fail("receiver 0 acquire submit failed");
    ControlWorkerCompletion completion{};
    if (!wait_completion(workers, ControlWorkerLane::tuner_dev1, completion) ||
        completion.error != Error::OK)
        return fail("receiver 0 acquire completion failed");
    const std::uint64_t lease0 = completion.tuner_acquire.lease_id;

    ControlWorkerTask tune{};
    tune.operation = ControlWorkerOperation::tuner_tune;
    tune.client_id = 10U;
    tune.receiver = 0U;
    tune.lease_id = lease0;
    tune.request_id = 12U;
    tune.tune.lease_id = lease0;
    tune.tune.system = ipc::System::ISDB_S;
    tune.tune.frequency_khz = 1172748U;
    tune.tune.stream_id = 0xffffU;
    tune.tune.slot = 0U;
    tune.tune.bandwidth_hz = 0U;
    tune.tune.lnb_voltage = 0U;
    tune.tune.timeout_ms = 1000U;
    if (!workers.submit(ControlWorkerLane::tuner_dev1, tune))
        return fail("blocking tune submit failed");
    backend.wait_for_tune();

    ControlWorkerTask other{};
    other.operation = ControlWorkerOperation::tuner_acquire;
    other.client_id = 20U;
    other.receiver = 4U;
    other.request_id = 21U;
    if (!workers.submit(ControlWorkerLane::tuner_dev2, other) ||
        !wait_completion(workers, ControlWorkerLane::tuner_dev2, completion) ||
        completion.error != Error::OK || completion.client_id != 20U)
        return fail("other receiver lane did not progress while receiver 0 blocked");

    std::size_t accepted = 0U;
    for (std::size_t index = 0U; index < kControlWorkerQueueCapacity; ++index) {
        ControlWorkerTask status{};
        status.operation = ControlWorkerOperation::tuner_status;
        status.kind = ControlWorkerTaskKind::background;
        status.request_id = static_cast<std::uint32_t>(index + 100U);
        status.status_scope.id = 700U + index;
        if (workers.submit(ControlWorkerLane::tuner_dev1, status)) ++accepted;
    }
    if (accepted != kControlWorkerQueueCapacity)
        return fail("lane did not accept the bounded owned task queue");
    ControlWorkerTask rejected{};
    rejected.operation = ControlWorkerOperation::tuner_status;
    if (workers.submit(ControlWorkerLane::tuner_dev1, rejected).error() != Error::BUSY)
        return fail("full task queue was not rejected");
    if (!workers.request_stop()) return fail("worker stop request failed");
    if (workers.submit(ControlWorkerLane::tuner_dev1, rejected).error() != Error::NOT_READY)
        return fail("stopped worker accepted a new task");

    backend.release_tune();
    const auto completion_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(5);
    while (workers.pending_completions_for_test(ControlWorkerLane::tuner_dev1) <
               kControlWorkerQueueCapacity &&
           std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (workers.pending_completions_for_test(ControlWorkerLane::tuner_dev1) !=
        kControlWorkerQueueCapacity)
        return fail("completion queue did not fill to its bounded capacity");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (workers.pending_completions_for_test(ControlWorkerLane::tuner_dev1) !=
        kControlWorkerQueueCapacity || workers.idle())
        return fail("full completion queue did not apply backpressure");

    std::size_t seen = 0U;
    while (seen < kControlWorkerQueueCapacity + 1U) {
        if (!wait_completion(workers, ControlWorkerLane::tuner_dev1, completion))
            return fail("accepted worker completion was not delivered");
        if (completion.request_id >= 100U && completion.request_id <
            100U + kControlWorkerQueueCapacity) {
            const std::uint64_t expected_scope = 600U + completion.request_id;
            if (completion.status_scope.id != expected_scope)
                return fail("task-owned status scope changed before completion");
        }
        ++seen;
    }

    if (!workers.join()) return fail("worker join after completion drain failed");
    if (tuner.disconnect_client(20U).error() != Error::OK || backend.closed() != 4U)
        return fail("disconnect_client cleanup after worker close failed");
    if (tuner.disconnect_client(10U).error() != Error::OK || backend.closed() != 0U)
        return fail("disconnect_client cleanup for blocked-lane owner failed");

    auto close_created = ControlWorkerLanes::create(card, tuner);
    if (!close_created) return fail("worker close fixture create failed");
    ControlWorkerTask close_acquire{};
    close_acquire.operation = ControlWorkerOperation::tuner_acquire;
    close_acquire.client_id = 30U;
    close_acquire.receiver = 5U;
    if (!close_created.value()->submit(ControlWorkerLane::tuner_dev2, close_acquire))
        return fail("close fixture acquire submit failed");
    if (!close_created.value()->stop_and_join())
        return fail("stop_and_join did not drain accepted close fixture task");
    if (tuner.disconnect_client(30U).error() != Error::OK || backend.closed() != 5U)
        return fail("disconnect_client cleanup after stop_and_join failed");

    std::cout << "worker queue, completion drain, receiver progress, and disconnect close passed\n";
    return 0;
}
