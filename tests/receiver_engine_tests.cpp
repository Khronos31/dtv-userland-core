// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"
#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
#include "dtv/userland/detail/packet_stream_session.h"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
namespace dtv::userland::detail {

struct ReceiverEngineTestAccess final {
    static bool retain_current_final(ReceiverEngine& engine,
                                     const StreamAttachTicket& ticket)
    {
        if (ticket.slot >= kReceiverEngineCapacity) return false;
        std::lock_guard<std::mutex> lock(engine.mutex_);
        if (!engine.stream_cleanup_ticket_matches_locked(ticket.slot, ticket))
            return false;
        ReceiverEngine::Lease& lease = engine.leases_[ticket.slot];
        if (!lease.stream_cleanup_completed || !lease.stream_final_valid ||
            !lease.stream_final_physical || lease.stream_final_released ||
            engine.retained_stream_finals_[ticket.slot].valid)
            return false;
        ReceiverEngine::RetainedStreamFinal& retained =
            engine.retained_stream_finals_[ticket.slot];
        retained.valid = true;
        retained.ticket = ticket;
        retained.attachment = engine.attachment_for(lease, lease.stream_reservation);
        retained.attachment.token = lease.stream_consumed_token;
        return true;
    }

    static void clear_retained_final(ReceiverEngine& engine, std::size_t slot)
    {
        if (slot >= kReceiverEngineCapacity) return;
        std::lock_guard<std::mutex> lock(engine.mutex_);
        engine.retained_stream_finals_[slot] = ReceiverEngine::RetainedStreamFinal{};
    }
};

}  // namespace dtv::userland::detail
#endif

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define ENGINE_CHECK(condition)                                                       \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "receiver engine check failed at line %d: %s\n",     \
                         __LINE__, #condition);                                       \
            return false;                                                              \
        }                                                                              \
    } while (false)

// Bound for every cross-thread wait.  Gates order the threads; a wait that
// runs out records a failure and lets its thread continue, so a regression
// ends the test instead of hanging it.  CTest's TIMEOUT is the outer bound.
constexpr std::chrono::milliseconds kWaitBound{10000};
// Shorter bound for the deliberately serialized experiment, which is expected
// to run out.
constexpr std::chrono::milliseconds kExpectedTimeout{200};
constexpr int kNoReceiver = -1;

// Sparse, non-contiguous identities.  513 is satellite-only; every other ID
// is terrestrial-only.
constexpr ReceiverId kTerrestrial = 17U;
constexpr ReceiverId kSatellite = 513U;
constexpr ReceiverId kSecondTerrestrial = 4097U;

std::string num(std::uint64_t value)
{
    return std::to_string(value);
}

std::string sys(ReceiverSystem system)
{
    return std::to_string(static_cast<unsigned>(system));
}

class Calls final {
public:
    void add(int receiver, std::string text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(Entry{receiver, std::move(text)});
    }

    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> texts;
        for (const Entry& entry : entries_) texts.push_back(entry.text);
        entries_.clear();
        return texts;
    }

    std::vector<std::string> for_receiver(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> texts;
        for (const Entry& entry : entries_) {
            if (entry.receiver == receiver) texts.push_back(entry.text);
        }
        return texts;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    struct Entry final {
        int receiver;
        std::string text;
    };

    std::mutex mutex_;
    std::vector<Entry> entries_;
};

bool same_calls(const char* scenario, const std::vector<std::string>& actual,
                const std::vector<std::string>& expected)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "call mismatch in %s\n  expected:\n", scenario);
    for (const std::string& entry : expected) std::fprintf(stderr, "    %s\n", entry.c_str());
    std::fprintf(stderr, "  actual:\n");
    for (const std::string& entry : actual) std::fprintf(stderr, "    %s\n", entry.c_str());
    return false;
}

Result<void> take_result(std::deque<Error>& failures) noexcept
{
    if (failures.empty()) return Result<void>::success();
    const Error error = failures.front();
    failures.pop_front();
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}

// A one-shot rendezvous: the operation thread enters and waits until the test
// opens the gate or the bound runs out.
class Gate final {
public:
    void arm(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_ = receiver;
        entered_ = false;
        open_ = false;
        timed_out_ = false;
    }

    void pass(int receiver)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (receiver_ != receiver) return;
        receiver_ = kNoReceiver;
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(lock, kWaitBound, [&]() noexcept { return open_; }))
            timed_out_ = true;
    }

    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWaitBound, [&]() noexcept { return entered_; });
    }

    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        receiver_ = kNoReceiver;
        changed_.notify_all();
    }

    bool timed_out()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return timed_out_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int receiver_ = kNoReceiver;
    bool entered_ = false;
    bool open_ = false;
    bool timed_out_ = false;
};

class Done final {
public:
    void set()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        changed_.notify_all();
    }

    bool wait(std::chrono::milliseconds bound)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&]() noexcept { return done_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
};

// Runs `held` on one thread until it reaches the armed gate, then `probe` on a
// second thread, and waits a bounded time for the probe to finish while the
// first operation is still held.  The gate is always opened and both threads
// joined, so a probe that cannot finish (for example because the operations
// were serialized) fails the check instead of hanging the process.
template <typename Held, typename Probe>
bool probe_while_held(Gate& gate, int receiver, Held held, Probe probe,
                      std::chrono::milliseconds bound = kWaitBound)
{
    gate.arm(receiver);
    std::thread holder(held);
    const bool entered = gate.wait_entered();
    Done probed;
    std::thread prober([&]() {
        probe();
        probed.set();
    });
    const bool finished = probed.wait(bound);
    gate.open();
    prober.join();
    holder.join();
    if (!entered) std::fprintf(stderr, "held operation did not reach its gate\n");
    if (!finished) std::fprintf(stderr, "probe did not finish while the operation was held\n");
    return entered && finished && !gate.timed_out();
}

class FakeClock final : public ReceiverClock {
public:
    explicit FakeClock(Calls& calls) noexcept : calls_(calls) {}

    std::uint64_t monotonic_ms() noexcept override { return now.load(); }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        calls_.add(kNoReceiver, "sleep " + num(milliseconds));
        now.fetch_add(milliseconds);
    }

    std::atomic<std::uint64_t> now{0U};

private:
    Calls& calls_;
};

class FakeBackend final : public ReceiverBackend {
public:
    FakeBackend(Calls& calls, ReceiverClock& clock) noexcept : calls_(calls), clock_(clock) {}

    bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept override
    {
        calls_.add(receiver, "supports " + num(receiver) + " " + sys(system));
        std::unique_lock<std::mutex> serial(serial_, std::defer_lock);
        if (serialize) serial.lock();
        supports_gate.pass(receiver);
        return receiver == kSatellite ? system == ReceiverSystem::satellite
                                      : system == ReceiverSystem::terrestrial;
    }

