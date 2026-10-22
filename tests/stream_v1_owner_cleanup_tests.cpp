// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define OWN_CHECK(condition)                                                       \
    do {                                                                           \
        if (!(condition)) {                                                        \
            std::fprintf(stderr, "stream v1 owner cleanup check failed at line %d: %s\n", \
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
    bool entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return entered_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool open_ = false;
};

// RAII: opening and joining happen even when an assertion returns early.
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
        return shutdown_result == Error::OK ? Result<void>::success()
                                            : Result<void>::failure(shutdown_result);
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
        return stop_result == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(stop_result);
    }
    Error start_result = Error::OK;
    Error stop_result = Error::OK;
    Error shutdown_result = Error::OK;
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
        return attach_result == Error::OK ? Result<void>::success()
                                          : Result<void>::failure(attach_result);
    }
    Result<void> detach(const Attachment& attachment) noexcept override
    {
        ++detaches;
        last_detach = attachment;
        detach_token = attachment.token;
        return detach_result == Error::OK ? Result<void>::success()
                                          : Result<void>::failure(detach_result);
    }
    Result<StreamCounters> stats(const Attachment&) const noexcept override
    {
        return Result<StreamCounters>::success(StreamCounters{});
    }
    Result<StreamFinalSnapshot> final_snapshot(const Attachment&) const noexcept override
    {
        return Result<StreamFinalSnapshot>::success(StreamFinalSnapshot{});
    }
    Error attach_result = Error::OK;
    Error detach_result = Error::OK;
    int attaches = 0;
    int detaches = 0;
    Attachment last_attach{};
    Attachment last_detach{};
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
    OWN_CHECK(f.engine.configure_strict_capabilities(capabilities.data(),
                                                     capabilities.size()));
    const auto lease = f.engine.acquire(1U, 1U);
    OWN_CHECK(lease);
    lease_id = lease.value().lease_id;
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 5000U;
    const auto admitted = f.engine.try_admit_tune(1U, request, 0U);
    OWN_CHECK(admitted);
    OWN_CHECK(f.engine.execute_admitted_tune(admitted.value()));
    revision = admitted.value().tune_revision;
    return true;
}

struct Active final {
    std::uint64_t stream_id = 0U;
    StreamAttachTicket ticket{};
    ReceiverToken token{};
};

bool start_active(Fixture& f, std::uint64_t lease, std::uint64_t revision, Active& out)
{
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    out.stream_id = started.value().stream_id;
    out.token = started.value().token;
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     out.stream_id, out.token, 1000U);
    OWN_CHECK(reserved);
    out.ticket = reserved.value();
    OWN_CHECK(f.engine.prepare_attach_v1(out.ticket));
    OWN_CHECK(f.engine.commit_attach_v1(out.ticket));
    return true;
}

// Real backend stop and exact-identity D2 detach exactly once.
bool test_real_cleanup_identity_and_count()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    Active a;
    OWN_CHECK(start_active(f, lease, revision, a));
    OWN_CHECK(f.stream.attaches == 1);
    const auto stop = f.engine.stop_stream_v1(1U, lease, a.stream_id);
    OWN_CHECK(stop);
    OWN_CHECK(stop.value().cleanup_owner);
    OWN_CHECK(!stop.value().quiesced);
    // Metadata STOP performs no backend I/O.
    OWN_CHECK(f.backend.stops == 0);
    OWN_CHECK(f.stream.detaches == 0);
    // The worker cleanup performs stop and the exact START-token detach once.
    OWN_CHECK(f.engine.run_stream_cleanup_v1(a.ticket));
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    OWN_CHECK(f.stream.detach_token == a.token);
    OWN_CHECK(f.stream.last_detach.attachment_id == a.ticket.reservation);
    OWN_CHECK(f.stream.last_detach.owner_client_id == 1U);
    OWN_CHECK(f.stream.last_detach.lease_id == lease);
    // A second cleanup is refused without repeating I/O.
    OWN_CHECK(f.engine.run_stream_cleanup_v1(a.ticket).error() == Error::NOT_FOUND);
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    const auto last = f.engine.stream_last_terminal_v1(1U, lease);
    OWN_CHECK(last);
    OWN_CHECK(last.value().stream_id == a.stream_id);
    OWN_CHECK(last.value().quiesced);
    return true;
}

