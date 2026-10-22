// SPDX-License-Identifier: MIT
// Regression tests for the late-prepare / cleanup ownership interleaving and
// the Final/quiescence observations found by the independent counterexample.
// The test-only `private public` observation only reads the authoritative
// registry state under its real mutex; it never fabricates or mutates it.
// Pre-include the standard headers so `private public` only applies to the
// engine declarations, not to standard-library internals.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define private public
#include "dtv/userland/detail/receiver_engine.h"
#undef private

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define LATE_CHECK(condition)                                                      \
    do {                                                                           \
        if (!(condition)) {                                                        \
            std::fprintf(stderr, "late-prepare check failed at line %d: %s\n",     \
                         __LINE__, #condition);                                    \
            return false;                                                          \
        }                                                                          \
    } while (false)

constexpr std::uint64_t kEpoch = 0x1234U;

class Gate final {
public:
    void enter_and_wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(10), [this] { return open_; });
    }
    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10),
                                 [this] { return entered_; });
    }
    void open()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool open_ = false;
};

class GateRelease final {
public:
    explicit GateRelease(Gate& gate) noexcept : gate_(gate) {}
    ~GateRelease() { gate_.open(); }
    GateRelease(const GateRelease&) = delete;
    GateRelease& operator=(const GateRelease&) = delete;

private:
    Gate& gate_;
};

class Joiner final {
public:
    explicit Joiner(std::thread& thread) noexcept : thread_(thread) {}
    ~Joiner()
    {
        if (thread_.joinable()) thread_.join();
    }
    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;

private:
    std::thread& thread_;
};

class MockBackend final : public ReceiverBackend {
public:
    bool supports(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial;
    }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override
    {
        return 0U;
    }
    Result<void> open(ReceiverId) noexcept override { return Result<void>::success(); }
    Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(ReceiverId, std::uint8_t, std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_tsid(ReceiverId, std::uint16_t, std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> close(ReceiverId) noexcept override { return Result<void>::success(); }
    Result<void> begin_tune_power(ReceiverId, ReceiverSystem, std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> commit_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> rollback_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    void mark_disconnected(ReceiverId) noexcept override {}
    Result<void> shutdown() noexcept override
    {
        ++shutdowns;
        return Result<void>::success();
    }
    Result<void> start_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        ++starts;
        if (gate_start) start_gate.enter_and_wait();
        return start_result == Error::OK ? Result<void>::success()
                                         : Result<void>::failure(start_result);
    }
    Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        ++stops;
        return Result<void>::success();
    }
    Error start_result = Error::OK;
    bool gate_start = false;
    Gate start_gate;
    int starts = 0;
    int stops = 0;
    int shutdowns = 0;
};

class RecordingStream final : public ReceiverStreamControl {
public:
    Result<void> attach(const Attachment& attachment) noexcept override
    {
        ++attaches;
        last_attach = attachment;
        return Result<void>::success();
    }
    Result<void> detach(const Attachment& attachment) noexcept override
    {
        ++detaches;
        detach_token = attachment.token;
        return Result<void>::success();
    }
    Result<StreamCounters> stats(const Attachment&) const noexcept override
    {
        return Result<StreamCounters>::success(StreamCounters{});
    }
    Result<StreamFinalSnapshot> final_snapshot(const Attachment&) const noexcept override
    {
        return Result<StreamFinalSnapshot>::success(StreamFinalSnapshot{});
    }
    int attaches = 0;
    int detaches = 0;
    Attachment last_attach{};
    ReceiverToken detach_token{};
};

class MockTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        ReceiverToken token{};
        token[0U] = next++;
        return Result<ReceiverToken>::success(token);
    }
    std::uint8_t next = 1U;
};

class MockClock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t) noexcept override {}
    std::uint64_t now = 1000U;
};

struct Fixture final {
    MockBackend backend;
    MockTokens tokens;
    MockClock clock;
    RecordingStream stream;
    std::array<ReceiverId, 1> receivers{1U};
    ReceiverEngine engine{backend, receivers.data(), receivers.size(), tokens, clock,
                          nullptr, nullptr, &stream};
};

