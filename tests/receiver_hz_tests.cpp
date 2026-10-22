// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define HZ_CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "receiver Hz check failed at line %d: %s\n", \
                         __LINE__, #condition); \
            return false; \
        } \
    } while (false)

class Backend final : public ReceiverBackend {
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
    Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t frequency_khz,
                      std::uint32_t) noexcept override
    {
        ++khz_calls;
        last_khz = frequency_khz;
        return Result<void>::success();
    }
    Result<TuneProgramming> program_hz(ReceiverId, ReceiverSystem,
                                       std::uint64_t requested_hz,
                                       std::uint32_t) noexcept override
    {
        ++hz_calls;
        last_hz = requested_hz;
        if (program_error != Error::OK)
            return Result<TuneProgramming>::failure(program_error);
        TuneProgramming programming{};
        programming.programmed_hz = programmed_hz;
        programming.lock = lock;
        programming.ts_synchronized = ts_synchronized;
        return Result<TuneProgramming>::success(programming);
    }
    Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(ReceiverId, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_tsid(ReceiverId, std::uint16_t,
                                       std::uint32_t) noexcept override
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
        return Result<void>::success();
    }
    Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }

    std::uint64_t last_hz = 0U;
    std::uint32_t last_khz = 0U;
    int hz_calls = 0;
    int khz_calls = 0;
    Error program_error = Error::OK;
    std::optional<std::uint64_t> programmed_hz = 473143000U;
    wire::Measurement<bool> lock{};
    bool ts_synchronized = true;
};

class Tokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        return Result<ReceiverToken>::success(ReceiverToken{1U});
    }
};

class Clock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now += milliseconds; }
    std::uint64_t now = 10U;
};

bool test_exact_hz_and_legacy_khz_paths()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, true}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);

    HzTuneRequest hz{};
    hz.lease_id = lease.value().lease_id;
    hz.requested_hz = 473142857U;
    hz.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(1U, hz, clock.now);
    HZ_CHECK(ticket && ticket.value().exact_hz &&
             ticket.value().requested_hz == hz.requested_hz);
    HZ_CHECK(engine.execute_admitted_tune(ticket.value()));
    HZ_CHECK(backend.hz_calls == 1 && backend.last_hz == hz.requested_hz &&
             backend.khz_calls == 0);
    auto status = engine.status();
    HZ_CHECK(status && status.value().tune_metadata[0U].current_valid);
    const TuneRecord& current = status.value().tune_metadata[0U].current;
    HZ_CHECK(current.exact_hz && current.requested_hz == hz.requested_hz &&
             current.programmed_hz == backend.programmed_hz);

    TuneRequest khz{};
    khz.lease_id = lease.value().lease_id;
    khz.frequency_khz = 473142U;
    khz.timeout_ms = 1000U;
    const auto old_ticket = engine.try_admit_tune(1U, khz, clock.now);
    HZ_CHECK(old_ticket && !old_ticket.value().exact_hz);
    HZ_CHECK(engine.execute_admitted_tune(old_ticket.value()));
    HZ_CHECK(backend.hz_calls == 1 && backend.khz_calls == 1 &&
             backend.last_khz == khz.frequency_khz);
    return true;
}

bool test_unsupported_hz_does_not_advance_revision()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, false}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);

    HzTuneRequest request{};
    request.lease_id = lease.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(!ticket && ticket.error() == Error::UNSUPPORTED);
    const auto status = engine.status();
    HZ_CHECK(status && status.value().tune_metadata[0U].revision == 0U &&
             !status.value().tune_metadata[0U].current_valid &&
             backend.hz_calls == 0 && backend.khz_calls == 0);
    return true;
}

bool test_programming_failure_keeps_current_invalid()
{
    Backend backend;
    backend.program_error = Error::NOT_READY;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, true}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);

    HzTuneRequest request{};
    request.lease_id = lease.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(ticket && !engine.execute_admitted_tune(ticket.value()));
    const auto status = engine.status();
    HZ_CHECK(status && status.value().tune_metadata[0U].revision == 1U &&
             !status.value().tune_metadata[0U].current_valid &&
             !status.value().tune_metadata[0U].last_successful_valid &&
             backend.hz_calls == 1 && backend.khz_calls == 0);
    return true;
}