// Partial start failure retains attempted capture ownership and cleans once.
bool test_partial_start_failure_retains()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.backend.start_result = Error::USB_IO;
    OWN_CHECK(f.engine.prepare_attach_v1(reserved.value()).error() == Error::USB_IO);
    // The failed start was attempted, so cleanup still stops capture/detaches.
    OWN_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    OWN_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    const auto stop = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    OWN_CHECK(stop);
    OWN_CHECK(stop.value().cleanup_error == Error::USB_IO);
    return true;
}

// A failed D2 publication retains the attempted attach identity and detaches it.
bool test_failed_attach_retains()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.stream.attach_result = Error::INTERNAL;
    OWN_CHECK(f.engine.prepare_attach_v1(reserved.value()).error() == Error::INTERNAL);
    OWN_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    OWN_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    OWN_CHECK(f.stream.detaches == 1);
    OWN_CHECK(f.stream.detach_token == started.value().token);
    return true;
}

// Owner disconnect integrates with the exact-ticket cleanup: it revokes the
// metadata, retains the lease and never uses the legacy ACQUIRE token.
bool test_owner_disconnect_active()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    Active a;
    OWN_CHECK(start_active(f, lease, revision, a));
    // Owner disconnect retains the physical owner: no detach with the ACQUIRE
    // token, no free.
    OWN_CHECK(f.engine.disconnect_client(1U).error() == Error::NOT_READY);
    OWN_CHECK(f.stream.detaches == 0);
    OWN_CHECK(f.engine.status());
    // The worker reconciles with the exact ticket.
    OWN_CHECK(f.engine.run_stream_cleanup_v1(a.ticket));
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    OWN_CHECK(f.stream.detach_token == a.token);
    // A second owner disconnect now frees the lease.
    OWN_CHECK(f.engine.disconnect_client(1U));
    auto status = f.engine.status();
    OWN_CHECK(status);
    OWN_CHECK(status.value().leased[0U] == false);
    OWN_CHECK(f.backend.shutdowns == 0);
    return true;
}

// Owner disconnect while prepare is held by a finite gate.
bool test_owner_disconnect_held_prepare()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.backend.gate_start = true;
    Error prepare_error = Error::OK;
    std::thread worker([&] {
        const auto prepared = f.engine.prepare_attach_v1(reserved.value());
        prepare_error = prepared ? Error::OK : prepared.error();
    });
    // Declaration order gives failure-safe teardown: the gate is released
    // before the thread is joined (reverse destruction order).
    Joiner joiner(worker);
    GateRelease release(f.backend.start_gate);
    OWN_CHECK(f.backend.start_gate.wait_entered());
    OWN_CHECK(f.engine.disconnect_client(1U).error() == Error::NOT_READY);
    // The legacy identity must not be detached by the disconnect.
    OWN_CHECK(f.stream.detaches == 0);
    f.backend.start_gate.open();
    worker.join();
    OWN_CHECK(prepare_error == Error::NOT_FOUND);
    // The late prepare did not publish active and did not lose ownership.
    auto status = f.engine.status();
    OWN_CHECK(status);
    OWN_CHECK(status.value().stream_phases[0U] ==
              static_cast<std::uint8_t>(StreamPhase::closing));
    OWN_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    OWN_CHECK(f.stream.detach_token == started.value().token);
    OWN_CHECK(f.engine.disconnect_client(1U));
    return true;
}

// Shutdown retains the v1 physical owner and does not call backend shutdown.
bool test_shutdown_retains_v1()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    Active a;
    OWN_CHECK(start_active(f, lease, revision, a));
    OWN_CHECK(f.engine.shutdown().error() == Error::NOT_READY);
    OWN_CHECK(f.backend.shutdowns == 0);
    OWN_CHECK(f.engine.run_stream_cleanup_v1(a.ticket));
    OWN_CHECK(f.engine.shutdown());
    OWN_CHECK(f.backend.shutdowns == 1);
    return true;
}

