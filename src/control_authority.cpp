// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/control_authority.h"

#include <cassert>
#include <limits>

namespace dtv::userland::detail {
namespace {

bool receiver_exists(const wire::ListResponse& list, std::uint64_t id) noexcept
{
    if (!list.receivers.present) return false;
    for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
        if (list.receivers.value.items[i].id == id) return true;
    }
    return false;
}

bool reader_exists(const wire::ListResponse& list, std::uint16_t id) noexcept
{
    return list.readers.present && list.readers.value.count == 1U &&
           list.readers.value.items[0U].id == id;
}

std::uint16_t card_reader_id(const wire::ControlRequest& request) noexcept
{
    switch (request.kind) {
    case wire::ControlRequestKind::card_status: return request.card_status.key.reader_id;
    case wire::ControlRequestKind::card_connect: return request.card_connect.key.reader_id;
    case wire::ControlRequestKind::card_reconnect: return request.card_reconnect.key.reader_id;
    case wire::ControlRequestKind::card_disconnect: return request.card_disconnect.key.reader_id;
    case wire::ControlRequestKind::card_reset: return request.card_reset.key.reader_id;
    case wire::ControlRequestKind::card_transmit: return request.card_transmit.key.reader_id;
    case wire::ControlRequestKind::begin_transaction:
        return request.begin_transaction.key.reader_id;
    case wire::ControlRequestKind::end_transaction:
        return request.end_transaction.key.reader_id;
    case wire::ControlRequestKind::card_handle_status:
        return request.card_handle_status.key.reader_id;
    default: return UINT16_MAX;
    }
}

const char* card_operation_name(wire::ControlRequestKind kind) noexcept
{
    switch (kind) {
    case wire::ControlRequestKind::card_status: return "card_status";
    case wire::ControlRequestKind::card_connect: return "card_connect";
    case wire::ControlRequestKind::card_reconnect: return "card_reconnect";
    case wire::ControlRequestKind::card_disconnect: return "card_disconnect";
    case wire::ControlRequestKind::card_reset: return "card_reset";
    case wire::ControlRequestKind::card_transmit: return "card_transmit";
    case wire::ControlRequestKind::begin_transaction: return "begin_transaction";
    case wire::ControlRequestKind::end_transaction: return "end_transaction";
    case wire::ControlRequestKind::card_handle_status: return "card_handle_status";
    default: return "card";
    }
}

bool supports_lnb_power(const wire::ListResponse& list) noexcept
{
    if (!list.receivers.present) return false;
    for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
        const wire::Capabilities& capabilities = list.receivers.value.items[i].capabilities;
        if ((capabilities.controls & wire::kControlBitLnb) == 0U) continue;
        bool supports_off = false;
        bool supports_on = false;
        for (std::size_t j = 0U; j < capabilities.lnb.values.count; ++j) {
            supports_off = supports_off || capabilities.lnb.values.items[j] == 0U;
            supports_on = supports_on || capabilities.lnb.values.items[j] == 15U;
        }
        if (supports_off && supports_on) return true;
    }
    return false;
}

bool same_producer(const wire::Producer& left, const wire::Producer& right) noexcept
{
    std::array<std::uint8_t, 128> left_bytes{};
    std::array<std::uint8_t, 128> right_bytes{};
    const auto left_size = wire::encode_producer(left,
        MutableByteView{left_bytes.data(), left_bytes.size()});
    const auto right_size = wire::encode_producer(right,
        MutableByteView{right_bytes.data(), right_bytes.size()});
    if (!left_size || !right_size || left_size.value() != right_size.value()) return false;
    for (std::size_t i = 0U; i < left_size.value(); ++i)
        if (left_bytes[i] != right_bytes[i]) return false;
    return true;
}

template <std::size_t N>
bool same_text(const wire::WString<N>& left, const wire::WString<N>& right) noexcept
{
    if (left.length != right.length || left.length > N) return false;
    for (std::size_t i = 0U; i < left.length; ++i)
        if (left.bytes[i] != right.bytes[i]) return false;
    return true;
}

template <typename T>
bool same_optional(const wire::Optional<T>& left,
                   const wire::Optional<T>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || left.value == right.value);
}

template <std::size_t N>
bool same_optional_text(const wire::Optional<wire::WString<N>>& left,
                        const wire::Optional<wire::WString<N>>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || same_text(left.value, right.value));
}

bool same_error(const wire::ErrorRecord& left,
                const wire::ErrorRecord& right) noexcept
{
    return left.code == right.code && same_text(left.operation, right.operation) &&
           same_text(left.detail, right.detail);
}

bool same_optional_error(const wire::Optional<wire::ErrorRecord>& left,
                         const wire::Optional<wire::ErrorRecord>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || same_error(left.value, right.value));
}

bool same_function_identity(const wire::UsbFunction& left,
                            const wire::UsbFunction& right) noexcept
{
    return left.id == right.id && left.vid == right.vid && left.pid == right.pid &&
           left.mode == right.mode &&
           same_optional_text(left.location.usb_path, right.location.usb_path) &&
           same_optional(left.location.bus, right.location.bus) &&
           same_optional(left.location.address, right.location.address) &&
           left.serial_state == right.serial_state &&
           same_optional_text(left.serial, right.serial) &&
           left.model.present == right.model.present &&
           (!left.model.present || same_text(left.model.value.name,
                                             right.model.value.name)) &&
           left.identification_reason == right.identification_reason &&
           same_optional_error(left.last_error, right.last_error);
}

const wire::ReceiverDescriptor* receiver_descriptor(const wire::ListResponse& list,
                                                      std::uint64_t id) noexcept
{
    if (!list.receivers.present) return nullptr;
    for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
        if (list.receivers.value.items[i].id == id) return &list.receivers.value.items[i];
    }
    return nullptr;
}

Error validate_profile(const wire::ReceiverDescriptor& descriptor,
                       const wire::TuneRequest& request, std::uint64_t features) noexcept
{
    const std::uint8_t system_bit = request.system == wire::System::isdb_t ? 1U : 2U;
    if ((descriptor.systems.mask & system_bit) == 0U) return Error::UNSUPPORTED;
    if (request.lnb_voltage.present) {
        if ((descriptor.capabilities.controls & wire::kControlBitLnb) == 0U)
            return Error::UNSUPPORTED;
        bool found = false;
        for (std::size_t i = 0U; i < descriptor.capabilities.lnb.values.count; ++i) {
            if (descriptor.capabilities.lnb.values.items[i] == request.lnb_voltage.value)
                found = true;
        }
        if (!found) return Error::UNSUPPORTED;
    }
    if (request.lna.present || request.pids.present) return Error::UNSUPPORTED;
    (void)features;
    if (request.selection == wire::Selection::tsid &&
        request.system != wire::System::isdb_s)
        return Error::INVALID_ARGUMENT;
    if (request.selection == wire::Selection::slot &&
        request.system != wire::System::isdb_s)
        return Error::INVALID_ARGUMENT;
    return Error::OK;
}

}  // namespace

ControlServiceSession::ControlServiceSession(ControlAuthority& authority) noexcept
    : authority_(authority), connection_(authority.server())
{
}

ControlServiceSession::~ControlServiceSession() noexcept
{
    // The owner must call ControlAuthority::close() and drain the returned job
    // before destroying a live session.
    assert(!connected() && !pending_);
}

