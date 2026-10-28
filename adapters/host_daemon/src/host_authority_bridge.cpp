// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/host_authority_bridge.h"

#include <array>
#include <limits>

namespace px4::userland::ipc::posix {
namespace {

using dtv::userland::detail::AuthorityJob;
using dtv::userland::detail::AuthorityJobKind;
using dtv::userland::detail::AuthorityReply;
using dtv::userland::detail::ControlServiceSession;

std::uint8_t job_receiver(const AuthorityJob& job) noexcept
{
    return job.kind == AuthorityJobKind::tune ? job.tune.receiver : job.receiver;
}

}  // namespace

HostAuthorityBridge::HostAuthorityBridge(
    TunerService& tuner_service, ControlWorkerLanes& workers,
    const dtv::userland::wire::ServerHello& server,
    const dtv::userland::wire::ListResponse& canonical,
    dtv::userland::detail::ControlAuthority::TuneValidator validate_tune) noexcept
    : authority_owner_(tuner_service, server, canonical, validate_tune),
      tuner_service_(tuner_service), workers_(workers),
      slots_(new (std::nothrow) Slot[kHostAuthoritySessionCapacity])
{
}

#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
void HostAuthorityBridge::close_session_for_test(std::uint8_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return;
    Slot& slot = slots_[index];
    if (!slot.session) return;
    const AuthorityJob cleanup = authority_owner_.authority().close(*slot.session);
    if (cleanup.kind == AuthorityJobKind::disconnect) {
        (void)authority_owner_.authority().cleanup_submitted(cleanup, false);
        (void)authority_owner_.authority().complete(cleanup.key, Error::OK);
    }
    slot.session.reset();
}
#endif

Result<std::uint8_t> HostAuthorityBridge::open_session() noexcept
{
    if (workers_.error() != Error::OK) enter_fatal();
    if (!accepting_) return Result<std::uint8_t>::failure(Error::NOT_READY);
    if (!slots_) return Result<std::uint8_t>::failure(Error::RESOURCE_LIMIT);
    for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
        Slot& slot = slots_[i];
        if (slot.state != SlotState::free) continue;
        slot.session.emplace(authority_owner_.authority());
        if (authority_owner_.authority().open(*slot.session) != Error::OK) {
            slot.session.reset();
            return Result<std::uint8_t>::failure(Error::INTERNAL);
        }
        slot.client_id = slot.session->client_id();
        slot.state = SlotState::active;
        return Result<std::uint8_t>::success(static_cast<std::uint8_t>(i));
    }
    return Result<std::uint8_t>::failure(Error::RESOURCE_LIMIT);
}

dtv::userland::detail::AuthorityReply HostAuthorityBridge::accept(
    std::uint8_t index, const dtv::userland::wire::Frame& frame,
    const dtv::userland::wire::Context& context,
    const dtv::userland::wire::Snapshot& snapshot,
    std::uint64_t accepted_at_ms) noexcept
{
    if (workers_.error() != Error::OK) enter_fatal();
    if (!accepting_ || !slots_ || index >= kHostAuthoritySessionCapacity) return {};
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return {};
    const auto reply = authority_owner_.authority().accept(
        *slot.session, frame, context, snapshot, accepted_at_ms, *this);
    if (frame.header.opcode == dtv::userland::wire::kOpcodeStartStream &&
        reply.action == dtv::userland::wire::InboundAction::respond &&
        reply.frame.size >= dtv::userland::wire::kHeaderSize) {
        const auto header = dtv::userland::wire::decode_header(
            {reply.frame.data, dtv::userland::wire::kHeaderSize});
        if (header && header.value().kind == dtv::userland::wire::Kind::response) {
            const auto request = dtv::userland::wire::decode_start_request(frame.payload);
            const auto response = dtv::userland::wire::decode_start_response(
                {reply.frame.data + dtv::userland::wire::kHeaderSize,
                 reply.frame.size - dtv::userland::wire::kHeaderSize});
            if (!request || !response ||
                !remember_stream(index, request.value().lease_id,
                                 response.value().stream_id))
                enter_fatal();
        }
    }
    if (reply.action == dtv::userland::wire::InboundAction::close)
        close_slot(index, reply);
    return reply;
}

void HostAuthorityBridge::close_slot(std::size_t index,
                                     const AuthorityReply& reply) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return;
    Slot& slot = slots_[index];
    slot.state = SlotState::tombstone;
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
    close_pending_before_accept_ = slot.close_pending;
    accept_preserved_close_pending_ = false;
#endif
    slot.close_pending = slot.close_pending || (reply.cleanup_required &&
        reply.cleanup.kind == AuthorityJobKind::disconnect);
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
    accept_preserved_close_pending_ = close_pending_before_accept_ &&
                                     slot.close_pending;
#endif
    if (slot.session && slot.session->client_id() == 0U) slot.session.reset();
    if (reply.cleanup_required) retain_cleanup(reply.cleanup, index);
    if (slot.close_pending) close_owned_streams(index);
}

Error HostAuthorityBridge::response_sent(std::uint8_t index,
                                         bool complete_frame) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return Error::INVALID_ARGUMENT;
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return Error::NOT_FOUND;
    if (complete_frame) {
        AuthorityJob rejected{};
        const Error error = authority_owner_.authority().response_sent(
            *slot.session, true, *this, &rejected);
        if (error == Error::OK && slot.session->client_id() == 0U) {
            slot.state = SlotState::tombstone;
            slot.session.reset();
        } else if (rejected.kind == AuthorityJobKind::disconnect) {
            slot.state = SlotState::tombstone;
            slot.session.reset();
            retain_cleanup(rejected, index);
        }
        return error;
    }

    slot.state = SlotState::tombstone;
    AuthorityJob rejected{};
    const Error error = authority_owner_.authority().response_sent(
        *slot.session, false, *this, &rejected);
    if (error == Error::OK) {
        slot.session.reset();
    } else if (rejected.kind == AuthorityJobKind::disconnect) {
        slot.session.reset();
        retain_cleanup(rejected, index);
    }
    return error;
}

dtv::userland::detail::AuthorityReply HostAuthorityBridge::take_response(
    std::uint8_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return {};
    Slot& slot = slots_[index];
    if (!slot.response_ready) return {};
    slot.response_ready = false;
    return slot.response;
}

Error HostAuthorityBridge::close_session(std::uint8_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return Error::INVALID_ARGUMENT;
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return Error::NOT_FOUND;
    slot.state = SlotState::tombstone;
    slot.close_pending = true;
    authority_owner_.authority().cancel_pending_admission(*slot.session);
    close_owned_streams(index);
    advance();
    return Error::OK;
}

Error HostAuthorityBridge::set_ingress_stop(IngressStop stop_other_ingress,
                                             void* context) noexcept
{
    if (!accepting_) return Error::NOT_READY;
    stop_other_ingress_ = stop_other_ingress;
    stop_other_ingress_context_ = context;
    return Error::OK;
}

Error HostAuthorityBridge::set_control_ingress_stop(IngressStop stop_control_ingress,
                                                     void* context) noexcept
{
    if (!accepting_) return Error::NOT_READY;
    if (stop_control_ingress == nullptr) return Error::INVALID_ARGUMENT;
    stop_control_ingress_ = stop_control_ingress;
    stop_control_ingress_context_ = context;
    return Error::OK;
}

