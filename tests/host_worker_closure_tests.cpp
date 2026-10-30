#include "dtv/userland/host_daemon/control_workers.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <cerrno>
#include <poll.h>
#endif

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

    bool wait_for_tune()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5),
                                   [this]() { return tune_entered; });
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

class ReleaseTuneGuard final {
public:
    explicit ReleaseTuneGuard(FakeBackend& backend) : backend_(backend) {}
    ~ReleaseTuneGuard() { backend_.release_tune(); }

private:
    FakeBackend& backend_;
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

bool acquire_receiver(ControlWorkerLanes& workers, std::uint64_t client_id,
                     std::uint32_t request_id, std::uint64_t& lease_id)
{
    ControlWorkerTask acquire{};
    acquire.operation = ControlWorkerOperation::tuner_acquire;
    acquire.client_id = client_id;
    acquire.receiver = 0U;
    acquire.request_id = request_id;
    if (!workers.submit(ControlWorkerLane::tuner_dev1, acquire)) return false;
    ControlWorkerCompletion completion{};
    if (!wait_completion(workers, ControlWorkerLane::tuner_dev1, completion) ||
        completion.request_id != request_id || completion.error != Error::OK)
        return false;
    lease_id = completion.tuner_acquire.lease_id;
    return lease_id != 0U;
}

bool poll_wake(ControlWorkerLanes& workers, int timeout_ms, bool& readable)
{
#if defined(_WIN32)
    WSAPOLLFD descriptor{};
    descriptor.fd = static_cast<SOCKET>(workers.wake_fd());
    descriptor.events = POLLRDNORM;
    const int result = ::WSAPoll(&descriptor, 1U, timeout_ms);
    if (result == SOCKET_ERROR) return false;
    readable = result != 0 && (descriptor.revents & POLLRDNORM) != 0;
#else
    pollfd descriptor{workers.wake_fd(), POLLIN, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1U, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return false;
    readable = result != 0 && (descriptor.revents & POLLIN) != 0;
#endif
    return true;
}

bool wait_for_pending(ControlWorkerLanes& workers, ControlWorkerLane lane,
                      std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (workers.pending_completions_for_test(lane) == expected) return true;
        std::this_thread::yield();
    }
    return workers.pending_completions_for_test(lane) == expected;
}

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

int test_active_worker_idle_visibility()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService tuner(backend, nonce, time);
    FakeCardBackend card_backend;
    FakeCardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return fail("idle fixture worker create failed");
    auto workers = std::move(created.value());
    ReleaseTuneGuard release(backend);

    std::uint64_t lease_id = 0U;
    if (!acquire_receiver(*workers, 41U, 900U, lease_id))
        return fail("idle fixture receiver acquire failed");

    ControlWorkerTask tune{};
    tune.operation = ControlWorkerOperation::tuner_tune;
    tune.client_id = 41U;
    tune.receiver = 0U;
    tune.lease_id = lease_id;
    tune.request_id = 1U;
    tune.tune.lease_id = lease_id;
    tune.tune.system = ipc::System::ISDB_S;
    tune.tune.frequency_khz = 1172748U;
    tune.tune.stream_id = 0xffffU;
    tune.tune.timeout_ms = 1000U;
    if (!workers->submit(ControlWorkerLane::tuner_dev1, tune) ||
        !backend.wait_for_tune())
        return fail("idle fixture worker did not enter backend barrier");
    if (workers->idle()) return fail("dequeued active worker reported idle");

    backend.release_tune();
    ControlWorkerCompletion completion{};
    if (!wait_completion(*workers, ControlWorkerLane::tuner_dev1, completion) ||
        completion.request_id != tune.request_id || completion.error != Error::OK ||
        !workers->stop_and_join())
        return fail("idle fixture did not release and join worker");
    std::puts("dequeued backend barrier remains non-idle: PASS");
    return 0;
}

int test_fifo_completion_and_wake()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService tuner(backend, nonce, time);
    FakeCardBackend card_backend;
    FakeCardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return fail("FIFO fixture worker create failed");
    auto workers = std::move(created.value());
    ReleaseTuneGuard release(backend);

    std::uint64_t lease_id = 0U;
    if (!acquire_receiver(*workers, 42U, 900U, lease_id))
        return fail("FIFO fixture receiver acquire failed");

    ControlWorkerTask first{};
    first.operation = ControlWorkerOperation::tuner_tune;
    first.client_id = 42U;
    first.receiver = 0U;
    first.lease_id = lease_id;
    first.request_id = 1U;
    first.tune.lease_id = lease_id;
    first.tune.system = ipc::System::ISDB_S;
    first.tune.frequency_khz = 1172748U;
    first.tune.stream_id = 0xffffU;
    first.tune.timeout_ms = 1000U;
    if (!workers->submit(ControlWorkerLane::tuner_dev1, first) ||
        !backend.wait_for_tune())
        return fail("FIFO first task did not enter backend barrier");

    for (std::uint32_t request_id = 2U; request_id <= 3U; ++request_id) {
        ControlWorkerTask status{};
        status.operation = ControlWorkerOperation::tuner_status;
        status.kind = ControlWorkerTaskKind::background;
        status.request_id = request_id;
        if (!workers->submit(ControlWorkerLane::tuner_dev1, status))
            return fail("FIFO status task submit failed");
    }
    backend.release_tune();
    if (!wait_for_pending(*workers, ControlWorkerLane::tuner_dev1, 3U))
        return fail("FIFO completion queue did not reach three accepted tasks");

    bool readable = false;
    if (!poll_wake(*workers, 1000, readable) || !readable)
        return fail("worker completion did not make wake handle pollable");
    workers->drain_wake();
    if (!poll_wake(*workers, 0, readable) || readable)
        return fail("wake drain left the handle readable");

    for (std::uint32_t request_id = 1U; request_id <= 3U; ++request_id) {
        ControlWorkerCompletion completion{};
        if (!workers->try_pop(ControlWorkerLane::tuner_dev1, completion) ||
            completion.request_id != request_id || completion.error != Error::OK)
            return fail("FIFO completion order changed");
    }
    if (!workers->stop_and_join())
        return fail("FIFO fixture worker stop_and_join failed");
    std::puts("FIFO completion 1->2->3 and wake poll/drain: PASS");
    return 0;
}

