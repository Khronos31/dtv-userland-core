/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

namespace l3_owner_prototype {

namespace {
// Prototype candidate; queue capacity is not a frozen product value.
constexpr std::size_t kQueueCapacityPackets = 4;
constexpr int kOpenLaneMask = 1;
constexpr int kControlLaneMask = 2;

thread_local int callback_frame_depth = 0;
std::atomic<int> lane_start_failure_mask{0};

std::chrono::milliseconds wait_duration(std::uint32_t timeout_ms) {
    return std::chrono::milliseconds(timeout_ms);
}

struct LaneStartFailure {};
}  // namespace

OwnerEngine::OwnerEngine(Provider &provider, std::uint64_t binding_value)
    : provider_(provider), binding_(std::make_shared<Binding>(binding_value)) {
    if (test_lane_start_fails(kControlLaneMask)) {
        throw LaneStartFailure{};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control_lane_ = DTV_L3_LANE_RUNNING;
    }
    worker_ = std::thread([this] {
        worker_loop();
        std::lock_guard<std::mutex> lock(mutex_);
        control_lane_ = DTV_L3_LANE_EXITED;
        state_condition_.notify_all();
    });
}

OwnerEngine::~OwnerEngine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_stopping_ = true;
        work_condition_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

OwnerEngine::Borrow::Borrow(OwnerEngine &owner)
    : owner_(owner), admitted_(false) {
    std::lock_guard<std::mutex> lock(owner_.mutex_);
    if (!owner_.retired_gate_ && !owner_.released_) {
        ++owner_.active_borrows_;
        admitted_ = true;
    }
}

OwnerEngine::Borrow::~Borrow() {
    if (!admitted_) {
        return;
    }
    std::lock_guard<std::mutex> lock(owner_.mutex_);
    --owner_.active_borrows_;
}

void OwnerEngine::initialize_claims(std::uint32_t count) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialization_phase_ = InitializationPhase::InProgress;
        claims_.assign(count, ClaimState::NotAttempted);
        claim_quiesced_.assign(count, true);
        claim_cleanup_errors_.assign(count, DTV_L3_OK);
        if (close_requested_) {
            open_state_ = OpenState::Closing;
        } else {
            open_state_ = OpenState::Opening;
            open_result_ = DTV_L3_ACCEPTED;
        }
    }

    bool failed = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::shared_ptr<Binding> binding;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (close_requested_) {
                break;
            }
            claims_[index] = ClaimState::InFlight;
            claim_quiesced_[index] = false;
            claim_in_flight_ = true;
            ++provider_calls_in_flight_;
            binding = binding_;
        }

        CallbackFrame frame;
        const auto claim = provider_.claim(binding.get(), index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            claim_in_flight_ = false;
            if (claim.result == DTV_L3_OK &&
                claim.residual_state == DTV_L3_CLAIM_RETAINED) {
                claims_[index] = ClaimState::Ready;
                claim_quiesced_[index] = false;
                ++claimed_count_;
            } else {
                failed = true;
                open_result_ = claim.result == DTV_L3_OK
                                   ? DTV_L3_FAILED
                                   : claim.result;
                open_state_ = OpenState::Failed;
                close_requested_ = true;
                if (claim.residual_state == DTV_L3_CLAIM_NONE) {
                    claims_[index] = ClaimState::Released;
                    claim_quiesced_[index] = true;
                } else if (claim.residual_state == DTV_L3_CLAIM_RETAINED) {
                    claims_[index] = ClaimState::Retained;
                    claim_quiesced_[index] = false;
                } else {
                    claims_[index] = ClaimState::Unknown;
                    claim_quiesced_[index] = false;
                }
            }
            state_condition_.notify_all();
            if (close_requested_) {
                break;
            }
        }
    }

    bool must_finish_close = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (close_requested_) {
            if (open_state_ != OpenState::Failed &&
                open_state_ != OpenState::Closed) {
                open_state_ = OpenState::Closing;
            }
        } else if (!failed) {
            open_state_ = OpenState::Ready;
            open_result_ = DTV_L3_OK;
        }
        initialization_phase_ = InitializationPhase::ResponsibilitiesRegistered;
        must_finish_close = failed || close_requested_;
    }

    if (must_finish_close) {
        finish_close();
        std::unique_lock<std::mutex> lock(mutex_);
        state_condition_.wait(lock, [this] {
            return cleanup_attempted_ && !cleanup_started_;
        });
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialization_phase_ = InitializationPhase::Returned;
        open_lane_ = DTV_L3_LANE_EXIT_COMMITTED;
        maybe_commit_control_exit_locked();
        state_condition_.notify_all();
        work_condition_.notify_all();
    }
}

void OwnerEngine::mark_open_lane_running() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_lane_spawned_ = true;
    open_lane_ = DTV_L3_LANE_RUNNING;
}

void OwnerEngine::mark_open_lane_exited() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_lane_ = DTV_L3_LANE_EXITED;
    state_condition_.notify_all();
}

dtv_l3_result OwnerEngine::start(
    const dtv_l3_transfer_request &request,
    std::shared_ptr<Operation> *operation) {
    if (operation == nullptr ||
        (request.out_size != 0 && request.out_data == nullptr)) {
        return DTV_L3_INVALID;
    }
    if ((request.kind == DTV_L3_TRANSFER_BULK &&
         (request.endpoint == 0 || request.control.request_type != 0 ||
          request.control.request != 0 || request.control.value != 0 ||
          request.control.index != 0)) ||
        (request.kind == DTV_L3_TRANSFER_CONTROL && request.endpoint != 0) ||
        (request.kind != DTV_L3_TRANSFER_BULK &&
         request.kind != DTV_L3_TRANSFER_CONTROL)) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    auto op = std::make_shared<Operation>();
    std::shared_ptr<Binding> binding;
    dtv_l3_transfer_request provider_request = request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.claim_index >= claims_.size()) {
            return DTV_L3_INVALID;
        }
        if (open_state_ != OpenState::Ready || close_requested_ ||
            claims_[request.claim_index] != ClaimState::Ready) {
            return DTV_L3_BUSY;
        }
        binding = binding_;
        op->owner = this;
        op->binding = binding;
        op->id = next_transfer_id_++;
        if (request.out_size != 0) {
            op->retained_payload.assign(
                request.out_data, request.out_data + request.out_size);
            provider_request.out_data = op->retained_payload.data();
        }
        operations_.emplace(op->id, op);
        *operation = op;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++provider_calls_in_flight_;
    }
    const auto result = provider_.submit(
        binding.get(), op->id, provider_request, &OwnerEngine::completion_thunk,
        op.get());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        if (result == DTV_L3_ACCEPTED) {
            op->submitted = true;
            if (op->cancel_requested) {
                queue_cancel_locked(op);
            }
        } else {
            if (op->state == DTV_L3_OP_OPENING) {
                op->state = DTV_L3_OP_FAILED;
                op->result = result;
            }
            op->submitted = true;
        }
        state_condition_.notify_all();
    }
    finish_close();
    return result;
}