// Armed expiry retains a terminal and the last terminal survives the next START.
bool test_expiry_and_last_terminal()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto a = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(a);
    const std::uint64_t a_id = a.value().stream_id;
    OWN_CHECK(f.engine.expire_armed_v1(1U, lease, a_id, 1000U).error() ==
              Error::NOT_READY);
    const auto expired = f.engine.expire_armed_v1(1U, lease, a_id, 6000U);
    OWN_CHECK(expired);
    OWN_CHECK(expired.value().final_valid);
    OWN_CHECK(expired.value().quiesced);
    OWN_CHECK(expired.value().stream_id == a_id);
    auto last = f.engine.stream_last_terminal_v1(1U, lease);
    OWN_CHECK(last);
    OWN_CHECK(last.value().stream_id == a_id);
    // A new armed stream does not lose the previous lease-lifetime terminal.
    const auto b = f.engine.start_stream_v1(1U, lease, revision, 7000U, 5000U);
    OWN_CHECK(b);
    last = f.engine.stream_last_terminal_v1(1U, lease);
    OWN_CHECK(last);
    OWN_CHECK(last.value().stream_id == a_id);
    return true;
}

// Every contract field of the returned ticket is revalidated.
bool test_full_ticket_fields()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    const StreamAttachTicket good = reserved.value();
    std::array<StreamAttachTicket, 6> bad{};
    bad[0] = good; bad[0].token[0] = static_cast<std::uint8_t>(good.token[0] + 1U);
    bad[1] = good; bad[1].revision += 1U;
    bad[2] = good; bad[2].deadline_ms += 1U;
    bad[3] = good; bad[3].client_id += 1U;
    bad[4] = good; bad[4].epoch += 1U;
    bad[5] = good; bad[5].reservation += 1U;
    for (const auto& ticket : bad) {
        OWN_CHECK(f.engine.prepare_attach_v1(ticket).error() == Error::NOT_FOUND);
        OWN_CHECK(f.engine.commit_attach_v1(ticket).error() == Error::NOT_FOUND);
    }
    // The genuine ticket still prepares and commits.
    OWN_CHECK(f.engine.prepare_attach_v1(good));
    OWN_CHECK(f.engine.commit_attach_v1(good));
    // Each bad ticket is refused by completion too.
    OWN_CHECK(f.engine.stop_stream_v1(1U, lease, good.stream_id));
    for (const auto& ticket : bad)
        OWN_CHECK(f.engine.complete_stream_cleanup_v1(ticket, Error::OK, true).error() ==
                  Error::NOT_FOUND);
    OWN_CHECK(f.engine.complete_stream_cleanup_v1(good, Error::OK, true));
    return true;
}

// A failed real cleanup retains its primary error while a late quiescence
// observation updates independently.
bool test_cleanup_failure_and_late_quiescence()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    Active a;
    OWN_CHECK(start_active(f, lease, revision, a));
    OWN_CHECK(f.engine.stop_stream_v1(1U, lease, a.stream_id));
    f.backend.stop_result = Error::USB_IO;
    OWN_CHECK(f.engine.run_stream_cleanup_v1(a.ticket).error() == Error::USB_IO);
    const auto stop = f.engine.stop_stream_v1(1U, lease, a.stream_id);
    OWN_CHECK(stop);
    OWN_CHECK(stop.value().cleanup_error == Error::USB_IO);
    OWN_CHECK(!stop.value().quiesced);
    // Late metadata confirmation updates quiescence but never clears the error.
    OWN_CHECK(f.engine.complete_stream_cleanup_v1(a.ticket, Error::OK, true));
    const auto late = f.engine.stop_stream_v1(1U, lease, a.stream_id);
    OWN_CHECK(late);
    OWN_CHECK(late.value().cleanup_error == Error::USB_IO);
    OWN_CHECK(late.value().quiesced);
    return true;
}

