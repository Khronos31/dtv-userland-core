// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define V1_CHECK(condition)                                                        \
    do {                                                                           \
        if (!(condition)) {                                                        \
            std::fprintf(stderr,                                                   \
                         "stream v1 authority check failed at line %d: %s\n",      \
                         __LINE__, #condition);                                    \
            return false;                                                          \
        }                                                                          \
    } while (false)

constexpr std::uint64_t kEpoch = 0x1234U;

class Gate final {
public:
    // Worker side: publish that the backend was entered, then wait for release.
    void enter_and_wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(10), [this] { return open_; });
    }
    // Test side: wait until the worker actually entered the gated section.
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

// Failure-safe teardown: declared after the thread so it opens before join.
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
    Result<void> shutdown() noexcept override { return Result<void>::success(); }
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
    bool gate_start = false;
    Gate start_gate;
    int starts = 0;
    int stops = 0;
};

class MockTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        if (fail) return Result<ReceiverToken>::failure(Error::RESOURCE_LIMIT);
        ReceiverToken token{};
        token[0U] = next++;
        return Result<ReceiverToken>::success(token);
    }
    bool fail = false;
    std::uint8_t next = 1U;
};

class MockClock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t) noexcept override {}
    std::uint64_t now = 1000U;
};

class MockStream final : public ReceiverStreamControl {
public:
    Result<void> attach(const Attachment& attachment) noexcept override
    {
        ++attaches;
        if (attach_result != Error::OK)
            return Result<void>::failure(attach_result);
        if (attached || final_valid)
            return Result<void>::failure(Error::BUSY);
        identity = attachment;
        attached = true;
        return Result<void>::success();
    }
    Result<void> detach(const Attachment& attachment) noexcept override
    {
        ++detaches;
        if (!attached || !same_identity(identity, attachment))
            return Result<void>::failure(Error::NOT_FOUND);
        attached = false;
        final_identity = identity;
        final_valid = true;
        return Result<void>::success();
    }
    Result<StreamCounters> stats(const Attachment&) const noexcept override
    {
        return Result<StreamCounters>::success(StreamCounters{});
    }
    Result<StreamFinalSnapshot> final_snapshot(const Attachment& attachment) const noexcept override
    {
        return final_valid && same_identity(final_identity, attachment)
            ? Result<StreamFinalSnapshot>::success(StreamFinalSnapshot{})
            : Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(const Attachment& attachment) noexcept override
    {
        if (!final_valid || !same_identity(final_identity, attachment))
            return Result<void>::failure(Error::NOT_FOUND);
        final_valid = false;
        ++final_releases;
        return Result<void>::success();
    }
    static bool same_identity(const Attachment& left, const Attachment& right) noexcept
    {
        return left.owner_client_id == right.owner_client_id &&
               left.lease_id == right.lease_id &&
               left.attachment_id == right.attachment_id &&
               left.receiver == right.receiver && left.system == right.system &&
               left.token == right.token;
    }
    Error attach_result = Error::OK;
    bool attached = false;
    bool final_valid = false;
    Attachment identity{};
    Attachment final_identity{};
    int attaches = 0;
    int detaches = 0;
    int final_releases = 0;
};

struct Fixture final {
    MockBackend backend;
    MockTokens tokens;
    MockClock clock;
    MockStream stream;
    std::array<ReceiverId, 1> receivers{1U};
    ReceiverEngine engine{backend, receivers.data(), receivers.size(), tokens, clock,
                          nullptr, nullptr, &stream};
};

// A real strict successful tune: the capability table is configured, the tune
// is admitted, and execution succeeds so tune_metadata current_valid and
// current_revision are actually established.  No legacy tune or revision seed
// substitutes for it.
bool acquire_and_tune(Fixture& f, std::uint64_t& lease_id, std::uint64_t& revision)
{
    const std::array<ReceiverCapability, 1> capabilities{
        ReceiverCapability{1U, true, false}};
    V1_CHECK(f.engine.configure_strict_capabilities(capabilities.data(),
                                                    capabilities.size()));
    const auto lease = f.engine.acquire(1U, 1U);
    V1_CHECK(lease);
    lease_id = lease.value().lease_id;
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 5000U;
    const auto admitted = f.engine.try_admit_tune(1U, request, 0U);
    V1_CHECK(admitted);
    V1_CHECK(f.engine.execute_admitted_tune(admitted.value()));
    revision = admitted.value().tune_revision;
    return true;
}

bool test_contract_selection()
{
    Fixture f;
    V1_CHECK(f.engine.stream_contract() == StreamContract::legacy);
    V1_CHECK(!f.engine.configure_stream_contract(StreamContract::legacy, 0U));
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    V1_CHECK(!f.engine.configure_stream_contract(StreamContract::legacy, kEpoch));
    V1_CHECK(f.engine.stream_contract() == StreamContract::v1);
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    V1_CHECK(!f.engine.configure_stream_contract(StreamContract::legacy, kEpoch));
    return true;
}

bool test_mode_mixing_rejected()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    V1_CHECK(f.engine.start_stream(1U, lease).error() == Error::UNSUPPORTED);
    V1_CHECK(f.engine.stop_stream(1U, lease).error() == Error::UNSUPPORTED);
    V1_CHECK(f.engine.revoke_stream(1U, lease).error() == Error::UNSUPPORTED);
    Fixture legacy;
    std::uint64_t llease = 0U;
    std::uint64_t lrevision = 0U;
    V1_CHECK(acquire_and_tune(legacy, llease, lrevision));
    V1_CHECK(legacy.engine.start_stream_v1(1U, llease, lrevision, 1000U, 5000U).error() ==
             Error::UNSUPPORTED);
    return true;
}