dtv_l3_result OwnerEngine::acquire_lease(std::uint32_t receiver_id,
                                         std::uint32_t *lease_id) {
    if (lease_id == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    dtv_l3_scope_info scope{};
    const auto scope_result = provider_.scope_query(receiver_id, &scope);
    if (scope_result != DTV_L3_OK) {
        return scope_result;
    }
    if (scope.compatibility != DTV_L3_LEASE_SHAREABLE &&
        scope.compatibility != DTV_L3_LEASE_EXCLUSIVE) {
        return DTV_L3_INVALID;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (close_requested_ || open_state_ != OpenState::Ready) {
        return DTV_L3_BUSY;
    }
    for (const auto &entry : leases_) {
        if (entry.second.receiver_id == receiver_id) {
            return DTV_L3_BUSY;
        }
        if (entry.second.bridge_id == scope.bridge_id &&
            (scope.compatibility == DTV_L3_LEASE_EXCLUSIVE ||
             entry.second.compatibility == DTV_L3_LEASE_EXCLUSIVE)) {
            return DTV_L3_BUSY;
        }
    }
    LeaseRecord record;
    record.receiver_id = receiver_id;
    record.bridge_id = scope.bridge_id;
    record.compatibility = scope.compatibility;
    record.state = DTV_L3_LEASE_FREE;
    const auto id = next_lease_id_++;
    leases_.emplace(id, record);
    *lease_id = id;
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::tune_lease(std::uint32_t lease_id,
                                      std::uint64_t requested_hz,
                                      std::uint64_t *programmed_hz,
                                      std::uint64_t *tune_revision) {
    if (programmed_hz == nullptr || tune_revision == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::uint32_t receiver_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = leases_.find(lease_id);
        if (found == leases_.end()) {
            return DTV_L3_STALE;
        }
        if (close_requested_ || open_state_ != OpenState::Ready) {
            return DTV_L3_BUSY;
        }
        if (found->second.state == DTV_L3_LEASE_ACTIVE ||
            found->second.state == DTV_L3_LEASE_RETAINED ||
            found->second.active_stream != 0 ||
            found->second.stop_residual) {
            return DTV_L3_BUSY;
        }
        receiver_id = found->second.receiver_id;
        ++provider_calls_in_flight_;
    }

    std::uint64_t programmed = requested_hz;
    const auto tune_result =
        provider_.tune(receiver_id, requested_hz, &programmed);

    dtv_l3_result result = DTV_L3_OK;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        state_condition_.notify_all();
        if (close_requested_) {
            result = DTV_L3_BUSY;
        } else if (tune_result != DTV_L3_OK) {
            result = tune_result;
        } else {
            const auto found = leases_.find(lease_id);
            if (found == leases_.end()) {
                result = DTV_L3_STALE;
            } else if (found->second.active_stream != 0 ||
                       found->second.stop_residual) {
                result = DTV_L3_BUSY;
            } else {
                ++found->second.tune_revision;
                found->second.state = DTV_L3_LEASE_TUNED;
                found->second.requested_hz = requested_hz;
                found->second.programmed_hz = programmed;
                *programmed_hz = programmed;
                *tune_revision = found->second.tune_revision;
            }
        }
    }
    finish_close();
    return result;
}

dtv_l3_result OwnerEngine::start_lease(std::uint32_t lease_id) {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease_id);
    if (found == leases_.end()) {
        return DTV_L3_STALE;
    }
    if (close_requested_ || open_state_ != OpenState::Ready) {
        return DTV_L3_BUSY;
    }
    if (found->second.state != DTV_L3_LEASE_TUNED ||
        found->second.active_stream != 0 || found->second.stop_residual) {
        return DTV_L3_BUSY;
    }
    found->second.state = DTV_L3_LEASE_ARMED;
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::attach_lease(std::uint32_t lease_id,
                                        std::uint64_t *stream_generation) {
    if (stream_generation == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    std::shared_ptr<StreamRecord> stream;
    std::uint32_t bridge_id = 0;
    std::uint32_t receiver_id = 0;
    std::uint64_t generation = 0;
    bool need_start = false;
    bool start_aborted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = leases_.find(lease_id);
        if (found == leases_.end()) {
            return DTV_L3_STALE;
        }
        if (close_requested_ || open_state_ != OpenState::Ready) {
            return DTV_L3_BUSY;
        }
        if (found->second.state != DTV_L3_LEASE_ARMED ||
            found->second.active_stream != 0 ||
            found->second.stop_residual) {
            return DTV_L3_BUSY;
        }
        auto &bridge = bridges_[found->second.bridge_id];
        if (bridge.start_pending || bridge.stop_pending) {
            return DTV_L3_BUSY;
        }
        if (!bridge.running) {
            bridge.start_pending = true;
            need_start = true;
        }
        ++bridge.pending_attaches;
        bridge_id = found->second.bridge_id;
        receiver_id = found->second.receiver_id;
        generation = next_stream_generation_++;
        stream = std::make_shared<StreamRecord>();
        stream->generation = generation;
        stream->lease_id = lease_id;
        stream->receiver_id = receiver_id;
        stream->bridge_id = bridge_id;
        stream->tune_revision = found->second.tune_revision;
        stream->phase = DTV_L3_STREAM_ACTIVE;
        stream->attaching = true;
        stream->sink = std::make_shared<StreamSink>();
        stream->sink->owner = this;
        stream->sink->generation = generation;
        stream->sink->binding = binding_.get();
        streams_.emplace(generation, stream);
        found->second.active_stream = generation;
        found->second.state = DTV_L3_LEASE_ACTIVE;
    }

    if (need_start) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++provider_calls_in_flight_;
        }
        const auto start_result = provider_.bridge_start(bridge_id, receiver_id);
        bool start_ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            auto &bridge = bridges_[bridge_id];
            bridge.start_pending = false;
            start_ok = start_result == DTV_L3_OK;
            if (start_ok) {
                bridge.running = true;
                ++bridge.start_count;
                start_aborted = close_requested_;
            } else {
                --bridge.pending_attaches;
                streams_.erase(generation);
                const auto found = leases_.find(lease_id);
                if (found != leases_.end()) {
                    found->second.active_stream = 0;
                    found->second.stop_residual = true;
                    found->second.state = DTV_L3_LEASE_RETAINED;
                }
            }
            state_condition_.notify_all();
        }
        if (!start_ok) {
            finish_close();
            return start_result;
        }
    }

    dtv_l3_result attach_result = DTV_L3_BUSY;
    bool abort = false;
    if (start_aborted) {
        std::lock_guard<std::mutex> lock(mutex_);
        stream->attaching = false;
        abort = true;
        state_condition_.notify_all();
    } else {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++provider_calls_in_flight_;
        }
        attach_result = provider_.stream_attach(
            generation, &StreamSink::deliver, stream->sink.get());
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        stream->attaching = false;
        abort = attach_result != DTV_L3_OK || close_requested_;
        state_condition_.notify_all();
    }
    if (abort) {
        const bool attached = attach_result == DTV_L3_OK;
        dtv_l3_result stop_result = DTV_L3_OK;
        if (attached) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++provider_calls_in_flight_;
            }
            stop_result = provider_.stream_stop(generation);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --provider_calls_in_flight_;
                state_condition_.notify_all();
            }
        }

        bool stop_bridge = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &bridge = bridges_[bridge_id];
            if (bridge.users == 0 && bridge.pending_attaches <= 1 &&
                bridge.running && !bridge.stop_pending) {
                bridge.stop_pending = true;
                stop_bridge = true;
                ++provider_calls_in_flight_;
            }
        }
        dtv_l3_result bridge_result = DTV_L3_OK;
        if (stop_bridge) {
            bridge_result = provider_.bridge_stop(bridge_id);
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            auto &bridge = bridges_[bridge_id];
            bridge.stop_pending = false;
            if (bridge_result == DTV_L3_OK) {
                bridge.running = false;
                bridge.stop_residual = false;
                ++bridge.stop_count;
            } else {
                bridge.running = true;
                bridge.stop_residual = true;
            }
            state_condition_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &bridge = bridges_[bridge_id];
            if (bridge.pending_attaches > 0) {
                --bridge.pending_attaches;
            }
            const auto found = leases_.find(lease_id);
            if (attached && stop_result != DTV_L3_OK) {
                stream->attaching = false;
                stream->stop_residual = true;
                stream->physical_stopped = false;
                stream->retire_requested = true;
                stream->retire_mode =
                    DTV_L3_STREAM_RELEASE_DISCARD_UNREAD;
                stream->phase = DTV_L3_STREAM_RETIRING;
                if (found != leases_.end() &&
                    found->second.active_stream == generation) {
                    found->second.stop_residual = true;
                    found->second.state = DTV_L3_LEASE_RETAINED;
                }
            } else {
                streams_.erase(generation);
                if (found != leases_.end() &&
                    found->second.active_stream == generation) {
                    found->second.active_stream = 0;
                    if (bridge_result != DTV_L3_OK ||
                        (need_start && bridge.stop_residual)) {
                        found->second.stop_residual = true;
                        found->second.state = DTV_L3_LEASE_RETAINED;
                    } else {
                        found->second.stop_residual = false;
                        found->second.state = DTV_L3_LEASE_FREE;
                    }
                }
            }
            state_condition_.notify_all();
        }
        finish_close();
        if (attach_result != DTV_L3_OK) {
            return attach_result;
        }
        return DTV_L3_BUSY;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto &bridge = bridges_[bridge_id];
        if (bridge.pending_attaches > 0) {
            --bridge.pending_attaches;
        }
        ++bridge.users;
        stream->user_counted = true;
        stream->token_exposed = true;
        state_condition_.notify_all();
    }
    *stream_generation = generation;
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::stop_stream(std::uint64_t generation,
                                       std::uint32_t timeout_ms) {
    (void)timeout_ms;
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    std::shared_ptr<StreamRecord> stream;
    std::uint32_t bridge_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = streams_.find(generation);
        if (found == streams_.end()) {
            return DTV_L3_STALE;
        }
        stream = found->second;
        if (stream->attaching || stream->stopping) {
            return DTV_L3_BUSY;
        }
        const bool retry = stream->stop_residual &&
                           stream->phase != DTV_L3_STREAM_RETIRED;
        if (!retry && stream->phase != DTV_L3_STREAM_ACTIVE) {
            return DTV_L3_OK;
        }
        stream->stopping = true;
        bridge_id = stream->bridge_id;
        ++provider_calls_in_flight_;
    }

    const auto stop_result = provider_.stream_stop(generation);
    bool need_bridge_stop = false;
    dtv_l3_result bridge_result = DTV_L3_OK;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        stream->stopping = false;
        if (stream->user_counted) {
            stream->user_counted = false;
            auto &bridge = bridges_[bridge_id];
            if (bridge.users > 0) {
                --bridge.users;
            }
        }
        if (stop_result == DTV_L3_OK) {
            stream->stop_residual = false;
            stream->physical_stopped = true;
        } else {
            stream->stop_residual = true;
            auto found = leases_.find(stream->lease_id);
            if (found != leases_.end()) {
                found->second.stop_residual = true;
                found->second.state = DTV_L3_LEASE_RETAINED;
            }
        }
        if (stop_result == DTV_L3_OK &&
            (close_requested_ || stream->retire_requested)) {
            stream->retire_requested = true;
            stream->retire_mode = DTV_L3_STREAM_RELEASE_DISCARD_UNREAD;
            stream->phase = DTV_L3_STREAM_RETIRING;
            if (stream->read_borrows == 0) {
                finalize_retire_locked(generation);
            }
        } else if (close_requested_ || stream->retire_requested) {
            stream->retire_requested = true;
            stream->retire_mode = DTV_L3_STREAM_RELEASE_DISCARD_UNREAD;
            stream->phase = DTV_L3_STREAM_RETIRING;
        } else {
            stream->phase = DTV_L3_STREAM_DRAINING;
        }
        auto &bridge = bridges_[bridge_id];
        if (bridge.users == 0 && bridge.pending_attaches == 0 &&
            bridge.running && !bridge.stop_pending) {
            bridge.stop_pending = true;
            need_bridge_stop = true;
            ++provider_calls_in_flight_;
        }
        state_condition_.notify_all();
    }

    if (need_bridge_stop) {
        bridge_result = provider_.bridge_stop(bridge_id);
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        auto &bridge = bridges_[bridge_id];
        bridge.stop_pending = false;
        if (bridge_result == DTV_L3_OK) {
            bridge.running = false;
            bridge.stop_residual = false;
            ++bridge.stop_count;
        } else {
            bridge.running = true;
            bridge.stop_residual = true;
            const auto found = leases_.find(stream->lease_id);
            if (found != leases_.end()) {
                found->second.stop_residual = true;
                found->second.state = DTV_L3_LEASE_RETAINED;
            }
        }
        refresh_lease_locked(stream->lease_id);
        state_condition_.notify_all();
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_lease_locked(stream->lease_id);
        state_condition_.notify_all();
    }

    finish_close();
    if (stop_result != DTV_L3_OK) {
        return stop_result;
    }
    return bridge_result;
}