    bool selects_satellite_stream_before_tune() const noexcept override { return false; }

    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial ? settle_ms : 0U;
    }

    Result<void> open(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "open " + num(receiver));
        return Result<void>::success();
    }

    Result<void> tune(ReceiverId receiver, ReceiverSystem system,
                      std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "tune " + num(receiver) + " " + sys(system) + " " +
                                 num(frequency_khz) + " " + num(timeout_ms));
        std::unique_lock<std::mutex> serial(serial_, std::defer_lock);
        if (serialize) serial.lock();
        tune_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<bool> is_locked(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "lock " + num(receiver));
        return Result<bool>::success(true);
    }

    Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "slot " + num(receiver) + " " + num(slot) + " " +
                                 num(timeout_ms));
        return Result<void>::success();
    }

    Result<void> select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "tsid " + num(receiver) + " " + num(tsid) + " " +
                                 num(timeout_ms));
        return Result<void>::success();
    }

    Result<void> close(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "close " + num(receiver));
        return Result<void>::success();
    }

    Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem,
                                  std::uint8_t lnb_voltage) noexcept override
    {
        calls_.add(receiver, "power " + num(receiver) + " " + num(lnb_voltage));
        if (power_wait_ms != 0U) clock_.sleep_ms(power_wait_ms);
        return Result<void>::success();
    }

    Result<void> commit_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "commit " + num(receiver));
        return Result<void>::success();
    }

    Result<void> rollback_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "rollback " + num(receiver));
        return Result<void>::success();
    }

    void mark_disconnected(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "lost " + num(receiver));
    }

    Result<void> shutdown() noexcept override
    {
        calls_.add(kNoReceiver, "shutdown");
        return Result<void>::success();
    }

    Result<void> start_capture(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "start " + num(receiver));
        return Result<void>::success();
    }

    Result<void> stop_capture(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "stop " + num(receiver));
        return Result<void>::success();
    }

    std::uint32_t settle_ms = 0U;
    std::uint32_t power_wait_ms = 0U;
    // Test-only regression model: one lock across every receiver's support
    // query and frontend tune, as if separate receivers were serialized.
    bool serialize = false;
    mutable Gate supports_gate;
    Gate tune_gate;

private:
    Calls& calls_;
    ReceiverClock& clock_;
    mutable std::mutex serial_;
};

class FakeStream final : public ReceiverStreamControl {
public:
    explicit FakeStream(Calls& calls) noexcept : calls_(calls) {}

    Result<void> attach(const Attachment& attachment) noexcept override
    {
        calls_.add(attachment.receiver, "attach " + identity(attachment));
        return Result<void>::success();
    }

    Result<void> detach(const Attachment& attachment) noexcept override
    {
        calls_.add(attachment.receiver, "detach " + identity(attachment));
        std::lock_guard<std::mutex> lock(mutex_);
        return take_result(detach_failures);
    }

    Result<StreamCounters> stats(const Attachment& attachment) const noexcept override
    {
        calls_.add(attachment.receiver, "stats " + identity(attachment));
        StreamCounters counters{};
        counters.packets = attachment.attachment_id * 10U;
        return Result<StreamCounters>::success(counters);
    }

    Result<StreamFinalSnapshot> final_snapshot(
        const Attachment& attachment) const noexcept override
    {
        calls_.add(attachment.receiver, "final " + identity(attachment));
        StreamFinalSnapshot snapshot{};
        snapshot.counters.packets = attachment.attachment_id;
        return Result<StreamFinalSnapshot>::success(snapshot);
    }

    static std::string identity(const Attachment& attachment)
    {
        return num(attachment.receiver) + " " + num(attachment.lease_id) + " " +
               num(attachment.attachment_id);
    }

    std::deque<Error> detach_failures;

private:
    Calls& calls_;
    std::mutex mutex_;
};

#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
class PacketSessionStream final : public ReceiverStreamControl {
public:
    Result<void> attach(const Attachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        return session.begin_attach_locked(attachment, 4U, StreamQueuePolicy::evict_oldest);
    }

    Result<void> detach(const Attachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        return session.detach_locked(attachment);
    }

    Result<StreamCounters> stats(const Attachment& attachment) const noexcept override
    {
        return session.stats(attachment);
    }

    Result<StreamFinalSnapshot> final_snapshot(
        const Attachment& attachment) const noexcept override
    {
        return session.final_snapshot_query(attachment);
    }

    Result<void> release_final(const Attachment& attachment) noexcept override
    {
        const auto released = fail_next_release.exchange(false)
            ? Result<void>::failure(Error::INTERNAL)
            : session.release_final(attachment);
        {
            std::unique_lock<std::mutex> lock(release_barrier_mutex);
            if (release_barrier_enabled) {
                release_barrier_entered = true;
                release_barrier_condition.notify_all();
                release_barrier_condition.wait(lock,
                    [this] { return release_barrier_released; });
                release_barrier_enabled = false;
            }
        }
        return released;
    }

    void hold_next_release()
    {
        std::lock_guard<std::mutex> lock(release_barrier_mutex);
        release_barrier_enabled = true;
        release_barrier_entered = false;
        release_barrier_released = false;
    }

    bool wait_release_barrier()
    {
        std::unique_lock<std::mutex> lock(release_barrier_mutex);
        return release_barrier_condition.wait_for(lock, kWaitBound,
            [this] { return release_barrier_entered; });
    }

    void release_release_barrier()
    {
        std::lock_guard<std::mutex> lock(release_barrier_mutex);
        release_barrier_released = true;
        release_barrier_condition.notify_all();
    }

    Result<StreamReadResult> read_eof(const Attachment& attachment,
                                      MutableByteView output) noexcept
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(attachment);
        if (!ticket) return Result<StreamReadResult>::failure(ticket.error());
        return session.finish_read_locked(attachment, ticket.value(), output);
    }

    Result<StreamReadResult> read_eof_after_barrier(
        const Attachment& attachment, MutableByteView output) noexcept
    {
        const auto ticket = [&]() {
            std::lock_guard<std::mutex> lock(session.mutex);
            return session.begin_read_locked(attachment);
        }();
        if (!ticket) return Result<StreamReadResult>::failure(ticket.error());
        {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            barrier_entered = true;
            barrier_condition.notify_all();
            barrier_condition.wait(lock, [this] { return barrier_released; });
        }
        std::lock_guard<std::mutex> lock(session.mutex);
        return session.finish_read_locked(attachment, ticket.value(), output);
    }

    bool wait_read_barrier()
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        return barrier_condition.wait_for(lock, kWaitBound,
            [this] { return barrier_entered; });
    }

    void release_read_barrier()
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        barrier_released = true;
        barrier_condition.notify_all();
    }

    PacketStreamSession session;
    std::mutex barrier_mutex;
    std::condition_variable barrier_condition;
    bool barrier_entered = false;
    bool barrier_released = false;
    std::mutex release_barrier_mutex;
    std::condition_variable release_barrier_condition;
    bool release_barrier_enabled = false;
    bool release_barrier_entered = false;
    bool release_barrier_released = false;
    std::atomic<bool> fail_next_release{false};
};