bool test_revision_and_identity_rejection()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    V1_CHECK(f.engine.start_stream_v1(1U, lease, revision + 99U, 1000U, 5000U).error() ==
             Error::INVALID_ARGUMENT);
    V1_CHECK(f.engine.start_stream_v1(1U, lease, 0U, 1000U, 5000U).error() ==
             Error::INVALID_ARGUMENT);
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    V1_CHECK(f.engine.reserve_attach_v1(1U, lease, kEpoch, started.value().stream_id,
                                        ReceiverToken{}, 1000U)
                 .error() == Error::NOT_FOUND);
    V1_CHECK(f.engine.reserve_attach_v1(1U, lease, kEpoch + 1U, started.value().stream_id,
                                        started.value().token, 1000U)
                 .error() == Error::INVALID_ARGUMENT);
    V1_CHECK(f.engine.reserve_attach_v1(1U, lease, kEpoch, started.value().stream_id + 1U,
                                        started.value().token, 1000U)
                 .error() == Error::NOT_FOUND);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    V1_CHECK(f.engine.reserve_attach_v1(1U, lease, kEpoch, started.value().stream_id,
                                        started.value().token, 1000U)
                 .error() == Error::NOT_FOUND);
    return true;
}

// START must reject an admitted revision that never tuned successfully, and a
// revoked current tune, until a fresh successful tune exists.
bool test_start_requires_current_success()
{
    const std::array<ReceiverCapability, 1> capabilities{
        ReceiverCapability{1U, true, false}};

    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    V1_CHECK(f.engine.configure_strict_capabilities(capabilities.data(),
                                                    capabilities.size()));
    const auto lease = f.engine.acquire(1U, 1U);
    V1_CHECK(lease);
    const std::uint64_t id = lease.value().lease_id;
    TuneRequest request{};
    request.lease_id = id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 5000U;
    const auto success = f.engine.try_admit_tune(1U, request, 0U);
    V1_CHECK(success);
    V1_CHECK(f.engine.execute_admitted_tune(success.value()));
    const std::uint64_t good = success.value().tune_revision;
    // Admit a second revision but abort it without executing: it never became
    // the successful current tune, and current is now invalid.
    const auto aborted = f.engine.try_admit_tune(1U, request, 0U);
    V1_CHECK(aborted);
    V1_CHECK(f.engine.abort_unstarted_tune(aborted.value(), Error::TIMEOUT));
    V1_CHECK(f.engine.start_stream_v1(1U, id, aborted.value().tune_revision, 1000U, 5000U)
                 .error() == Error::INVALID_ARGUMENT);
    V1_CHECK(f.engine.start_stream_v1(1U, id, good, 1000U, 5000U).error() ==
             Error::INVALID_ARGUMENT);
    // A fresh successful tune re-establishes current success.
    const auto fresh = f.engine.try_admit_tune(1U, request, 0U);
    V1_CHECK(fresh);
    V1_CHECK(f.engine.execute_admitted_tune(fresh.value()));
    V1_CHECK(f.engine.start_stream_v1(1U, id, fresh.value().tune_revision, 1000U, 5000U));

    // revoke_tune_ticket invalidates current while the counter is unchanged.
    Fixture g;
    V1_CHECK(g.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    V1_CHECK(g.engine.configure_strict_capabilities(capabilities.data(),
                                                    capabilities.size()));
    const auto glelease = g.engine.acquire(1U, 1U);
    V1_CHECK(glelease);
    const std::uint64_t gid = glelease.value().lease_id;
    TuneRequest grequest{};
    grequest.lease_id = gid;
    grequest.system = ReceiverSystem::terrestrial;
    grequest.frequency_khz = 557000U;
    grequest.timeout_ms = 5000U;
    const auto gsuccess = g.engine.try_admit_tune(1U, grequest, 0U);
    V1_CHECK(gsuccess);
    V1_CHECK(g.engine.execute_admitted_tune(gsuccess.value()));
    V1_CHECK(g.engine.revoke_tune_ticket(1U, gid));
    V1_CHECK(g.engine.start_stream_v1(1U, gid, gsuccess.value().tune_revision, 1000U, 5000U)
                 .error() == Error::INVALID_ARGUMENT);
    return true;
}

bool test_prepare_required_and_single_prepare()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    // Commit before prepare is refused and publishes nothing.
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()).error() == Error::NOT_FOUND);
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::attaching));
    V1_CHECK(f.backend.starts == 0);
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()));
    V1_CHECK(f.backend.starts == 1);
    // A second prepare for the same ticket is refused and does not start again.
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()).error() == Error::BUSY);
    V1_CHECK(f.backend.starts == 1);
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()));
    status = f.engine.status();
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::active));
    return true;
}