dtv_l3_result OwnerEngine::read_stream(
    std::uint64_t generation,
    std::uint8_t *buffer,
    std::size_t capacity_bytes,
    std::size_t *bytes_read,
    dtv_l3_read_state *state,
    const std::function<void()> &after_borrow) {
    if (bytes_read == nullptr || state == nullptr ||
        (capacity_bytes != 0 && buffer == nullptr) ||
        capacity_bytes % 188 != 0) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    std::shared_ptr<StreamRecord> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = streams_.find(generation);
        if (found == streams_.end() ||
            found->second->phase == DTV_L3_STREAM_RETIRED) {
            return DTV_L3_STALE;
        }
        if (close_requested_) {
            return DTV_L3_BUSY;
        }
        if (found->second->phase != DTV_L3_STREAM_ACTIVE &&
            found->second->phase != DTV_L3_STREAM_DRAINING) {
            return DTV_L3_STALE;
        }
        stream = found->second;
        ++stream->read_borrows;
    }

    if (after_borrow) {
        after_borrow();
    }

    bool finalized = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t packets = 0;
        const std::size_t limit = capacity_bytes / 188;
        while (packets < limit && !stream->queue.empty()) {
            std::memcpy(buffer + packets * 188, stream->queue.front().data(),
                        188);
            stream->queue.pop_front();
            ++packets;
        }
        *bytes_read = packets * 188;
        if (packets != 0) {
            *state = DTV_L3_READ_DATA;
        } else if (stream->eof_pending) {
            stream->eof_observed = true;
            *state = DTV_L3_READ_EOF;
        } else {
            *state = DTV_L3_READ_WAIT;
        }
        if (stream->read_borrows > 0) {
            --stream->read_borrows;
        }
        if (stream->retire_requested && stream->read_borrows == 0) {
            finalize_retire_locked(generation);
            finalized = true;
        }
        state_condition_.notify_all();
    }
    if (finalized) {
        finish_close();
    }
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::stream_status(std::uint64_t generation,
                                         dtv_l3_stream_status *status) {
    if (status == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(generation);
    if (found == streams_.end()) {
        return DTV_L3_STALE;
    }
    const auto &stream = *found->second;
    status->phase = stream.phase;
    status->stream_generation = stream.generation;
    status->tune_revision = stream.tune_revision;
    status->queued_packets = stream.queue.size();
    status->staged_bytes = stream.staging.size();
    status->dropped_overflow_packets = stream.dropped_overflow_packets;
    status->discarded_unread_packets = stream.discarded_unread_packets;
    status->eof_observed = stream.eof_observed ? 1 : 0;
    status->read_borrows = static_cast<std::uint8_t>(
        std::min<std::size_t>(stream.read_borrows, 255));
    std::memset(status->reserved, 0, sizeof(status->reserved));
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::release_stream(
    std::uint64_t generation,
    dtv_l3_stream_release_mode mode) {
    if (mode != DTV_L3_STREAM_RELEASE_DRAINED &&
        mode != DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    bool finalized = false;
    dtv_l3_result result = DTV_L3_OK;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = streams_.find(generation);
        if (found == streams_.end()) {
            return DTV_L3_STALE;
        }
        auto &stream = *found->second;
        if (stream.phase == DTV_L3_STREAM_RETIRED) {
            return DTV_L3_OK;
        }
        if (stream.stop_residual || stream.stopping || stream.attaching) {
            return DTV_L3_BUSY;
        }
        if (stream.retire_requested) {
            return stream.read_borrows == 0 ? DTV_L3_OK : DTV_L3_ACCEPTED;
        }
        if (stream.phase != DTV_L3_STREAM_DRAINING) {
            return DTV_L3_BUSY;
        }
        stream.retire_requested = true;
        stream.retire_mode = mode;
        if (mode == DTV_L3_STREAM_RELEASE_DRAINED) {
            if (!stream.eof_observed || !stream.queue.empty() ||
                stream.read_borrows != 0) {
                stream.retire_requested = false;
                return DTV_L3_BUSY;
            }
            stream.phase = DTV_L3_STREAM_RETIRING;
            finalize_retire_locked(generation);
            finalized = true;
        } else if (stream.read_borrows != 0) {
            stream.phase = DTV_L3_STREAM_RETIRING;
            result = DTV_L3_ACCEPTED;
        } else {
            stream.phase = DTV_L3_STREAM_RETIRING;
            finalize_retire_locked(generation);
            finalized = true;
        }
        state_condition_.notify_all();
    }
    if (finalized) {
        finish_close();
    }
    return result;
}

dtv_l3_result OwnerEngine::destroy_stream(std::uint64_t generation) {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(generation);
    if (found == streams_.end()) {
        return DTV_L3_STALE;
    }
    if (found->second->phase != DTV_L3_STREAM_RETIRED ||
        found->second->read_borrows != 0 ||
        found->second->stop_residual) {
        return DTV_L3_BUSY;
    }
    streams_.erase(found);
    state_condition_.notify_all();
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::lease_status(std::uint32_t lease_id,
                                        dtv_l3_lease_status *status) {
    if (status == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease_id);
    if (found == leases_.end()) {
        return DTV_L3_STALE;
    }
    const auto &lease = found->second;
    status->state = lease.stop_residual ? DTV_L3_LEASE_RETAINED : lease.state;
    status->receiver_id = lease.receiver_id;
    status->bridge_id = lease.bridge_id;
    status->compatibility = lease.compatibility;
    status->tune_revision = lease.tune_revision;
    status->stream_generation = lease.active_stream;
    status->requested_hz = lease.requested_hz;
    status->programmed_hz = lease.programmed_hz;
    status->stop_residual = lease.stop_residual ? 1 : 0;
    const auto bridge = bridges_.find(lease.bridge_id);
    if (bridge == bridges_.end()) {
        status->bridge_active_users = 0;
        status->bridge_running = 0;
    } else {
        status->bridge_active_users =
            static_cast<std::uint32_t>(bridge->second.users);
        status->bridge_running = bridge->second.running ? 1 : 0;
    }
    status->reserved[0] = status->reserved[1] = 0;
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::release_lease(std::uint32_t lease_id) {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    {
        bool retry_stream_stop = false;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto lease = leases_.find(lease_id);
            if (lease != leases_.end() && lease->second.active_stream != 0) {
                const auto stream =
                    streams_.find(lease->second.active_stream);
                if (stream != streams_.end() &&
                    stream->second->stop_residual &&
                    !stream->second->attaching &&
                    !stream->second->stopping) {
                    generation = stream->second->generation;
                    retry_stream_stop = true;
                }
            }
        }
        if (retry_stream_stop) {
            (void)stop_stream(generation, 0);
        }
    }

    std::uint32_t bridge_id = 0;
    bool retry_stop = false;
    dtv_l3_result result = DTV_L3_BUSY;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = leases_.find(lease_id);
        if (found == leases_.end()) {
            result = DTV_L3_STALE;
        } else if (found->second.active_stream != 0) {
            result = DTV_L3_BUSY;
        } else if (!found->second.stop_residual) {
            leases_.erase(found);
            result = DTV_L3_OK;
        } else {
            bridge_id = found->second.bridge_id;
            const auto bridge = bridges_.find(bridge_id);
            if (bridge == bridges_.end() || !bridge->second.running) {
                leases_.erase(found);
                result = DTV_L3_OK;
            } else if (bridge->second.users != 0 ||
                       bridge->second.pending_attaches != 0 ||
                       bridge->second.stop_pending) {
                result = DTV_L3_BUSY;
            } else {
                bridge->second.stop_pending = true;
                retry_stop = true;
                ++provider_calls_in_flight_;
            }
        }
        state_condition_.notify_all();
    }

    if (!retry_stop) {
        finish_close();
        return result;
    }
    const auto stop_result = provider_.bridge_stop(bridge_id);
    result = DTV_L3_BUSY;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        auto &bridge = bridges_[bridge_id];
        bridge.stop_pending = false;
        const auto found = leases_.find(lease_id);
        if (found == leases_.end()) {
            result = DTV_L3_STALE;
        } else if (stop_result != DTV_L3_OK) {
            bridge.stop_residual = true;
            found->second.stop_residual = true;
            found->second.state = DTV_L3_LEASE_RETAINED;
        } else {
            bridge.running = false;
            bridge.stop_residual = false;
            ++bridge.stop_count;
            leases_.erase(found);
            result = DTV_L3_OK;
        }
        state_condition_.notify_all();
    }
    finish_close();
    return result;
}

void OwnerEngine::StreamSink::deliver(void *callback_context,
                                      const std::uint8_t *data,
                                      std::size_t size) {
    auto *sink = static_cast<StreamSink *>(callback_context);
    if (sink == nullptr || sink->owner == nullptr) {
        return;
    }
    sink->owner->on_ts_deliver(sink->generation, data, size);
}

void OwnerEngine::on_ts_deliver(std::uint64_t generation,
                                const std::uint8_t *data,
                                std::size_t size) {
    Borrow borrow(*this);
    if (!borrow) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = streams_.find(generation);
    if (found == streams_.end()) {
        return;
    }
    auto &stream = *found->second;
    if (stream.phase == DTV_L3_STREAM_RETIRING ||
        stream.phase == DTV_L3_STREAM_RETIRED) {
        return;
    }
    if (size == 0) {
        stream.eof_pending = true;
        state_condition_.notify_all();
        return;
    }
    if (data == nullptr) {
        return;
    }
    stream.staging.insert(stream.staging.end(), data, data + size);
    while (stream.staging.size() >= 188) {
        if (stream.queue.size() >= kQueueCapacityPackets) {
            ++stream.dropped_overflow_packets;
        } else {
            stream.queue.emplace_back(stream.staging.begin(),
                                      stream.staging.begin() + 188);
        }
        stream.staging.erase(stream.staging.begin(),
                             stream.staging.begin() + 188);
    }
}

void OwnerEngine::finalize_retire_locked(std::uint64_t generation) {
    const auto found = streams_.find(generation);
    if (found == streams_.end()) {
        return;
    }
    auto &stream = *found->second;
    if (stream.read_borrows != 0 || stream.stop_residual) {
        return;
    }
    if (stream.retire_mode == DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) {
        stream.discarded_unread_packets += stream.queue.size();
    }
    stream.queue.clear();
    stream.staging.clear();
    stream.phase = DTV_L3_STREAM_RETIRED;
    const auto lease = leases_.find(stream.lease_id);
    if (lease != leases_.end() && lease->second.active_stream == generation) {
        lease->second.active_stream = 0;
        lease->second.state = lease->second.stop_residual
                                  ? DTV_L3_LEASE_RETAINED
                                  : DTV_L3_LEASE_FREE;
    }
    if (!stream.token_exposed) {
        streams_.erase(generation);
    }
    state_condition_.notify_all();
}

void OwnerEngine::refresh_lease_locked(std::uint32_t lease_id) {
    const auto lease = leases_.find(lease_id);
    if (lease == leases_.end()) {
        return;
    }
    const auto bridge = bridges_.find(lease->second.bridge_id);
    const bool bridge_residual =
        bridge != bridges_.end() && bridge->second.stop_residual;
    bool stream_residual = false;
    for (const auto &entry : streams_) {
        if (entry.second->lease_id == lease_id &&
            entry.second->stop_residual) {
            stream_residual = true;
            break;
        }
    }
    lease->second.stop_residual = stream_residual || bridge_residual;
    if (lease->second.active_stream == 0) {
        lease->second.state = lease->second.stop_residual
                                  ? DTV_L3_LEASE_RETAINED
                                  : DTV_L3_LEASE_FREE;
    }
    state_condition_.notify_all();
}

void OwnerEngine::retire_stream_for_close(std::uint64_t generation) {
    std::shared_ptr<StreamRecord> stream;
    std::uint32_t bridge_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = streams_.find(generation);
        if (found == streams_.end()) {
            --provider_calls_in_flight_;
            state_condition_.notify_all();
        } else {
            stream = found->second;
            bridge_id = stream->bridge_id;
        }
    }
    if (!stream) {
        finish_close();
        return;
    }

    CallbackFrame frame;
    const auto stop_result = provider_.stream_stop(generation);
    bool need_bridge_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream->stopping = false;
        stream->phase = DTV_L3_STREAM_RETIRING;
        stream->retire_requested = true;
        stream->retire_mode = DTV_L3_STREAM_RELEASE_DISCARD_UNREAD;
        if (stream->user_counted) {
            stream->user_counted = false;
            auto &bridge = bridges_[bridge_id];
            if (bridge.users > 0) {
                --bridge.users;
            }
        }
        if (stop_result == DTV_L3_OK) {
            stream->stop_residual = false;
            stream->physical_stopped = true;
        } else {
            stream->stop_residual = true;
            auto lease = leases_.find(stream->lease_id);
            if (lease != leases_.end()) {
                lease->second.stop_residual = true;
                lease->second.state = DTV_L3_LEASE_RETAINED;
            }
        }
        auto &bridge = bridges_[bridge_id];
        if (bridge.users == 0 && bridge.pending_attaches == 0 &&
            bridge.running && !bridge.stop_pending) {
            bridge.stop_pending = true;
            need_bridge_stop = true;
            ++provider_calls_in_flight_;
        }
        state_condition_.notify_all();
    }

    if (need_bridge_stop) {
        CallbackFrame frame;
        const auto bridge_result = provider_.bridge_stop(bridge_id);
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        auto &bridge = bridges_[bridge_id];
        bridge.stop_pending = false;
        if (bridge_result == DTV_L3_OK) {
            bridge.running = false;
            bridge.stop_residual = false;
            ++bridge.stop_count;
        } else {
            bridge.running = true;
            bridge.stop_residual = true;
            const auto lease = leases_.find(stream->lease_id);
            if (lease != leases_.end()) {
                lease->second.stop_residual = true;
                lease->second.state = DTV_L3_LEASE_RETAINED;
            }
        }
        refresh_lease_locked(stream->lease_id);
        state_condition_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        if (stream->read_borrows == 0 && !stream->stop_residual) {
            finalize_retire_locked(generation);
        }
        refresh_lease_locked(stream->lease_id);
        state_condition_.notify_all();
    }
    finish_close();
}