Error HostAuthorityBridge::set_data_ingress_stop(IngressStop stop_data_ingress,
                                                  void* context) noexcept
{
    if (!accepting_) return Error::NOT_READY;
    if (stop_data_ingress == nullptr) return Error::INVALID_ARGUMENT;
    stop_data_ingress_ = stop_data_ingress;
    stop_data_ingress_context_ = context;
    return Error::OK;
}

Result<HostAuthorityDataConnection> HostAuthorityBridge::open_data_session() noexcept
{
    if (workers_.error() != Error::OK) enter_fatal();
    if (!accepting_) return Result<HostAuthorityDataConnection>::failure(Error::NOT_READY);
    for (std::size_t i = 0U; i < data_slots_.size(); ++i) {
        DataSlot& slot = data_slots_[i];
        if (slot.state != DataSlotState::free) continue;
        if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
            slot.state = DataSlotState::retired;
            continue;
        }
        ++slot.generation;
        slot.state = DataSlotState::active;
        slot.connection_open = true;
        return Result<HostAuthorityDataConnection>::success(
            HostAuthorityDataConnection{static_cast<std::uint8_t>(i), slot.generation});
    }
    return Result<HostAuthorityDataConnection>::failure(Error::RESOURCE_LIMIT);
}

Error HostAuthorityBridge::close_data_session(
    HostAuthorityDataConnection connection) noexcept
{
    if (connection.slot >= data_slots_.size()) return Error::INVALID_ARGUMENT;
    DataSlot& slot = data_slots_[connection.slot];
    if ((slot.state != DataSlotState::active &&
         slot.state != DataSlotState::tombstone) || !slot.connection_open ||
        slot.generation != connection.generation) return Error::NOT_FOUND;
    if (slot.ticket.stream_id != 0U) {
        slot.connection_open = false;
        slot.state = DataSlotState::tombstone;
        slot.attach_result = HostAuthorityAttachResult{};
        if (!slot.attach_pending && slot.prepared &&
            !queue_stream_cleanup(connection.slot) && workers_.error() != Error::OK)
            enter_fatal();
        if (slot.cleanup_pending) return Error::RESOURCE_LIMIT;
        if (slot.attach_failed && slot.cleanup_quiesced)
            return finish_failed_attach_cleanup(connection.slot);
        return Error::OK;
    }
    slot.connection_open = false;
    slot.state = DataSlotState::free;
    return Error::OK;
}

Error HostAuthorityBridge::finish_failed_attach_cleanup(std::size_t index) noexcept
{
    if (index >= data_slots_.size()) return Error::INVALID_ARGUMENT;
    DataSlot& data = data_slots_[index];
    if (!data.attach_failed || !data.cleanup_quiesced || data.connection_open ||
        data.attach_pending || data.prepared || data.cleanup_pending ||
        data.cleanup_inflight)
        return Error::NOT_READY;
    if (data.final_available) {
        const auto released = tuner_service_.authority_release_stream_final_v1(data.ticket);
        if (!released) {
            data.state = DataSlotState::fatal_unresolved;
            enter_fatal();
            return released.error();
        }
    }
    const auto terminal = tuner_service_.authority_stream_last_terminal_v1(
        data.ticket.client_id, data.lease_id);
    if (!terminal || terminal.value().stream_id != data.ticket.stream_id ||
        !terminal.value().quiesced) return Error::INTERNAL;
    if (terminal.value().cleanup_error == Error::OK)
        forget_stream(data.ticket.client_id, data.ticket.stream_id);
    const std::uint64_t generation = data.generation;
    data = DataSlot{generation, DataSlotState::free};
    return Error::OK;
}

Error HostAuthorityBridge::configure_stream_contract(
    std::uint64_t engine_epoch, std::uint64_t arm_budget_ms) noexcept
{
    if (engine_epoch == 0U || arm_budget_ms == 0U) return Error::INVALID_ARGUMENT;
    if (stream_engine_epoch_ != 0U) return Error::BUSY;
    if (tuner_service_.engine_.stream_contract() !=
            dtv::userland::detail::StreamContract::v1 ||
        tuner_service_.engine_.stream_context_epoch() != engine_epoch)
        return Error::INVALID_ARGUMENT;
    const Error authority = authority_owner_.authority().configure_stream_contract(
        engine_epoch, arm_budget_ms);
    if (authority == Error::OK) stream_engine_epoch_ = engine_epoch;
    return authority;
}

Result<dtv::userland::detail::StreamStartResult> HostAuthorityBridge::start_stream(
    std::uint8_t control_slot, std::uint64_t lease_id,
    std::uint64_t tune_revision, std::uint64_t now_ms,
    std::uint64_t arm_budget_ms) noexcept
{
    if (!slots_ || control_slot >= kHostAuthoritySessionCapacity)
        return Result<dtv::userland::detail::StreamStartResult>::failure(
            Error::INVALID_ARGUMENT);
    Slot& slot = slots_[control_slot];
    if (slot.state != SlotState::active || !slot.session)
        return Result<dtv::userland::detail::StreamStartResult>::failure(Error::NOT_FOUND);
    const auto started = tuner_service_.authority_start_stream_v1(slot.client_id, lease_id,
        tune_revision, now_ms, arm_budget_ms);
    if (!started) return started;
    if (!remember_stream(control_slot, lease_id, started.value().stream_id))
        return Result<dtv::userland::detail::StreamStartResult>::failure(Error::RESOURCE_LIMIT);
    return started;
}

Error HostAuthorityBridge::prepare_data_attach(
    HostAuthorityDataConnection connection, std::uint8_t control_slot,
    const dtv::userland::wire::AttachRequest& request,
    std::uint64_t now_ms) noexcept
{
    if (!slots_ || control_slot >= kHostAuthoritySessionCapacity ||
        connection.slot >= data_slots_.size()) return Error::INVALID_ARGUMENT;
    Slot& owner = slots_[control_slot];
    DataSlot& data = data_slots_[connection.slot];
    if (owner.state != SlotState::active || !owner.session ||
        data.state != DataSlotState::active ||
        data.generation != connection.generation) return Error::NOT_FOUND;
    if (stream_engine_epoch_ == 0U ||
        request.epoch != authority_owner_.authority().server().epoch)
        return Error::INVALID_ARGUMENT;
    if (data.attach_pending || data.ticket.stream_id != 0U) return Error::BUSY;

    dtv::userland::detail::ReceiverToken token{};
    token = request.attach_token;
    const auto reserved = tuner_service_.authority_reserve_attach_v1(
        owner.client_id, request.lease_id, stream_engine_epoch_, request.stream_id,
        token, now_ms);
    if (!reserved) return reserved.error();
    data.ticket = reserved.value();
    data.lease_id = request.lease_id;
    data.attach_result = HostAuthorityAttachResult{};
    data.attach_pending = true;
    data.prepared = false;

    ControlWorkerTask task{};
    task.kind = ControlWorkerTaskKind::client_request;
    task.operation = ControlWorkerOperation::tuner_prepare_attach_v1;
    task.stream_ticket = data.ticket;
    task.receiver = static_cast<std::uint8_t>(data.ticket.slot);
    task.client_id = owner.client_id;
    task.lease_id = request.lease_id;
    task.data_slot = connection.slot;
    task.data_generation = connection.generation;
    task.authority_stream = true;
    const auto lane = workers_.authority_lane(task.receiver, false);
    if (!lane || !workers_.submit(lane.value(), task)) {
        data.attach_pending = false;
        data.attach_failed = true;
        data.state = DataSlotState::tombstone;
        (void)tuner_service_.authority_abort_attach_v1(data.ticket,
                                                       Error::RESOURCE_LIMIT);
        (void)queue_stream_cleanup(connection.slot);
        return Error::RESOURCE_LIMIT;
    }
    return Error::OK;
}

