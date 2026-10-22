// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/status_projection.h"

#include "dtv/userland/control_connection.h"

namespace dtv::userland::detail {
namespace {

wire::ReceiverState project_state(ReceiverState state) noexcept
{
    switch (state) {
    case ReceiverState::free: return wire::ReceiverState::free;
    case ReceiverState::leased: return wire::ReceiverState::leased;
    case ReceiverState::tuned: return wire::ReceiverState::tuned;
    case ReceiverState::streaming: return wire::ReceiverState::streaming;
    case ReceiverState::error: return wire::ReceiverState::error;
    }
    return wire::ReceiverState::error;
}

wire::Operation project_operation(ReceiverTuneOperation operation) noexcept
{
    switch (operation) {
    case ReceiverTuneOperation::idle: return wire::Operation::idle;
    case ReceiverTuneOperation::tuning: return wire::Operation::tuning;
    case ReceiverTuneOperation::cleanup: return wire::Operation::releasing;
    }
    return wire::Operation::idle;
}

wire::System project_system(ReceiverSystem system) noexcept
{
    return system == ReceiverSystem::satellite ? wire::System::isdb_s : wire::System::isdb_t;
}

bool valid_state(ReceiverState state) noexcept
{
    return state == ReceiverState::free || state == ReceiverState::leased ||
           state == ReceiverState::tuned || state == ReceiverState::streaming ||
           state == ReceiverState::error;
}

bool valid_operation(ReceiverTuneOperation operation) noexcept
{
    return operation == ReceiverTuneOperation::idle ||
           operation == ReceiverTuneOperation::tuning ||
           operation == ReceiverTuneOperation::cleanup;
}

bool valid_tune_system(ReceiverSystem system) noexcept
{
    return system == ReceiverSystem::terrestrial || system == ReceiverSystem::satellite;
}

wire::Tune project_tune(const TuneRecord& source, std::uint64_t revision) noexcept
{
    wire::Tune tune;
    tune.revision = revision;
    tune.system = project_system(source.system);
    tune.frequency_hz = source.exact_hz
                            ? source.requested_hz
                            : static_cast<std::uint64_t>(source.frequency_khz) * 1000U;
    tune.programmed_hz.present = source.programmed_hz.has_value();
    if (source.programmed_hz.has_value()) {
        tune.programmed_hz.value = *source.programmed_hz;
    }
    if (source.stream_id != 0xffffU) {
        tune.selection = wire::Selection::tsid;
        tune.selection_value = source.stream_id;
    } else if (source.slot != 0xffffU) {
        tune.selection = wire::Selection::slot;
        tune.selection_value = source.slot;
    }
    tune.verification = wire::Verification::commands;
    return tune;
}

void set_unobserved_metrics(wire::ReceiverStatus& receiver) noexcept
{
    if (receiver.measurements.entries.count != 0U) {
        return;
    }
    const std::uint8_t mask = receiver.descriptor.capabilities.measurement_mask;
    if ((mask & wire::kMetricMaskLock) != 0U) {
        wire::MetricEntry& entry = receiver.measurements.entries.items[
            receiver.measurements.entries.count++];
        entry.id = wire::kMetricIdLock;
        entry.boolean.state = wire::MeasureState::unknown;
        entry.boolean.reason = wire::UnknownReason::not_sampled;
    }
    if ((mask & wire::kMetricMaskCnrMdb) != 0U) {
        wire::MetricEntry& entry = receiver.measurements.entries.items[
            receiver.measurements.entries.count++];
        entry.id = wire::kMetricIdCnrMdb;
        entry.integer.state = wire::MeasureState::unknown;
        entry.integer.reason = wire::UnknownReason::not_sampled;
    }
    if ((mask & wire::kMetricMaskSignalMdbm) != 0U) {
        wire::MetricEntry& entry = receiver.measurements.entries.items[
            receiver.measurements.entries.count++];
        entry.id = wire::kMetricIdSignalMdbm;
        entry.integer.state = wire::MeasureState::unknown;
        entry.integer.reason = wire::UnknownReason::not_sampled;
    }
}

}  // namespace

Error project_receiver_engine_status(const wire::ListResponse& canonical,
                                     const ReceiverEngineStatus& engine,
                                     wire::StatusResponse& status) noexcept
{
    if (engine.receiver_count > kReceiverEngineCapacity ||
        wire::validate_list_response(canonical) != Error::OK ||
        wire::validate_status_response_scope(status) != Error::OK) {
        return Error::INVALID_ARGUMENT;
    }
    for (std::size_t i = 0U; i < engine.receiver_count; ++i) {
        if (!valid_state(engine.states[i]) || !valid_operation(engine.operations[i])) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t j = 0U; j < i; ++j) {
            if (engine.receivers[i] == engine.receivers[j]) {
                return Error::INVALID_ARGUMENT;
            }
        }
    }

    if (status.receivers.present) {
        for (std::size_t i = 0U; i < status.receivers.value.count; ++i) {
            wire::ReceiverStatus& receiver = status.receivers.value.items[i];
            std::size_t slot = 0U;
            while (slot < engine.receiver_count &&
                   engine.receivers[slot] != receiver.descriptor.id) {
                ++slot;
            }
            if (slot == engine.receiver_count) {
                return Error::NOT_FOUND;
            }
            receiver.state = project_state(engine.states[slot]);
            receiver.operation = project_operation(engine.operations[slot]);
            receiver.leased = engine.leased[slot];
            const TuneMetadata& metadata = engine.tune_metadata[slot];
            if ((metadata.current_valid && !valid_tune_system(metadata.current.system)) ||
                (metadata.last_successful_valid &&
                 !valid_tune_system(metadata.last_successful.system))) {
                return Error::INVALID_ARGUMENT;
            }
            receiver.tune_revision = metadata.revision;
            receiver.current_tune.present = metadata.current_valid;
            if (metadata.current_valid) {
                receiver.current_tune.value = project_tune(metadata.current,
                                                           metadata.current_revision);
            }
            receiver.last_successful_tune.present = metadata.last_successful_valid;
            if (metadata.last_successful_valid) {
                receiver.last_successful_tune.value = project_tune(
                    metadata.last_successful, metadata.last_successful_revision);
            }
            set_unobserved_metrics(receiver);
        }
    }
    if (status.readers.present) {
        for (std::size_t i = 0U; i < status.readers.value.count; ++i) {
            wire::CardObservation& card = status.readers.value.items[i].card;
            if (card.state == wire::CardState::unknown && !card.sampled_at_ns.present &&
                card.reason == 0U) {
                card.reason = static_cast<std::uint8_t>(wire::UnknownReason::not_sampled);
            }
        }
    }

    wire::StatusContext context;
    context.list = &canonical;
    return wire::validate_status_response(status, context);
}

}  // namespace dtv::userland::detail