dtv_l3_result OwnerEngine::cancel(const std::shared_ptr<Operation> &operation) {
    if (!operation) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    bool before_submit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = operations_.find(operation->id);
        if (found == operations_.end() || found->second != operation) {
            return DTV_L3_INVALID;
        }
        if (operation->state != DTV_L3_OP_OPENING &&
            operation->state != DTV_L3_OP_RUNNING) {
            return DTV_L3_OK;
        }
        operation->cancel_requested = true;
        if (operation->cancel_in_progress || operation->cancel_sent) {
            return DTV_L3_ACCEPTED;
        }
        before_submit = !operation->submitted;
        operation->cancel_in_progress = true;
        ++provider_calls_in_flight_;
    }
    const auto result = provider_.cancel(operation->binding.get(), operation->id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        operation->cancel_in_progress = false;
        operation->cancel_sent =
            !(before_submit && result == DTV_L3_INVALID);
        if (before_submit && result == DTV_L3_INVALID && operation->submitted) {
            queue_cancel_locked(operation);
        }
        state_condition_.notify_all();
    }
    finish_close();
    return result;
}

dtv_l3_result OwnerEngine::cancel_open_if_opening() {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_state_ != OpenState::Opening || close_requested_) {
            return DTV_L3_OK;
        }
        close_requested_ = true;
        if (initialization_phase_ != InitializationPhase::NotStarted) {
            open_state_ = OpenState::Closing;
        }
        state_condition_.notify_all();
    }
    finish_close();
    return DTV_L3_ACCEPTED;
}