ControlAuthority::ControlAuthority(const wire::ServerHello& server,
                                   const wire::ListResponse& canonical,
                                   ReceiverEngine& engine,
                                   TuneValidator validate_tune) noexcept
    : server_(server), canonical_(canonical), engine_(engine), validate_tune_(validate_tune)
{
    valid_ = server_.epoch == canonical_.epoch &&
             wire::validate_list_response(canonical_) == Error::OK &&
             same_producer(server_.producer, canonical_.producer);
}

ControlAuthority::~ControlAuthority() noexcept
{
    // Sessions and the executor are borrowed owners and outlive this authority.
    assert(sessions_ == nullptr);
}

ControlServiceSession* ControlAuthority::find_session(std::uint64_t client_id) const noexcept
{
    for (ControlServiceSession* session = sessions_; session != nullptr;
         session = session->next_) {
        if (session->client_id_ == client_id) return session;
    }
    return nullptr;
}

Error ControlAuthority::open(ControlServiceSession& session) noexcept
{
    if (!valid_ || (presence_cache_enabled_ &&
        (!presence_cache_ready_ || presence_epoch_invalidated_)) ||
        &session.authority_ != this || session.connected() || session.next_ != nullptr ||
        session.used_)
        return Error::INVALID_ARGUMENT;
    const auto client_id = client_ids_.next();
    if (!client_id) return Error::INVALID_ARGUMENT;
    session.client_id_ = client_id.value();
    session_opened_ = true;
    session.closed_ = false;
    session.used_ = true;
    session.next_ = sessions_;
    sessions_ = &session;
    return Error::OK;
}

Error ControlAuthority::enable_presence_cache() noexcept
{
    if (!valid_ || presence_cache_enabled_ || session_opened_)
        return Error::INVALID_ARGUMENT;
    if (canonical_.receivers.present) {
        for (std::size_t i = 0U; i < canonical_.receivers.value.count; ++i) {
            const auto& receiver = canonical_.receivers.value.items[i];
            if (!receiver.binding.present) return Error::UNSUPPORTED;
            bool found = false;
            for (std::size_t j = 0U; j < canonical_.functions.count; ++j)
                found = found || canonical_.functions.items[j].id ==
                    receiver.binding.value.function_id;
            if (!found) return Error::INVALID_ARGUMENT;
        }
    }
    presence_cache_enabled_ = true;
    return Error::OK;
}

Error ControlAuthority::update_presence(
    const FunctionPresenceSnapshot& snapshot) noexcept
{
    if (!presence_cache_enabled_ || presence_epoch_invalidated_)
        return Error::INVALID_ARGUMENT;
    const auto reject = [this]() noexcept {
        if (presence_cache_ready_) presence_epoch_invalidated_ = true;
        return Error::INVALID_ARGUMENT;
    };
    if (snapshot.epoch != server_.epoch ||
        snapshot.functions.count != canonical_.functions.count ||
        (presence_cache_ready_ && snapshot.sequence <= presence_sequence_))
        return reject();

    std::array<bool, wire::kMaxFunctions> seen{};
    for (std::size_t i = 0U; i < snapshot.functions.count; ++i) {
        const auto& observed = snapshot.functions.items[i];
        std::size_t slot = canonical_.functions.count;
        for (std::size_t j = 0U; j < canonical_.functions.count; ++j) {
            if (canonical_.functions.items[j].id == observed.function.id) {
                slot = j;
                break;
            }
        }
        if (slot == canonical_.functions.count || seen[slot] ||
            !same_function_identity(canonical_.functions.items[slot], observed.function) ||
            (observed.function.present != observed.connection_generation.present) ||
            (presence_cache_ready_ && !canonical_.functions.items[slot].present &&
             observed.function.present) ||
            (presence_cache_ready_ && canonical_.functions.items[slot].present &&
             observed.function.present &&
             (!presence_generations_[slot].present ||
              presence_generations_[slot].value !=
                  observed.connection_generation.value)))
            return reject();
        seen[slot] = true;
    }
    for (std::size_t i = 0U; i < canonical_.functions.count; ++i)
        if (!seen[i]) return reject();

    for (std::size_t i = 0U; i < snapshot.functions.count; ++i) {
        const auto& observed = snapshot.functions.items[i];
        for (std::size_t j = 0U; j < canonical_.functions.count; ++j) {
            auto& current = canonical_.functions.items[j];
            if (current.id != observed.function.id) continue;
            current.present = observed.function.present;
            if (observed.connection_generation.present)
                presence_generations_[j] = observed.connection_generation;
        }
    }
    presence_sequence_ = snapshot.sequence;
    presence_cache_ready_ = true;
    return Error::OK;
}

Error ControlAuthority::invalidate_presence_epoch() noexcept
{
    if (!presence_cache_enabled_ || !presence_cache_ready_ || presence_epoch_invalidated_)
        return Error::NOT_READY;
    presence_epoch_invalidated_ = true;
    return Error::OK;
}

Error ControlAuthority::response_owner_check(
    const ControlServiceSession& session) const noexcept
{
    if (&session.authority_ != this || !session.connected() ||
        find_session(session.client_id_) != &session)
        return Error::NOT_FOUND;
    if (presence_cache_enabled_ && presence_epoch_invalidated_)
        return Error::NOT_READY;
    return Error::OK;
}

Error ControlAuthority::configure_stream_contract(std::uint64_t engine_epoch,
                                                  std::uint64_t arm_budget_ms) noexcept
{
    if (engine_epoch == 0U || arm_budget_ms == 0U) return Error::INVALID_ARGUMENT;
    if (stream_engine_epoch_ != 0U) return Error::BUSY;
    stream_engine_epoch_ = engine_epoch;
    stream_arm_budget_ms_ = arm_budget_ms;
    return Error::OK;
}

AuthorityReply ControlAuthority::reply(ControlServiceSession& session, std::size_t size,
                                       bool close_after) noexcept
{
    AuthorityReply result;
    result.action = wire::InboundAction::respond;
    result.frame = ByteView{session.response_.data(), size};
    result.close_after_reply = close_after;
    session.close_after_reply_ = close_after;
    return result;
}

AuthorityReply ControlAuthority::error_reply(ControlServiceSession& session,
                                              std::uint16_t opcode,
                                              std::uint32_t request_id, Error error,
                                              const char* operation) noexcept
{
    const auto written = wire::encode_error_frame(opcode, request_id, server_.epoch,
        static_cast<std::uint32_t>(error), operation, wire::Scope{},
        MutableByteView{session.response_.data(), session.response_.size()});
    if (!written) return {};
    return reply(session, written.value());
}

bool ControlAuthority::remember_lease(ControlServiceSession& session,
                                      std::uint64_t lease_id, ReceiverId receiver) noexcept
{
    if (session.lease_count_ == session.lease_ids_.size()) return false;
    session.lease_ids_[session.lease_count_] = lease_id;
    session.lease_receivers_[session.lease_count_] = receiver;
    ++session.lease_count_;
    return true;
}

void ControlAuthority::forget_lease(ControlServiceSession& session,
                                    std::uint64_t lease_id) noexcept
{
    for (std::size_t i = 0U; i < session.lease_count_; ++i) {
        if (session.lease_ids_[i] != lease_id) continue;
        --session.lease_count_;
        session.lease_ids_[i] = session.lease_ids_[session.lease_count_];
        session.lease_receivers_[i] = session.lease_receivers_[session.lease_count_];
        session.lease_ids_[session.lease_count_] = 0U;
        session.lease_receivers_[session.lease_count_] = 0U;
        return;
    }
}

