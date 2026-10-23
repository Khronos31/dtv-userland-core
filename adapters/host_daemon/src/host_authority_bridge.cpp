// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/host_authority_bridge.h"

namespace px4::userland::ipc::posix {
namespace {

using dtv::userland::detail::AuthorityJob;
using dtv::userland::detail::AuthorityJobKind;
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
    : authority_owner_(tuner_service, server, canonical, validate_tune), workers_(workers)
{
}

Result<std::uint8_t> HostAuthorityBridge::open_session() noexcept
{
    if (workers_.error() != Error::OK) enter_fatal();
    if (!accepting_) return Result<std::uint8_t>::failure(Error::NOT_READY);
    for (std::size_t i = 0U; i < slots_.size(); ++i) {
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
    if (!accepting_ || index >= slots_.size()) return {};
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return {};
    const auto reply = authority_owner_.authority().accept(
        *slot.session, frame, context, snapshot, accepted_at_ms, *this);
    if (reply.action == dtv::userland::wire::InboundAction::close &&
        !reply.cleanup_required) {
        slot.state = SlotState::tombstone;
        if (slot.session && slot.session->client_id() == 0U) slot.session.reset();
    }
    return reply;
}

Error HostAuthorityBridge::response_sent(std::uint8_t index,
                                         bool complete_frame) noexcept
{
    if (index >= slots_.size()) return Error::INVALID_ARGUMENT;
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return Error::NOT_FOUND;
    if (complete_frame) {
        const Error error = authority_owner_.authority().response_sent(
            *slot.session, true, *this, nullptr);
        if (error == Error::OK && slot.session->client_id() == 0U) {
            slot.state = SlotState::tombstone;
            slot.session.reset();
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
    if (index >= slots_.size()) return {};
    Slot& slot = slots_[index];
    if (!slot.response_ready) return {};
    slot.response_ready = false;
    return slot.response;
}

Error HostAuthorityBridge::close_session(std::uint8_t index) noexcept
{
    if (index >= slots_.size()) return Error::INVALID_ARGUMENT;
    Slot& slot = slots_[index];
    if (slot.state != SlotState::active || !slot.session) return Error::NOT_FOUND;
    const AuthorityJob cleanup = authority_owner_.authority().close(*slot.session);
    if (cleanup.kind != AuthorityJobKind::disconnect) return Error::OK;
    slot.state = SlotState::tombstone;
    if (cleanup.deferred) {
        slot.deferred_session = true;
        return Error::OK;
    }
    slot.session.reset();
    retain_cleanup(cleanup, index);
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
    if (index == slots_.size()) return false;
    return queue_job(job, index);
}

bool HostAuthorityBridge::queue_job(const AuthorityJob& job,
                                    std::size_t index) noexcept
{
    if (index >= slots_.size() || job.key.request_id > 0xffffffffU) return false;
    Slot& slot = slots_[index];
    if (job.kind == AuthorityJobKind::disconnect && slot.inflight != 0U) {
        retain_cleanup(job, index);
        return true;
    }
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::tuner_authority_job;
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
    const auto lane = workers_.authority_lane(
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

void HostAuthorityBridge::retain_cleanup(const AuthorityJob& job,
                                         std::size_t index) noexcept
{
    Slot& slot = slots_[index];
    slot.cleanup = job;
    slot.cleanup_pending = true;
    slot.cleanup_accepted = false;
    slot.cleanup_inflight = false;
    if (slot.session && slot.session->client_id() != 0U)
        (void)authority_owner_.authority().cleanup_submitted(job, false);
}

std::size_t HostAuthorityBridge::find_slot(std::uint64_t id) const noexcept
{
    for (std::size_t i = 0U; i < slots_.size(); ++i) {
        if (slots_[i].state != SlotState::free && slots_[i].client_id == id)
            return i;
    }
    return slots_.size();
}

void HostAuthorityBridge::consume(const ControlWorkerCompletion& completion,
                                  std::size_t lane) noexcept
{
    if (completion.operation != ControlWorkerOperation::tuner_authority_job) {
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
    if (index == slots_.size()) {
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
    slot.deferred_session = false;
    slot.response_ready = false;
}

void HostAuthorityBridge::close_all() noexcept
{
    for (std::size_t i = 0U; i < slots_.size(); ++i) {
        if (slots_[i].state == SlotState::active) (void)close_session(static_cast<std::uint8_t>(i));
    }
}

void HostAuthorityBridge::enter_fatal() noexcept
{
    if (!fatal_) {
        accepting_ = false;
        fatal_ = true;
        close_all();
    }
    if (!other_ingress_stopped_ && stop_other_ingress_ != nullptr) {
        stop_other_ingress_(stop_other_ingress_context_);
        other_ingress_stopped_ = true;
    }
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
    if (!other_ingress_stopped_ && stop_other_ingress_ != nullptr) {
        stop_other_ingress_(stop_other_ingress_context_);
        other_ingress_stopped_ = true;
    }
    accepting_ = false;
    shutting_down_ = true;
    close_all();
    advance();
    return fatal_ ? Error::INTERNAL : Error::OK;
}

Error HostAuthorityBridge::poll() noexcept
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
    advance();
    return stopped_ ? shutdown_result() : Error::OK;
}

void HostAuthorityBridge::advance() noexcept
{
    for (std::size_t i = 0U; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.state != SlotState::free && slot.inflight == 0U &&
            slot.cleanup_pending && !fatal_) {
            if (!queue_job(slot.cleanup, i)) {
                if (workers_.error() != Error::OK) enter_fatal();
            }
        }
    }
    if (fatal_) {
        if (!stop_requested_) {
            stop_requested_ = true;
            (void)workers_.request_stop();
        }
    } else if (shutting_down_ && !stop_requested_) {
        bool owned = false;
        for (const Slot& slot : slots_) owned = owned || slot.state != SlotState::free;
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
            for (Slot& slot : slots_) {
                if (slot.state == SlotState::tombstone) slot.state = SlotState::fatal_unresolved;
            }
        }
    }
}

HostAuthorityBridgeCounts HostAuthorityBridge::counts() const noexcept
{
    HostAuthorityBridgeCounts result{};
    for (const Slot& slot : slots_) {
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
    return index < slots_.size() ? slots_[index].client_id : 0U;
}

Error HostAuthorityBridge::shutdown_result() const noexcept
{
    if (!stopped_) return Error::NOT_READY;
    return fatal_ || counts().fatal_unresolved != 0U ? Error::INTERNAL : Error::OK;
}

}  // namespace px4::userland::ipc::posix