bool acquire_and_tune(Fixture& f, std::uint64_t& lease_id, std::uint64_t& revision)
{
    const std::array<ReceiverCapability, 1> capabilities{
        ReceiverCapability{1U, true, false}};
    LATE_CHECK(f.engine.configure_strict_capabilities(capabilities.data(),
                                                      capabilities.size()));
    const auto lease = f.engine.acquire(1U, 1U);
    LATE_CHECK(lease);
    lease_id = lease.value().lease_id;
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 5000U;
    const auto admitted = f.engine.try_admit_tune(1U, request, 0U);
    LATE_CHECK(admitted);
    LATE_CHECK(f.engine.execute_admitted_tune(admitted.value()));
    revision = admitted.value().tune_revision;
    return true;
}

bool start_active(Fixture& f, std::uint64_t lease, std::uint64_t revision,
                  std::uint64_t& stream_id, ReceiverToken& token,
                  StreamAttachTicket& ticket)
{
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    LATE_CHECK(started);
    stream_id = started.value().stream_id;
    token = started.value().token;
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     stream_id, token, 1000U);
    LATE_CHECK(reserved);
    ticket = reserved.value();
    LATE_CHECK(f.engine.prepare_attach_v1(ticket));
    LATE_CHECK(f.engine.commit_attach_v1(ticket));
    return true;
}

// The exact independent counterexample: cleanup waits behind a held prepare and
// must re-read the ownership published by that prepare.
bool test_cleanup_waits_for_held_prepare()
{
    Fixture f;
    LATE_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    LATE_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    LATE_CHECK(started);
    const std::uint64_t stream_id = started.value().stream_id;
    const ReceiverToken token = started.value().token;
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch, stream_id,
                                                     token, 1000U);
    LATE_CHECK(reserved);
    const StreamAttachTicket ticket = reserved.value();
    f.backend.gate_start = true;
    Error prepare_error = Error::OK;
    std::thread prepare([&] {
        const auto prepared = f.engine.prepare_attach_v1(ticket);
        prepare_error = prepared ? Error::OK : prepared.error();
    });
    Joiner prepare_joiner(prepare);
    GateRelease prepare_release(f.backend.start_gate);
    LATE_CHECK(f.backend.start_gate.wait_entered());
    LATE_CHECK(f.engine.stop_stream_v1(1U, lease, stream_id).has_value());
    Error cleanup_error = Error::OK;
    std::thread cleanup([&] {
        const auto cleaned = f.engine.run_stream_cleanup_v1(ticket);
        cleanup_error = cleaned ? Error::OK : cleaned.error();
    });
    Joiner cleanup_joiner(cleanup);
    // Deterministic: wait until the cleanup has claimed and is blocked behind
    // the held prepare, observed under the real registry mutex.
    bool claimed = false;
    {
        auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < limit) {
            {
                std::lock_guard<std::mutex> lock(f.engine.mutex_);
                claimed = f.engine.leases_[ticket.slot].stream_cleanup_started;
            }
            if (claimed) break;
            std::this_thread::yield();
        }
    }
    LATE_CHECK(claimed);
    f.backend.start_gate.open();
    prepare.join();
    cleanup.join();
    LATE_CHECK(prepare_error == Error::NOT_FOUND);
    LATE_CHECK(cleanup_error == Error::OK);
    // The real stop/detach must have happened despite the earlier false
    // snapshot, with the exact START token identity.
    LATE_CHECK(f.backend.starts == 1);
    LATE_CHECK(f.stream.attaches == 1);
    LATE_CHECK(f.backend.stops == 1);
    LATE_CHECK(f.stream.detaches == 1);
    LATE_CHECK(f.stream.detach_token == token);
    const auto final = f.engine.stop_stream_v1(1U, lease, stream_id);
    LATE_CHECK(final);
    LATE_CHECK(final.value().quiesced);
    LATE_CHECK(f.engine.release(1U, lease));
    return true;
}