bool ControlAuthority::queue_admission(ControlServiceSession& session) noexcept
{
    if (admission_queue_count_ == admission_queue_.size()) return false;
    const std::size_t tail = (admission_queue_head_ + admission_queue_count_) %
                             admission_queue_.size();
    admission_queue_[tail] = &session;
    ++admission_queue_count_;
    return true;
}

void ControlAuthority::remove_admission(ControlServiceSession& session) noexcept
{
    for (std::size_t i = 0U; i < admission_queue_count_; ++i) {
        const std::size_t index = (admission_queue_head_ + i) % admission_queue_.size();
        if (admission_queue_[index] != &session) continue;
        for (std::size_t j = i; j + 1U < admission_queue_count_; ++j) {
            const std::size_t current = (admission_queue_head_ + j) % admission_queue_.size();
            const std::size_t next = (admission_queue_head_ + j + 1U) % admission_queue_.size();
            admission_queue_[current] = admission_queue_[next];
        }
        const std::size_t tail = (admission_queue_head_ + admission_queue_count_ - 1U) %
                                 admission_queue_.size();
        admission_queue_[tail] = nullptr;
        --admission_queue_count_;
        return;
    }
}

AuthorityReply ControlAuthority::submit_admitted_tune(
    ControlServiceSession& session, const wire::ControlRequest& request,
    const AdmittedTune& ticket, ControlAuthorityExecutor& executor) noexcept
{
    AuthorityJob job;
    job.kind = AuthorityJobKind::tune;
    job.key = SessionKey{session.client_id_, request.request_id};
    job.tune = ticket;
    if (!executor.submit(job)) {
        (void)engine_.abort_unstarted_tune(ticket, Error::RESOURCE_LIMIT);
        session.pending_ = false;
        session.pending_admission_ = false;
        return error_reply(session, request.opcode, request.request_id,
                           Error::RESOURCE_LIMIT, "tune");
    }
    session.pending_ = true;
    session.pending_admission_ = false;
    session.pending_request_id_ = request.request_id;
    session.pending_lease_id_ = request.tune.lease_id;
    session.pending_tune_revision_ = ticket.tune_revision;
    session.pending_kind_ = AuthorityJobKind::tune;
    session.pending_requested_hz_ = request.tune.frequency_hz;
    session.pending_selection_ = request.tune.selection;
    session.pending_selection_value_ = request.tune.selection_value;
    AuthorityReply queued;
    queued.action = wire::InboundAction::dispatch;
    queued.queued = true;
    return queued;
}

AuthorityReply ControlAuthority::accept(ControlServiceSession& session,
                                        const wire::Frame& frame,
                                        const wire::Context& context,
                                        const wire::Snapshot& snapshot,
                                        std::uint64_t accepted_at_ms,
                                        ControlAuthorityExecutor& executor) noexcept
{
    if (!session.connected() || find_session(session.client_id_) != &session) return {};
    if (presence_cache_enabled_ && presence_epoch_invalidated_) {
        AuthorityReply result;
        result.action = wire::InboundAction::close;
        result.cleanup = close_cleanup(session);
        if (!executor.submit(result.cleanup)) result.cleanup_required = true;
        return result;
    }
    wire::Inbound inbound = session.connection_.accept(frame, session.connection_request_,
        MutableByteView{session.response_.data(), session.response_.size()});
    if (inbound.action == wire::InboundAction::close) {
        AuthorityReply result;
        result.action = wire::InboundAction::close;
        result.cleanup = close_cleanup(session);
        if (!executor.submit(result.cleanup)) result.cleanup_required = true;
        return result;
    }
    if (inbound.action == wire::InboundAction::respond)
        return reply(session, inbound.reply_size, inbound.close_after_reply);
    return dispatch(session, session.connection_request_, context, snapshot,
                    accepted_at_ms, executor);
}

AuthorityPollResult ControlAuthority::retry_pending_admission(
    std::uint64_t now_ms, ControlAuthorityExecutor& executor) noexcept
{
    AuthorityPollResult result;
    ControlServiceSession* session = nullptr;
    while (admission_queue_count_ != 0U) {
        session = admission_queue_[admission_queue_head_];
        admission_queue_[admission_queue_head_] = nullptr;
        admission_queue_head_ = (admission_queue_head_ + 1U) % admission_queue_.size();
        --admission_queue_count_;
        if (session != nullptr && session->pending_admission_ && session->connected() &&
            find_session(session->client_id_) == session)
            break;
        session = nullptr;
    }
    if (session == nullptr) return result;

    result.attempted = true;
    result.session = session;
    if (presence_cache_enabled_ && presence_epoch_invalidated_) {
        result.reply.action = wire::InboundAction::close;
        result.close_required = true;
        result.reply.cleanup = close_cleanup(*session);
        if (!executor.submit(result.reply.cleanup)) result.reply.cleanup_required = true;
        return result;
    }
    const wire::ControlRequest& request = session->connection_request_;
    const std::uint64_t accepted_at_ms = session->pending_admission_accepted_at_ms_;
    if (now_ms >= accepted_at_ms &&
        now_ms - accepted_at_ms >= session->pending_admission_request_.timeout_ms) {
        session->pending_admission_ = false;
        session->pending_ = false;
        result.reply = error_reply(*session, request.opcode, request.request_id,
                                   Error::TIMEOUT, "tune");
        result.reply_ready = true;
        return result;
    }

    TuneAdmissionFailure failure = TuneAdmissionFailure::none;
    const auto ticket = engine_.try_admit_tune_detailed(
        session->client_id_, session->pending_admission_request_,
        accepted_at_ms, failure);
    if (!ticket && failure == TuneAdmissionFailure::registry_contention) {
        if (!queue_admission(*session)) {
            session->pending_admission_ = false;
            session->pending_ = false;
            result.reply = error_reply(*session, request.opcode, request.request_id,
                                       Error::RESOURCE_LIMIT, "tune");
            result.reply_ready = true;
        }
        return result;
    }
    if (!ticket) {
        session->pending_admission_ = false;
        session->pending_ = false;
        result.reply = error_reply(*session, request.opcode, request.request_id,
                                   ticket.error(), "tune");
        result.reply_ready = true;
        return result;
    }
    result.reply = submit_admitted_tune(*session, request, ticket.value(), executor);
    result.reply_ready = result.reply.action == wire::InboundAction::respond;
    return result;
}

std::uint32_t ControlAuthority::admission_wait_limit_ms(
    std::uint64_t now_ms, std::uint32_t max_wait_ms) const noexcept
{
    std::uint64_t wait_ms = max_wait_ms;
    for (const ControlServiceSession* session = sessions_; session != nullptr;
         session = session->next_) {
        if (!session->pending_admission_) continue;
        const std::uint64_t accepted_at_ms =
            session->pending_admission_accepted_at_ms_;
        const std::uint64_t timeout_ms =
            session->pending_admission_request_.timeout_ms;
        const std::uint64_t elapsed_ms = now_ms >= accepted_at_ms
            ? now_ms - accepted_at_ms : 0U;
        if (elapsed_ms >= timeout_ms) return 0U;
        const std::uint64_t remaining_ms = timeout_ms - elapsed_ms;
        if (remaining_ms < wait_ms) wait_ms = remaining_ms;
    }
    return static_cast<std::uint32_t>(wait_ms);
}