constexpr std::uint64_t kReleaseMigrationEpoch = 0x52454c454153454dULL;

bool configure_v1_session_engine(ReceiverEngine& engine)
{
    ReceiverCapability capability{};
    capability.receiver = kTerrestrial;
    capability.supports_terrestrial = true;
    const std::array<ReceiverCapability, 1U> capabilities{capability};
    return engine.configure_strict_capabilities(capabilities.data(), capabilities.size()) &&
           engine.configure_stream_contract(StreamContract::v1, kReleaseMigrationEpoch);
}

#endif

class CountingTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        ReceiverToken token{};
        token[0U] = next.fetch_add(1U);
        return Result<ReceiverToken>::success(token);
    }

    std::atomic<std::uint8_t> next{1U};
};

struct Fixture final {
    Calls calls;
    FakeClock clock{calls};
    FakeBackend backend{calls, clock};
    FakeStream stream{calls};
    CountingTokens tokens;
    std::array<ReceiverId, 3> ids{kTerrestrial, kSatellite, kSecondTerrestrial};
};

TuneRequest terrestrial(std::uint64_t lease_id, Error validation = Error::OK)
{
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 1000U;
    request.validation = validation;
    return request;
}

#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
bool create_eof_final(Fixture& f, PacketSessionStream& stream, ReceiverEngine& engine,
                      std::uint64_t client, std::uint64_t lease_id,
                      StreamAttachTicket& ticket)
{
    const auto admitted = engine.try_admit_tune(
        client, terrestrial(lease_id), f.clock.monotonic_ms());
    if (!admitted || !engine.execute_admitted_tune(admitted.value())) return false;
    const auto status = engine.status();
    if (!status) return false;
    const auto started = engine.start_stream_v1(client, lease_id,
        status.value().tune_metadata[0U].current_revision, 10U, 5000U);
    if (!started) return false;
    const auto reserved = engine.reserve_attach_v1(client, lease_id,
        kReleaseMigrationEpoch, started.value().stream_id, started.value().token, 11U);
    if (!reserved || !engine.prepare_attach_v1(reserved.value()) ||
        !engine.commit_attach_v1(reserved.value())) return false;
    const auto stopped = engine.stop_stream_v1(client, lease_id, started.value().stream_id);
    if (!stopped || !engine.run_stream_cleanup_v1(reserved.value())) return false;
    const auto attachment = engine.stream_attachment_v1(reserved.value());
    if (!attachment) return false;
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    const auto eof = stream.read_eof(attachment.value(),
        MutableByteView{output.data(), output.size()});
    if (!eof || !eof.value().eof || eof.value().timed_out) return false;
    ticket = reserved.value();
    return true;
}
#endif

TuneRequest satellite_slot(std::uint64_t lease_id, std::uint16_t slot)
{
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::satellite;
    request.frequency_khz = 1049480U;
    request.slot = slot;
    request.timeout_ms = 1000U;
    return request;
}

bool state_at(const ReceiverEngine& engine, std::size_t slot, ReceiverState state)
{
    const auto status = engine.status();
    return status && status.value().states[slot] == state;
}

template <typename T>
bool failed(const Result<T>& result, Error expected)
{
    return !result && result.error() == expected;
}