bool test_cleanup_wins_before_late_prepare()
{
    Fixture f;
    LATE_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    LATE_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    LATE_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(
        1U, lease, kEpoch, started.value().stream_id, started.value().token, 1000U);
    LATE_CHECK(reserved);
    const StreamAttachTicket ticket = reserved.value();
    const auto stopped = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    LATE_CHECK(stopped && stopped.value().cleanup_owner && !stopped.value().quiesced);
    LATE_CHECK(f.engine.run_stream_cleanup_v1(ticket));
    const auto late_prepare = f.engine.prepare_attach_v1(ticket);
    LATE_CHECK(!late_prepare && late_prepare.error() == Error::NOT_FOUND);
    const auto final = f.engine.stream_last_terminal_v1(1U, lease);
    LATE_CHECK(final && final.value().final_valid && final.value().quiesced);
    LATE_CHECK(f.backend.starts == 0 && f.stream.attaches == 0 &&
               f.backend.stops == 0 && f.stream.detaches == 0);
    LATE_CHECK(f.engine.release(1U, lease));
    return true;
}

// abort must retain the terminal stream identity.
bool test_abort_final_identity()
{
    Fixture f;
    LATE_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    LATE_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    LATE_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    LATE_CHECK(reserved);
    LATE_CHECK(f.engine.prepare_attach_v1(reserved.value()));
    LATE_CHECK(f.engine.abort_attach_v1(reserved.value(), Error::USB_IO));
    LATE_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    const auto stop = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    const auto last = f.engine.stream_last_terminal_v1(1U, lease);
    LATE_CHECK(stop);
    LATE_CHECK(last);
    LATE_CHECK(stop.value().stream_id == started.value().stream_id);
    LATE_CHECK(last.value().stream_id == started.value().stream_id);
    LATE_CHECK(stop.value().cleanup_error == Error::USB_IO);
    LATE_CHECK(last.value().cleanup_error == Error::USB_IO);
    return true;
}

// armed STOP quiescence must survive the next START.
bool test_armed_stop_quiescence_survives_start()
{
    Fixture f;
    LATE_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    LATE_CHECK(acquire_and_tune(f, lease, revision));
    const auto a = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    LATE_CHECK(a);
    LATE_CHECK(f.engine.stop_stream_v1(1U, lease, a.value().stream_id));
    const auto before = f.engine.stream_last_terminal_v1(1U, lease);
    LATE_CHECK(before);
    LATE_CHECK(before.value().quiesced);
    const auto b = f.engine.start_stream_v1(1U, lease, revision, 2000U, 5000U);
    LATE_CHECK(b);
    const auto after = f.engine.stream_last_terminal_v1(1U, lease);
    LATE_CHECK(after);
    LATE_CHECK(after.value().stream_id == a.value().stream_id);
    LATE_CHECK(after.value().quiesced);
    return true;
}

// A primary error discovered on a late completion must be visible through both
// the current and last observations.
bool test_late_first_error_consistent()
{
    Fixture f;
    LATE_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    LATE_CHECK(acquire_and_tune(f, lease, revision));
    std::uint64_t stream_id = 0U;
    ReceiverToken token{};
    StreamAttachTicket ticket{};
    LATE_CHECK(start_active(f, lease, revision, stream_id, token, ticket));
    LATE_CHECK(f.engine.stop_stream_v1(1U, lease, stream_id));
    LATE_CHECK(f.engine.complete_stream_cleanup_v1(ticket, Error::OK, false));
    LATE_CHECK(f.engine.complete_stream_cleanup_v1(ticket, Error::USB_IO, true));
    const auto stop = f.engine.stop_stream_v1(1U, lease, stream_id);
    const auto last = f.engine.stream_last_terminal_v1(1U, lease);
    LATE_CHECK(stop);
    LATE_CHECK(last);
    LATE_CHECK(stop.value().cleanup_error == Error::USB_IO);
    LATE_CHECK(last.value().cleanup_error == Error::USB_IO);
    LATE_CHECK(stop.value().quiesced);
    LATE_CHECK(last.value().quiesced);
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_cleanup_waits_for_held_prepare() &&
                    test_cleanup_wins_before_late_prepare() &&
                    test_abort_final_identity() &&
                    test_armed_stop_quiescence_survives_start() &&
                    test_late_first_error_consistent();
    if (!ok) return 1;
    std::printf("stream v1 late-prepare tests: PASS\n");
    return 0;
}