HostAuthorityAttachResult HostAuthorityBridge::take_data_attach_result(
    HostAuthorityDataConnection connection) noexcept
{
    if (connection.slot >= data_slots_.size()) return {};
    DataSlot& slot = data_slots_[connection.slot];
    if (slot.generation != connection.generation || !slot.attach_result.ready) return {};
    HostAuthorityAttachResult result = slot.attach_result;
    slot.attach_result = HostAuthorityAttachResult{};
    return result;
}

Error HostAuthorityBridge::data_attach_response_sent(
    HostAuthorityDataConnection connection, bool complete_frame) noexcept
{
    if (connection.slot >= data_slots_.size()) return Error::INVALID_ARGUMENT;
    DataSlot& slot = data_slots_[connection.slot];
    if (slot.generation != connection.generation || !slot.prepared)
        return Error::NOT_FOUND;
    if (complete_frame) {
        const auto committed = tuner_service_.authority_commit_attach_v1(slot.ticket);
        if (!committed) {
            (void)tuner_service_.authority_abort_attach_v1(slot.ticket,
                                                           committed.error());
            slot.state = DataSlotState::tombstone;
            return queue_stream_cleanup(connection.slot)
                ? committed.error() : Error::RESOURCE_LIMIT;
        }
        return Error::OK;
    }
    (void)tuner_service_.authority_abort_attach_v1(slot.ticket, Error::OK);
    slot.state = DataSlotState::tombstone;
    return queue_stream_cleanup(connection.slot) ? Error::OK : Error::RESOURCE_LIMIT;
}

HostAuthorityDataReadResult HostAuthorityBridge::read_data(
    HostAuthorityDataConnection connection, MutableByteView output,
    Timeout timeout) noexcept
{
    HostAuthorityDataReadResult result{};
    if (connection.slot >= data_slots_.size()) {
        result.error = Error::INVALID_ARGUMENT;
        return result;
    }
    DataSlot& data = data_slots_[connection.slot];
    if (data.generation != connection.generation || !data.connection_open ||
        (data.state != DataSlotState::active &&
         data.state != DataSlotState::tombstone) || !data.prepared ||
        data.ticket.stream_id == 0U) {
        result.error = Error::NOT_FOUND;
        return result;
    }
    if (data.next_sequence == std::numeric_limits<std::uint64_t>::max()) {
        result.error = Error::RESOURCE_LIMIT;
        return result;
    }
    const auto read = tuner_service_.authority_read_stream_v1(data.ticket, output,
                                                               timeout);
    if (!read) {
        result.error = read.error();
        return result;
    }
    result.eof = read.value().eof;
    result.timed_out = read.value().timed_out;
    result.terminal = read.value().terminal;
    if (result.eof) data.drained = true;
    if (result.eof || result.timed_out) return result;
    if (read.value().bytes == 0U || output.data == nullptr ||
        read.value().bytes > output.size ||
        (read.value().bytes % dtv::userland::wire::kTsPacketSize) != 0U) {
        result.error = Error::PROTOCOL_ERROR;
        return result;
    }
    result.data.stream_id = data.ticket.stream_id;
    result.data.sequence = data.next_sequence++;
    result.data.cumulative_queue_dropped_packets =
        read.value().cumulative_queue_dropped_packets;
    result.data.packets = ByteView{output.data, read.value().bytes};
    return result;
}

HostAuthorityStreamEndResult HostAuthorityBridge::take_data_stream_end(
    HostAuthorityDataConnection connection) noexcept
{
    if (connection.slot >= data_slots_.size()) return {};
    const DataSlot& slot = data_slots_[connection.slot];
    if (slot.generation != connection.generation || !slot.stream_end.ready ||
        !slot.drained) return {};
    return slot.stream_end;
}

Error HostAuthorityBridge::data_stream_end_sent(
    HostAuthorityDataConnection connection, bool complete_frame) noexcept
{
    if (connection.slot >= data_slots_.size()) return Error::INVALID_ARGUMENT;
    DataSlot& slot = data_slots_[connection.slot];
    if (slot.generation != connection.generation || !slot.stream_end.ready ||
        !slot.drained)
        return Error::NOT_FOUND;
    if (!complete_frame) return Error::OK;
    const auto released = tuner_service_.authority_release_stream_final_v1(slot.ticket);
    if (!released) {
        slot.state = DataSlotState::fatal_unresolved;
        enter_fatal();
        return released.error();
    }
    forget_stream(slot.ticket.client_id, slot.ticket.stream_id);
    slot = DataSlot{slot.generation, DataSlotState::free};
    return Error::OK;
}

Result<dtv::userland::detail::StreamStopResult> HostAuthorityBridge::stop_stream(
    std::uint8_t control_slot, std::uint64_t lease_id,
    std::uint64_t stream_id) noexcept
{
    return stop_stream_impl(control_slot, lease_id, stream_id, false);
}

Result<dtv::userland::detail::StreamStopResult> HostAuthorityBridge::stop_stream_v1(
    std::uint64_t client_id, std::uint64_t lease_id,
    std::uint64_t stream_id) noexcept
{
    const std::size_t index = find_slot(client_id);
    if (index == kHostAuthoritySessionCapacity)
        return Result<dtv::userland::detail::StreamStopResult>::failure(Error::NOT_FOUND);
    return stop_stream_impl(static_cast<std::uint8_t>(index), lease_id,
                            stream_id, true);
}

dtv::userland::wire::Optional<bool> HostAuthorityBridge::lnb_power_state() noexcept
{
    return tuner_service_.independent_lnb_power_state();
}

bool HostAuthorityBridge::supports_internal_card_observation() const noexcept
{
    return tuner_service_.supports_internal_card_observation();
}

Result<dtv::userland::detail::StreamStopResult> HostAuthorityBridge::stop_stream_impl(
    std::uint8_t control_slot, std::uint64_t lease_id,
    std::uint64_t stream_id, bool retain_submit_rejection) noexcept
{
    if (!slots_ || control_slot >= kHostAuthoritySessionCapacity)
        return Result<dtv::userland::detail::StreamStopResult>::failure(
            Error::INVALID_ARGUMENT);
    Slot& owner = slots_[control_slot];
    if (owner.state != SlotState::active || !owner.session)
        return Result<dtv::userland::detail::StreamStopResult>::failure(Error::NOT_FOUND);
    auto stopped = tuner_service_.authority_stop_stream_v1(
        owner.client_id, lease_id, stream_id);
    if (!stopped) return stopped;
    bool found_data = false;
    for (std::size_t i = 0U; i < data_slots_.size(); ++i) {
        DataSlot& data = data_slots_[i];
        if (data.ticket.client_id != owner.client_id ||
            data.lease_id != lease_id || data.ticket.stream_id != stream_id)
            continue;
        found_data = true;
        data.state = DataSlotState::tombstone;
        if (stopped.value().cleanup_owner && !data.attach_pending &&
            (!retain_submit_rejection ||
             (!data.cleanup_inflight && !data.stream_end.ready)) &&
            !queue_stream_cleanup(i)) {
            if (retain_submit_rejection && workers_.error() != Error::OK) {
                enter_fatal();
                stopped.value().cleanup_error = workers_.error();
            }
            if (!retain_submit_rejection)
                return Result<dtv::userland::detail::StreamStopResult>::failure(
                    Error::RESOURCE_LIMIT);
        }
        break;
    }
    if (retain_submit_rejection && !found_data) {
        if (stopped.value().quiesced) {
            forget_stream(owner.client_id, stream_id);
        } else {
            enter_fatal();
            stopped.value().cleanup_error = Error::INTERNAL;
        }
    }
    return stopped;
}