bool test_sparse_ids_through_lifecycle()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock, nullptr, nullptr,
                          &f.stream);
    ENGINE_CHECK(engine.configured());
    const auto initial = engine.status();
    ENGINE_CHECK(initial && initial.value().receiver_count == 2U &&
                 initial.value().receivers[0U] == kTerrestrial &&
                 initial.value().receivers[1U] == kSatellite &&
                 initial.value().generation == 1U);

    const auto terrestrial_lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(terrestrial_lease && terrestrial_lease.value().lease_id == 1U);
    ENGINE_CHECK(same_calls("acquire 17", f.calls.take(), {"open 17", "supports 17 2"}));
    const auto satellite_lease = engine.acquire(2U, kSatellite);
    ENGINE_CHECK(satellite_lease && satellite_lease.value().lease_id == 2U);
    ENGINE_CHECK(same_calls("acquire 513", f.calls.take(),
                            {"open 513", "supports 513 2", "supports 513 1"}));
    ENGINE_CHECK(failed(engine.acquire(3U, kSatellite), Error::BUSY));

    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("tune 17", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    ENGINE_CHECK(engine.tune(2U, satellite_slot(2U, 3U)));
    ENGINE_CHECK(same_calls("tune 513", f.calls.take(),
                            {"supports 513 2", "power 513 0", "tune 513 2 1049480 1000",
                             "lock 513", "slot 513 3 1000", "commit 513"}));

    ENGINE_CHECK(engine.start_stream(1U, 1U) && engine.start_stream(2U, 2U));
    const auto first = engine.attach_stream(1U, terrestrial_lease.value().token);
    const auto second = engine.attach_stream(2U, satellite_lease.value().token);
    ENGINE_CHECK(first && first.value().receiver == kTerrestrial &&
                 first.value().attachment_id == 1U &&
                 first.value().system == ReceiverSystem::terrestrial);
    ENGINE_CHECK(second && second.value().receiver == kSatellite &&
                 second.value().attachment_id == 2U &&
                 second.value().system == ReceiverSystem::satellite &&
                 second.value().owner_client_id == 2U);
    ENGINE_CHECK(same_calls("attach", f.calls.take(),
                            {"start 17", "attach 17 1 1", "start 513", "attach 513 2 2"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::streaming) &&
                 state_at(engine, 1U, ReceiverState::streaming));

    const auto stats = engine.stats(2U, 2U);
    ENGINE_CHECK(stats && stats.value().packets == 20U);
    ENGINE_CHECK(failed(engine.stats(1U, 2U), Error::NOT_FOUND));
    const auto stopped = engine.stop_stream(1U, 1U);
    ENGINE_CHECK(stopped && stopped.value().counters.packets == 1U);
    ENGINE_CHECK(engine.detach_stream(second.value()));
    ENGINE_CHECK(same_calls("stop", f.calls.take(),
                            {"stats 513 2 2", "detach 17 1 1", "stop 17", "final 17 1 1",
                             "detach 513 2 2", "stop 513"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::tuned) &&
                 state_at(engine, 1U, ReceiverState::tuned));

    ENGINE_CHECK(engine.release(1U, 1U));
    ENGINE_CHECK(engine.disconnect_client(2U));
    ENGINE_CHECK(engine.shutdown());
    ENGINE_CHECK(same_calls("cleanup", f.calls.take(), {"close 17", "close 513", "shutdown"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::free) &&
                 state_at(engine, 1U, ReceiverState::free));
    return true;
}

bool test_invalid_missing_ids_and_capacity()
{
    Fixture f;
    {
        ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
        for (const ReceiverId missing : {ReceiverId{0U}, ReceiverId{16U}, ReceiverId{18U},
                                         ReceiverId{512U}, ReceiverId{514U},
                                         kSecondTerrestrial, ReceiverId{0xffffU}}) {
            ENGINE_CHECK(failed(engine.acquire(1U, missing), Error::INVALID_ARGUMENT));
        }
        ENGINE_CHECK(failed(engine.acquire(0U, kTerrestrial), Error::INVALID_ARGUMENT));
        Attachment forged{};
        forged.owner_client_id = 1U;
        forged.lease_id = 1U;
        forged.attachment_id = 1U;
        forged.receiver = 18U;
        ENGINE_CHECK(failed(engine.detach_stream(forged), Error::NOT_FOUND));
        ENGINE_CHECK(same_calls("missing ids", f.calls.take(), {}));
    }
    f.calls.clear();

    // The slot ceiling bounds how many receivers exist, not which IDs.
    const std::array<ReceiverId, 9> nine{1U, 100U, 200U, 300U, 400U, 500U, 600U, 0xffffU, 7U};
    {
        ReceiverEngine full(f.backend, nine.data(), 8U, f.tokens, f.clock);
        ENGINE_CHECK(full.configured());
        ENGINE_CHECK(full.acquire(1U, 0xffffU));
        ENGINE_CHECK(failed(full.acquire(1U, 7U), Error::INVALID_ARGUMENT));
        const auto status = full.status();
        ENGINE_CHECK(status && status.value().receiver_count == 8U &&
                     status.value().states[7U] == ReceiverState::leased);
    }
    {
        ReceiverEngine over(f.backend, nine.data(), nine.size(), f.tokens, f.clock);
        ENGINE_CHECK(!over.configured());
        ENGINE_CHECK(failed(over.acquire(1U, 1U), Error::INVALID_ARGUMENT));
        const auto status = over.status();
        ENGINE_CHECK(status && status.value().receiver_count == 0U);
    }
    {
        const std::array<ReceiverId, 2> repeated{kTerrestrial, kTerrestrial};
        ReceiverEngine duplicate(f.backend, repeated.data(), repeated.size(), f.tokens,
                                 f.clock);
        ENGINE_CHECK(!duplicate.configured());
        ENGINE_CHECK(failed(duplicate.acquire(1U, kTerrestrial), Error::INVALID_ARGUMENT));
    }
    return true;
}

struct IndependentTunes final {
    bool proceeded = false;
    bool observed = false;
    Result<void> held = Result<void>::failure(Error::INTERNAL);
    Result<void> other = Result<void>::failure(Error::INTERNAL);
};

// Receiver 513 must complete its tune, and status must answer, while 17 is
// held inside its frontend tune.
IndependentTunes tune_while_other_receiver_held(Fixture& f, ReceiverEngine& engine,
                                                std::chrono::milliseconds bound)
{
    IndependentTunes result;
    result.proceeded = probe_while_held(
        f.backend.tune_gate, kTerrestrial,
        [&]() { result.held = engine.tune(1U, terrestrial(1U)); },
        [&]() {
            result.other = engine.tune(1U, satellite_slot(2U, 0U));
            result.observed = state_at(engine, 0U, ReceiverState::leased) &&
                              state_at(engine, 1U, ReceiverState::tuned);
        },
        bound);
    return result;
}

bool test_separate_receivers_are_not_serialized()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSatellite));
    f.calls.clear();

    const IndependentTunes result = tune_while_other_receiver_held(f, engine, kWaitBound);
    ENGINE_CHECK(result.proceeded && result.observed && result.held && result.other);
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::tuned));
    ENGINE_CHECK(same_calls("blocked 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    return true;
}

bool test_serialized_regression_fails_without_hanging()
{
    // Failure experiment: with every receiver behind one lock, the probe
    // cannot finish while 17 is held.  The check must report that and return;
    // the gate release then lets both operations complete.
    Fixture f;
    f.backend.serialize = true;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSatellite));

    const auto started = std::chrono::steady_clock::now();
    const IndependentTunes result =
        tune_while_other_receiver_held(f, engine, kExpectedTimeout);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ENGINE_CHECK(!result.proceeded);
    ENGINE_CHECK(elapsed < kWaitBound);
    // Both operations still completed after the gate was released.
    ENGINE_CHECK(result.held && result.other);
    return true;
}

bool test_validation_travels_with_each_request()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSecondTerrestrial));
    f.calls.clear();

    // Precedence: the decision is not consulted before the lease, stream and
    // support checks.
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(99U, Error::INVALID_ARGUMENT)),
                        Error::NOT_FOUND));
    TuneRequest unsupported = satellite_slot(1U, 0U);
    unsupported.validation = Error::UNSUPPORTED;
    ENGINE_CHECK(failed(engine.tune(1U, unsupported), Error::INVALID_ARGUMENT));
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(1U, Error::UNSUPPORTED)),
                        Error::UNSUPPORTED));
    ENGINE_CHECK(same_calls("precedence", f.calls.take(), {"supports 17 2", "supports 17 1"}));

    // Two requests whose neutral fields coincide (apart from the lease) run on
    // separate receivers.  The valid one waits at the validation point while
    // the invalid one completes; neither sees the other's decision.
    Result<void> held = Result<void>::failure(Error::INTERNAL);
    Result<void> rejected = Result<void>::failure(Error::INTERNAL);
    const bool proceeded = probe_while_held(
        f.backend.supports_gate, kTerrestrial,
        [&]() { held = engine.tune(1U, terrestrial(1U)); },
        [&]() { rejected = engine.tune(1U, terrestrial(2U, Error::INVALID_ARGUMENT)); });
    ENGINE_CHECK(proceeded && held && failed(rejected, Error::INVALID_ARGUMENT));
    ENGINE_CHECK(same_calls("rejected 4097", f.calls.for_receiver(kSecondTerrestrial),
                            {"supports 4097 1"}));
    ENGINE_CHECK(same_calls("held 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    f.calls.clear();

    // Reverse roles: the invalid request waits while the valid one completes.
    held = Result<void>::failure(Error::INTERNAL);
    Result<void> accepted = Result<void>::failure(Error::INTERNAL);
    const bool reproceeded = probe_while_held(
        f.backend.supports_gate, kTerrestrial,
        [&]() { held = engine.tune(1U, terrestrial(1U, Error::INVALID_ARGUMENT)); },
        [&]() { accepted = engine.tune(1U, terrestrial(2U)); });
    ENGINE_CHECK(reproceeded && accepted && failed(held, Error::INVALID_ARGUMENT));
    ENGINE_CHECK(same_calls("held invalid 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1"}));
    ENGINE_CHECK(same_calls("accepted 4097", f.calls.for_receiver(kSecondTerrestrial),
                            {"supports 4097 1", "power 4097 0", "tune 4097 1 557000 1000",
                             "lock 4097", "commit 4097"}));

    // An armed stream (BUSY) also precedes the decision.
    ENGINE_CHECK(engine.start_stream(1U, 2U));
    f.calls.clear();
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(2U, Error::INVALID_ARGUMENT)),
                        Error::BUSY));
    ENGINE_CHECK(same_calls("busy", f.calls.take(), {}));
    return true;
}