dtv_l3_result OwnerEngine::poll(
    const std::shared_ptr<Operation> &operation,
    dtv_l3_op_state *state) {
    if (!operation || state == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = operations_.find(operation->id);
    if (found == operations_.end() || found->second != operation) {
        return DTV_L3_INVALID;
    }
    *state = operation->state;
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::poll_result(
    const std::shared_ptr<Operation> &operation,
    dtv_l3_op_state *state,
    dtv_l3_result *result,
    const std::uint8_t **data,
    std::size_t *data_size) {
    if (!operation || state == nullptr || result == nullptr || data == nullptr ||
        data_size == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = operations_.find(operation->id);
    if (found == operations_.end() || found->second != operation) {
        return DTV_L3_INVALID;
    }
    *state = operation->state;
    *result = operation->result;
    *data = operation->response.empty() ? nullptr : operation->response.data();
    *data_size = operation->response.size();
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::wait_operation(
    const std::shared_ptr<Operation> &operation,
    std::uint32_t timeout_ms) {
    if (!operation) {
        return DTV_L3_INVALID;
    }
    const bool nonblocking = in_callback_frame();
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = operations_.find(operation->id);
    if (found == operations_.end() || found->second != operation) {
        return DTV_L3_INVALID;
    }
    const auto terminal = [&operation] {
        return operation->state == DTV_L3_OP_DONE ||
               operation->state == DTV_L3_OP_FAILED;
    };
    const auto ready = [this, &terminal] {
        return terminal() || close_requested_;
    };
    if (nonblocking) {
        if (terminal()) {
            return DTV_L3_OK;
        }
        return close_requested_ ? DTV_L3_CLOSING : DTV_L3_BUSY;
    }
    if (!ready()) {
        if (timeout_ms == DTV_L3_WAIT_FOREVER) {
            state_condition_.wait(lock, ready);
        } else {
            state_condition_.wait_for(lock, wait_duration(timeout_ms), ready);
        }
    }
    if (terminal()) {
        return DTV_L3_OK;
    }
    return close_requested_ ? DTV_L3_CLOSING : DTV_L3_WAIT_TIMEOUT;
}

dtv_l3_result OwnerEngine::wait_open(std::uint32_t timeout_ms) {
    const bool nonblocking = in_callback_frame();
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const auto terminal = [this] {
        return open_state_ == OpenState::Ready ||
               open_state_ == OpenState::Failed ||
               open_state_ == OpenState::Closed;
    };
    const auto ready = [this, &terminal] {
        return terminal() || close_requested_;
    };
    if (nonblocking) {
        if (terminal()) {
            return DTV_L3_OK;
        }
        return close_requested_ ? DTV_L3_CLOSING : DTV_L3_BUSY;
    }
    if (!ready()) {
        if (timeout_ms == DTV_L3_WAIT_FOREVER) {
            state_condition_.wait(lock, ready);
        } else {
            state_condition_.wait_for(lock, wait_duration(timeout_ms), ready);
        }
    }
    if (terminal()) {
        return DTV_L3_OK;
    }
    return close_requested_ ? DTV_L3_CLOSING : DTV_L3_WAIT_TIMEOUT;
}

dtv_l3_result OwnerEngine::wait_read_stream(
    std::uint64_t generation,
    std::uint8_t *buffer,
    std::size_t capacity_bytes,
    std::size_t *bytes_read,
    dtv_l3_read_state *state,
    std::uint32_t timeout_ms) {
    if (bytes_read == nullptr || state == nullptr ||
        (capacity_bytes != 0 && buffer == nullptr) ||
        capacity_bytes % 188 != 0) {
        return DTV_L3_INVALID;
    }
    if (in_callback_frame()) {
        return DTV_L3_BUSY;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }

    std::shared_ptr<StreamRecord> stream;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = streams_.find(generation);
    if (found == streams_.end() ||
        found->second->phase == DTV_L3_STREAM_RETIRED) {
        return DTV_L3_STALE;
    }
    stream = found->second;
    if (close_requested_) {
        return DTV_L3_CLOSING;
    }
    if (stream->phase != DTV_L3_STREAM_ACTIVE &&
        stream->phase != DTV_L3_STREAM_DRAINING) {
        return DTV_L3_STALE;
    }
    const auto readable = [this, &stream] {
        return !stream->queue.empty() || stream->eof_pending ||
               stream->retire_requested || close_requested_ ||
               stream->phase == DTV_L3_STREAM_RETIRING ||
               stream->phase == DTV_L3_STREAM_RETIRED;
    };
    const bool data_ready = !stream->queue.empty();
    const bool eof_ready = stream->eof_pending && stream->queue.empty();
    if (!data_ready && !eof_ready) {
        ++stream->read_borrows;
        if (timeout_ms == DTV_L3_WAIT_FOREVER) {
            state_condition_.wait(lock, readable);
        } else {
            state_condition_.wait_for(lock, wait_duration(timeout_ms),
                                      readable);
        }
        if (stream->read_borrows > 0) {
            --stream->read_borrows;
        }
    }

    const bool finalized = [this, &stream, generation] {
        if (stream->retire_requested && stream->read_borrows == 0) {
            finalize_retire_locked(generation);
            return true;
        }
        return false;
    }();

    if (close_requested_) {
        *bytes_read = 0;
        *state = DTV_L3_READ_WAIT;
        lock.unlock();
        if (finalized) {
            finish_close();
        }
        return DTV_L3_CLOSING;
    }
    if (stream->phase == DTV_L3_STREAM_RETIRING ||
        stream->phase == DTV_L3_STREAM_RETIRED) {
        *bytes_read = 0;
        *state = DTV_L3_READ_WAIT;
        lock.unlock();
        if (finalized) {
            finish_close();
        }
        return DTV_L3_STALE;
    }

    std::size_t packets = 0;
    const std::size_t limit = capacity_bytes / 188;
    while (packets < limit && !stream->queue.empty()) {
        std::memcpy(buffer + packets * 188, stream->queue.front().data(), 188);
        stream->queue.pop_front();
        ++packets;
    }
    *bytes_read = packets * 188;
    if (packets != 0) {
        *state = DTV_L3_READ_DATA;
    } else if (stream->eof_pending) {
        stream->eof_observed = true;
        *state = DTV_L3_READ_EOF;
    } else {
        *state = DTV_L3_READ_WAIT;
    }
    state_condition_.notify_all();
    lock.unlock();
    if (finalized) {
        finish_close();
    }
    return *state == DTV_L3_READ_WAIT ? DTV_L3_WAIT_TIMEOUT : DTV_L3_OK;
}

dtv_l3_result OwnerEngine::release_operation(
    const std::shared_ptr<Operation> &operation) {
    if (!operation) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = operations_.find(operation->id);
        if (found == operations_.end() || found->second != operation) {
            return DTV_L3_INVALID;
        }
        if (operation->state == DTV_L3_OP_OPENING ||
            operation->state == DTV_L3_OP_RUNNING) {
            return DTV_L3_BUSY;
        }
        operation->released = true;
        operations_.erase(found);
    }
    finish_close();
    return DTV_L3_OK;
}


dtv_l3_result OwnerEngine::close(std::uint32_t timeout_ms) {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    const bool nonblocking = in_callback_frame();

    std::vector<std::shared_ptr<Operation>> pending;
    std::vector<std::uint64_t> stop_streams;
    std::vector<std::uint64_t> finish_streams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        close_requested_ = true;
        if (open_state_ != OpenState::Opening && open_state_ != OpenState::Failed &&
            open_state_ != OpenState::Closed) {
            open_state_ = OpenState::Closing;
        }
        for (const auto &entry : operations_) {
            const auto &op = entry.second;
            if (op->state == DTV_L3_OP_OPENING ||
                op->state == DTV_L3_OP_RUNNING) {
                op->cancel_requested = true;
                pending.push_back(op);
            }
        }
        for (const auto &op : pending) {
            queue_cancel_locked(op);
        }
        for (const auto &entry : streams_) {
            auto &stream = *entry.second;
            if (stream.phase == DTV_L3_STREAM_RETIRED) {
                continue;
            }
            stream.retire_requested = true;
            stream.retire_mode = DTV_L3_STREAM_RELEASE_DISCARD_UNREAD;
            if (stream.attaching || stream.stopping) {
                stream.phase = DTV_L3_STREAM_RETIRING;
                stream.close_retire = true;
            } else if (stream.phase == DTV_L3_STREAM_ACTIVE) {
                stream.phase = DTV_L3_STREAM_RETIRING;
                stream.stopping = true;
                stream.close_retire = true;
                ++provider_calls_in_flight_;
                stop_streams.push_back(stream.generation);
            } else {
                stream.phase = DTV_L3_STREAM_RETIRING;
                if (stream.read_borrows == 0 && !stream.stop_residual) {
                    finish_streams.push_back(stream.generation);
                }
            }
        }
        for (const auto generation : finish_streams) {
            finalize_retire_locked(generation);
        }
        for (const auto generation : stop_streams) {
            enqueue_locked([this, generation] {
                retire_stream_for_close(generation);
            });
        }
        state_condition_.notify_all();
        work_condition_.notify_all();
    }

    finish_close();

    std::unique_lock<std::mutex> lock(mutex_);
    if (nonblocking) {
        return physical_quiesced_ && physical_settled_locked()
                   ? DTV_L3_OK
                   : DTV_L3_ACCEPTED;
    }
    if (timeout_ms != DTV_L3_WAIT_FOREVER && timeout_ms != 0 &&
        !(physical_quiesced_ && physical_settled_locked())) {
        state_condition_.wait_for(
            lock, wait_duration(timeout_ms), [this] {
                return physical_quiesced_ && physical_settled_locked();
            });
    } else if (timeout_ms == DTV_L3_WAIT_FOREVER &&
               !(physical_quiesced_ && physical_settled_locked())) {
        state_condition_.wait(lock, [this] {
            return physical_quiesced_ && physical_settled_locked();
        });
    }
    return physical_quiesced_ && physical_settled_locked() ? DTV_L3_OK
                                                           : DTV_L3_TIMEOUT;
}

dtv_l3_result OwnerEngine::close_status(Snapshot *snapshot) {
    if (snapshot == nullptr) {
        return DTV_L3_INVALID;
    }
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot->open_state = open_state_;
    snapshot->physical_quiesced = physical_quiesced_;
    snapshot->all_ops_recovered = all_ops_recovered_locked();
    snapshot->cleanup_complete = cleanup_complete_;
    snapshot->active_borrows = active_borrows_;
    snapshot->claims_attempted = std::count_if(
        claims_.begin(), claims_.end(), [](ClaimState state) {
            return state != ClaimState::NotAttempted;
        });
    snapshot->transfer_ready_claims =
        std::count(claims_.begin(), claims_.end(), ClaimState::Ready);
    snapshot->cleanup_claims_remaining = std::count_if(
        claims_.begin(), claims_.end(), [](ClaimState state) {
            return state == ClaimState::InFlight || state == ClaimState::Ready ||
                   state == ClaimState::Retained || state == ClaimState::Unknown;
        });
    snapshot->cleanup_epoch = cleanup_epoch_;
    snapshot->last_cleanup_error = last_cleanup_error_;
    snapshot->open_result = open_result_;
    snapshot->cleanup_result = last_cleanup_error_;
    snapshot->restoration_failed = restoration_failed_;
    snapshot->open_lane = open_lane_;
    snapshot->control_lane = control_lane_;
    const auto open_lane_live =
        open_lane_spawned_ &&
        (open_lane_ == DTV_L3_LANE_RUNNING ||
         open_lane_ == DTV_L3_LANE_EXIT_COMMITTED);
    snapshot->live_lanes =
        (open_lane_live ? 1u : 0u) +
        (control_lane_ == DTV_L3_LANE_RUNNING ||
         control_lane_ == DTV_L3_LANE_EXIT_COMMITTED ? 1u : 0u);
    snapshot->releasable =
        physical_quiesced_ && cleanup_complete_ && all_ops_recovered_locked() &&
        lanes_exit_committed_locked() && provider_calls_in_flight_ == 0 &&
        work_queue_.empty();
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::retry_cleanup() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (retired_gate_ || released_ || !close_requested_ || cleanup_started_ ||
        !cleanup_attempted_ || provider_calls_in_flight_ != 0 ||
        claim_in_flight_ || !operations_.empty() ||
        !physical_settled_locked()) {
        return DTV_L3_BUSY;
    }
    if (std::find(claims_.begin(), claims_.end(), ClaimState::Unknown) !=
        claims_.end()) {
        return DTV_L3_BUSY;
    }
    if (!cleanup_claims_locked()) {
        return physical_quiesced_ ? DTV_L3_OK : DTV_L3_BUSY;
    }
    cleanup_started_ = false;
    cleanup_attempted_ = false;
    cleanup_complete_ = false;
    physical_quiesced_ = false;
    ++cleanup_epoch_;
    lock.unlock();
    finish_close();
    return DTV_L3_ACCEPTED;
}

dtv_l3_result OwnerEngine::read_ts(const std::function<void()> &after_borrow) {
    Borrow borrow(*this);
    if (!borrow) {
        return DTV_L3_BUSY;
    }
    std::shared_ptr<void> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream = stream_;
    }
    if (!stream) {
        return DTV_L3_FAILED;
    }
    if (after_borrow) {
        after_borrow();
    }
    return DTV_L3_OK;
}

dtv_l3_result OwnerEngine::release(
    const std::function<void()> &after_retire_gate) {
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (released_) {
            return DTV_L3_INVALID;
        }
        maybe_commit_control_exit_locked();
        if (!physical_quiesced_ || !all_ops_recovered_locked() ||
            !cleanup_complete_ || !streams_.empty() || !leases_.empty() ||
            !lanes_exit_committed_locked()) {
            return DTV_L3_BUSY;
        }
        epoch = cleanup_epoch_;
        retired_gate_ = true;
    }

    if (after_retire_gate) {
        after_retire_gate();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_borrows_ != 0 || cleanup_epoch_ != epoch ||
        !physical_quiesced_ || !cleanup_complete_ ||
        !all_ops_recovered_locked() || !streams_.empty() ||
        !leases_.empty() || !lanes_exit_committed_locked()) {
        return DTV_L3_BUSY;
    }
    released_ = true;
    ++release_count_;
    stream_.reset();
    binding_.reset();
    return DTV_L3_OK;
}