bool test_commit_only_after_prepare_ack()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()));
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()));
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()).error() == Error::NOT_FOUND);
    V1_CHECK(f.engine.abort_attach_v1(reserved.value(), Error::OK).error() ==
             Error::NOT_FOUND);
    return true;
}

// An explicitly unquiesced (OK,false) completion must not release the scope.
bool test_unquiesced_completion_retains_scope()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()));
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()));
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    // The cleanup succeeded but is explicitly unquiesced: the scope stays
    // excluded and ownership is retained.
    V1_CHECK(f.engine.complete_stream_cleanup_v1(reserved.value(), Error::OK, false));
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::closing));
    V1_CHECK(f.engine.start_stream_v1(1U, lease, revision, 2000U, 5000U).error() !=
             Error::OK);
    // The legacy release must not silently detach the still-pending v1 stream.
    V1_CHECK(f.engine.release(1U, lease).error() == Error::NOT_READY);
    // A late (OK,true) records quiescence and finalizes the retained outcome.
    V1_CHECK(f.engine.complete_stream_cleanup_v1(reserved.value(), Error::OK, true));
    status = f.engine.status();
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::none));
    V1_CHECK(f.engine.start_stream_v1(1U, lease, revision, 3000U, 5000U));
    return true;
}

// A recorded non-OK primary error is never cleared by a late (OK,true).
bool test_primary_error_preserved()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()));
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()));
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    V1_CHECK(f.engine.complete_stream_cleanup_v1(reserved.value(), Error::USB_IO, false));
    V1_CHECK(f.engine.complete_stream_cleanup_v1(reserved.value(), Error::OK, true));
    const auto stop = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    V1_CHECK(stop);
    // The primary error is preserved, but the late physically quiesced
    // observation is updated independently.
    V1_CHECK(stop.value().cleanup_error == Error::USB_IO);
    V1_CHECK(stop.value().quiesced);
    auto status = f.engine.status();
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::closing));
    return true;
}