void ControlAuthority::cancel_pending_admission(
    ControlServiceSession& session) noexcept
{
    if (&session.authority_ != this || !session.pending_admission_) return;
    remove_admission(session);
    session.pending_admission_ = false;
    session.pending_ = false;
}

AuthorityReply ControlAuthority::dispatch(ControlServiceSession& session,
                                           const wire::ControlRequest& request,
                                           const wire::Context& context,
                                           const wire::Snapshot& snapshot,
                                           std::uint64_t accepted_at_ms,
                                           ControlAuthorityExecutor& executor) noexcept
{
    const MutableByteView output{session.response_.data(), session.response_.size()};
    switch (request.kind) {
    case wire::ControlRequestKind::list: {
        const auto size = wire::encode_record_frame(wire::Kind::response, request.opcode,
            request.request_id, canonical_, &wire::encode_list_response, output);
        return size ? reply(session, size.value())
                    : error_reply(session, request.opcode, request.request_id,
                                  size.error(), "list");
    }
    case wire::ControlRequestKind::status: {
        const Error scope = wire::check_status_scope(canonical_, request.status.scope);
        if (scope != Error::OK)
            return error_reply(session, request.opcode, request.request_id, scope, "status");
        wire::StatusResponse& status = session.status_response_;
        Error projected = wire::build_status_shell(canonical_, context, snapshot,
                                                    request.status.scope, status);
        if (projected == Error::OK) {
            if (request.status.scope.kind == wire::ScopeKind::enclosure) {
                const auto lnb = executor.lnb_power_state();
                status.lnb_15v_enabled.present = lnb.present;
                if (lnb.present) status.lnb_15v_enabled.value = lnb.value;
            }
            const auto engine_status = engine_.status();
            if (!engine_status) projected = engine_status.error();
            else projected = project_receiver_engine_status(canonical_, engine_status.value(),
                                                              status);
            if (projected == Error::OK && presence_cache_enabled_) {
                for (std::size_t i = 0U; i < status.receivers.value.count; ++i) {
                    auto& receiver = status.receivers.value.items[i];
                    if (!receiver.descriptor.binding.present) {
                        projected = Error::UNSUPPORTED;
                        break;
                    }
                    bool found = false;
                    for (std::size_t j = 0U; j < canonical_.functions.count; ++j) {
                        const auto& function = canonical_.functions.items[j];
                        if (function.id != receiver.descriptor.binding.value.function_id)
                            continue;
                        receiver.present = function.present;
                        found = true;
                        break;
                    }
                    if (!found) {
                        projected = Error::INVALID_ARGUMENT;
                        break;
                    }
                }
            }
        }
        if (projected != Error::OK)
            return error_reply(session, request.opcode, request.request_id,
                               projected, "status");
        if (request.status.scope.kind == wire::ScopeKind::enclosure &&
            executor.supports_internal_card_observation()) {
            AuthorityJob job;
            job.kind = AuthorityJobKind::internal_card_status;
            job.key = SessionKey{session.client_id_, request.request_id};
            job.client_id = session.client_id_;
            if (!executor.submit(job))
                return error_reply(session, request.opcode, request.request_id,
                                   Error::RESOURCE_LIMIT, "status");
            session.pending_ = true;
            session.pending_request_id_ = request.request_id;
            session.pending_kind_ = AuthorityJobKind::internal_card_status;
            session.pending_status_opcode_ = request.opcode;
            AuthorityReply queued;
            queued.action = wire::InboundAction::dispatch;
            queued.queued = true;
            return queued;
        }
        const auto size = wire::encode_status_frame(request.request_id, status, canonical_, output);
        return size ? reply(session, size.value())
                    : error_reply(session, request.opcode, request.request_id,
                                  size.error(), "status");
    }
    case wire::ControlRequestKind::acquire: {
        const std::uint64_t receiver = request.acquire.receiver_id;
        if (!receiver_exists(canonical_, receiver))
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "acquire");
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "acquire");
        AuthorityJob job;
        job.kind = AuthorityJobKind::acquire;
        job.key = SessionKey{session.client_id_, request.request_id};
        job.client_id = session.client_id_;
        job.receiver = static_cast<ReceiverId>(receiver);
        if (!executor.submit(job))
            return error_reply(session, request.opcode, request.request_id,
                               Error::RESOURCE_LIMIT, "acquire");
        session.pending_ = true;
        session.pending_request_id_ = request.request_id;
        session.pending_receiver_ = static_cast<ReceiverId>(receiver);
        session.pending_kind_ = AuthorityJobKind::acquire;
        AuthorityReply queued;
        queued.action = wire::InboundAction::dispatch;
        queued.queued = true;
        return queued;
    }
    case wire::ControlRequestKind::tune: {
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "tune");
        std::size_t lease_slot = 0U;
        while (lease_slot < session.lease_count_ &&
               session.lease_ids_[lease_slot] != request.tune.lease_id)
            ++lease_slot;
        if (lease_slot == session.lease_count_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "tune");
        const ReceiverId receiver = session.lease_receivers_[lease_slot];
        const wire::ReceiverDescriptor* descriptor =
            receiver_descriptor(canonical_, static_cast<std::uint64_t>(receiver));
        if (descriptor == nullptr)
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "tune");
        Error validation = validate_profile(*descriptor, request.tune,
                                             session.connection_.features());
        if (validation == Error::OK && validate_tune_ != nullptr)
            validation = validate_tune_(receiver, request.tune);
        HzTuneRequest engine_request;
        engine_request.lease_id = request.tune.lease_id;
        engine_request.system = request.tune.system == wire::System::isdb_s
                                    ? ReceiverSystem::satellite
                                    : ReceiverSystem::terrestrial;
        engine_request.requested_hz = request.tune.frequency_hz;
        engine_request.timeout_ms = request.tune.timeout_ms;
        engine_request.validation = validation;
        if (request.tune.selection == wire::Selection::tsid)
            engine_request.stream_id = request.tune.selection_value;
        else if (request.tune.selection == wire::Selection::slot)
            engine_request.slot = request.tune.selection_value;
        if (request.tune.lnb_voltage.present)
            engine_request.lnb_voltage = request.tune.lnb_voltage.value;
        TuneAdmissionFailure failure = TuneAdmissionFailure::none;
        const auto ticket = engine_.try_admit_tune_detailed(
            session.client_id_, engine_request, accepted_at_ms, failure);
        if (!ticket && failure == TuneAdmissionFailure::registry_contention) {
            if (admission_queue_count_ == admission_queue_.size())
                return error_reply(session, request.opcode, request.request_id,
                                   Error::RESOURCE_LIMIT, "tune");
            session.pending_ = true;
            session.pending_admission_ = true;
            session.pending_request_id_ = request.request_id;
            session.pending_kind_ = AuthorityJobKind::tune;
            session.pending_receiver_ = receiver;
            session.pending_requested_hz_ = request.tune.frequency_hz;
            session.pending_selection_ = request.tune.selection;
            session.pending_selection_value_ = request.tune.selection_value;
            session.pending_admission_request_ = engine_request;
            session.pending_admission_accepted_at_ms_ = accepted_at_ms;
            if (!queue_admission(session)) {
                session.pending_ = false;
                session.pending_admission_ = false;
                return error_reply(session, request.opcode, request.request_id,
                                   Error::RESOURCE_LIMIT, "tune");
            }
            AuthorityReply queued;
            queued.action = wire::InboundAction::dispatch;
            queued.queued = true;
            return queued;
        }
        if (!ticket)
            return error_reply(session, request.opcode, request.request_id,
                               ticket.error(), "tune");
        session.pending_receiver_ = receiver;
        return submit_admitted_tune(session, request, ticket.value(), executor);
    }
    case wire::ControlRequestKind::release: {
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "release");
        std::size_t lease_slot = 0U;
        while (lease_slot < session.lease_count_ &&
               session.lease_ids_[lease_slot] != request.release.lease_id)
            ++lease_slot;
        if (lease_slot == session.lease_count_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "release");
        AuthorityJob job;
        job.kind = AuthorityJobKind::release;
        job.key = SessionKey{session.client_id_, request.request_id};
        job.client_id = session.client_id_;
        job.lease_id = request.release.lease_id;
        job.receiver = session.lease_receivers_[lease_slot];
        if (!executor.submit(job))
            return error_reply(session, request.opcode, request.request_id,
                               Error::RESOURCE_LIMIT, "release");
        session.pending_ = true;
        session.pending_request_id_ = request.request_id;
        session.pending_lease_id_ = request.release.lease_id;
        session.pending_kind_ = AuthorityJobKind::release;
        AuthorityReply queued;
        queued.action = wire::InboundAction::dispatch;
        queued.queued = true;
        return queued;
    }
    case wire::ControlRequestKind::card_status:
    case wire::ControlRequestKind::card_connect:
    case wire::ControlRequestKind::card_reconnect:
    case wire::ControlRequestKind::card_disconnect:
    case wire::ControlRequestKind::card_reset:
    case wire::ControlRequestKind::card_transmit:
    case wire::ControlRequestKind::begin_transaction:
    case wire::ControlRequestKind::end_transaction:
    case wire::ControlRequestKind::card_handle_status: {
        if ((session.connection_.features() & wire::kFeatureCardService) == 0U)
            return error_reply(session, request.opcode, request.request_id,
                               Error::UNSUPPORTED, card_operation_name(request.kind));
        if (!reader_exists(canonical_, card_reader_id(request)))
            return error_reply(session, request.opcode, request.request_id,
                               Error::UNSUPPORTED, card_operation_name(request.kind));
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, card_operation_name(request.kind));
        AuthorityJob job;
        job.kind = AuthorityJobKind::card;
        job.key = SessionKey{session.client_id_, request.request_id};
        job.client_id = session.client_id_;
        job.card_kind = request.kind;
        job.card_request = request;
        if (!executor.submit(job))
            return error_reply(session, request.opcode, request.request_id,
                               Error::RESOURCE_LIMIT, card_operation_name(request.kind));
        session.pending_ = true;
        session.pending_request_id_ = request.request_id;
        session.pending_kind_ = AuthorityJobKind::card;
        session.pending_card_kind_ = request.kind;
        session.pending_card_opcode_ = request.opcode;
        AuthorityReply queued;
        queued.action = wire::InboundAction::dispatch;
        queued.queued = true;
        return queued;
    }
    case wire::ControlRequestKind::lnb_power: {
        if (!supports_lnb_power(canonical_))
            return error_reply(session, request.opcode, request.request_id,
                               Error::UNSUPPORTED, "lnb_power");
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "lnb_power");
        AuthorityJob job;
        job.kind = AuthorityJobKind::lnb_power;
        job.key = SessionKey{session.client_id_, request.request_id};
        job.client_id = session.client_id_;
        job.lnb_enabled = request.lnb_power.enabled;
        if (!executor.submit(job))
            return error_reply(session, request.opcode, request.request_id,
                               Error::RESOURCE_LIMIT, "lnb_power");
        session.pending_ = true;
        session.pending_request_id_ = request.request_id;
        session.pending_kind_ = AuthorityJobKind::lnb_power;
        session.pending_lnb_enabled_ = request.lnb_power.enabled;
        AuthorityReply queued;
        queued.action = wire::InboundAction::dispatch;
        queued.queued = true;
        return queued;
    }
    case wire::ControlRequestKind::start_stream: {
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "start_stream");
        if (stream_engine_epoch_ == 0U || request.start_stream.epoch != server_.epoch)
            return error_reply(session, request.opcode, request.request_id,
                               Error::INVALID_ARGUMENT, "start_stream");
        std::size_t lease_slot = 0U;
        while (lease_slot < session.lease_count_ &&
               session.lease_ids_[lease_slot] != request.start_stream.lease_id)
            ++lease_slot;
        if (lease_slot == session.lease_count_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "start_stream");
        const auto started = engine_.start_stream_v1(session.client_id_,
            request.start_stream.lease_id, request.start_stream.tune_revision,
            accepted_at_ms, stream_arm_budget_ms_);
        if (!started)
            return error_reply(session, request.opcode, request.request_id,
                               started.error(), "start_stream");
        wire::StartResponse response;
        response.epoch = server_.epoch;
        response.stream_id = started.value().stream_id;
        response.attach_token = started.value().token;
        const std::uint64_t remaining = started.value().expires_at_ms > accepted_at_ms
            ? started.value().expires_at_ms - accepted_at_ms : 0U;
        response.expires_after_ms = remaining > UINT32_MAX
            ? UINT32_MAX : static_cast<std::uint32_t>(remaining);
        const auto size = wire::encode_record_frame(wire::Kind::response,
            wire::kOpcodeStartStream, request.request_id, response,
            &wire::encode_start_response, output);
        return size ? reply(session, size.value())
                    : error_reply(session, request.opcode, request.request_id,
                                  size.error(), "start_stream");
    }
    case wire::ControlRequestKind::stop_stream:
    {
        if (session.pending_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::BUSY, "stop_stream");
        if (stream_engine_epoch_ == 0U || request.stop_stream.epoch != server_.epoch)
            return error_reply(session, request.opcode, request.request_id,
                               Error::INVALID_ARGUMENT, "stop_stream");
        std::size_t lease_slot = 0U;
        while (lease_slot < session.lease_count_ &&
               session.lease_ids_[lease_slot] != request.stop_stream.lease_id)
            ++lease_slot;
        if (lease_slot == session.lease_count_)
            return error_reply(session, request.opcode, request.request_id,
                               Error::NOT_FOUND, "stop_stream");
        const auto stopped = executor.stop_stream_v1(session.client_id_,
            request.stop_stream.lease_id, request.stop_stream.stream_id);
        if (!stopped)
            return error_reply(session, request.opcode, request.request_id,
                               stopped.error(), "stop_stream");
        if (!stopped.value().final_valid ||
            stopped.value().stream_id != request.stop_stream.stream_id ||
            stopped.value().revision == 0U)
            return error_reply(session, request.opcode, request.request_id,
                               Error::INTERNAL, "stop_stream");

        wire::StopResponse response;
        response.epoch = server_.epoch;
        response.final.id = stopped.value().stream_id;
        response.final.tune_revision = stopped.value().revision;
        switch (static_cast<StreamTerminal>(stopped.value().terminal)) {
        case StreamTerminal::slow_consumer:
            response.final.reason = wire::StreamEndReason::slow_consumer;
            break;
        case StreamTerminal::usb_error:
            response.final.reason = wire::StreamEndReason::device_lost;
            break;
        case StreamTerminal::sync_error:
        case StreamTerminal::bridge_fatal:
            response.final.reason = wire::StreamEndReason::backend_error;
            break;
        case StreamTerminal::disconnected:
            response.final.reason = wire::StreamEndReason::owner_closed;
            break;
        case StreamTerminal::none:
        case StreamTerminal::stopped:
            response.final.reason = wire::StreamEndReason::stopped;
            break;
        }
        response.quiesced = stopped.value().quiesced;
        if (stopped.value().cleanup_error != Error::OK || !response.quiesced) {
            response.final.error.present = true;
            response.final.error.value.code = static_cast<std::uint32_t>(
                stopped.value().cleanup_error != Error::OK
                    ? stopped.value().cleanup_error : Error::BUSY);
            response.final.error.value.operation = wire::WString<32>{};
            constexpr char operation[] = "stop_stream";
            response.final.error.value.operation.length = sizeof(operation) - 1U;
            for (std::size_t i = 0U; i < sizeof(operation) - 1U; ++i)
                response.final.error.value.operation.bytes[i] = operation[i];
            if (!response.quiesced && stopped.value().cleanup_error == Error::OK) {
                response.final.error.value.detail = wire::WString<256>{};
                constexpr char detail[] = "cleanup pending";
                response.final.error.value.detail.length = sizeof(detail) - 1U;
                for (std::size_t i = 0U; i < sizeof(detail) - 1U; ++i)
                    response.final.error.value.detail.bytes[i] = detail[i];
            }
        }
        const auto size = wire::encode_record_frame(wire::Kind::response,
            request.opcode, request.request_id, response,
            &wire::encode_stop_response, output);
        return size ? reply(session, size.value())
                    : error_reply(session, request.opcode, request.request_id,
                                  size.error(), "stop_stream");
    }
    }
    return error_reply(session, request.opcode, request.request_id,
                       Error::UNSUPPORTED, "control");
}