bool test_stale_attachment_cleanup()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock, nullptr, nullptr,
                          &f.stream);
    const auto lease = engine.acquire(1U, kSatellite);
    ENGINE_CHECK(lease && engine.tune(1U, satellite_slot(1U, 1U)) && engine.start_stream(1U, 1U));
    const auto first = engine.attach_stream(1U, lease.value().token);
    ENGINE_CHECK(first && engine.stop_stream(1U, 1U));
    ENGINE_CHECK(engine.tune(1U, satellite_slot(1U, 1U)) && engine.start_stream(1U, 1U));
    const auto second = engine.attach_stream(1U, lease.value().token);
    ENGINE_CHECK(second && second.value().attachment_id == 2U);
    f.calls.clear();

    // Only the exact current identity may detach.
    ENGINE_CHECK(failed(engine.detach_stream(first.value()), Error::NOT_FOUND));
    Attachment moved = second.value();
    moved.receiver = kTerrestrial;
    ENGINE_CHECK(failed(engine.detach_stream(moved), Error::NOT_FOUND));
    Attachment forged = second.value();
    forged.token[0U] ^= 0xffU;
    ENGINE_CHECK(failed(engine.detach_stream(forged), Error::NOT_FOUND));
    ENGINE_CHECK(failed(engine.revoke_stream(1U, 1U, forged.token), Error::NOT_FOUND));
    ENGINE_CHECK(same_calls("stale", f.calls.take(), {}));
    ENGINE_CHECK(state_at(engine, 1U, ReceiverState::streaming));

    // Release of a streaming lease cleans up exactly the current identity.
    ENGINE_CHECK(engine.release(1U, 1U));
    ENGINE_CHECK(same_calls("release", f.calls.take(),
                            {"detach 513 1 2", "stop 513", "close 513"}));
    ENGINE_CHECK(failed(engine.detach_stream(second.value()), Error::NOT_FOUND));

    // A lost data plane quarantines the sparse receiver without further I/O.
    const auto again = engine.acquire(2U, kSatellite);
    ENGINE_CHECK(again && engine.tune(2U, satellite_slot(2U, 1U)) && engine.start_stream(2U, 2U));
    ENGINE_CHECK(engine.attach_stream(2U, again.value().token));
    f.calls.clear();
    f.stream.detach_failures = {Error::DISCONNECTED};
    ENGINE_CHECK(failed(engine.disconnect_client(2U), Error::DISCONNECTED));
    ENGINE_CHECK(same_calls("lost", f.calls.take(), {"detach 513 2 3", "lost 513"}));
    ENGINE_CHECK(state_at(engine, 1U, ReceiverState::error));
    ENGINE_CHECK(failed(engine.acquire(3U, kSatellite), Error::NOT_READY));
    return true;
}