// closing scope rejects both TUNE entrances before any revision/current change.
bool test_closing_excludes_tune()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    TuneRequest request{};
    request.lease_id = lease;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 5000U;
    // Admitted entrance is BUSY while the cleanup is pending.
    V1_CHECK(f.engine.try_admit_tune(1U, request, 0U).error() == Error::BUSY);
    // Direct entrance is refused too.
    V1_CHECK(f.engine.tune(1U, request).error() == Error::BUSY);
    // The retained v1 stream identity and result are unchanged.
    const auto stop = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    V1_CHECK(stop);
    return true;
}

bool test_old_a_to_new_b_isolation()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto a = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(a);
    const auto ar = f.engine.reserve_attach_v1(1U, lease, kEpoch, a.value().stream_id,
                                               a.value().token, 1000U);
    V1_CHECK(ar);
    V1_CHECK(f.engine.prepare_attach_v1(ar.value()));
    V1_CHECK(f.engine.commit_attach_v1(ar.value()));
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, a.value().stream_id));
    V1_CHECK(f.engine.complete_stream_cleanup_v1(ar.value(), Error::OK, true));
    const auto b = f.engine.start_stream_v1(1U, lease, revision, 2000U, 5000U);
    V1_CHECK(b);
    V1_CHECK(b.value().stream_id != a.value().stream_id);
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, a.value().stream_id).error() ==
             Error::NOT_FOUND);
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::armed));
    V1_CHECK(f.engine.commit_attach_v1(ar.value()).error() == Error::NOT_FOUND);
    V1_CHECK(f.engine.abort_attach_v1(ar.value(), Error::OK).error() == Error::NOT_FOUND);
    return true;
}

bool test_armed_expiry_and_exhaustion()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    V1_CHECK(f.engine.reserve_attach_v1(1U, lease, kEpoch, started.value().stream_id,
                                        started.value().token, 1000U + 5000U)
                 .error() == Error::TIMEOUT);
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::none));
    V1_CHECK(f.engine.start_stream_v1(1U, lease, revision, 7000U, 5000U));

    Fixture g;
    V1_CHECK(g.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t glease = 0U;
    std::uint64_t grevision = 0U;
    V1_CHECK(acquire_and_tune(g, glease, grevision));
    g.engine.set_stream_issuance_for_test(std::numeric_limits<std::uint64_t>::max(), 1U);
    V1_CHECK(g.engine.start_stream_v1(1U, glease, grevision, 1000U, 5000U).error() ==
             Error::RESOURCE_LIMIT);
    g.engine.set_stream_issuance_for_test(1U, std::numeric_limits<std::uint64_t>::max());
    V1_CHECK(g.engine.start_stream_v1(1U, glease, grevision, 1000U, 5000U).error() ==
             Error::RESOURCE_LIMIT);
    g.engine.set_stream_issuance_for_test(1U, 1U);
    g.tokens.fail = true;
    V1_CHECK(g.engine.start_stream_v1(1U, glease, grevision, 1000U, 5000U).error() ==
             Error::RESOURCE_LIMIT);
    return true;
}

bool test_prepare_failure_closes_stream()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    f.backend.start_result = Error::USB_IO;
    V1_CHECK(f.engine.prepare_attach_v1(reserved.value()).error() == Error::USB_IO);
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::closing));
    // A failed prepare retained its primary error; a later (OK,true) does not
    // clear it.
    V1_CHECK(f.engine.complete_stream_cleanup_v1(reserved.value(), Error::OK, true));
    const auto stop = f.engine.stop_stream_v1(1U, lease, started.value().stream_id);
    V1_CHECK(stop);
    V1_CHECK(stop.value().cleanup_error == Error::USB_IO);
    status = f.engine.status();
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::closing));
    return true;
}