AuthorityJob ControlAuthority::close(ControlServiceSession& session) noexcept
{
    AuthorityJob job{};
    close_cleanup(session).copy_to(job);
    return job;
}

AuthorityCleanup ControlAuthority::close_cleanup(ControlServiceSession& session) noexcept
{
    AuthorityCleanup job;
    if (&session.authority_ != this || !session.connected()) return job;
    if (session.pending_admission_) {
        remove_admission(session);
        session.pending_admission_ = false;
        session.pending_ = false;
    }
    if (session.pending_ &&
        (session.pending_kind_ == AuthorityJobKind::acquire ||
         session.pending_kind_ == AuthorityJobKind::rollback_release)) {
        session.closed_ = true;
        job.kind = AuthorityJobKind::disconnect;
        job.client_id = session.client_id_;
        job.key = SessionKey{session.client_id_, session.pending_request_id_};
        job.deferred = true;
        return job;
    }
    ControlServiceSession** link = &sessions_;
    while (*link != nullptr && *link != &session) link = &(*link)->next_;
    if (*link == &session) *link = session.next_;
    session.next_ = nullptr;
    session.closed_ = true;
    for (std::size_t i = 0U; i < session.lease_count_; ++i)
        (void)engine_.revoke_tune_ticket(session.client_id_, session.lease_ids_[i]);
    job.kind = AuthorityJobKind::disconnect;
    job.client_id = session.client_id_;
    job.key = SessionKey{session.client_id_, session.pending_request_id_};
    session.client_id_ = 0U;
    session.pending_ = false;
    session.lease_count_ = 0U;
    return job;
}