HostAuthorityDataCounts HostAuthorityBridge::data_counts() const noexcept
{
    HostAuthorityDataCounts result{};
    for (const DataSlot& slot : data_slots_) {
        switch (slot.state) {
        case DataSlotState::free: ++result.free; break;
        case DataSlotState::active: ++result.active; break;
        case DataSlotState::tombstone: ++result.tombstone; break;
        case DataSlotState::fatal_unresolved: ++result.fatal_unresolved; break;
        case DataSlotState::retired: ++result.retired; break;
        }
    }
    return result;
}

Error HostAuthorityBridge::set_legacy_completion_sink(
    LegacyCompletionSink sink, void* context) noexcept
{
    if (!accepting_ || sink == nullptr) return Error::INVALID_ARGUMENT;
    legacy_completion_sink_ = sink;
    legacy_completion_context_ = context;
    return Error::OK;
}

bool HostAuthorityBridge::take_legacy_completion(
    ControlWorkerCompletion& completion) noexcept
{
    for (std::size_t lane = 0U; lane < deferred_legacy_valid_.size(); ++lane) {
        if (!deferred_legacy_valid_[lane]) continue;
        completion = deferred_legacy_[lane];
        deferred_legacy_valid_[lane] = false;
        deferred_legacy_[lane] = ControlWorkerCompletion{};
        return true;
    }
    return false;
}

bool HostAuthorityBridge::submit(const AuthorityJob& job) noexcept
{
    const std::uint64_t id = job.kind == AuthorityJobKind::tune
        ? job.tune.client_id : job.client_id;
    const std::size_t index = find_slot(id);
    if (index == kHostAuthoritySessionCapacity) return false;
    return queue_job(job, index);
}

bool HostAuthorityBridge::queue_job(const AuthorityJob& job,
                                    std::size_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity ||
        job.key.request_id > 0xffffffffU) return false;
    Slot& slot = slots_[index];
    if (job.kind == AuthorityJobKind::disconnect && has_owned_streams(index)) {
        slot.state = SlotState::tombstone;
        slot.close_pending = true;
        retain_cleanup(job, index);
        close_owned_streams(index);
        return true;
    }
    if (job.kind == AuthorityJobKind::disconnect && slot.inflight != 0U) {
        retain_cleanup(job, index);
        return true;
    }
    if (job.kind == AuthorityJobKind::disconnect && !slot.card_cleanup_done) {
        ControlWorkerTask task{};
        task.operation = ControlWorkerOperation::card_authority_cleanup;
        task.kind = ControlWorkerTaskKind::authority_cleanup;
        task.authority_job = job;
        task.client_id = job.client_id;
        task.request_id = static_cast<std::uint32_t>(job.key.request_id);
        if (!workers_.submit(workers_.reader_lane(), task)) return false;
        slot.cleanup = job;
        slot.cleanup_accepted = true;
        slot.cleanup_pending = false;
        slot.cleanup_inflight = true;
        slot.card_cleanup_inflight = true;
        return true;
    }
    ControlWorkerTask task{};
    task.operation = job.kind == AuthorityJobKind::card
        ? ControlWorkerOperation::card_authority_job
        : ControlWorkerOperation::tuner_authority_job;
    task.kind = job.kind == AuthorityJobKind::disconnect
        ? ControlWorkerTaskKind::authority_cleanup
        : ControlWorkerTaskKind::authority_request;
    task.authority_job = job;
    task.client_id = job.kind == AuthorityJobKind::tune
        ? job.tune.client_id : job.client_id;
    task.request_id = static_cast<std::uint32_t>(job.key.request_id);
    task.receiver = job_receiver(job);
    task.lease_id = job.kind == AuthorityJobKind::tune
        ? job.tune.lease_id : job.lease_id;
    const auto lane = job.kind == AuthorityJobKind::card
        ? Result<ControlWorkerLane>::success(workers_.reader_lane())
        : job.kind == AuthorityJobKind::lnb_power
        ? workers_.authority_lnb_lane()
        : job.kind == AuthorityJobKind::internal_card_status
        ? Result<ControlWorkerLane>::success(workers_.authority_status_lane())
        : workers_.authority_lane(
              task.receiver, job.kind == AuthorityJobKind::disconnect);
    if (!lane || !workers_.submit(lane.value(), task)) return false;

    if (job.kind == AuthorityJobKind::disconnect) {
        slot.cleanup = job;
        slot.cleanup_accepted = true;
        slot.cleanup_pending = false;
        slot.cleanup_inflight = true;
        if (slot.deferred_session && slot.session) {
            (void)authority_owner_.authority().cleanup_submitted(job, true);
            slot.deferred_session = false;
        }
    } else {
        slot.cleanup_pending = false;
        slot.inflight_key = job.key;
        slot.inflight_kind = job.kind;
        slot.request_inflight = true;
        ++slot.inflight;
    }
    return true;
}

bool HostAuthorityBridge::queue_job(const dtv::userland::detail::AuthorityCleanup& job,
                                    std::size_t index) noexcept
{
    AuthorityJob value{};
    job.copy_to(value);
    return queue_job(value, index);
}

void HostAuthorityBridge::retain_cleanup(const AuthorityJob& job,
                                         std::size_t index) noexcept
{
    Slot& slot = slots_[index];
    slot.cleanup = job;
    slot.cleanup_pending = true;
    slot.cleanup_accepted = false;
    slot.cleanup_inflight = false;
    slot.card_cleanup_done = false;
    slot.card_cleanup_inflight = false;
    if (slot.session && slot.session->client_id() != 0U)
        (void)authority_owner_.authority().cleanup_submitted(job, false);
}

void HostAuthorityBridge::retain_cleanup(
    const dtv::userland::detail::AuthorityCleanup& job,
    std::size_t index) noexcept
{
    Slot& slot = slots_[index];
    slot.cleanup = job;
    slot.cleanup_pending = true;
    slot.cleanup_accepted = false;
    slot.cleanup_inflight = false;
    slot.card_cleanup_done = false;
    slot.card_cleanup_inflight = false;
    if (slot.session && slot.session->client_id() != 0U)
        (void)authority_owner_.authority().cleanup_submitted(job, false);
}

std::size_t HostAuthorityBridge::find_slot(std::uint64_t id) const noexcept
{
    if (!slots_) return kHostAuthoritySessionCapacity;
    for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
        if (slots_[i].state != SlotState::free && slots_[i].client_id == id)
            return i;
    }
    return kHostAuthoritySessionCapacity;
}