// STOP wins while a gated prepare is physically starting.  Ownership and the
// one cleanup responsibility must survive; release must not free the scope.
bool test_stop_during_gated_prepare_retains_ownership()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto reserved = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                     started.value().stream_id,
                                                     started.value().token, 1000U);
    V1_CHECK(reserved);
    f.backend.gate_start = true;
    Error prepare_error = Error::OK;
    std::thread worker([&] {
        const auto prepared = f.engine.prepare_attach_v1(reserved.value());
        prepare_error = prepared ? Error::OK : prepared.error();
    });
    // Declaration order gives failure-safe teardown: open before join.
    Joiner joiner(worker);
    GateRelease release(f.backend.start_gate);
    // Require the deterministic entered handshake, not a sleep.
    V1_CHECK(f.backend.start_gate.wait_entered());
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    f.backend.start_gate.open();
    worker.join();
    // The late prepare did not publish active and reported stale.
    V1_CHECK(prepare_error == Error::NOT_FOUND);
    auto status = f.engine.status();
    V1_CHECK(status);
    V1_CHECK(status.value().stream_phases[0U] ==
             static_cast<std::uint8_t>(StreamPhase::closing));
    // The physical ownership is retained and the legacy release refuses.
    V1_CHECK(f.engine.release(1U, lease).error() == Error::NOT_READY);
    // A commit on the stale ticket cannot resurrect the stream.
    V1_CHECK(f.engine.commit_attach_v1(reserved.value()).error() == Error::NOT_FOUND);
    // The exact-ticket cleanup performs the real stop/detach once.
    V1_CHECK(f.engine.run_stream_cleanup_v1(reserved.value()));
    V1_CHECK(f.backend.stops == 1);
    V1_CHECK(f.stream.detaches == 1);
    V1_CHECK(f.engine.release(1U, lease));
    return true;
}

bool test_released_lease_retains_exact_stream_final()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto ticket = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                   started.value().stream_id,
                                                   started.value().token, 1000U);
    V1_CHECK(ticket);
    V1_CHECK(f.engine.prepare_attach_v1(ticket.value()));
    V1_CHECK(f.engine.abort_attach_v1(ticket.value(), Error::OK));
    V1_CHECK(f.engine.run_stream_cleanup_v1(ticket.value()));
    V1_CHECK(f.engine.release(1U, lease));

    auto wrong = ticket.value();
    ++wrong.revision;
    V1_CHECK(f.engine.release_stream_final_v1(wrong).error() == Error::NOT_FOUND);
    V1_CHECK(f.stream.final_releases == 0);
    V1_CHECK(f.engine.release_stream_final_v1(ticket.value()));
    V1_CHECK(f.stream.final_releases == 1);
    V1_CHECK(f.engine.release_stream_final_v1(ticket.value()).error() == Error::NOT_FOUND);
    V1_CHECK(f.stream.final_releases == 1);
    return true;
}

bool test_released_final_is_not_retained_twice()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t lease = 0U;
    std::uint64_t revision = 0U;
    V1_CHECK(acquire_and_tune(f, lease, revision));
    const auto started = f.engine.start_stream_v1(1U, lease, revision, 1000U, 5000U);
    V1_CHECK(started);
    const auto ticket = f.engine.reserve_attach_v1(1U, lease, kEpoch,
                                                   started.value().stream_id,
                                                   started.value().token, 1000U);
    V1_CHECK(ticket);
    V1_CHECK(f.engine.prepare_attach_v1(ticket.value()));
    V1_CHECK(f.engine.commit_attach_v1(ticket.value()));
    V1_CHECK(f.engine.stop_stream_v1(1U, lease, started.value().stream_id));
    V1_CHECK(f.engine.run_stream_cleanup_v1(ticket.value()));
    V1_CHECK(f.engine.release_stream_final_v1(ticket.value()));
    V1_CHECK(f.engine.release(1U, lease));
    V1_CHECK(f.engine.release_stream_final_v1(ticket.value()).error() == Error::NOT_FOUND);
    V1_CHECK(f.stream.final_releases == 1);
    return true;
}