bool test_ticket_keeps_admitted_hz()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, true}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);

    HzTuneRequest request{};
    request.lease_id = lease.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto admitted = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(admitted);
    AdmittedTune altered = admitted.value();
    ++altered.requested_hz;
    HZ_CHECK(!engine.execute_admitted_tune(altered));
    const auto status = engine.status();
    HZ_CHECK(status && status.value().tune_metadata[0U].revision == 1U &&
             !status.value().tune_metadata[0U].current_valid &&
             backend.hz_calls == 0);
    return true;
}

bool test_terminal_lock_requires_valid_positive_evidence()
{
    Backend backend;
    backend.lock.state = wire::MeasureState::valid;
    backend.lock.value = true;
    backend.lock.sampled_at_ns = 123000000U;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, true, TuneReadiness::terminal_lock, true}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);
    HzTuneRequest request{};
    request.lease_id = lease.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(ticket && engine.execute_admitted_tune(ticket.value()));
    auto status = engine.status();
    HZ_CHECK(status && status.value().tune_metadata[0U].current.verification ==
                          wire::Verification::lock &&
             status.value().tune_metadata[0U].current.lock.state ==
                          wire::MeasureState::valid &&
             status.value().tune_metadata[0U].current.lock.value);

    backend.lock = wire::Measurement<bool>{};
    const auto second = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(second);
    const auto missing = engine.execute_admitted_tune(second.value());
    HZ_CHECK(!missing && missing.error() == Error::PROTOCOL_ERROR);
    return true;
}

bool test_commands_and_ts_sync_readiness()
{
    for (const TuneReadiness readiness : {TuneReadiness::commands, TuneReadiness::ts_sync}) {
        Backend backend;
        backend.lock.state = wire::MeasureState::valid;
        backend.lock.value = false;
        backend.lock.sampled_at_ns = 456000000U;
        Tokens tokens;
        Clock clock;
        const std::array<ReceiverId, 1> receivers{{17U}};
        ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
        const std::array<ReceiverCapability, 1> capabilities{{
            ReceiverCapability{17U, true, false, true, readiness, false}}};
        HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
        const auto lease = engine.acquire(1U, 17U);
        HZ_CHECK(lease);
        HzTuneRequest request{};
        request.lease_id = lease.value().lease_id;
        request.requested_hz = 473142857U;
        request.timeout_ms = 1000U;
        const auto ticket = engine.try_admit_tune(1U, request, clock.now);
        HZ_CHECK(ticket);
        const auto tuned = engine.execute_admitted_tune(ticket.value());
        HZ_CHECK(tuned);
        if (tuned) {
            const auto status = engine.status();
            HZ_CHECK(status && status.value().tune_metadata[0U].current.verification ==
                                  (readiness == TuneReadiness::commands
                                       ? wire::Verification::commands
                                       : wire::Verification::ts_sync));
            if (readiness == TuneReadiness::commands)
                HZ_CHECK(status.value().tune_metadata[0U].current.lock.state ==
                         wire::MeasureState::unsupported);
            else
                HZ_CHECK(status.value().tune_metadata[0U].current.lock.state ==
                             wire::MeasureState::valid &&
                         !status.value().tune_metadata[0U].current.lock.value);
        }
    }
    return true;
}

bool test_negative_terminal_lock_is_not_ready()
{
    Backend backend;
    backend.lock.state = wire::MeasureState::valid;
    backend.lock.value = false;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{{17U}};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> capabilities{{
        ReceiverCapability{17U, true, false, true, TuneReadiness::terminal_lock, true}}};
    HZ_CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(1U, 17U);
    HZ_CHECK(lease);
    HzTuneRequest request{};
    request.lease_id = lease.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(1U, request, clock.now);
    HZ_CHECK(ticket);
    const auto tuned = engine.execute_admitted_tune(ticket.value());
    HZ_CHECK(!tuned && tuned.error() == Error::TIMEOUT);
    return true;
}

}  // namespace

int main()
{
    return test_exact_hz_and_legacy_khz_paths() &&
                   test_unsupported_hz_does_not_advance_revision() &&
                   test_programming_failure_keeps_current_invalid() &&
                   test_ticket_keeps_admitted_hz() &&
                   test_terminal_lock_requires_valid_positive_evidence() &&
                   test_commands_and_ts_sync_readiness() &&
                   test_negative_terminal_lock_is_not_ready()
        ? 0
        : 1;
}
