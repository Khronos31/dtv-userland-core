// SPDX-License-Identifier: MIT
#include "dtv/userland/control_connection.h"
#include "dtv/userland/detail/status_projection.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::detail;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition) \
    do { \
        ++g_checks; \
        if (!(condition)) { \
            ++g_failures; \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #condition); \
        } \
    } while (0)

class Backend final : public ReceiverBackend {
public:
    bool supports(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial;
    }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override { return 0U; }
    Result<void> open(ReceiverId) noexcept override { return Result<void>::success(); }
    Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t,
                      std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<TuneProgramming> program_hz(ReceiverId, ReceiverSystem, std::uint64_t requested_hz,
                                       std::uint32_t) noexcept override
    {
        if (program_error != Error::OK) {
            return Result<TuneProgramming>::failure(program_error);
        }
        return Result<TuneProgramming>::success(TuneProgramming{requested_hz + 7U});
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
        return rollback_error == Error::OK ? Result<void>::success()
                                           : Result<void>::failure(rollback_error);
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

    Error program_error = Error::OK;
    Error rollback_error = Error::OK;
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

template <std::size_t N>
w::WString<N> make_text(const char* text)
{
    w::WString<N> out;
    while (text[out.length] != '\0') {
        out.bytes[out.length] = text[out.length];
        ++out.length;
    }
    return out;
}

w::ListResponse make_list()
{
    w::ListResponse list;
    for (std::size_t i = 0U; i < list.epoch.size(); ++i) {
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    }
    list.producer.product = make_text<32>("projection-test");
    list.producer.version = make_text<64>("1");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    w::UsbFunction& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    list.enclosure.value.model.value.name = make_text<128>("PX-Q3U4");
    list.receivers.present = true;
    list.receivers.value.count = 2U;
    for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
        w::ReceiverDescriptor& descriptor = list.receivers.value.items[i];
        descriptor.id = i == 0U ? 18U : 17U;
        descriptor.systems.mask = 0x01U;
        descriptor.binding.present = true;
        descriptor.binding.value.function_id = 1U;
        descriptor.binding.value.local_id = static_cast<std::uint16_t>(i);
        descriptor.capabilities.measurement_mask = w::kMetricMaskAll;
    }
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 4U;
    return list;
}

bool make_shell(const w::ListResponse& list, w::StatusResponse& status,
                w::Scope scope = w::Scope{w::ScopeKind::enclosure, 0U})
{
    w::Context context;
    context.epoch = list.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    snapshot.sequence = 4U;
    snapshot.observed_at_ns = 100U;
    return w::build_status_shell(list, context, snapshot, scope, status) == Error::OK;
}

bool test_engine_projection_and_validation()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 2> receiver_ids{{17U, 18U}};
    ReceiverEngine engine(backend, receiver_ids.data(), receiver_ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 2> capabilities{{
        ReceiverCapability{17U, true, false, true},
        ReceiverCapability{18U, true, false, true},
    }};
    CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto lease = engine.acquire(9U, 17U);
    CHECK(lease);

    HzTuneRequest tune;
    tune.lease_id = lease.value().lease_id;
    tune.requested_hz = 473142857U;
    tune.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(9U, tune, clock.now);
    CHECK(ticket && engine.execute_admitted_tune(ticket.value()));

    const w::ListResponse list = make_list();
    CHECK(w::validate_list_response(list) == Error::OK);
    w::StatusResponse status;
    CHECK(make_shell(list, status));
    status.receivers.value.items[0U].present = false;
    status.receivers.value.items[1U].present = true;
    status.lnb_15v_enabled.present = true;
    status.lnb_15v_enabled.value = false;
    status.card_inserted.present = true;
    status.card_inserted.value = true;

    const auto engine_status = engine.status();
    CHECK(engine_status);
    CHECK(project_receiver_engine_status(list, engine_status.value(), status) == Error::OK);
    CHECK(status.receivers.value.items[0U].descriptor.id == 18U);
    CHECK(status.receivers.value.items[0U].state == w::ReceiverState::free);
    CHECK(!status.receivers.value.items[0U].present);
    CHECK(!status.receivers.value.items[0U].leased);
    const w::ReceiverStatus& projected = status.receivers.value.items[1U];
    CHECK(projected.descriptor.id == 17U && projected.present);
    CHECK(projected.state == w::ReceiverState::tuned && projected.leased);
    CHECK(projected.tune_revision == 1U && projected.current_tune.present);
    CHECK(projected.current_tune.value.frequency_hz == tune.requested_hz);
    CHECK(projected.current_tune.value.programmed_hz.present &&
          projected.current_tune.value.programmed_hz.value == tune.requested_hz + 7U);
    CHECK(projected.last_successful_tune.present);
    CHECK(projected.measurements.entries.count == 3U);
    CHECK(projected.measurements.entries.items[0U].boolean.state == w::MeasureState::unknown);
    CHECK(projected.measurements.entries.items[1U].integer.state == w::MeasureState::unknown);
    CHECK(status.lnb_15v_enabled.present && !status.lnb_15v_enabled.value);
    CHECK(status.card_inserted.present && status.card_inserted.value);
    CHECK(status.enclosure.present && status.enclosure.value.model.present &&
          status.enclosure.value.model.value.name.length == 7U);
    w::StatusContext context;
    context.list = &list;
    CHECK(w::validate_status_response(status, context) == Error::OK);

    std::unique_ptr<std::array<std::uint8_t, w::kMaxControlPayload>> encoded(
        new std::array<std::uint8_t, w::kMaxControlPayload>());
    CHECK(w::encode_status_response(status, MutableByteView{encoded->data(), encoded->size()}));

    CHECK(make_shell(list, status, w::Scope{w::ScopeKind::receiver, 17U}));
    status.receivers.value.items[0U].present = true;
    const auto receiver_engine_status = engine.status();
    CHECK(receiver_engine_status);
    CHECK(project_receiver_engine_status(list, receiver_engine_status.value(), status) ==
          Error::OK);
    CHECK(status.receivers.value.count == 1U &&
          status.receivers.value.items[0U].descriptor.id == 17U);
    CHECK(!status.lnb_15v_enabled.present && !status.card_inserted.present);

    CHECK(make_shell(list, status, w::Scope{w::ScopeKind::reader, 4U}));
    CHECK(status.readers.present && status.readers.value.count == 1U &&
          status.readers.value.items[0U].descriptor.id == 4U);
    CHECK(project_receiver_engine_status(list, receiver_engine_status.value(), status) ==
          Error::OK);
    CHECK(status.readers.value.items[0U].card.state == w::CardState::unknown);

    backend.program_error = Error::TIMEOUT;
    backend.rollback_error = Error::USB_IO;
    tune.requested_hz += 1000000U;
    const auto failed_ticket = engine.try_admit_tune(9U, tune, clock.now);
    CHECK(failed_ticket && !engine.execute_admitted_tune(failed_ticket.value()));
    CHECK(make_shell(list, status));
    status.receivers.value.items[0U].present = true;
    status.receivers.value.items[1U].present = true;
    const auto failed_engine_status = engine.status();
    CHECK(failed_engine_status);
    CHECK(project_receiver_engine_status(list, failed_engine_status.value(), status) == Error::OK);
    CHECK(status.receivers.value.items[1U].state == w::ReceiverState::error);
    CHECK(status.receivers.value.items[1U].leased);
    CHECK(!status.receivers.value.items[1U].current_tune.present);
    CHECK(status.receivers.value.items[1U].last_successful_tune.present);

    ReceiverEngineStatus missing;
    missing.receiver_count = 1U;
    missing.receivers[0U] = 88U;
    CHECK(make_shell(list, status));
    status.receivers.value.items[0U].present = true;
    status.receivers.value.items[1U].present = true;
    CHECK(project_receiver_engine_status(list, missing, status) == Error::NOT_FOUND);

    CHECK(make_shell(list, status));
    status.receivers.value.items[0U].present = true;
    status.receivers.value.items[1U].present = true;
    status.receivers.value.items[0U].descriptor.systems.mask = 0x03U;
    CHECK(project_receiver_engine_status(list, engine_status.value(), status) ==
          Error::INVALID_ARGUMENT);

    CHECK(make_shell(list, status, w::Scope{w::ScopeKind::receiver, 500U}) == false);
    return true;
}

}  // namespace

int main()
{
    test_engine_projection_and_validation();
    std::printf("status_projection_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