std::shared_ptr<Binding> OwnerEngine::binding() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return binding_;
}

std::size_t OwnerEngine::cleanup_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cleanup_count_;
}

std::size_t OwnerEngine::release_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return release_count_;
}

std::size_t OwnerEngine::live_operation_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operations_.size();
}

std::size_t OwnerEngine::live_lease_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return leases_.size();
}

std::size_t OwnerEngine::live_stream_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.size();
}

std::weak_ptr<void> OwnerEngine::stream_lifetime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_;
}

void OwnerEngine::completion_thunk(
    Binding *binding,
    std::uint64_t id,
    const dtv_l3_transfer_completion *completion,
    void *callback_context) {
    if (completion == nullptr || callback_context == nullptr) {
        return;
    }
    auto *operation = static_cast<Operation *>(callback_context);
    CallbackFrame frame;
    operation->owner->on_completion(binding, id, *completion, operation);
}

void OwnerEngine::on_completion(
    Binding *binding,
    std::uint64_t id,
    const dtv_l3_transfer_completion &completion,
    Operation *operation) {
    Borrow borrow(*this);
    if (!borrow) {
        return;
    }
    bool should_finish_close = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = operations_.find(id);
        if (binding != binding_.get() || found == operations_.end() ||
            found->second.get() != operation ||
            operation->state == DTV_L3_OP_DONE ||
            operation->state == DTV_L3_OP_FAILED) {
            return;
        }
        operation->result = completion.result;
        if (completion.size != 0 && completion.data != nullptr) {
            operation->response.assign(
                completion.data, completion.data + completion.size);
        }
        operation->state = completion.result == DTV_L3_OK
                               ? DTV_L3_OP_DONE
                               : DTV_L3_OP_FAILED;
        should_finish_close = close_requested_;
    }
    if (should_finish_close) {
        finish_close();
    }
}