AuthorityReply ControlAuthority::complete(const SessionKey& key, Error result) noexcept
{
    ControlServiceSession* session = find_session(key.client_id);
    if (session == nullptr || !session->pending_ ||
        session->pending_admission_ ||
        session->pending_request_id_ != key.request_id)
        return {};
    if (session->pending_kind_ == AuthorityJobKind::acquire ||
        session->pending_kind_ == AuthorityJobKind::rollback_release) {
        AuthorityReply rejected;
        rejected.action = wire::InboundAction::ignored;
        return rejected;
    }
    return complete(AuthorityCompletion{key, session->pending_kind_, result, 0U,
                                        session->pending_receiver_});
}

AuthorityJob ControlAuthority::disconnect_job(ControlServiceSession& session) noexcept
{
    AuthorityJob job;
    job.kind = AuthorityJobKind::disconnect;
    job.key = SessionKey{session.client_id_, session.pending_request_id_};
    job.client_id = session.client_id_;
    session.closed_ = true;
    session.pending_ = true;
    session.pending_kind_ = AuthorityJobKind::disconnect;
    return job;
}

Error ControlAuthority::cleanup_submitted(const AuthorityJob& job, bool accepted) noexcept
{
    if (job.kind != AuthorityJobKind::disconnect || job.deferred)
        return Error::INVALID_ARGUMENT;
    ControlServiceSession* session = find_session(job.client_id);
    if (session == nullptr || !session->closed_ ||
        (session->pending_ && session->pending_kind_ != AuthorityJobKind::disconnect) ||
        job.key.client_id != session->client_id_ ||
        job.key.request_id != session->pending_request_id_)
        return Error::NOT_FOUND;
    session->pending_ = accepted;
    session->pending_kind_ = AuthorityJobKind::disconnect;
    return Error::OK;
}

Error ControlAuthority::cleanup_submitted(const AuthorityCleanup& job, bool accepted) noexcept
{
    if (job.kind != AuthorityJobKind::disconnect || job.deferred)
        return Error::INVALID_ARGUMENT;
    ControlServiceSession* session = find_session(job.client_id);
    if (session == nullptr || !session->closed_ ||
        (session->pending_ && session->pending_kind_ != AuthorityJobKind::disconnect) ||
        job.key.client_id != session->client_id_ ||
        job.key.request_id != session->pending_request_id_)
        return Error::NOT_FOUND;
    session->pending_ = accepted;
    session->pending_kind_ = AuthorityJobKind::disconnect;
    return Error::OK;
}