#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
bool test_v1_attach_waits_for_retained_eof_final()
{
    Fixture f;
    PacketSessionStream stream;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock,
                          nullptr, nullptr, &stream);
    constexpr std::uint64_t epoch = 0x454f4646494e414cULL;
    ReceiverCapability capability{};
    capability.receiver = kTerrestrial;
    capability.supports_terrestrial = true;
    const std::array<ReceiverCapability, 1U> capabilities{capability};
    ENGINE_CHECK(engine.configure_strict_capabilities(capabilities.data(),
                                                       capabilities.size()));
    ENGINE_CHECK(engine.configure_stream_contract(StreamContract::v1, epoch));

    const auto arm = [&engine, &f](std::uint64_t client, std::uint64_t lease_id,
                               std::uint64_t now) {
        const auto admitted = engine.try_admit_tune(
            client, terrestrial(lease_id), f.clock.monotonic_ms());
        if (!admitted)
            return Result<StreamStartResult>::failure(admitted.error());
        const auto tuned = engine.execute_admitted_tune(admitted.value());
        if (!tuned)
            return Result<StreamStartResult>::failure(tuned.error());
        const auto status = engine.status();
        if (!status) return Result<StreamStartResult>::failure(status.error());
        return engine.start_stream_v1(client, lease_id,
            status.value().tune_metadata[0U].current_revision, now, 5000U);
    };
    const auto attach = [&engine, epoch](std::uint64_t client, std::uint64_t lease_id,
                                         const StreamStartResult& started,
                                         std::uint64_t now) {
        return engine.reserve_attach_v1(client, lease_id, epoch,
            started.stream_id, started.token, now);
    };
    const auto finish_stream = [&engine](std::uint64_t client, std::uint64_t lease_id,
                                         const StreamAttachTicket& ticket) {
        const auto stopped = engine.stop_stream_v1(client, lease_id, ticket.stream_id);
        return stopped && engine.run_stream_cleanup_v1(ticket);
    };

    const auto first_lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(first_lease);
    const auto first_start = arm(1U, first_lease.value().lease_id, 10U);
    ENGINE_CHECK(first_start);
    const auto first_ticket = attach(1U, first_lease.value().lease_id,
                                     first_start.value(), 11U);
    ENGINE_CHECK(first_ticket && engine.prepare_attach_v1(first_ticket.value()) &&
                 engine.commit_attach_v1(first_ticket.value()) &&
                 finish_stream(1U, first_lease.value().lease_id, first_ticket.value()));

    const auto first_attachment = engine.stream_attachment_v1(first_ticket.value());
    ENGINE_CHECK(first_attachment);
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    const auto eof = stream.read_eof(first_attachment.value(),
        MutableByteView{output.data(), output.size()});
    ENGINE_CHECK(eof && eof.value().eof && !eof.value().timed_out &&
                 eof.value().terminal == StreamTerminal::stopped);

    const auto same_lease_start = arm(1U, first_lease.value().lease_id, 20U);
    ENGINE_CHECK(same_lease_start &&
                 same_lease_start.value().stream_id == first_start.value().stream_id + 1U);
    const auto same_lease_rejected = attach(1U, first_lease.value().lease_id,
                                            same_lease_start.value(), 21U);
    ENGINE_CHECK(failed(same_lease_rejected, Error::BUSY));
    ENGINE_CHECK(engine.release_stream_final_v1(first_ticket.value()));
    const auto same_lease_ticket = attach(1U, first_lease.value().lease_id,
                                          same_lease_start.value(), 22U);
    ENGINE_CHECK(same_lease_ticket &&
                 engine.prepare_attach_v1(same_lease_ticket.value()) &&
                 engine.commit_attach_v1(same_lease_ticket.value()) &&
                 finish_stream(1U, first_lease.value().lease_id,
                               same_lease_ticket.value()));
    const unsigned tokens_before_guard = f.tokens.next.load();
    ENGINE_CHECK(dtv::userland::detail::ReceiverEngineTestAccess::retain_current_final(
        engine, same_lease_ticket.value()));
    const auto status_before_guard = engine.status();
    ENGINE_CHECK(status_before_guard);
    const auto guarded_start = engine.start_stream_v1(1U,
        first_lease.value().lease_id,
        status_before_guard.value().tune_metadata[0U].current_revision, 29U, 5000U);
    const auto retained_snapshot = engine.stream_final_snapshot_v1(
        same_lease_ticket.value());
    ENGINE_CHECK(failed(guarded_start, Error::INTERNAL) &&
                 f.tokens.next.load() == tokens_before_guard && retained_snapshot);
    dtv::userland::detail::ReceiverEngineTestAccess::clear_retained_final(engine, 0U);
    ENGINE_CHECK(engine.release(1U, first_lease.value().lease_id) &&
                 engine.release_stream_final_v1(same_lease_ticket.value()));

    const auto second_lease = engine.acquire(2U, kTerrestrial);
    ENGINE_CHECK(second_lease);
    const auto second_start = arm(2U, second_lease.value().lease_id, 20U);
    ENGINE_CHECK(second_start);
    const auto second_ticket = attach(2U, second_lease.value().lease_id,
                                      second_start.value(), 22U);
    ENGINE_CHECK(second_ticket && engine.prepare_attach_v1(second_ticket.value()) &&
                 engine.commit_attach_v1(second_ticket.value()) &&
                 finish_stream(2U, second_lease.value().lease_id, second_ticket.value()) &&
                 engine.release(2U, second_lease.value().lease_id));

    const auto third_lease = engine.acquire(3U, kTerrestrial);
    ENGINE_CHECK(third_lease);
    const auto third_start = arm(3U, third_lease.value().lease_id, 30U);
    ENGINE_CHECK(third_start);
    const auto third_ticket = attach(3U, third_lease.value().lease_id,
                                     third_start.value(), 31U);
    ENGINE_CHECK(failed(third_ticket, Error::BUSY));
    ENGINE_CHECK(engine.release_stream_final_v1(second_ticket.value()));
    const auto third_retry = attach(3U, third_lease.value().lease_id,
                                    third_start.value(), 32U);
    ENGINE_CHECK(third_retry && engine.prepare_attach_v1(third_retry.value()) &&
                 engine.commit_attach_v1(third_retry.value()) &&
                 finish_stream(3U, third_lease.value().lease_id, third_retry.value()));
    const auto third_attachment = engine.stream_attachment_v1(third_retry.value());
    ENGINE_CHECK(third_attachment);

    std::array<std::uint8_t, PacketStreamSession::packet_size> barrier_output{};
    Result<StreamReadResult> barrier_read =
        Result<StreamReadResult>::failure(Error::NOT_FOUND);
    std::thread reader([&] {
        barrier_read = stream.read_eof_after_barrier(third_attachment.value(),
            MutableByteView{barrier_output.data(), barrier_output.size()});
    });
    if (!stream.wait_read_barrier()) {
        stream.release_read_barrier();
        reader.join();
        return false;
    }
    const auto fourth_start = arm(3U, third_lease.value().lease_id, 40U);
    if (!fourth_start) {
        stream.release_read_barrier();
        reader.join();
        return false;
    }
    const auto barrier_rejected = attach(3U, third_lease.value().lease_id,
                                         fourth_start.value(), 41U);
    const bool rejected_busy = failed(barrier_rejected, Error::BUSY);
    stream.release_read_barrier();
    reader.join();
    ENGINE_CHECK(rejected_busy && barrier_read && barrier_read.value().eof &&
                 engine.release_stream_final_v1(third_retry.value()));
    const auto fourth_ticket = attach(3U, third_lease.value().lease_id,
                                      fourth_start.value(), 42U);
    ENGINE_CHECK(fourth_ticket && engine.prepare_attach_v1(fourth_ticket.value()) &&
                 engine.commit_attach_v1(fourth_ticket.value()) &&
                 finish_stream(3U, third_lease.value().lease_id, fourth_ticket.value()) &&
                 engine.release(3U, third_lease.value().lease_id) &&
                 engine.release_stream_final_v1(fourth_ticket.value()));

    PacketStreamSession legacy;
    Attachment legacy_first{};
    legacy_first.owner_client_id = 99U;
    legacy_first.lease_id = 1U;
    legacy_first.attachment_id = 1U;
    legacy_first.receiver = kTerrestrial;
    {
        std::lock_guard<std::mutex> lock(legacy.mutex);
        ENGINE_CHECK(legacy.begin_attach_locked(legacy_first, 4U));
        ENGINE_CHECK(legacy.detach_locked(legacy_first));
        const auto ticket = legacy.begin_read_locked(legacy_first);
        ENGINE_CHECK(ticket);
        const auto legacy_eof = legacy.finish_read_locked(legacy_first, ticket.value(),
            MutableByteView{output.data(), output.size()});
        ENGINE_CHECK(legacy_eof && legacy_eof.value().eof);
        Attachment legacy_second = legacy_first;
        legacy_second.attachment_id = 2U;
        ENGINE_CHECK(legacy.begin_attach_locked(legacy_second, 4U));
    }
    return true;
}