void OwnerEngine::finish_close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!close_requested_ || cleanup_started_ || cleanup_attempted_ ||
        cleanup_complete_ ||
        (initialization_phase_ != InitializationPhase::ResponsibilitiesRegistered &&
         initialization_phase_ != InitializationPhase::Returned) ||
        open_state_ == OpenState::Opening || provider_calls_in_flight_ != 0) {
        return;
    }
    for (const auto &entry : operations_) {
        if (entry.second->state == DTV_L3_OP_OPENING ||
            entry.second->state == DTV_L3_OP_RUNNING) {
            return;
        }
    }
    if (!physical_settled_locked()) {
        return;
    }
    cleanup_started_ = true;
    cleanup_attempted_ = true;
    enqueue_locked([this] { run_cleanup(); });
}

void OwnerEngine::queue_cancel_locked(
    const std::shared_ptr<Operation> &operation) {
    if (operation->cancel_in_progress ||
        operation->cancel_sent || operation->state == DTV_L3_OP_DONE ||
        operation->state == DTV_L3_OP_FAILED) {
        return;
    }
    operation->cancel_in_progress = true;
    ++provider_calls_in_flight_;
    const bool before_submit = !operation->submitted;
    enqueue_locked([this, operation, before_submit] {
        run_cancel(operation, before_submit);
    });
}