bool test_phantom_final_does_not_block_next_physical_final()
{
    Fixture f;
    V1_CHECK(f.engine.configure_stream_contract(StreamContract::v1, kEpoch));
    std::uint64_t first_lease = 0U;
    std::uint64_t first_revision = 0U;
    V1_CHECK(acquire_and_tune(f, first_lease, first_revision));
    const auto first_start = f.engine.start_stream_v1(
        1U, first_lease, first_revision, 1000U, 5000U);
    V1_CHECK(first_start);
    const auto first_ticket = f.engine.reserve_attach_v1(
        1U, first_lease, kEpoch, first_start.value().stream_id,
        first_start.value().token, 1000U);
    V1_CHECK(first_ticket);
    V1_CHECK(f.engine.abort_attach_v1(first_ticket.value(), Error::OK));
    V1_CHECK(f.engine.run_stream_cleanup_v1(first_ticket.value()));
    V1_CHECK(f.engine.release_stream_final_v1(first_ticket.value()).error() ==
             Error::NOT_FOUND);
    V1_CHECK(f.engine.release(1U, first_lease));
    V1_CHECK(f.stream.final_releases == 0);
    V1_CHECK(f.engine.release_stream_final_v1(first_ticket.value()).error() ==
             Error::NOT_FOUND);

    std::uint64_t second_lease = 0U;
    std::uint64_t second_revision = 0U;
    V1_CHECK(acquire_and_tune(f, second_lease, second_revision));
    const auto second_start = f.engine.start_stream_v1(
        1U, second_lease, second_revision, 1000U, 5000U);
    V1_CHECK(second_start);
    const auto second_ticket = f.engine.reserve_attach_v1(
        1U, second_lease, kEpoch, second_start.value().stream_id,
        second_start.value().token, 1000U);
    V1_CHECK(second_ticket);
    V1_CHECK(f.engine.prepare_attach_v1(second_ticket.value()));
    V1_CHECK(f.engine.commit_attach_v1(second_ticket.value()));
    V1_CHECK(f.engine.stop_stream_v1(1U, second_lease, second_start.value().stream_id));
    V1_CHECK(f.engine.run_stream_cleanup_v1(second_ticket.value()));
    V1_CHECK(f.engine.release(1U, second_lease));

    std::uint64_t third_lease = 0U;
    std::uint64_t third_revision = 0U;
    V1_CHECK(acquire_and_tune(f, third_lease, third_revision));
    const auto third_start = f.engine.start_stream_v1(
        1U, third_lease, third_revision, 1000U, 5000U);
    V1_CHECK(third_start);
    const auto third_ticket = f.engine.reserve_attach_v1(
        1U, third_lease, kEpoch, third_start.value().stream_id,
        third_start.value().token, 1000U);
    V1_CHECK(!third_ticket && third_ticket.error() == Error::BUSY);
    V1_CHECK(f.engine.expire_armed_v1(1U, third_lease,
        third_start.value().stream_id, 6000U));
    V1_CHECK(f.engine.release(1U, third_lease));
    V1_CHECK(f.engine.release_stream_final_v1(second_ticket.value()));
    V1_CHECK(f.stream.final_releases == 1);
    V1_CHECK(f.engine.release_stream_final_v1(second_ticket.value()).error() ==
             Error::NOT_FOUND);
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_contract_selection() && test_mode_mixing_rejected() &&
                    test_revision_and_identity_rejection() &&
                    test_start_requires_current_success() &&
                    test_prepare_required_and_single_prepare() &&
                    test_commit_only_after_prepare_ack() &&
                    test_unquiesced_completion_retains_scope() &&
                    test_primary_error_preserved() && test_closing_excludes_tune() &&
                    test_old_a_to_new_b_isolation() &&
                    test_armed_expiry_and_exhaustion() &&
                    test_prepare_failure_closes_stream() &&
                    test_stop_during_gated_prepare_retains_ownership() &&
                    test_released_lease_retains_exact_stream_final() &&
                    test_released_final_is_not_retained_twice() &&
                    test_phantom_final_does_not_block_next_physical_final();
    if (!ok) return 1;
    std::printf("stream v1 authority tests: PASS\n");
    return 0;
}