// Held owner shutdown: retains ownership, defers backend shutdown, then
// reclaims once through the exact ticket.
bool test_owner_shutdown_held_prepare()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.backend.gate_start = true;
    Error prepare_error = Error::OK;
    std::thread worker([&] {
        const auto prepared = f.engine.prepare_attach_v1(reserved.value());
        prepare_error = prepared ? Error::OK : prepared.error();
    });
    Joiner joiner(worker);
    GateRelease release(f.backend.start_gate);
    OWN_CHECK(f.backend.start_gate.wait_entered());
    OWN_CHECK(f.engine.shutdown().error() == Error::NOT_READY);
    OWN_CHECK(f.backend.shutdowns == 0);
    OWN_CHECK(f.stream.detaches == 0);
    f.backend.start_gate.open();
    worker.join();
    OWN_CHECK(prepare_error == Error::NOT_FOUND);
    OWN_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    OWN_CHECK(f.backend.stops == 1);
    OWN_CHECK(f.stream.detaches == 1);
    OWN_CHECK(f.stream.detach_token == started.value().token);
    OWN_CHECK(f.engine.shutdown());
    OWN_CHECK(f.backend.shutdowns == 1);
    return true;
}

// A concurrent duplicate prepare performs exactly one physical start/attach.
bool test_concurrent_duplicate_prepare()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.backend.gate_start = true;
    Error first_error = Error::OK;
    std::thread first([&] {
        const auto prepared = f.engine.prepare_attach_v1(reserved.value());
        first_error = prepared ? Error::OK : prepared.error();
    });
    Joiner joiner(first);
    GateRelease release(f.backend.start_gate);
    OWN_CHECK(f.backend.start_gate.wait_entered());
    // The second prepare is refused by the claim and does not enter the backend.
    OWN_CHECK(f.engine.prepare_attach_v1(reserved.value()).error() == Error::BUSY);
    f.backend.start_gate.open();
    first.join();
    OWN_CHECK(first_error == Error::OK);
    OWN_CHECK(f.backend.starts == 1);
    OWN_CHECK(f.stream.attaches == 1);
    return true;
}

// Commit while prepare is physically held cannot publish active.
bool test_commit_during_prepare()
{
    Fixture f;
    OWN_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    OWN_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    OWN_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    OWN_CHECK(reserved);
    f.backend.gate_start = true;
    Error prepare_error = Error::OK;
    std::thread worker([&] {
        const auto prepared = f.engine.prepare_attach_v1(reserved.value());
        prepare_error = prepared ? Error::OK : prepared.error();
    });
    Joiner joiner(worker);
    GateRelease release(f.backend.start_gate);
    OWN_CHECK(f.backend.start_gate.wait_entered());
    OWN_CHECK(f.engine.commit_attach_v1(reserved.value()).error() == Error::NOT_FOUND);
    auto status = f.engine.status();
    OWN_CHECK(status);
    OWN_CHECK(status.value().stream_phases[0U] ==
              static_cast<std::uint8_t>(StreamPhase::attaching));
    f.backend.start_gate.open();
    worker.join();
    OWN_CHECK(prepare_error == Error::OK);
    OWN_CHECK(f.engine.commit_attach_v1(reserved.value()));
    status = f.engine.status();
    OWN_CHECK(status.value().stream_phases[0U] ==
              static_cast<std::uint8_t>(StreamPhase::active));
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_real_cleanup_identity_and_count() &&
                    test_partial_start_failure_retains() &&
                    test_failed_attach_retains() &&
                    test_owner_disconnect_active() &&
                    test_owner_disconnect_held_prepare() &&
                    test_shutdown_retains_v1() && test_expiry_and_last_terminal() &&
                    test_full_ticket_fields() &&
                    test_cleanup_failure_and_late_quiescence() &&
                    test_owner_shutdown_held_prepare() &&
                    test_concurrent_duplicate_prepare() &&
                    test_commit_during_prepare();
    if (!ok) return 1;
    std::printf("stream v1 owner cleanup tests: PASS\n");
    return 0;
}