void OwnerEngine::run_cancel(const std::shared_ptr<Operation> &operation,
                             bool before_submit) {
    CallbackFrame frame;
    const auto result = provider_.cancel(operation->binding.get(), operation->id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        operation->cancel_in_progress = false;
        operation->cancel_sent =
            !(before_submit && result == DTV_L3_INVALID);
        if (before_submit && result == DTV_L3_INVALID && operation->submitted) {
            queue_cancel_locked(operation);
        }
        state_condition_.notify_all();
    }
    finish_close();
}

void OwnerEngine::run_cleanup() {
    std::shared_ptr<Binding> binding;
    std::vector<std::uint32_t> release_indices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        binding = binding_;
        for (std::uint32_t index = 0; index < claims_.size(); ++index) {
            if (claims_[index] == ClaimState::Ready ||
                claims_[index] == ClaimState::Retained) {
                release_indices.push_back(index);
            }
        }
    }

    for (auto index = release_indices.rbegin(); index != release_indices.rend();
         ++index) {
        CallbackFrame frame;
        const auto result = provider_.release_claim(binding.get(), *index);
        std::lock_guard<std::mutex> lock(mutex_);
        claim_quiesced_[*index] =
            result.quiescence == DTV_L3_RELEASE_QUIESCED;
        if (result.residual_state == DTV_L3_CLAIM_NONE) {
            claims_[*index] = ClaimState::Released;
        } else if (result.residual_state == DTV_L3_CLAIM_RETAINED) {
            claims_[*index] = ClaimState::Retained;
        } else {
            claims_[*index] = ClaimState::Unknown;
        }
        claim_cleanup_errors_[*index] = result.error;
        if (result.restoration == DTV_L3_RESTORE_FAILED ||
            result.restoration == DTV_L3_RESTORE_UNKNOWN) {
            restoration_failed_ = true;
            if (claim_cleanup_errors_[*index] == DTV_L3_OK) {
                claim_cleanup_errors_[*index] = DTV_L3_FAILED;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        claimed_count_ = std::count(claims_.begin(), claims_.end(), ClaimState::Ready);
        ++cleanup_count_;
        physical_quiesced_ =
            std::all_of(claim_quiesced_.begin(), claim_quiesced_.end(),
                        [](bool quiesced) { return quiesced; }) &&
            physical_settled_locked();
        last_cleanup_error_ = DTV_L3_OK;
        for (auto error = claim_cleanup_errors_.rbegin();
             error != claim_cleanup_errors_.rend(); ++error) {
            if (*error != DTV_L3_OK) {
                last_cleanup_error_ = *error;
            }
        }
        cleanup_complete_ = !cleanup_claims_locked();
        if (open_state_ != OpenState::Failed) {
            open_state_ = OpenState::Closed;
        }
        cleanup_started_ = false;
        maybe_commit_control_exit_locked();
        state_condition_.notify_all();
        work_condition_.notify_all();
    }
}

bool OwnerEngine::cleanup_claims_locked() const {
    return std::any_of(claims_.begin(), claims_.end(), [](ClaimState state) {
        return state == ClaimState::InFlight || state == ClaimState::Ready ||
               state == ClaimState::Retained || state == ClaimState::Unknown;
    });
}

void OwnerEngine::worker_loop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_condition_.wait(lock, [this] {
                return worker_stopping_ || !work_queue_.empty() ||
                       control_lane_may_exit_locked();
            });
            if (work_queue_.empty()) {
                if (worker_stopping_) {
                    return;
                }
                if (control_lane_may_exit_locked()) {
                    control_lane_ = DTV_L3_LANE_EXIT_COMMITTED;
                    state_condition_.notify_all();
                    return;
                }
            }
            work = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        work();
    }
}

void OwnerEngine::enqueue_locked(std::function<void()> work) {
    work_queue_.push_back(std::move(work));
    work_condition_.notify_one();
}

bool OwnerEngine::control_lane_may_exit_locked() const {
    if (!close_requested_ || !cleanup_complete_ || !physical_quiesced_ ||
        provider_calls_in_flight_ != 0 || !work_queue_.empty() ||
        initialization_phase_ != InitializationPhase::Returned) {
        return false;
    }
    return std::none_of(operations_.begin(), operations_.end(),
                        [](const auto &entry) {
                            return entry.second->state == DTV_L3_OP_OPENING ||
                                   entry.second->state == DTV_L3_OP_RUNNING;
                        });
}

void OwnerEngine::maybe_commit_control_exit_locked() {
    if (control_lane_ == DTV_L3_LANE_NOT_SPAWNED ||
        control_lane_ == DTV_L3_LANE_EXITED) {
        return;
    }
    if (control_lane_may_exit_locked()) {
        control_lane_ = DTV_L3_LANE_EXIT_COMMITTED;
        work_condition_.notify_all();
    }
}

bool OwnerEngine::lanes_exit_committed_locked() const {
    const auto open_lane_ok =
        open_lane_ == DTV_L3_LANE_NOT_SPAWNED ||
        open_lane_ == DTV_L3_LANE_EXIT_COMMITTED ||
        open_lane_ == DTV_L3_LANE_EXITED;
    const auto control_lane_ok =
        control_lane_ == DTV_L3_LANE_NOT_SPAWNED ||
        control_lane_ == DTV_L3_LANE_EXIT_COMMITTED ||
        control_lane_ == DTV_L3_LANE_EXITED;
    return open_lane_ok && control_lane_ok;
}

bool OwnerEngine::all_ops_recovered_locked() const {
    return operations_.empty() && physical_settled_locked();
}

bool OwnerEngine::active_streams_locked() const {
    return std::any_of(streams_.begin(), streams_.end(),
                       [](const auto &entry) {
                           return entry.second->phase != DTV_L3_STREAM_RETIRED;
                       });
}

bool OwnerEngine::physical_settled_locked() const {
    if (active_streams_locked()) {
        return false;
    }
    return std::none_of(bridges_.begin(), bridges_.end(),
                        [](const auto &entry) {
                            const auto &bridge = entry.second;
                            return bridge.running || bridge.users != 0 ||
                                   bridge.pending_attaches != 0 ||
                                   bridge.start_pending || bridge.stop_pending ||
                                   bridge.stop_residual;
                        });
}

void TestBarrier::arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    arrived_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
}

void TestBarrier::wait_until_arrived() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return arrived_; });
}

void TestBarrier::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
}

bool in_callback_frame() {
    return callback_frame_depth > 0;
}

CallbackFrame::CallbackFrame() {
    ++callback_frame_depth;
}

CallbackFrame::~CallbackFrame() {
    --callback_frame_depth;
}

void set_test_lane_start_failure_mask(int mask) {
    lane_start_failure_mask.store(mask, std::memory_order_release);
}

bool test_lane_start_fails(int lane_mask_bit) {
    return (lane_start_failure_mask.load(std::memory_order_acquire) &
            lane_mask_bit) != 0;
}

}  // namespace l3_owner_prototype