AuthorityReply ControlAuthority::complete(const AuthorityCompletion& completion) noexcept
{
    const SessionKey& key = completion.key;
    ControlServiceSession* session = find_session(key.client_id);
    if (session == nullptr || !session->pending_ ||
        session->pending_admission_ ||
        session->pending_request_id_ != key.request_id ||
        session->pending_kind_ != completion.kind)
        return {};
    if (presence_cache_enabled_ && presence_epoch_invalidated_ && !session->closed_) {
        AuthorityReply reply;
        reply.action = wire::InboundAction::close;
        reply.cleanup = close_cleanup(*session);
        reply.cleanup_required = true;
        return reply;
    }
    const Error result = completion.error;
    if (completion.kind == AuthorityJobKind::acquire) {
        if (session->closed_) {
            AuthorityReply reply;
            reply.action = wire::InboundAction::close;
            reply.cleanup = disconnect_job(*session);
            reply.cleanup_required = true;
            return reply;
        }
        if (result != Error::OK) {
            session->pending_ = false;
            return error_reply(*session, wire::kOpcodeAcquire,
                               static_cast<std::uint32_t>(key.request_id), result, "acquire");
        }
        Error acquire_error = Error::OK;
        bool lease_remembered = false;
        if (completion.lease_id == 0U) {
            session->closed_ = true;
            AuthorityReply unresolved;
            unresolved.action = wire::InboundAction::close;
            unresolved.cleanup = disconnect_job(*session);
            unresolved.cleanup_required = true;
            return unresolved;
        }
        if (completion.receiver != session->pending_receiver_)
            acquire_error = Error::INTERNAL;
        else if (!remember_lease(*session, completion.lease_id, completion.receiver))
            acquire_error = Error::RESOURCE_LIMIT;
        else
            lease_remembered = true;
        if (acquire_error == Error::OK) {
            wire::AcquireResponse response;
            response.epoch = server_.epoch;
            response.lease_id = completion.lease_id;
            const MutableByteView output{session->response_.data(), session->response_.size()};
            const auto size = wire::encode_acquire_response(response,
                MutableByteView{output.data + wire::kHeaderSize,
                                output.size - wire::kHeaderSize});
            if (!size) acquire_error = size.error();
            else {
                wire::Header header;
                header.kind = wire::Kind::response;
                header.opcode = wire::kOpcodeAcquire;
                header.request_id = static_cast<std::uint32_t>(key.request_id);
                header.payload_length = static_cast<std::uint32_t>(size.value());
                const auto encoded = wire::encode_header(header, output);
                if (!encoded) acquire_error = encoded.error();
                else {
                    session->pending_ = false;
                    return reply(*session, wire::kHeaderSize + size.value());
                }
            }
        }
        if (lease_remembered)
            forget_lease(*session, completion.lease_id);
        session->pending_kind_ = AuthorityJobKind::rollback_release;
        session->pending_error_ = acquire_error;
        session->pending_lease_id_ = completion.lease_id;
        AuthorityReply rollback;
        rollback.action = wire::InboundAction::dispatch;
        rollback.queued = true;
        rollback.cleanup_required = true;
        rollback.cleanup.kind = AuthorityJobKind::rollback_release;
        rollback.cleanup.key = key;
        rollback.cleanup.client_id = key.client_id;
        rollback.cleanup.lease_id = completion.lease_id;
        rollback.cleanup.receiver = session->pending_receiver_;
        rollback.cleanup.rollback_error = acquire_error;
        return rollback;
    }
    if (completion.kind == AuthorityJobKind::rollback_release) {
        if (completion.lease_id != session->pending_lease_id_ ||
            completion.receiver != session->pending_receiver_)
            return {};
        if (result != Error::OK || session->closed_) {
            session->closed_ = true;
            AuthorityReply cleanup;
            cleanup.action = wire::InboundAction::close;
            cleanup.cleanup = disconnect_job(*session);
            cleanup.cleanup_required = true;
            return cleanup;
        }
        session->pending_ = false;
        return error_reply(*session, wire::kOpcodeAcquire,
                           static_cast<std::uint32_t>(key.request_id),
                           session->pending_error_,
                           "acquire");
    }
    if (completion.kind == AuthorityJobKind::disconnect) {
        if (result != Error::OK) {
            session->pending_ = false;
            AuthorityReply retained;
            retained.action = wire::InboundAction::close;
            retained.cleanup_required = true;
            retained.cleanup.kind = AuthorityJobKind::disconnect;
            retained.cleanup.key = key;
            retained.cleanup.client_id = key.client_id;
            return retained;
        }
        ControlServiceSession** link = &sessions_;
        while (*link != nullptr && *link != session) link = &(*link)->next_;
        if (*link == session) *link = session->next_;
        session->next_ = nullptr;
        session->pending_ = false;
        session->client_id_ = 0U;
        session->lease_count_ = 0U;
        return {};
    }
    if (completion.kind == AuthorityJobKind::internal_card_status) {
        if (session->closed_) return {};
        if (result != Error::OK) {
            session->pending_ = false;
            return error_reply(*session, session->pending_status_opcode_,
                               static_cast<std::uint32_t>(key.request_id), result,
                               "status");
        }
        session->status_response_.card_inserted = completion.internal_card_inserted;
        const auto size = wire::encode_status_frame(
            static_cast<std::uint32_t>(key.request_id), session->status_response_,
            canonical_, MutableByteView{session->response_.data(), session->response_.size()});
        session->pending_ = false;
        return size ? reply(*session, size.value())
                    : error_reply(*session, session->pending_status_opcode_,
                                  static_cast<std::uint32_t>(key.request_id), size.error(),
                                  "status");
    }
    session->pending_ = false;
    if (completion.kind == AuthorityJobKind::card) {
        const wire::ControlRequestKind kind = session->pending_card_kind_;
        if (completion.card_kind != kind)
            return error_reply(*session, session->pending_card_opcode_,
                               static_cast<std::uint32_t>(key.request_id),
                               Error::INTERNAL, "card");
        if (kind != wire::ControlRequestKind::card_disconnect && result != Error::OK)
            return error_reply(*session, session->pending_card_opcode_,
                               static_cast<std::uint32_t>(key.request_id), result,
                               card_operation_name(kind));
        const MutableByteView output{session->response_.data(), session->response_.size()};
        Result<std::size_t> size = Result<std::size_t>::failure(Error::INTERNAL);
        switch (kind) {
        case wire::ControlRequestKind::card_status:
            size = wire::encode_record_frame(wire::Kind::response,
                wire::kOpcodeCardStatus, static_cast<std::uint32_t>(key.request_id),
                completion.card_status, &wire::encode_card_status_response, output);
            break;
        case wire::ControlRequestKind::card_connect:
        case wire::ControlRequestKind::card_reconnect:
        case wire::ControlRequestKind::card_reset:
        case wire::ControlRequestKind::card_handle_status:
            size = wire::encode_record_frame(wire::Kind::response,
                session->pending_card_opcode_,
                static_cast<std::uint32_t>(key.request_id), completion.card_session,
                &wire::encode_card_session_result, output);
            break;
        case wire::ControlRequestKind::card_disconnect:
            size = wire::encode_record_frame(wire::Kind::response,
                wire::kOpcodeCardDisconnect, static_cast<std::uint32_t>(key.request_id),
                completion.card_disconnect, &wire::encode_card_disconnect_response, output);
            break;
        case wire::ControlRequestKind::card_transmit:
            size = wire::encode_record_frame(wire::Kind::response,
                wire::kOpcodeCardTransmit, static_cast<std::uint32_t>(key.request_id),
                completion.card_transmit, &wire::encode_card_transmit_response, output);
            break;
        case wire::ControlRequestKind::begin_transaction:
            size = wire::encode_record_frame(wire::Kind::response,
                wire::kOpcodeBeginTransaction, static_cast<std::uint32_t>(key.request_id),
                completion.card_transaction, &wire::encode_begin_transaction_response, output);
            break;
        case wire::ControlRequestKind::end_transaction: {
            wire::Header header;
            header.kind = wire::Kind::response;
            header.opcode = wire::kOpcodeEndTransaction;
            header.request_id = static_cast<std::uint32_t>(key.request_id);
            const auto encoded = wire::encode_header(header, output);
            if (encoded) size = Result<std::size_t>::success(wire::kHeaderSize);
            else size = Result<std::size_t>::failure(encoded.error());
            break;
        }
        default:
            return error_reply(*session, session->pending_card_opcode_,
                               static_cast<std::uint32_t>(key.request_id),
                               Error::INTERNAL, "card");
        }
        if (!size)
            return error_reply(*session, session->pending_card_opcode_,
                               static_cast<std::uint32_t>(key.request_id), size.error(),
                               card_operation_name(kind));
        return reply(*session, size.value());
    }
    if (completion.kind == AuthorityJobKind::lnb_power) {
        if (completion.lnb_enabled != session->pending_lnb_enabled_)
            return error_reply(*session, wire::kOpcodeLnbPower,
                               static_cast<std::uint32_t>(key.request_id),
                               Error::INTERNAL, "lnb_power");
        if (result != Error::OK)
            return error_reply(*session, wire::kOpcodeLnbPower,
                               static_cast<std::uint32_t>(key.request_id), result,
                               "lnb_power");
        wire::LnbPowerResponse response;
        response.epoch = server_.epoch;
        response.enabled = completion.lnb_enabled;
        const auto size = wire::encode_record_frame(wire::Kind::response,
            wire::kOpcodeLnbPower, static_cast<std::uint32_t>(key.request_id), response,
            &wire::encode_lnb_power_response,
            MutableByteView{session->response_.data(), session->response_.size()});
        return size ? reply(*session, size.value())
                    : error_reply(*session, wire::kOpcodeLnbPower,
                                  static_cast<std::uint32_t>(key.request_id), size.error(),
                                  "lnb_power");
    }
    if (session->pending_kind_ == AuthorityJobKind::tune) {
        if (result != Error::OK)
            return error_reply(*session, wire::kOpcodeTune,
                               static_cast<std::uint32_t>(key.request_id), result, "tune");
        wire::TuneResult response;
        response.epoch = server_.epoch;
        response.requested_hz = session->pending_requested_hz_;
        const auto status = engine_.status();
        if (!status)
            return error_reply(*session, wire::kOpcodeTune,
                               static_cast<std::uint32_t>(key.request_id), status.error(), "tune");
        bool matched = false;
        for (std::size_t i = 0U; i < status.value().receiver_count; ++i) {
            const auto& metadata = status.value().tune_metadata[i];
            if (status.value().receivers[i] == session->pending_receiver_ &&
                metadata.current_valid && metadata.current.exact_hz &&
                metadata.current_revision == session->pending_tune_revision_ &&
                metadata.current.requested_hz == session->pending_requested_hz_) {
                response.tune_revision = metadata.current_revision;
                response.programmed_hz.present = metadata.current.programmed_hz.has_value();
                if (metadata.current.programmed_hz)
                    response.programmed_hz.value = *metadata.current.programmed_hz;
                response.verification = metadata.current.verification;
                response.lock = metadata.current.lock;
                matched = true;
                break;
            }
        }
        if (!matched)
            return error_reply(*session, wire::kOpcodeTune,
                               static_cast<std::uint32_t>(key.request_id),
                               Error::INTERNAL, "tune");
        if (session->pending_selection_ == wire::Selection::tsid) {
            response.selected_tsid.present = true;
            response.selected_tsid.value = session->pending_selection_value_;
        } else if (session->pending_selection_ == wire::Selection::slot) {
            response.selected_slot.present = true;
            response.selected_slot.value = session->pending_selection_value_;
        }
        const auto size = wire::encode_record_frame(wire::Kind::response, wire::kOpcodeTune,
            static_cast<std::uint32_t>(key.request_id), response, &wire::encode_tune_result,
            MutableByteView{session->response_.data(), session->response_.size()});
        return size ? reply(*session, size.value())
                    : error_reply(*session, wire::kOpcodeTune,
                                  static_cast<std::uint32_t>(key.request_id), size.error(), "tune");
    }
    wire::ReleaseResponse response;
    response.epoch = server_.epoch;
    response.lease_id = session->pending_lease_id_;
    response.released = result == Error::OK;
    response.quiesced = result == Error::OK;
    if (result != Error::OK) {
        response.error.present = true;
        response.error.value.code = static_cast<std::uint32_t>(result);
        response.error.value.operation = wire::WString<32>{};
        const char op[] = "release";
        response.error.value.operation.length = sizeof(op) - 1U;
        for (std::size_t i = 0U; i < sizeof(op) - 1U; ++i)
            response.error.value.operation.bytes[i] = op[i];
    } else {
        forget_lease(*session, response.lease_id);
    }
    const auto size = wire::encode_record_frame(wire::Kind::response, wire::kOpcodeRelease,
        static_cast<std::uint32_t>(key.request_id), response, &wire::encode_release_response,
        MutableByteView{session->response_.data(), session->response_.size()});
    return size ? reply(*session, size.value())
                : error_reply(*session, wire::kOpcodeRelease,
                              static_cast<std::uint32_t>(key.request_id), size.error(), "release");
}