bool test_v1_retained_final_expiry_precedes_busy()
{
    Fixture f;
    PacketSessionStream stream;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock,
                          nullptr, nullptr, &stream);
    constexpr std::uint64_t epoch = 0x4558504952454649ULL;
    ReceiverCapability capability{};
    capability.receiver = kTerrestrial;
    capability.supports_terrestrial = true;
    const std::array<ReceiverCapability, 1U> capabilities{capability};
    ENGINE_CHECK(engine.configure_strict_capabilities(capabilities.data(),
                                                       capabilities.size()));
    ENGINE_CHECK(engine.configure_stream_contract(StreamContract::v1, epoch));
    const auto lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(lease);
    const auto admitted = engine.try_admit_tune(
        1U, terrestrial(1U), f.clock.monotonic_ms());
    ENGINE_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto status = engine.status();
    ENGINE_CHECK(status);
    const auto first_start = engine.start_stream_v1(1U, lease.value().lease_id,
        status.value().tune_metadata[0U].current_revision, 10U, 5000U);
    ENGINE_CHECK(first_start);
    const auto first_ticket = engine.reserve_attach_v1(1U, lease.value().lease_id,
        epoch, first_start.value().stream_id, first_start.value().token, 11U);
    ENGINE_CHECK(first_ticket && engine.prepare_attach_v1(first_ticket.value()) &&
                 engine.commit_attach_v1(first_ticket.value()));
    const auto stopped = engine.stop_stream_v1(1U, lease.value().lease_id,
                                               first_start.value().stream_id);
    ENGINE_CHECK(stopped && engine.run_stream_cleanup_v1(first_ticket.value()));

    const auto expiring_start = engine.start_stream_v1(1U, lease.value().lease_id,
        status.value().tune_metadata[0U].current_revision, 20U, 10U);
    ENGINE_CHECK(expiring_start);
    const auto expired = engine.reserve_attach_v1(1U, lease.value().lease_id,
        epoch, expiring_start.value().stream_id, expiring_start.value().token,
        expiring_start.value().expires_at_ms);
    ENGINE_CHECK(failed(expired, Error::TIMEOUT));
    ENGINE_CHECK(engine.release_stream_final_v1(first_ticket.value()));

    const auto retry_start = engine.start_stream_v1(1U, lease.value().lease_id,
        status.value().tune_metadata[0U].current_revision, 31U, 5000U);
    ENGINE_CHECK(retry_start);
    const auto retry_ticket = engine.reserve_attach_v1(1U, lease.value().lease_id,
        epoch, retry_start.value().stream_id, retry_start.value().token, 32U);
    ENGINE_CHECK(retry_ticket && engine.prepare_attach_v1(retry_ticket.value()) &&
                 engine.commit_attach_v1(retry_ticket.value()));
    const auto retry_stopped = engine.stop_stream_v1(1U, lease.value().lease_id,
                                                      retry_start.value().stream_id);
    ENGINE_CHECK(retry_stopped && engine.run_stream_cleanup_v1(retry_ticket.value()) &&
                 engine.release(1U, lease.value().lease_id) &&
                 engine.release_stream_final_v1(retry_ticket.value()));
    return true;
}

bool test_v1_release_completion_consumes_migrated_final()
{
    Fixture f;
    PacketSessionStream stream;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock,
                          nullptr, nullptr, &stream);
    ENGINE_CHECK(configure_v1_session_engine(engine));
    const auto lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(lease);
    StreamAttachTicket old_ticket{};
    ENGINE_CHECK(create_eof_final(f, stream, engine, 1U, lease.value().lease_id,
                                  old_ticket));

    stream.hold_next_release();
    Result<void> old_release = Result<void>::failure(Error::INTERNAL);
    std::thread release_thread([&] {
        old_release = engine.release_stream_final_v1(old_ticket);
    });
    if (!stream.wait_release_barrier()) {
        stream.release_release_barrier();
        release_thread.join();
        return false;
    }
    const auto status = engine.status();
    const auto next_start = status
        ? engine.start_stream_v1(1U, lease.value().lease_id,
            status.value().tune_metadata[0U].current_revision, 20U, 5000U)
        : Result<StreamStartResult>::failure(Error::INTERNAL);
    const auto blocked = next_start
        ? engine.reserve_attach_v1(1U, lease.value().lease_id, kReleaseMigrationEpoch,
            next_start.value().stream_id, next_start.value().token, 21U)
        : Result<StreamAttachTicket>::failure(Error::INTERNAL);
    stream.release_release_barrier();
    release_thread.join();

    ENGINE_CHECK(next_start && failed(blocked, Error::BUSY) && old_release);
    const auto next_ticket = engine.reserve_attach_v1(1U, lease.value().lease_id,
        kReleaseMigrationEpoch, next_start.value().stream_id,
        next_start.value().token, 22U);
    ENGINE_CHECK(next_ticket);
    ENGINE_CHECK(engine.prepare_attach_v1(next_ticket.value()) &&
                 engine.commit_attach_v1(next_ticket.value()) &&
                 failed(engine.release_stream_final_v1(old_ticket), Error::NOT_FOUND));
    const auto stopped = engine.stop_stream_v1(1U, lease.value().lease_id,
                                               next_start.value().stream_id);
    ENGINE_CHECK(stopped && engine.run_stream_cleanup_v1(next_ticket.value()) &&
                 engine.release_stream_final_v1(next_ticket.value()) &&
                 engine.release(1U, lease.value().lease_id));

    const auto other_lease = engine.acquire(2U, kTerrestrial);
    ENGINE_CHECK(other_lease);
    const auto admitted = engine.try_admit_tune(
        2U, terrestrial(other_lease.value().lease_id), f.clock.monotonic_ms());
    ENGINE_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto other_status = engine.status();
    ENGINE_CHECK(other_status);
    const auto other_start = engine.start_stream_v1(2U, other_lease.value().lease_id,
        other_status.value().tune_metadata[0U].current_revision, 30U, 5000U);
    ENGINE_CHECK(other_start);
    const auto other_ticket = engine.reserve_attach_v1(2U, other_lease.value().lease_id,
        kReleaseMigrationEpoch, other_start.value().stream_id,
        other_start.value().token, 31U);
    ENGINE_CHECK(other_ticket && engine.prepare_attach_v1(other_ticket.value()) &&
                 engine.commit_attach_v1(other_ticket.value()));
    const auto other_stopped = engine.stop_stream_v1(2U, other_lease.value().lease_id,
                                                      other_start.value().stream_id);
    ENGINE_CHECK(other_stopped && engine.run_stream_cleanup_v1(other_ticket.value()) &&
                 engine.release_stream_final_v1(other_ticket.value()) &&
                 engine.release(2U, other_lease.value().lease_id));
    return true;
}

