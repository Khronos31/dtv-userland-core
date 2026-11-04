/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>

namespace l3_owner_prototype {

namespace {
// Prototype candidate; queue capacity is not a frozen product value.
constexpr std::size_t kQueueCapacityPackets = 4;
constexpr int kOpenLaneMask = 1;
constexpr int kControlLaneMask = 2;

thread_local int callback_frame_depth = 0;
std::atomic<int> lane_start_failure_mask{0};
std::atomic<unsigned int> test_exception_mask{0};

void throw_if_test_bit(unsigned int bit) {
    unsigned int current = test_exception_mask.load(std::memory_order_acquire);
    while ((current & bit) != 0) {
        if (test_exception_mask.compare_exchange_weak(
                current, current & ~bit, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw std::bad_alloc();
        }
    }
}

std::chrono::milliseconds wait_duration(std::uint32_t timeout_ms) {
    return std::chrono::milliseconds(timeout_ms);
}

std::uint64_t monotonic_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::chrono::steady_clock::time_point deadline_time_point(
    std::uint64_t deadline_ns) {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(deadline_ns));
}

struct LaneStartFailure {};
}  // namespace

OwnerEngine::OwnerEngine(Provider &provider, std::uint64_t binding_value)
    : OwnerEngine(provider, binding_value,
                  dtv_l3_logger{nullptr, nullptr, DTV_L3_LOG_ERROR}) {}

OwnerEngine::OwnerEngine(Provider &provider,
                         std::uint64_t binding_value,
                         const dtv_l3_logger &logger)
    : OwnerEngine(provider, binding_value, logger, nullptr) {}

OwnerEngine::OwnerEngine(Provider &provider,
                         std::uint64_t binding_value,
                         const dtv_l3_logger &logger,
                         ProductAdapter *product_adapter)
    : provider_(provider),
      logger_(logger),
      product_adapter_(product_adapter),
      binding_(std::make_shared<Binding>(binding_value)) {
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

ProductAdapter &OwnerEngine::product() {
    return product_adapter_ == nullptr
               ? static_cast<ProductAdapter &>(provider_)
               : *product_adapter_;
}

void OwnerEngine::log(dtv_l3_log_level level,
                      const char *fn,
                      const char *message) const noexcept {
    if (logger_.fn == nullptr || level > logger_.min_level) {
        return;
    }
    CallbackFrame frame;
    try {
        logger_.fn(logger_.userdata, level, fn, message);
    } catch (...) {
        /* Logging failure never propagates or recurses into another log. */
    }
}

void OwnerEngine::abort_open_after_lane_exception() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        close_requested_ = true;
        if (open_state_ != OpenState::Failed &&
            open_state_ != OpenState::Closed) {
            open_state_ = OpenState::Failed;
        }
        open_result_ = DTV_L3_FAILED;
        claims_.clear();
        claim_quiesced_.clear();
        claim_cleanup_errors_.clear();
        claim_cancel_requested_.clear();
        claim_in_flight_ = false;
        in_flight_claim_valid_ = false;
        initialization_phase_ = InitializationPhase::Returned;
        open_lane_ = DTV_L3_LANE_EXIT_COMMITTED;
        maybe_commit_control_exit_locked();
        state_condition_.notify_all();
        work_condition_.notify_all();
    }
    log(DTV_L3_LOG_ERROR, "open_lane", "contained C++ exception");
    finish_close();
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
        claim_cancel_requested_.assign(count, false);
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
            in_flight_claim_valid_ = true;
            in_flight_claim_index_ = index;
            ++provider_calls_in_flight_;
            binding = binding_;
        }

        CallbackFrame frame;
        dtv_l3_claim_result claim{DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
        bool claim_threw = false;
        try {
            maybe_throw_test_exception(kTestInjectClaim);
            claim = provider_.claim(binding.get(), index);
        } catch (...) {
            claim_threw = true;
            claim = {DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
        }
        if (claim_threw) {
            log(DTV_L3_LOG_WARN, "provider.claim",
                "contained C++ exception");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            claim_in_flight_ = false;
            in_flight_claim_valid_ = false;
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
        (request.kind == DTV_L3_TRANSFER_CONTROL &&
         (request.endpoint != 0 ||
          ((request.control.request_type & 0x80u) != 0
               ? request.out_size != 0
               : request.in_capacity != 0))) ||
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
    const std::uint32_t pipe_key =
        request.kind == DTV_L3_TRANSFER_CONTROL ? 0u : request.endpoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.claim_index >= claims_.size()) {
            return DTV_L3_INVALID;
        }
        if (disconnected_) {
            return DTV_L3_DISCONNECTED;
        }
        if (open_state_ != OpenState::Ready || close_requested_ ||
            claims_[request.claim_index] != ClaimState::Ready) {
            return DTV_L3_BUSY;
        }
        for (const auto &entry : operations_) {
            const auto &other = entry.second;
            if ((other->state == DTV_L3_OP_OPENING ||
                 other->state == DTV_L3_OP_RUNNING) &&
                other->pipe_key == pipe_key &&
                other->request_template.timeout_ms != request.timeout_ms) {
                return DTV_L3_INVALID;
            }
        }
        for (const auto &entry : in_flight_) {
            const auto &other = entry.second;
            if (other->pipe_key == pipe_key &&
                other->request_template.timeout_ms != request.timeout_ms) {
                return DTV_L3_INVALID;
            }
        }
        binding = binding_;
        op->owner = this;
        op->binding = binding;
        op->id = next_transfer_id_++;
        op->request_template = request;
        op->request_template.out_data = nullptr;
        op->pipe_key = pipe_key;
        if (request.out_size != 0) {
            maybe_throw_test_exception(kTestInjectStartPayload);
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
    bool submit_threw = false;
    const auto result = [&] {
        try {
            maybe_throw_test_exception(kTestInjectStartSubmit);
            return provider_.submit(binding.get(), op->id, provider_request,
                                    &OwnerEngine::completion_thunk, op.get());
        } catch (...) {
            submit_threw = true;
            return DTV_L3_FAILED;
        }
    }();
    bool pending_resubmit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        if (result == DTV_L3_ACCEPTED) {
            op->submitted = true;
            if (!op->completion_received) {
                in_flight_.emplace(op->id, op);
            }
            if (op->cancel_requested && !op->resubmit_pending) {
                queue_cancel_locked(op);
            }
            pending_resubmit = op->resubmit_pending;
        } else {
            if (op->state == DTV_L3_OP_OPENING ||
                op->state == DTV_L3_OP_RUNNING) {
                op->state = DTV_L3_OP_FAILED;
                op->result = result;
            }
            op->submitted = true;
            op->resubmit_pending = false;
        }
        state_condition_.notify_all();
        work_condition_.notify_all();
    }
    if (submit_threw) {
        log(DTV_L3_LOG_WARN, "provider.submit",
            "contained C++ exception");
    }
    if (pending_resubmit) {
        resubmit_operation(op);
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
    dtv_l3_result scope_result = DTV_L3_FAILED;
    bool scope_threw = false;
    try {
        scope_result = product().scope_query(receiver_id, &scope);
    } catch (...) {
        scope_threw = true;
        scope_result = DTV_L3_FAILED;
    }
    if (scope_threw) {
        log(DTV_L3_LOG_WARN, "provider.scope_query",
            "contained C++ exception");
    }
    if (scope_result != DTV_L3_OK) {
        return scope_result;
    }
    if (scope.compatibility != DTV_L3_LEASE_SHAREABLE &&
        scope.compatibility != DTV_L3_LEASE_EXCLUSIVE) {
        return DTV_L3_INVALID;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (disconnected_) {
        return DTV_L3_DISCONNECTED;
    }
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
        if (disconnected_) {
            return DTV_L3_DISCONNECTED;
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
    dtv_l3_result tune_result = DTV_L3_FAILED;
    bool tune_threw = false;
    try {
        tune_result = product().tune(receiver_id, requested_hz, &programmed);
    } catch (...) {
        tune_threw = true;
        tune_result = DTV_L3_FAILED;
    }

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
    if (tune_threw) {
        log(DTV_L3_LOG_WARN, "provider.tune",
            "contained C++ exception");
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
    if (disconnected_) {
        return DTV_L3_DISCONNECTED;
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
        if (disconnected_) {
            return DTV_L3_DISCONNECTED;
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
        dtv_l3_result start_result = DTV_L3_FAILED;
        bool start_threw = false;
        try {
            start_result = product().bridge_start(bridge_id, receiver_id);
        } catch (...) {
            start_threw = true;
            start_result = DTV_L3_FAILED;
        }
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
        if (start_threw) {
            log(DTV_L3_LOG_WARN, "provider.bridge_start",
                "contained C++ exception");
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
        bool attach_threw = false;
        try {
            attach_result = product().stream_attach(
                generation, &StreamSink::deliver, stream->sink.get());
        } catch (...) {
            attach_threw = true;
            attach_result = DTV_L3_FAILED;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            stream->attaching = false;
            abort = attach_result != DTV_L3_OK || close_requested_;
            state_condition_.notify_all();
        }
        if (attach_threw) {
            log(DTV_L3_LOG_WARN, "provider.stream_attach",
                "contained C++ exception");
        }
    }
    if (abort) {
        const bool attached = attach_result == DTV_L3_OK;
        dtv_l3_result stop_result = DTV_L3_OK;
        if (attached) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++provider_calls_in_flight_;
            }
            bool stop_threw = false;
            try {
                stop_result = product().stream_stop(generation);
            } catch (...) {
                stop_threw = true;
                stop_result = DTV_L3_FAILED;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --provider_calls_in_flight_;
                state_condition_.notify_all();
            }
            if (stop_threw) {
                log(DTV_L3_LOG_WARN, "provider.stream_stop",
                    "contained C++ exception");
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
            bool bridge_threw = false;
            try {
                bridge_result = product().bridge_stop(bridge_id);
            } catch (...) {
                bridge_threw = true;
                bridge_result = DTV_L3_FAILED;
            }
            {
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
            if (bridge_threw) {
                log(DTV_L3_LOG_WARN, "provider.bridge_stop",
                    "contained C++ exception");
            }
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

    dtv_l3_result stop_result = DTV_L3_FAILED;
    bool stop_threw = false;
    try {
        stop_result = product().stream_stop(generation);
    } catch (...) {
        stop_threw = true;
        stop_result = DTV_L3_FAILED;
    }
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
    if (stop_threw) {
        log(DTV_L3_LOG_WARN, "provider.stream_stop",
            "contained C++ exception");
    }

    if (need_bridge_stop) {
        bool bridge_threw = false;
        try {
            bridge_result = product().bridge_stop(bridge_id);
        } catch (...) {
            bridge_threw = true;
            bridge_result = DTV_L3_FAILED;
        }
        {
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
        }
        if (bridge_threw) {
            log(DTV_L3_LOG_WARN, "provider.bridge_stop",
                "contained C++ exception");
        }
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
    dtv_l3_result stop_result = DTV_L3_FAILED;
    bool bridge_threw = false;
    try {
        stop_result = product().bridge_stop(bridge_id);
    } catch (...) {
        bridge_threw = true;
        stop_result = DTV_L3_FAILED;
    }
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
    if (bridge_threw) {
        log(DTV_L3_LOG_WARN, "provider.bridge_stop",
            "contained C++ exception");
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
    try {
        sink->owner->on_ts_deliver(sink->generation, data, size);
    } catch (...) {
        sink->owner->log(DTV_L3_LOG_WARN, "ts.deliver",
                         "contained C++ exception");
    }
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
    maybe_throw_test_exception(kTestInjectTsDeliver);
    stream.staging.insert(stream.staging.end(), data, data + size);
    bool queued = false;
    while (stream.staging.size() >= 188) {
        if (stream.queue.size() >= kQueueCapacityPackets) {
            ++stream.dropped_overflow_packets;
        } else {
            stream.queue.emplace_back(stream.staging.begin(),
                                      stream.staging.begin() + 188);
            queued = true;
        }
        stream.staging.erase(stream.staging.begin(),
                             stream.staging.begin() + 188);
    }
    if (queued) {
        state_condition_.notify_all();
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
    dtv_l3_result stop_result = DTV_L3_FAILED;
    bool stop_threw = false;
    try {
        stop_result = product().stream_stop(generation);
    } catch (...) {
        stop_threw = true;
        stop_result = DTV_L3_FAILED;
    }
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
    if (stop_threw) {
        log(DTV_L3_LOG_WARN, "provider.stream_stop",
            "contained C++ exception");
    }

    if (need_bridge_stop) {
        CallbackFrame frame;
        dtv_l3_result bridge_result = DTV_L3_FAILED;
        bool bridge_threw = false;
        try {
            bridge_result = product().bridge_stop(bridge_id);
        } catch (...) {
            bridge_threw = true;
            bridge_result = DTV_L3_FAILED;
        }
        {
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
        if (bridge_threw) {
            log(DTV_L3_LOG_WARN, "provider.bridge_stop",
                "contained C++ exception");
        }
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
    dtv_l3_result result = DTV_L3_FAILED;
    bool cancel_threw = false;
    try {
        maybe_throw_test_exception(kTestInjectCancel);
        result = provider_.cancel(operation->binding.get(), operation->id);
    } catch (...) {
        cancel_threw = true;
        result = DTV_L3_FAILED;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        operation->cancel_in_progress = false;
        if (cancel_threw) {
            operation->cancel_sent = false;
        } else {
            operation->cancel_sent =
                !(before_submit && result == DTV_L3_INVALID);
        }
        if (before_submit && result == DTV_L3_INVALID && operation->submitted) {
            queue_cancel_locked(operation);
        }
        state_condition_.notify_all();
    }
    if (cancel_threw) {
        log(DTV_L3_LOG_WARN, "provider.cancel",
            "contained C++ exception");
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
        request_claim_cancel_locked();
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
    std::size_t *data_size,
    dtv_l3_cancel_cause *cancel_cause,
    std::uint32_t *provider_abort_resubmits) {
    if (!operation || state == nullptr || result == nullptr || data == nullptr ||
        data_size == nullptr || cancel_cause == nullptr ||
        provider_abort_resubmits == nullptr) {
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
    *cancel_cause = operation->cancel_cause;
    *provider_abort_resubmits = operation->provider_abort_resubmits;
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
        request_claim_cancel_locked();
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
    if (timeout_ms != 0 &&
        !(physical_quiesced_ && physical_settled_locked())) {
        state_condition_.wait_for(
            lock, wait_duration(timeout_ms), [this] {
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
    snapshot->disconnected = disconnected_;
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
            provider_calls_in_flight_ != 0 || !work_queue_.empty() ||
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
        !leases_.empty() || provider_calls_in_flight_ != 0 ||
        !work_queue_.empty() || !lanes_exit_committed_locked()) {
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
    try {
        operation->owner->on_completion(binding, id, *completion, operation);
    } catch (...) {
        operation->owner->log(DTV_L3_LOG_ERROR, "completion_thunk",
                              "contained C++ exception");
    }
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
    bool matched = false;
    bool do_resubmit = false;
    bool copy_threw = false;
    std::shared_ptr<Operation> op_shared;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto in_flight = in_flight_.find(id);
        if (in_flight != in_flight_.end()) {
            op_shared = in_flight->second;
            in_flight_.erase(in_flight);
        }
        const auto found = operations_.find(id);
        if (binding == binding_.get() && found != operations_.end() &&
            found->second.get() == operation) {
            matched = true;
            op_shared = found->second;
            Operation &op = *op_shared;
            op.completion_received = true;
            if (op.state == DTV_L3_OP_DONE || op.state == DTV_L3_OP_FAILED) {
                /* A late completion after an owner deadline or disconnect. */
            } else if (completion.result == DTV_L3_CANCELLED) {
                op.cancel_cause = op.cancel_requested
                                      ? DTV_L3_CANCEL_CAUSE_REQUESTED
                                      : DTV_L3_CANCEL_CAUSE_PROVIDER_ABORT;
                const bool outbound = op.request_template.out_size != 0;
                const bool proven_unsent =
                    completion.actual_length_known != 0 &&
                    completion.actual_length == 0;
                const bool can_resubmit =
                    op.cancel_cause == DTV_L3_CANCEL_CAUSE_PROVIDER_ABORT &&
                    op.request_template.resubmit_on_provider_abort != 0 &&
                    !op.cancel_requested && !close_requested_ &&
                    !disconnected_;
                if (outbound && !proven_unsent) {
                    op.result = DTV_L3_OUTCOME_UNKNOWN;
                    op.state = DTV_L3_OP_FAILED;
                } else if (outbound) {
                    op.result = DTV_L3_CANCELLED;
                    op.state = DTV_L3_OP_FAILED;
                } else if (can_resubmit && !op.submitted) {
                    if (op.provider_abort_resubmits == 0) {
                        /*
                         * The completion ran inside submit. Keep the logical
                         * operation open and resubmit after the submit stack
                         * returns; a synchronous abort of the retry is
                         * terminal instead of recursing.
                         */
                        op.resubmit_pending = true;
                    } else {
                        op.result = DTV_L3_CANCELLED;
                        op.state = DTV_L3_OP_FAILED;
                    }
                } else if (can_resubmit) {
                    do_resubmit = true;
                } else {
                    op.result = DTV_L3_CANCELLED;
                    op.state = DTV_L3_OP_FAILED;
                }
            } else if (completion.result == DTV_L3_DISCONNECTED) {
                op.cancel_cause = DTV_L3_CANCEL_CAUSE_NONE;
                op.result = DTV_L3_DISCONNECTED;
                op.state = DTV_L3_OP_FAILED;
                disconnected_ = true;
                for (auto &entry : operations_) {
                    auto &other = *entry.second;
                    if (&other == &op) {
                        continue;
                    }
                    if (other.state == DTV_L3_OP_OPENING ||
                        other.state == DTV_L3_OP_RUNNING) {
                        other.cancel_cause = DTV_L3_CANCEL_CAUSE_NONE;
                        other.result = DTV_L3_DISCONNECTED;
                        other.state = DTV_L3_OP_FAILED;
                    }
                }
            } else {
                op.cancel_cause = DTV_L3_CANCEL_CAUSE_NONE;
                bool copy_failed = false;
                if (completion.size != 0 && completion.data != nullptr) {
                    try {
                        maybe_throw_test_exception(kTestInjectCompletionCopy);
                        op.response.assign(completion.data,
                                           completion.data + completion.size);
                    } catch (...) {
                        copy_failed = true;
                        op.response.clear();
                    }
                }
                if (copy_failed) {
                    copy_threw = true;
                    op.result = DTV_L3_FAILED;
                    op.state = DTV_L3_OP_FAILED;
                } else {
                    op.result = completion.result;
                    op.state = completion.result == DTV_L3_OK
                                   ? DTV_L3_OP_DONE
                                   : DTV_L3_OP_FAILED;
                }
            }
        }
        should_finish_close = close_requested_;
        state_condition_.notify_all();
    }
    if (copy_threw) {
        log(DTV_L3_LOG_WARN, "completion.copy",
            "contained C++ exception");
    }
    if (!matched) {
        if (should_finish_close) {
            finish_close();
        }
        return;
    }
    if (do_resubmit) {
        resubmit_operation(op_shared);
    }
    if (should_finish_close) {
        finish_close();
    }
}

void OwnerEngine::resubmit_operation(
    const std::shared_ptr<Operation> &operation) {
    if (!operation) {
        return;
    }
    bool do_submit = false;
    bool need_finish_close = false;
    dtv_l3_transfer_request request{};
    std::uint64_t new_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        operation->resubmit_pending = false;
        if (operation->state == DTV_L3_OP_DONE ||
            operation->state == DTV_L3_OP_FAILED) {
            need_finish_close = close_requested_;
        } else if (operation->cancel_requested || close_requested_ ||
                   disconnected_) {
            operation->completion_received = true;
            operation->submitted = true;
            operation->cancel_cause =
                disconnected_ ? DTV_L3_CANCEL_CAUSE_NONE
                              : DTV_L3_CANCEL_CAUSE_REQUESTED;
            operation->result = disconnected_ ? DTV_L3_DISCONNECTED
                                              : DTV_L3_CANCELLED;
            operation->state = DTV_L3_OP_FAILED;
            state_condition_.notify_all();
            work_condition_.notify_all();
            need_finish_close = close_requested_;
        } else {
            operation->provider_abort_resubmits++;
            operation->completion_received = false;
            operation->submitted = false;
            operation->cancel_sent = false;
            operation->cancel_in_progress = false;
            const auto old_id = operation->id;
            new_id = next_transfer_id_++;
            operations_.erase(old_id);
            operation->id = new_id;
            operations_.emplace(new_id, operation);
            request = operation->request_template;
            if (!operation->retained_payload.empty()) {
                request.out_data = operation->retained_payload.data();
            }
            ++provider_calls_in_flight_;
            do_submit = true;
        }
    }
    if (!do_submit) {
        if (need_finish_close) {
            finish_close();
        }
        return;
    }

    CallbackFrame frame;
    dtv_l3_result result = DTV_L3_FAILED;
    bool submit_threw = false;
    try {
        result = provider_.submit(
            operation->binding.get(), new_id, request,
            &OwnerEngine::completion_thunk, operation.get());
    } catch (...) {
        submit_threw = true;
        result = DTV_L3_FAILED;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        if (result == DTV_L3_ACCEPTED) {
            operation->submitted = true;
            if (!operation->completion_received) {
                in_flight_.emplace(new_id, operation);
            }
            if (!operation->completion_received &&
                (operation->cancel_requested || close_requested_ ||
                 disconnected_)) {
                queue_cancel_locked(operation, true);
            }
        } else if (operation->state == DTV_L3_OP_OPENING ||
                   operation->state == DTV_L3_OP_RUNNING) {
            operation->state = DTV_L3_OP_FAILED;
            if (operation->cancel_requested || close_requested_) {
                operation->cancel_cause = DTV_L3_CANCEL_CAUSE_REQUESTED;
                operation->result = DTV_L3_CANCELLED;
            } else {
                operation->result = result;
            }
        }
        need_finish_close = close_requested_;
        state_condition_.notify_all();
        work_condition_.notify_all();
    }
    if (submit_threw) {
        log(DTV_L3_LOG_WARN, "provider.submit",
            "contained C++ exception");
    }
    if (need_finish_close) {
        finish_close();
    }
}

void OwnerEngine::finish_close() {
    bool enqueue_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!close_requested_ || cleanup_started_ || cleanup_attempted_ ||
            cleanup_complete_ ||
            (initialization_phase_ !=
                 InitializationPhase::ResponsibilitiesRegistered &&
             initialization_phase_ != InitializationPhase::Returned) ||
            open_state_ == OpenState::Opening ||
            provider_calls_in_flight_ != 0) {
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
        try {
            maybe_throw_test_exception(kTestInjectCleanupEnqueue);
            enqueue_locked([this] { run_cleanup(); });
        } catch (...) {
            /*
             * No cleanup work ran. Keep cleanup_attempted_ set so the host can
             * re-enter through cleanup_retry; do not report quiescence.
             */
            cleanup_started_ = false;
            enqueue_failed = true;
            state_condition_.notify_all();
            work_condition_.notify_all();
        }
    }
    if (enqueue_failed) {
        log(DTV_L3_LOG_ERROR, "cleanup.enqueue",
            "contained C++ exception");
    }
}

void OwnerEngine::queue_cancel_locked(
    const std::shared_ptr<Operation> &operation,
    bool allow_terminal) {
    if (operation->cancel_in_progress || operation->cancel_sent) {
        return;
    }
    if (operation->state == DTV_L3_OP_DONE ||
        operation->state == DTV_L3_OP_FAILED) {
        const bool outstanding =
            in_flight_.find(operation->id) != in_flight_.end();
        if (!allow_terminal || !outstanding) {
            return;
        }
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
    dtv_l3_result result = DTV_L3_FAILED;
    bool cancel_threw = false;
    try {
        result = provider_.cancel(operation->binding.get(), operation->id);
    } catch (...) {
        cancel_threw = true;
        result = DTV_L3_FAILED;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --provider_calls_in_flight_;
        operation->cancel_in_progress = false;
        if (cancel_threw) {
            operation->cancel_sent = false;
        } else {
            operation->cancel_sent =
                !(before_submit && result == DTV_L3_INVALID);
        }
        if (before_submit && result == DTV_L3_INVALID && operation->submitted) {
            queue_cancel_locked(operation);
        }
        state_condition_.notify_all();
    }
    if (cancel_threw) {
        log(DTV_L3_LOG_WARN, "provider.cancel",
            "contained C++ exception");
    }
    finish_close();
}

void OwnerEngine::run_cleanup() {
    std::shared_ptr<Binding> binding;
    std::vector<std::uint32_t> release_indices;
    bool registry_failed = false;
    try {
        maybe_throw_test_exception(kTestInjectCleanupRegistry);
        std::lock_guard<std::mutex> lock(mutex_);
        binding = binding_;
        for (std::uint32_t index = 0; index < claims_.size(); ++index) {
            if (claims_[index] == ClaimState::Ready ||
                claims_[index] == ClaimState::Retained) {
                release_indices.push_back(index);
            }
        }
    } catch (...) {
        registry_failed = true;
    }
    if (registry_failed) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            /* No claim was released; leave cleanup_attempted_ for a retry. */
            cleanup_started_ = false;
            state_condition_.notify_all();
            work_condition_.notify_all();
        }
        log(DTV_L3_LOG_ERROR, "cleanup.registry",
            "contained C++ exception");
        return;
    }

    for (auto index = release_indices.rbegin(); index != release_indices.rend();
         ++index) {
        CallbackFrame frame;
        dtv_l3_release_result result{
            DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
            DTV_L3_RESTORE_UNKNOWN, DTV_L3_FAILED};
        bool release_threw = false;
        try {
            maybe_throw_test_exception(kTestInjectReleaseClaim);
            result = provider_.release_claim(binding.get(), *index);
        } catch (...) {
            release_threw = true;
            result = {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN,
                      DTV_L3_CLAIM_RETAINED, DTV_L3_RESTORE_UNKNOWN,
                      DTV_L3_FAILED};
        }
        {
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
        if (release_threw) {
            log(DTV_L3_LOG_WARN, "provider.release_claim",
                "contained C++ exception");
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

void OwnerEngine::request_claim_cancel_locked() {
    if (!claim_in_flight_ || !in_flight_claim_valid_ ||
        in_flight_claim_index_ >= claim_cancel_requested_.size() ||
        claim_cancel_requested_[in_flight_claim_index_]) {
        return;
    }
    const auto index = in_flight_claim_index_;
    claim_cancel_requested_[index] = true;
    ++provider_calls_in_flight_;
    enqueue_locked([this, index] { run_claim_cancel(index); });
}

void OwnerEngine::run_claim_cancel(std::uint32_t index) {
    std::shared_ptr<Binding> binding;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!claim_in_flight_ || !in_flight_claim_valid_ ||
            in_flight_claim_index_ != index) {
            --provider_calls_in_flight_;
            state_condition_.notify_all();
        } else {
            binding = binding_;
        }
    }
    if (binding) {
        CallbackFrame frame;
        dtv_l3_result result = DTV_L3_FAILED;
        bool claim_cancel_threw = false;
        try {
            result = provider_.claim_cancel(binding.get(), index);
        } catch (...) {
            claim_cancel_threw = true;
            result = DTV_L3_FAILED;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --provider_calls_in_flight_;
            claim_cancel_result_ = result;
            state_condition_.notify_all();
        }
        if (claim_cancel_threw) {
            log(DTV_L3_LOG_WARN, "provider.claim_cancel",
                "contained C++ exception");
        }
    }
    finish_close();
}

bool OwnerEngine::expire_operations_locked() {
    if (close_requested_ || disconnected_) {
        return false;
    }
    const auto now = monotonic_now_ns();
    bool expired = false;
    for (auto &entry : operations_) {
        auto &op = *entry.second;
        if (!op.submitted ||
            (op.state != DTV_L3_OP_OPENING &&
             op.state != DTV_L3_OP_RUNNING) ||
            op.request_template.deadline_ns == DTV_L3_DEADLINE_NONE ||
            op.cancel_requested || now < op.request_template.deadline_ns) {
            continue;
        }
        /*
         * A response deadline is an operation result. It does not cancel the
         * shared IN pipe, so a late response can still be discarded and TS can
         * continue. The accepted completion remains outstanding until it
         * returns.
         */
        op.cancel_cause = DTV_L3_CANCEL_CAUSE_NONE;
        op.result = DTV_L3_TIMEOUT;
        op.state = DTV_L3_OP_FAILED;
        expired = true;
    }
    if (expired) {
        state_condition_.notify_all();
    }
    return expired;
}

std::optional<std::chrono::steady_clock::time_point>
OwnerEngine::next_deadline_locked() const {
    if (close_requested_ || disconnected_) {
        return std::nullopt;
    }
    std::optional<std::uint64_t> next;
    for (const auto &entry : operations_) {
        const auto &op = *entry.second;
        if (!op.submitted ||
            (op.state != DTV_L3_OP_OPENING &&
             op.state != DTV_L3_OP_RUNNING) ||
            op.request_template.deadline_ns == DTV_L3_DEADLINE_NONE ||
            op.cancel_requested) {
            continue;
        }
        if (!next || op.request_template.deadline_ns < *next) {
            next = op.request_template.deadline_ns;
        }
    }
    if (!next) {
        return std::nullopt;
    }
    return deadline_time_point(*next);
}

void OwnerEngine::worker_loop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [this] {
                return worker_stopping_ || !work_queue_.empty() ||
                       control_lane_may_exit_locked();
            };
            while (!ready()) {
                const auto next = next_deadline_locked();
                if (!next) {
                    work_condition_.wait(lock);
                    continue;
                }
                if (work_condition_.wait_until(lock, *next) ==
                    std::cv_status::timeout) {
                    /* The control lane owns the deadline timer; no host loop. */
                    (void)expire_operations_locked();
                }
            }
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
        try {
            work();
        } catch (...) {
            log(DTV_L3_LOG_ERROR, "control_lane",
                "contained C++ exception");
        }
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
    return operations_.empty() && in_flight_.empty() &&
           physical_settled_locked();
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

void set_test_exception_mask(unsigned int mask) {
    test_exception_mask.store(mask, std::memory_order_release);
}

void maybe_throw_test_exception(unsigned int bit) {
    throw_if_test_bit(bit);
}

void maybe_throw_test_entry_exception() {
    throw_if_test_bit(kTestInjectEntry);
}

}  // namespace l3_owner_prototype