Error ControlAuthority::response_sent(ControlServiceSession& session, bool complete_frame,
                                      ControlAuthorityExecutor& executor,
                                      AuthorityJob* rejected_cleanup) noexcept
{
    if (rejected_cleanup == nullptr && (!complete_frame || session.close_after_reply_))
        return Error::INVALID_ARGUMENT;
    if (!session.connected() || find_session(session.client_id_) != &session)
        return Error::NOT_FOUND;
    if (presence_cache_enabled_ && presence_epoch_invalidated_) {
        const AuthorityJob job = close(session);
        if (executor.submit(job)) return Error::OK;
        if (rejected_cleanup != nullptr) *rejected_cleanup = job;
        return Error::RESOURCE_LIMIT;
    }
    if (complete_frame) {
        if (session.close_after_reply_) {
            const AuthorityJob job = close(session);
            if (!executor.submit(job)) {
                if (rejected_cleanup != nullptr) *rejected_cleanup = job;
                return Error::RESOURCE_LIMIT;
            }
            return Error::OK;
        }
        session.connection_.complete();
        return Error::OK;
    }
    const AuthorityJob job = close(session);
    if (executor.submit(job)) return Error::OK;
    if (rejected_cleanup != nullptr) *rejected_cleanup = job;
    return Error::RESOURCE_LIMIT;
}

Error ControlAuthority::execute_job(ReceiverEngine& engine,
                                    const AuthorityJob& job) noexcept
{
    if (job.kind == AuthorityJobKind::acquire) return Error::INVALID_ARGUMENT;
    return execute_job_value(engine, job).error;
}

AuthorityCompletion ControlAuthority::execute_job_value(ReceiverEngine& engine,
                                                         const AuthorityJob& job) noexcept
{
    AuthorityCompletion completion;
    completion.key = job.key;
    completion.kind = job.kind;
    completion.receiver = job.receiver;
    completion.lease_id = job.lease_id;
    if (job.kind == AuthorityJobKind::tune) {
        completion.lease_id = job.tune.lease_id;
        completion.receiver = job.tune.receiver;
    }
    switch (job.kind) {
    case AuthorityJobKind::acquire: {
        const auto result = engine.acquire(job.client_id, job.receiver);
        completion.error = result ? Error::OK : result.error();
        if (result) completion.lease_id = result.value().lease_id;
        return completion;
    }
    case AuthorityJobKind::tune: {
        const auto result = engine.execute_admitted_tune(job.tune);
        completion.error = result ? Error::OK : result.error();
        return completion;
    }
    case AuthorityJobKind::lnb_power:
        completion.error = Error::UNSUPPORTED;
        return completion;
    case AuthorityJobKind::release:
    case AuthorityJobKind::rollback_release: {
        const auto result = engine.release(job.client_id, job.lease_id);
        completion.error = result ? Error::OK : result.error();
        return completion;
    }
    case AuthorityJobKind::disconnect: {
        const auto result = engine.disconnect_client(job.client_id);
        completion.error = result ? Error::OK : result.error();
        return completion;
    }
    case AuthorityJobKind::card:
    case AuthorityJobKind::internal_card_status:
        completion.error = Error::INVALID_ARGUMENT;
        return completion;
    }
    completion.error = Error::INVALID_ARGUMENT;
    return completion;
}

}  // namespace dtv::userland::detail