void HostAuthorityBridge::consume(const ControlWorkerCompletion& completion,
                                  std::size_t lane) noexcept
{
    if (completion.operation == ControlWorkerOperation::card_authority_cleanup) {
        const std::size_t index = find_slot(completion.authority_key.client_id);
        if (index == kHostAuthoritySessionCapacity) {
            enter_fatal();
            return;
        }
        Slot& slot = slots_[index];
        if (!slot.card_cleanup_inflight || !slot.cleanup_inflight ||
            slot.cleanup.kind != AuthorityJobKind::disconnect ||
            slot.cleanup.key.client_id != completion.authority_key.client_id ||
            slot.cleanup.key.request_id != completion.authority_key.request_id ||
            completion.authority_completion.key.client_id != completion.authority_key.client_id ||
            completion.authority_completion.key.request_id != completion.authority_key.request_id ||
            completion.authority_completion.kind != AuthorityJobKind::disconnect) {
            slot.state = SlotState::fatal_unresolved;
            enter_fatal();
            return;
        }
        slot.card_cleanup_inflight = false;
        slot.cleanup_inflight = false;
        if (completion.error != Error::OK) {
            slot.state = SlotState::fatal_unresolved;
            enter_fatal();
            return;
        }
        slot.card_cleanup_done = true;
        const dtv::userland::detail::AuthorityCleanup cleanup = slot.cleanup;
        if (!queue_job(cleanup, index)) {
            slot.cleanup = cleanup;
            slot.cleanup_pending = true;
            slot.cleanup_accepted = false;
        }
        return;
    }
    if (completion.authority_stream) {
        if (completion.data_slot >= data_slots_.size()) {
            enter_fatal();
            return;
        }
        DataSlot& data = data_slots_[completion.data_slot];
        if (data.generation != completion.data_generation ||
            data.ticket.stream_id != completion.stream_ticket.stream_id ||
            data.ticket.reservation != completion.stream_ticket.reservation) {
            enter_fatal();
            return;
        }
        if (completion.operation == ControlWorkerOperation::tuner_prepare_attach_v1) {
            data.attach_pending = false;
            if (completion.error == Error::OK && data.state == DataSlotState::active) {
                data.prepared = true;
                data.attach_result.error = Error::OK;
                data.attach_result.response.epoch =
                    authority_owner_.authority().server().epoch;
                data.attach_result.response.stream_id = data.ticket.stream_id;
                data.attach_result.ready = true;
            } else {
                data.drained = true;
                data.attach_failed = true;
                data.attach_result.error = completion.error == Error::OK
                    ? Error::NOT_READY : completion.error;
                data.attach_result.response.epoch =
                    authority_owner_.authority().server().epoch;
                data.attach_result.response.stream_id = data.ticket.stream_id;
                data.attach_result.ready = data.connection_open;
                (void)tuner_service_.authority_abort_attach_v1(
                    data.ticket, completion.error == Error::OK
                        ? Error::OK : completion.error);
                data.state = DataSlotState::tombstone;
                if (!queue_stream_cleanup(completion.data_slot) &&
                    workers_.error() != Error::OK) enter_fatal();
            }
            return;
        }
        if (completion.operation ==
            ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1 ||
            completion.operation == ControlWorkerOperation::tuner_run_stream_cleanup_v1) {
            data.cleanup_inflight = false;
            if (data.state != DataSlotState::tombstone) {
                data.state = DataSlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
            const auto terminal = tuner_service_.authority_stream_last_terminal_v1(
                data.ticket.client_id, data.lease_id);
            bool quiesced = terminal &&
                terminal.value().stream_id == data.ticket.stream_id &&
                terminal.value().quiesced;
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
            terminal_observed_ = static_cast<bool>(terminal);
            terminal_quiesced_ = quiesced;
            if ((force_next_terminal_unquiesced_ || force_terminal_unquiesced_) &&
                terminal) {
                quiesced = false;
                force_next_terminal_unquiesced_ = false;
                forced_terminal_unquiesced_ = true;
            }
            terminal_quiesced_ = quiesced;
#endif
            const bool missing_failed_final = data.attach_failed &&
                (completion.error == Error::NOT_FOUND ||
                 completion.error == Error::UNSUPPORTED);
            if (!terminal || !quiesced) {
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
                if (terminal && !quiesced)
                    quiesced_guard_rejected_ = true;
#endif
                data.state = DataSlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
            if (completion.error != Error::OK && !missing_failed_final) {
                data.state = DataSlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
            const auto final = completion.error == Error::OK
                ? tuner_service_.authority_final_snapshot_v1(data.ticket)
                : Result<px4::userland::TunerStreamFinalSnapshot>::failure(
                    completion.error);
            if (!final && data.attach_failed &&
                (final.error() == Error::NOT_FOUND ||
                 final.error() == Error::UNSUPPORTED)) {
                data.cleanup_quiesced = true;
                data.final_available = false;
                if (!data.connection_open &&
                    finish_failed_attach_cleanup(completion.data_slot) != Error::OK) {
                    data.state = DataSlotState::fatal_unresolved;
                    enter_fatal();
                }
                return;
            }
            if (!final && final.error() != Error::UNSUPPORTED) {
                data.state = DataSlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
            if (!final) {
                data = DataSlot{data.generation, DataSlotState::free};
                return;
            }
            if (data.attach_failed) {
                data.cleanup_quiesced = true;
                data.final_available = true;
                if (!data.connection_open &&
                    finish_failed_attach_cleanup(completion.data_slot) != Error::OK) {
                    data.state = DataSlotState::fatal_unresolved;
                    enter_fatal();
                }
                return;
            }
            auto& event = data.stream_end.event;
            event.final.id = data.ticket.stream_id;
            event.final.tune_revision = data.ticket.revision;
            switch (static_cast<dtv::userland::detail::StreamTerminal>(
                        final.value().terminal)) {
            case dtv::userland::detail::StreamTerminal::slow_consumer:
                event.final.reason = dtv::userland::wire::StreamEndReason::slow_consumer;
                break;
            case dtv::userland::detail::StreamTerminal::disconnected:
                event.final.reason = dtv::userland::wire::StreamEndReason::owner_closed;
                break;
            case dtv::userland::detail::StreamTerminal::stopped:
            case dtv::userland::detail::StreamTerminal::none:
                event.final.reason = dtv::userland::wire::StreamEndReason::stopped;
                break;
            default:
                event.final.reason = dtv::userland::wire::StreamEndReason::backend_error;
                break;
            }
            const auto& counters = final.value().counters;
            const std::array<std::uint64_t, 8U> values = {
                counters.packets, counters.queue_drops, counters.bytes,
                counters.sync_errors, counters.tei_packets,
                counters.continuity_errors, counters.usb_errors,
                counters.empty_intervals};
            const std::array<dtv::userland::wire::MeasureState, 8U> availability = {
                counters.availability[0U], counters.availability[5U],
                counters.availability[1U], counters.availability[2U],
                counters.availability[3U], counters.availability[4U],
                counters.availability[6U], counters.availability[7U]};
            const std::array<std::uint8_t, 8U> ids = {
                dtv::userland::wire::kCounterIdTransmittedPackets,
                dtv::userland::wire::kCounterIdQueueDroppedPackets,
                dtv::userland::wire::kCounterIdStreamBytes,
                dtv::userland::wire::kCounterIdSyncErrors,
                dtv::userland::wire::kCounterIdTeiPackets,
                dtv::userland::wire::kCounterIdContinuityErrors,
                dtv::userland::wire::kCounterIdStreamUsbErrors,
                dtv::userland::wire::kCounterIdEmptyIntervals};
            event.final.statistics.entries.count =
                static_cast<std::uint16_t>(ids.size());
            for (std::size_t i = 0U; i < ids.size(); ++i) {
                auto& counter = event.final.statistics.entries.items[i];
                counter.id = ids[i];
                counter.counter.measurement.state = availability[i];
                counter.counter.measurement.value = values[i];
                counter.counter.overflow = availability[i] ==
                    dtv::userland::wire::MeasureState::valid &&
                    values[i] == std::numeric_limits<std::uint64_t>::max();
            }
            data.stream_end.ready = true;
            if (!data.connection_open) {
                const auto released =
                    tuner_service_.authority_release_stream_final_v1(data.ticket);
                if (!released) {
                    data.state = DataSlotState::fatal_unresolved;
                    enter_fatal();
                    return;
                }
                forget_stream(data.ticket.client_id, data.ticket.stream_id);
                data = DataSlot{data.generation, DataSlotState::free};
            }
            return;
        }
        enter_fatal();
        return;
    }
    if (completion.operation != ControlWorkerOperation::tuner_authority_job &&
        completion.operation != ControlWorkerOperation::card_authority_job) {
        if (legacy_completion_sink_ != nullptr) {
            legacy_completion_sink_(legacy_completion_context_, completion);
        } else if (lane < deferred_legacy_.size()) {
            deferred_legacy_[lane] = completion;
            deferred_legacy_valid_[lane] = true;
        }
        return;
    }
    const auto& value = completion.authority_completion;
    const std::size_t index = find_slot(value.key.client_id);
    if (index == kHostAuthoritySessionCapacity) {
        enter_fatal();
        return;
    }
    Slot& slot = slots_[index];
    const bool outer_matches = completion.authority_key.client_id == value.key.client_id &&
        completion.authority_key.request_id == value.key.request_id &&
        completion.authority_job_kind == value.kind &&
        ((value.kind == AuthorityJobKind::disconnect &&
          completion.kind == ControlWorkerTaskKind::authority_cleanup) ||
         (value.kind != AuthorityJobKind::disconnect &&
          completion.kind == ControlWorkerTaskKind::authority_request));
    if (!outer_matches) {
        slot.state = SlotState::fatal_unresolved;
        enter_fatal();
        return;
    }
    if (value.kind == AuthorityJobKind::disconnect) {
        if (!slot.cleanup_inflight || slot.cleanup.key.client_id != value.key.client_id ||
            slot.cleanup.key.request_id != value.key.request_id ||
            slot.cleanup.kind != value.kind) {
            slot.state = SlotState::fatal_unresolved;
            enter_fatal();
            return;
        }
        slot.cleanup_inflight = false;
        if (slot.session && slot.session->client_id() != 0U) {
            const auto reply = authority_owner_.authority().complete(value);
            if (reply.cleanup_required && value.error != Error::OK) {
                slot.state = SlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
        }
        if (value.error != Error::OK) {
            if (!fatal_ && slot.state == SlotState::tombstone &&
                value.error == Error::NOT_READY &&
                slot.quiesced_sticky_cleanup_error != Error::OK) {
                finish_slot(index);
                return;
            }
            slot.state = SlotState::fatal_unresolved;
            enter_fatal();
            return;
        }
        if (slot.session) slot.session.reset();
        finish_slot(index);
        return;
    }

    if (!slot.request_inflight) {
        if (slot.last_completion_valid && slot.last_key.client_id == value.key.client_id &&
            slot.last_key.request_id == value.key.request_id && slot.last_kind == value.kind)
            return;
        slot.state = SlotState::fatal_unresolved;
        enter_fatal();
        return;
    }
    if (slot.inflight_key.client_id != value.key.client_id ||
        slot.inflight_key.request_id != value.key.request_id ||
        slot.inflight_kind != value.kind) {
        slot.state = SlotState::fatal_unresolved;
        enter_fatal();
        return;
    }
    slot.request_inflight = false;
    slot.last_key = value.key;
    slot.last_kind = value.kind;
    slot.last_completion_valid = true;
    if (slot.inflight != 0U) --slot.inflight;
    if (fatal_) {
        if (value.kind == AuthorityJobKind::acquire && value.error == Error::OK) {
            slot.cleanup = AuthorityJob{};
            slot.cleanup.kind = AuthorityJobKind::disconnect;
            slot.cleanup.key = value.key;
            slot.cleanup.client_id = value.key.client_id;
            slot.state = SlotState::fatal_unresolved;
        }
        return;
    }
    if (!slot.session || slot.session->client_id() == 0U) {
        if (slot.state == SlotState::tombstone && slot.inflight == 0U) advance();
        return;
    }

    const auto reply = authority_owner_.authority().complete(value);
    if (reply.action == dtv::userland::wire::InboundAction::respond) {
        slot.response = reply;
        slot.response_ready = true;
    }
    if (reply.cleanup_required) {
        if (reply.cleanup.kind == AuthorityJobKind::disconnect) {
            slot.deferred_session = true;
            retain_cleanup(reply.cleanup, index);
        } else if (!queue_job(reply.cleanup, index)) {
            slot.cleanup = reply.cleanup;
            slot.cleanup_pending = true;
            slot.cleanup_accepted = false;
        }
    }
}

void HostAuthorityBridge::finish_slot(std::size_t index) noexcept
{
    Slot& slot = slots_[index];
    slot.session.reset();
    slot.streams = {};
    slot.cleanup = AuthorityJob{};
    slot.response = {};
    slot.client_id = 0U;
    slot.inflight = 0U;
    slot.inflight_key = {};
    slot.inflight_kind = AuthorityJobKind::tune;
    slot.last_key = {};
    slot.last_kind = AuthorityJobKind::tune;
    slot.state = SlotState::free;
    slot.request_inflight = false;
    slot.last_completion_valid = false;
    slot.cleanup_pending = false;
    slot.cleanup_accepted = false;
    slot.cleanup_inflight = false;
    slot.card_cleanup_done = false;
    slot.card_cleanup_inflight = false;
    slot.deferred_session = false;
    slot.close_pending = false;
    slot.response_ready = false;
    slot.quiesced_sticky_cleanup_error = Error::OK;
}

void HostAuthorityBridge::close_all() noexcept
{
    if (!slots_) return;
    for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
        if (slots_[i].state == SlotState::active) (void)close_session(static_cast<std::uint8_t>(i));
    }
}

void HostAuthorityBridge::close_all_data() noexcept
{
    for (std::size_t i = 0U; i < data_slots_.size(); ++i) {
        DataSlot& slot = data_slots_[i];
        if (slot.state != DataSlotState::active) continue;
        if (slot.ticket.stream_id == 0U) {
            slot.connection_open = false;
            slot.state = DataSlotState::free;
            continue;
        }
        slot.connection_open = false;
        slot.state = DataSlotState::tombstone;
        if (!slot.attach_pending && slot.prepared &&
            !queue_stream_cleanup(i) && workers_.error() != Error::OK)
            enter_fatal();
    }
}

void HostAuthorityBridge::close_owned_streams(std::size_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return;
    Slot& owner = slots_[index];
    for (std::size_t data_index = 0U; data_index < data_slots_.size(); ++data_index) {
        DataSlot& data = data_slots_[data_index];
        if (data.ticket.stream_id == 0U || data.ticket.client_id != owner.client_id)
            continue;
        data.connection_open = false;
        data.state = DataSlotState::tombstone;
        data.attach_result = HostAuthorityAttachResult{};
        if (data.attach_pending || data.cleanup_pending || data.cleanup_inflight) continue;
        if (data.prepared) {
            if (!queue_stream_cleanup(data_index) && workers_.error() != Error::OK)
                enter_fatal();
            continue;
        }
        if (data.stream_end.ready) {
            const auto released = tuner_service_.authority_release_stream_final_v1(
                data.ticket);
            if (!released) {
                data.state = DataSlotState::fatal_unresolved;
                enter_fatal();
                return;
            }
            forget_stream(data.ticket.client_id, data.ticket.stream_id);
            data = DataSlot{data.generation, DataSlotState::free};
            continue;
        }
        const auto aborted = tuner_service_.authority_abort_attach_v1(
            data.ticket, Error::OK);
        if (!aborted && aborted.error() != Error::NOT_FOUND) {
            data.state = DataSlotState::fatal_unresolved;
            enter_fatal();
            return;
        }
        if (!queue_stream_cleanup(data_index) && workers_.error() != Error::OK)
            enter_fatal();
    }
    for (std::size_t stream_index = 0U; stream_index < owner.streams.size();
         ++stream_index) {
        auto& stream = owner.streams[stream_index];
        if (!stream.active) continue;
        bool found_data = false;
        for (std::size_t data_index = 0U; data_index < data_slots_.size(); ++data_index) {
            DataSlot& data = data_slots_[data_index];
            if (data.ticket.client_id != owner.client_id ||
                data.ticket.stream_id != stream.stream_id) continue;
            found_data = true;
            data.connection_open = false;
            data.state = DataSlotState::tombstone;
            data.attach_result = HostAuthorityAttachResult{};
            if (!data.attach_pending && data.prepared &&
                !queue_stream_cleanup(data_index) && workers_.error() != Error::OK)
                enter_fatal();
            break;
        }
        if (found_data) continue;

        const auto stopped = tuner_service_.authority_stop_stream_v1(
            owner.client_id, stream.lease_id, stream.stream_id);
        if (stopped && stopped.value().quiesced &&
            stopped.value().stream_id == stream.stream_id) {
            if (stopped.value().cleanup_error != Error::OK)
                owner.quiesced_sticky_cleanup_error = stopped.value().cleanup_error;
            stream = Slot::StreamOwner{};
            continue;
        }
        if (!stopped && stopped.error() == Error::NOT_FOUND) {
            const auto last = tuner_service_.authority_stream_last_terminal_v1(
                owner.client_id, stream.lease_id);
            if (last && last.value().stream_id == stream.stream_id &&
                last.value().quiesced) {
                if (last.value().cleanup_error != Error::OK)
                    owner.quiesced_sticky_cleanup_error = last.value().cleanup_error;
                stream = Slot::StreamOwner{};
                continue;
            }
        }
        enter_fatal();
        return;
    }
}

bool HostAuthorityBridge::has_owned_streams(std::size_t index) const noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return false;
    for (const auto& stream : slots_[index].streams)
        if (stream.active) return true;
    const std::uint64_t client_id = slots_[index].client_id;
    for (const DataSlot& data : data_slots_)
        if (data.ticket.stream_id != 0U && data.ticket.client_id == client_id)
            return true;
    return false;
}

void HostAuthorityBridge::finish_control_close(std::size_t index) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity) return;
    Slot& slot = slots_[index];
    if (!slot.close_pending || has_owned_streams(index)) return;
    if (slot.cleanup.kind == AuthorityJobKind::disconnect && slot.cleanup_pending) {
        slot.close_pending = false;
        return;
    }
    if (!slot.session) {
        slot.state = SlotState::fatal_unresolved;
        enter_fatal();
        return;
    }
    const AuthorityJob cleanup = authority_owner_.authority().close(*slot.session);
    if (cleanup.kind != AuthorityJobKind::disconnect) {
        slot.state = SlotState::fatal_unresolved;
        enter_fatal();
        return;
    }
    slot.close_pending = false;
    if (cleanup.deferred) {
        slot.deferred_session = true;
        return;
    }
    slot.session.reset();
    retain_cleanup(cleanup, index);
}

void HostAuthorityBridge::forget_stream(std::uint64_t client_id,
                                        std::uint64_t stream_id) noexcept
{
    if (!slots_ || client_id == 0U || stream_id == 0U) return;
    const std::size_t index = find_slot(client_id);
    if (index == kHostAuthoritySessionCapacity) return;
    for (auto& stream : slots_[index].streams) {
        if (stream.active && stream.stream_id == stream_id)
            stream = Slot::StreamOwner{};
    }
}

bool HostAuthorityBridge::remember_stream(std::size_t index,
                                          std::uint64_t lease_id,
                                          std::uint64_t stream_id) noexcept
{
    if (!slots_ || index >= kHostAuthoritySessionCapacity ||
        lease_id == 0U || stream_id == 0U) return false;
    Slot& slot = slots_[index];
    for (auto& stream : slot.streams) {
        if (stream.active && stream.lease_id == lease_id) {
            stream.stream_id = stream_id;
            return true;
        }
    }
    for (auto& stream : slot.streams) {
        if (stream.active) continue;
        stream.lease_id = lease_id;
        stream.stream_id = stream_id;
        stream.active = true;
        return true;
    }
    return false;
}

bool HostAuthorityBridge::queue_stream_cleanup(std::size_t index) noexcept
{
    if (index >= data_slots_.size()) return false;
    DataSlot& data = data_slots_[index];
    if (data.ticket.stream_id == 0U) return true;
    if (data.cleanup_inflight) return true;
    ControlWorkerTask task{};
    task.kind = ControlWorkerTaskKind::cleanup;
    task.operation = ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1;
    task.stream_ticket = data.ticket;
    task.receiver = static_cast<std::uint8_t>(data.ticket.slot);
    task.client_id = data.ticket.client_id;
    task.lease_id = data.lease_id;
    task.data_slot = static_cast<std::uint8_t>(index);
    task.data_generation = data.generation;
    task.authority_stream = true;
    const auto lane = workers_.authority_lane(task.receiver, false);
    if (!lane || !workers_.submit(lane.value(), task)) {
        data.cleanup_pending = true;
        return false;
    }
    data.cleanup_pending = false;
    data.cleanup_inflight = true;
    return true;
}

void HostAuthorityBridge::stop_ingresses() noexcept
{
    if (!control_ingress_stopped_) {
        if (stop_control_ingress_ != nullptr)
            stop_control_ingress_(stop_control_ingress_context_);
        control_ingress_stopped_ = true;
    }
    if (!data_ingress_stopped_) {
        if (stop_data_ingress_ != nullptr &&
            (stop_data_ingress_ != stop_control_ingress_ ||
             stop_data_ingress_context_ != stop_control_ingress_context_))
            stop_data_ingress_(stop_data_ingress_context_);
        data_ingress_stopped_ = true;
    }
    if (!other_ingress_stopped_) {
        if (stop_other_ingress_ != nullptr &&
            (stop_other_ingress_ != stop_control_ingress_ ||
             stop_other_ingress_context_ != stop_control_ingress_context_) &&
            (stop_other_ingress_ != stop_data_ingress_ ||
             stop_other_ingress_context_ != stop_data_ingress_context_))
            stop_other_ingress_(stop_other_ingress_context_);
        other_ingress_stopped_ = true;
    }
}

void HostAuthorityBridge::enter_fatal() noexcept
{
    if (!fatal_) {
        accepting_ = false;
        fatal_ = true;
        stop_ingresses();
        close_all();
        close_all_data();
    }
    stop_ingresses();
    if (!stop_requested_) {
        stop_requested_ = true;
        (void)workers_.request_stop();
    }
}

Error HostAuthorityBridge::begin_shutdown(IngressStop stop_other_ingress,
                                           void* context) noexcept
{
    if (stopped_) return Error::NOT_READY;
    if (shutting_down_) return Error::OK;
    if (stop_other_ingress != nullptr) {
        stop_other_ingress_ = stop_other_ingress;
        stop_other_ingress_context_ = context;
    }
    stop_ingresses();
    accepting_ = false;
    shutting_down_ = true;
    close_all();
    close_all_data();
    advance();
    return fatal_ ? Error::INTERNAL : Error::OK;
}

Error HostAuthorityBridge::poll() noexcept
{
    return poll(tuner_service_.monotonic_ms());
}

Error HostAuthorityBridge::poll(std::uint64_t now_ms) noexcept
{
    if (workers_.error() != Error::OK) enter_fatal();
    workers_.drain_wake();
    for (std::size_t lane = 0U; lane < 3U; ++lane) {
        if (deferred_legacy_valid_[lane]) {
            if (legacy_completion_sink_ == nullptr) continue;
            legacy_completion_sink_(legacy_completion_context_, deferred_legacy_[lane]);
            deferred_legacy_[lane] = ControlWorkerCompletion{};
            deferred_legacy_valid_[lane] = false;
        }
        ControlWorkerCompletion completion{};
        const auto worker_lane = static_cast<ControlWorkerLane>(lane);
        while (workers_.try_pop(worker_lane, completion)) {
            const bool legacy = completion.operation !=
                ControlWorkerOperation::tuner_authority_job;
            consume(completion, lane);
            if (legacy && legacy_completion_sink_ == nullptr) break;
        }
    }
    const auto admission = authority_owner_.authority().retry_pending_admission(
        now_ms, *this);
    bool admission_closed = false;
    if (admission.close_required && admission.session != nullptr &&
        admission.reply.action == dtv::userland::wire::InboundAction::close) {
        std::size_t index = kHostAuthoritySessionCapacity;
        for (std::size_t i = 0U; slots_ && i < kHostAuthoritySessionCapacity; ++i) {
            if (slots_[i].session && &*slots_[i].session == admission.session) {
                index = i;
                break;
            }
        }
        if (index == kHostAuthoritySessionCapacity ||
            slots_[index].state != SlotState::active) {
            enter_fatal();
        } else {
            close_slot(index, admission.reply);
            admission_closed = true;
        }
    } else if (admission.reply_ready && admission.session != nullptr) {
        const std::size_t index = find_slot(admission.session->client_id());
        if (!slots_ || index == kHostAuthoritySessionCapacity ||
            slots_[index].state != SlotState::active ||
            !slots_[index].session.has_value() || slots_[index].response_ready) {
            enter_fatal();
        } else {
            slots_[index].response = admission.reply;
            slots_[index].response_ready = true;
        }
    }
    if (!admission_closed) advance();
    return stopped_ ? shutdown_result() : Error::OK;
}

std::uint32_t HostAuthorityBridge::admission_wait_limit_ms(
    std::uint64_t now_ms, std::uint32_t max_wait_ms) const noexcept
{
    return authority_owner_.authority().admission_wait_limit_ms(now_ms, max_wait_ms);
}

void HostAuthorityBridge::advance() noexcept
{
    if (slots_) for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
        Slot& slot = slots_[i];
        if (slot.state != SlotState::free && !slot.close_pending && slot.inflight == 0U &&
            slot.cleanup_pending && !fatal_) {
            if (!queue_job(slot.cleanup, i)) {
                if (workers_.error() != Error::OK) enter_fatal();
            }
        }
    }
    for (std::size_t i = 0U; i < data_slots_.size(); ++i) {
        DataSlot& slot = data_slots_[i];
        if (slot.state == DataSlotState::tombstone && slot.cleanup_pending &&
            !slot.attach_pending && !slot.cleanup_inflight && !fatal_) {
            if (!queue_stream_cleanup(i) && workers_.error() != Error::OK)
                enter_fatal();
        }
    }
    if (slots_ && !fatal_) {
        for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
            Slot& slot = slots_[i];
            if (!slot.close_pending) continue;
            close_owned_streams(i);
            finish_control_close(i);
        }
    }
    if (fatal_) {
        if (!stop_requested_) {
            stop_requested_ = true;
            (void)workers_.request_stop();
        }
    } else if (shutting_down_ && !stop_requested_) {
        bool owned = false;
        if (slots_) {
            for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i)
                owned = owned || slots_[i].state != SlotState::free;
        }
        for (const DataSlot& slot : data_slots_)
            owned = owned || (slot.state != DataSlotState::free &&
                              slot.state != DataSlotState::retired);
        for (bool deferred : deferred_legacy_valid_) owned = owned || deferred;
        if (!owned && workers_.idle()) {
            stop_requested_ = true;
            (void)workers_.request_stop();
        }
    }
    if (stop_requested_ && workers_.live_thread_count() == 0U) {
        bool pending = false;
        for (std::size_t lane = 0U; lane < 3U; ++lane) {
            if (deferred_legacy_valid_[lane] && legacy_completion_sink_ == nullptr) {
                pending = true;
                continue;
            }
            if (deferred_legacy_valid_[lane]) {
                legacy_completion_sink_(legacy_completion_context_, deferred_legacy_[lane]);
                deferred_legacy_[lane] = ControlWorkerCompletion{};
                deferred_legacy_valid_[lane] = false;
            }
            ControlWorkerCompletion completion{};
            while (workers_.try_pop(static_cast<ControlWorkerLane>(lane), completion)) {
                const bool legacy = completion.operation !=
                    ControlWorkerOperation::tuner_authority_job;
                consume(completion, lane);
                if (legacy && legacy_completion_sink_ == nullptr) {
                    pending = true;
                    break;
                }
            }
        }
        if (!pending && workers_.live_thread_count() == 0U) {
            (void)workers_.join();
            stopped_ = true;
            if (slots_) for (std::size_t i = 0U; i < kHostAuthoritySessionCapacity; ++i) {
                if (slots_[i].state == SlotState::tombstone)
                    slots_[i].state = SlotState::fatal_unresolved;
            }
        }
    }
}

HostAuthorityBridgeCounts HostAuthorityBridge::counts() const noexcept
{
    HostAuthorityBridgeCounts result{};
    for (std::size_t i = 0U; slots_ && i < kHostAuthoritySessionCapacity; ++i) {
        const Slot& slot = slots_[i];
        switch (slot.state) {
        case SlotState::free: ++result.free; break;
        case SlotState::active: ++result.active; break;
        case SlotState::tombstone: ++result.tombstone; break;
        case SlotState::fatal_unresolved: ++result.fatal_unresolved; break;
        }
    }
    return result;
}

std::uint64_t HostAuthorityBridge::client_id(std::uint8_t index) const noexcept
{
    return slots_ && index < kHostAuthoritySessionCapacity ? slots_[index].client_id : 0U;
}

Error HostAuthorityBridge::shutdown_result() const noexcept
{
    if (!stopped_) return Error::NOT_READY;
    return fatal_ || counts().fatal_unresolved != 0U ? Error::INTERNAL : Error::OK;
}

}  // namespace px4::userland::ipc::posix