int test_stop_with_full_completion_ring()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService tuner(backend, nonce, time);
    FakeCardBackend card_backend;
    FakeCardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return fail("completion ring fixture worker create failed");
    auto workers = std::move(created.value());
    ReleaseTuneGuard release(backend);

    std::uint64_t lease_id = 0U;
    if (!acquire_receiver(*workers, 43U, 900U, lease_id))
        return fail("completion ring fixture receiver acquire failed");

    ControlWorkerTask tune{};
    tune.operation = ControlWorkerOperation::tuner_tune;
    tune.client_id = 43U;
    tune.receiver = 0U;
    tune.lease_id = lease_id;
    tune.request_id = 1U;
    tune.tune.lease_id = lease_id;
    tune.tune.system = ipc::System::ISDB_S;
    tune.tune.frequency_khz = 1172748U;
    tune.tune.stream_id = 0xffffU;
    tune.tune.timeout_ms = 1000U;
    if (!workers->submit(ControlWorkerLane::tuner_dev1, tune) ||
        !backend.wait_for_tune())
        return fail("completion ring first task did not enter backend barrier");
    for (std::size_t index = 0U; index < kControlWorkerQueueCapacity; ++index) {
        ControlWorkerTask status{};
        status.operation = ControlWorkerOperation::tuner_status;
        status.kind = ControlWorkerTaskKind::background;
        status.request_id = static_cast<std::uint32_t>(index + 2U);
        if (!workers->submit(ControlWorkerLane::tuner_dev1, status))
            return fail("completion ring accepted-task queue filled early");
    }
    if (!workers->request_stop())
        return fail("completion ring stop request failed");
    backend.release_tune();
    if (!wait_for_pending(*workers, ControlWorkerLane::tuner_dev1,
                          kControlWorkerQueueCapacity) || workers->idle())
        return fail("completion ring did not fill while worker remained active");
    if (!workers->stop_and_join() || workers->live_thread_count() != 0U ||
        workers->pending_completions_for_test(ControlWorkerLane::tuner_dev1) != 0U)
        return fail("stop_and_join did not drain full ring and join worker");
    std::puts("full completion ring stop_and_join and thread count zero: PASS");
    return 0;
}

int test_worker_lane_rejections()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService tuner(backend, nonce, time);
    FakeCardBackend card_backend;
    FakeCardSession card_session;
    CardService card(card_backend, card_session);
    ControlExecutionLayout execution{};
    execution.receiver_count = 1U;
    execution.receiver_lane[0U] = ControlWorkerLane::tuner_dev1;
    auto created = ControlWorkerLanes::create(card, tuner, nullptr, execution);
    if (!created) return fail("lane rejection fixture worker create failed");
    auto workers = std::move(created.value());

    ControlWorkerTask tuner_task{};
    tuner_task.operation = ControlWorkerOperation::tuner_acquire;
    tuner_task.receiver = 0U;
    if (workers->submit(ControlWorkerLane::tuner_dev2, tuner_task).error() !=
        Error::INVALID_ARGUMENT)
        return fail("tuner task on wrong lane was not rejected");
    if (workers->submit(ControlWorkerLane::card, tuner_task).error() !=
        Error::INVALID_ARGUMENT)
        return fail("tuner task on CARD lane was not rejected");
    tuner_task.receiver = 8U;
    if (workers->submit(ControlWorkerLane::tuner_dev1, tuner_task).error() !=
        Error::INVALID_ARGUMENT)
        return fail("receiver 8 was not rejected");
    tuner_task.receiver = 255U;
    if (workers->submit(ControlWorkerLane::tuner_dev1, tuner_task).error() !=
        Error::INVALID_ARGUMENT)
        return fail("receiver 255 was not rejected");
    ControlWorkerTask card_task{};
    card_task.operation = ControlWorkerOperation::card_status;
    if (workers->submit(ControlWorkerLane::tuner_dev1, card_task).error() !=
        Error::INVALID_ARGUMENT)
        return fail("CARD task on tuner lane was not rejected");

    ControlWorkerCompletion completion{};
    if (workers->try_pop(ControlWorkerLane::tuner_dev1, completion) ||
        workers->try_pop(ControlWorkerLane::tuner_dev2, completion) ||
        workers->try_pop(ControlWorkerLane::card, completion) ||
        !workers->stop_and_join())
        return fail("rejected lane task reached completion queue or failed cleanup");
    std::puts("CARD/tuner lane and receiver bounds rejection: PASS");
    return 0;
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
    ReleaseTuneGuard release(backend);

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
    if (!backend.wait_for_tune()) return fail("blocking tune did not enter backend");

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
    if (test_active_worker_idle_visibility() != 0) return 1;
    if (test_fifo_completion_and_wake() != 0) return 1;
    if (test_stop_with_full_completion_ring() != 0) return 1;
    if (test_worker_lane_rejections() != 0) return 1;
    return 0;
}