bool test_v1_release_failure_keeps_migrated_final()
{
    Fixture f;
    PacketSessionStream stream;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock,
                          nullptr, nullptr, &stream);
    ENGINE_CHECK(configure_v1_session_engine(engine));
    const auto lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(lease);
    StreamAttachTicket old_ticket{};
    ENGINE_CHECK(create_eof_final(f, stream, engine, 1U, lease.value().lease_id,
                                  old_ticket));

    stream.hold_next_release();
    stream.fail_next_release.store(true);
    Result<void> old_release = Result<void>::success();
    std::thread release_thread([&] {
        old_release = engine.release_stream_final_v1(old_ticket);
    });
    if (!stream.wait_release_barrier()) {
        stream.release_release_barrier();
        release_thread.join();
        return false;
    }
    const auto status = engine.status();
    const auto next_start = status
        ? engine.start_stream_v1(1U, lease.value().lease_id,
            status.value().tune_metadata[0U].current_revision, 20U, 5000U)
        : Result<StreamStartResult>::failure(Error::INTERNAL);
    stream.release_release_barrier();
    release_thread.join();

    ENGINE_CHECK(next_start && failed(old_release, Error::INTERNAL));
    const auto blocked = engine.reserve_attach_v1(1U, lease.value().lease_id,
        kReleaseMigrationEpoch, next_start.value().stream_id,
        next_start.value().token, 21U);
    ENGINE_CHECK(failed(blocked, Error::BUSY) &&
                 engine.release_stream_final_v1(old_ticket));
    const auto next_ticket = engine.reserve_attach_v1(1U, lease.value().lease_id,
        kReleaseMigrationEpoch, next_start.value().stream_id,
        next_start.value().token, 22U);
    ENGINE_CHECK(next_ticket && engine.prepare_attach_v1(next_ticket.value()) &&
                 engine.commit_attach_v1(next_ticket.value()));
    const auto stopped = engine.stop_stream_v1(1U, lease.value().lease_id,
                                               next_start.value().stream_id);
    ENGINE_CHECK(stopped && engine.run_stream_cleanup_v1(next_ticket.value()) &&
                 engine.release_stream_final_v1(next_ticket.value()) &&
                 engine.release(1U, lease.value().lease_id));
    return true;
}

bool test_v1_release_serializes_lease_release()
{
    Fixture f;
    PacketSessionStream stream;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock,
                          nullptr, nullptr, &stream);
    ENGINE_CHECK(configure_v1_session_engine(engine));
    const auto lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(lease);
    StreamAttachTicket old_ticket{};
    ENGINE_CHECK(create_eof_final(f, stream, engine, 1U, lease.value().lease_id,
                                  old_ticket));

    stream.hold_next_release();
    Result<void> old_release = Result<void>::failure(Error::INTERNAL);
    std::thread release_thread([&] {
        old_release = engine.release_stream_final_v1(old_ticket);
    });
    if (!stream.wait_release_barrier()) {
        stream.release_release_barrier();
        release_thread.join();
        return false;
    }
    const auto status = engine.status();
    const auto next_start = status
        ? engine.start_stream_v1(1U, lease.value().lease_id,
            status.value().tune_metadata[0U].current_revision, 20U, 5000U)
        : Result<StreamStartResult>::failure(Error::INTERNAL);
    const auto canceled = next_start
        ? engine.stop_stream_v1(1U, lease.value().lease_id,
                                next_start.value().stream_id)
        : Result<StreamStopResult>::failure(Error::INTERNAL);
    const auto before_release = engine.status();
    const bool idle_before_release = before_release &&
        before_release.value().operations[0U] == ReceiverTuneOperation::idle;
    std::atomic<bool> lease_release_done{false};
    Result<void> lease_release = Result<void>::failure(Error::INTERNAL);
    std::thread lease_release_thread([&] {
        lease_release = engine.release(1U, lease.value().lease_id);
        lease_release_done.store(true);
    });
    const auto wait_until = std::chrono::steady_clock::now() + kWaitBound;
    bool lease_release_entered = false;
    while (!lease_release_done.load() && std::chrono::steady_clock::now() < wait_until) {
        const auto releasing = engine.status();
        if (releasing && releasing.value().operations[0U] ==
                             ReceiverTuneOperation::cleanup) {
            lease_release_entered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool release_waited = idle_before_release && lease_release_entered &&
                                !lease_release_done.load();
    stream.release_release_barrier();
    release_thread.join();
    lease_release_thread.join();

    ENGINE_CHECK(next_start && canceled && canceled.value().final_valid &&
                 release_waited && old_release && lease_release &&
                 lease_release_done.load() &&
                 failed(engine.release_stream_final_v1(old_ticket), Error::NOT_FOUND));
    const auto other_lease = engine.acquire(2U, kTerrestrial);
    ENGINE_CHECK(other_lease);
    const auto admitted = engine.try_admit_tune(
        2U, terrestrial(other_lease.value().lease_id), f.clock.monotonic_ms());
    ENGINE_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto other_status = engine.status();
    ENGINE_CHECK(other_status);
    const auto other_start = engine.start_stream_v1(2U, other_lease.value().lease_id,
        other_status.value().tune_metadata[0U].current_revision, 30U, 5000U);
    ENGINE_CHECK(other_start);
    const auto other_ticket = engine.reserve_attach_v1(2U, other_lease.value().lease_id,
        kReleaseMigrationEpoch, other_start.value().stream_id,
        other_start.value().token, 31U);
    ENGINE_CHECK(other_ticket && engine.prepare_attach_v1(other_ticket.value()) &&
                 engine.commit_attach_v1(other_ticket.value()));
    const auto stopped = engine.stop_stream_v1(2U, other_lease.value().lease_id,
                                               other_start.value().stream_id);
    ENGINE_CHECK(stopped && engine.run_stream_cleanup_v1(other_ticket.value()) &&
                 engine.release_stream_final_v1(other_ticket.value()) &&
                 engine.release(2U, other_lease.value().lease_id));
    return true;
}
#endif

bool test_legacy_timing_is_preserved()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // Compatibility semantics, not the adopted v1 deadline: the tune budget
    // starts after power preparation, so a 5 s power wait does not consume a
    // 1 s budget.
    f.backend.power_wait_ms = 5000U;
    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("power outside budget", f.calls.take(),
                            {"supports 17 1", "power 17 0", "sleep 5000",
                             "tune 17 1 557000 1000", "lock 17", "commit 17"}));
    f.backend.power_wait_ms = 0U;

    // The settle interval is supplied by the backend and measured from the
    // first lock query.
    f.backend.settle_ms = 340U;
    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("settle", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "sleep 340", "commit 17"}));
    TuneRequest short_budget = terrestrial(1U);
    short_budget.timeout_ms = 340U;
    ENGINE_CHECK(failed(engine.tune(1U, short_budget), Error::TIMEOUT));
    ENGINE_CHECK(same_calls("settle beyond budget", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 340",
                             "lock 17", "rollback 17"}));
    return true;
}

}  // namespace

int main()
{
    return test_sparse_ids_through_lifecycle() && test_invalid_missing_ids_and_capacity() &&
                   test_separate_receivers_are_not_serialized() &&
                   test_serialized_regression_fails_without_hanging() &&
                   test_validation_travels_with_each_request() &&
                   test_stale_attachment_cleanup() &&
#if defined(DTV_USERLAND_TEST_PACKET_SESSION)
                   test_v1_attach_waits_for_retained_eof_final() &&
                   test_v1_retained_final_expiry_precedes_busy() &&
                   test_v1_release_completion_consumes_migrated_final() &&
                   test_v1_release_failure_keeps_migrated_final() &&
                   test_v1_release_serializes_lease_release() &&
#endif
                   test_legacy_timing_is_preserved()
               ? 0
               : 1;
}
