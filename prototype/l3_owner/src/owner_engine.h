/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DTV_L3_OWNER_ENGINE_H
#define DTV_L3_OWNER_ENGINE_H

#include <dtv/l3_owner.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct dtv_l3_provider_binding {
    explicit dtv_l3_provider_binding(std::uint64_t value) : value(value) {}
    std::uint64_t value;
};

namespace l3_owner_prototype {

using Binding = ::dtv_l3_provider_binding;

struct TransferKey {
    Binding *binding;
    std::uint64_t id;
    bool operator<(const TransferKey &other) const {
        return std::less<Binding *>{}(binding, other.binding) ||
               (binding == other.binding && id < other.id);
    }
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual dtv_l3_claim_result claim(Binding *binding, std::uint32_t index) = 0;
    virtual dtv_l3_release_result release_claim(
        Binding *binding, std::uint32_t index) = 0;
    virtual dtv_l3_result submit(
        Binding *binding,
        std::uint64_t id,
        const dtv_l3_transfer_request &request,
        dtv_l3_transfer_complete_fn complete,
        void *callback_context) = 0;
    virtual dtv_l3_result cancel(Binding *binding, std::uint64_t id) = 0;
    virtual dtv_l3_result scope_query(std::uint32_t receiver_id,
                                      dtv_l3_scope_info *scope) {
        (void)receiver_id;
        (void)scope;
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result bridge_start(std::uint32_t bridge_id,
                                       std::uint32_t receiver_id) {
        (void)bridge_id;
        (void)receiver_id;
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result bridge_stop(std::uint32_t bridge_id) {
        (void)bridge_id;
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result stream_attach(std::uint64_t stream_id,
                                        dtv_l3_ts_deliver_fn deliver,
                                        void *callback_context) {
        (void)stream_id;
        (void)deliver;
        (void)callback_context;
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result stream_stop(std::uint64_t stream_id) {
        (void)stream_id;
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result tune(std::uint32_t receiver_id,
                               std::uint64_t requested_hz,
                               std::uint64_t *programmed_hz) {
        (void)receiver_id;
        (void)requested_hz;
        (void)programmed_hz;
        return DTV_L3_UNSUPPORTED;
    }
};

class OwnerEngine {
public:
    enum class OpenState { Opening, Ready, Failed, Closing, Closed };

    struct Snapshot {
        OpenState open_state;
        bool physical_quiesced;
        bool all_ops_recovered;
        bool cleanup_complete;
        std::size_t active_borrows;
        std::size_t claims_attempted;
        std::size_t transfer_ready_claims;
        std::size_t cleanup_claims_remaining;
        std::uint64_t cleanup_epoch;
        dtv_l3_result open_result;
        dtv_l3_result cleanup_result;
        dtv_l3_result last_cleanup_error;
        bool restoration_failed;
    };

    enum class ClaimState { NotAttempted, InFlight, Ready, Retained, Unknown, Released };

    struct Operation {
        OwnerEngine *owner = nullptr;
        std::shared_ptr<Binding> binding;
        std::uint64_t id = 0;
        dtv_l3_op_state state = DTV_L3_OP_OPENING;
        dtv_l3_result result = DTV_L3_ACCEPTED;
        std::vector<std::uint8_t> retained_payload;
        std::vector<std::uint8_t> response;
        bool released = false;
        bool submitted = false;
        bool cancel_requested = false;
        bool cancel_in_progress = false;
        bool cancel_sent = false;
    };

    explicit OwnerEngine(Provider &provider, std::uint64_t binding_value);
    ~OwnerEngine();

    OwnerEngine(const OwnerEngine &) = delete;
    OwnerEngine &operator=(const OwnerEngine &) = delete;

    void initialize_claims(std::uint32_t count);
    dtv_l3_result retry_cleanup();
    dtv_l3_result start(const dtv_l3_transfer_request &request,
                       std::shared_ptr<Operation> *operation);
    dtv_l3_result acquire_lease(std::uint32_t receiver_id,
                                std::uint32_t *lease_id);
    dtv_l3_result tune_lease(std::uint32_t lease_id,
                             std::uint64_t requested_hz,
                             std::uint64_t *programmed_hz,
                             std::uint64_t *tune_revision);
    dtv_l3_result start_lease(std::uint32_t lease_id);
    dtv_l3_result attach_lease(std::uint32_t lease_id,
                               std::uint64_t *stream_generation);
    dtv_l3_result lease_status(std::uint32_t lease_id,
                               dtv_l3_lease_status *status);
    dtv_l3_result release_lease(std::uint32_t lease_id);
    dtv_l3_result stop_stream(std::uint64_t generation,
                              std::uint32_t timeout_ms);
    dtv_l3_result read_stream(
        std::uint64_t generation,
        std::uint8_t *buffer,
        std::size_t capacity_bytes,
        std::size_t *bytes_read,
        dtv_l3_read_state *state,
        const std::function<void()> &after_borrow = {});
    dtv_l3_result stream_status(std::uint64_t generation,
                                dtv_l3_stream_status *status);
    dtv_l3_result release_stream(std::uint64_t generation,
                                 dtv_l3_stream_release_mode mode);
    dtv_l3_result destroy_stream(std::uint64_t generation);
    dtv_l3_result cancel(const std::shared_ptr<Operation> &operation);
    dtv_l3_result cancel_open_if_opening();
    dtv_l3_result poll(const std::shared_ptr<Operation> &operation,
                       dtv_l3_op_state *state);
    dtv_l3_result poll_result(const std::shared_ptr<Operation> &operation,
                              dtv_l3_op_state *state,
                              dtv_l3_result *result,
                              const std::uint8_t **data,
                              std::size_t *data_size);
    dtv_l3_result release_operation(const std::shared_ptr<Operation> &operation);
    dtv_l3_result close(std::uint32_t timeout_ms);
    dtv_l3_result close_status(Snapshot *snapshot);
    dtv_l3_result read_ts(const std::function<void()> &after_borrow = {});
    dtv_l3_result release(const std::function<void()> &after_retire_gate = {});

    std::shared_ptr<Binding> binding() const;
    std::size_t cleanup_count() const;
    std::size_t release_count() const;
    std::size_t live_operation_count() const;
    std::size_t live_lease_count() const;
    std::size_t live_stream_count() const;
    std::weak_ptr<void> stream_lifetime() const;

private:
    enum class InitializationPhase {
        NotStarted,
        InProgress,
        ResponsibilitiesRegistered,
        Returned
    };

    struct StreamSink;

    struct BridgeRecord {
        std::size_t users = 0;
        bool running = false;
        bool start_pending = false;
        bool stop_residual = false;
        std::uint64_t start_count = 0;
        std::uint64_t stop_count = 0;
    };

    struct LeaseRecord {
        std::uint32_t receiver_id = 0;
        std::uint32_t bridge_id = 0;
        std::uint32_t compatibility = DTV_L3_LEASE_SHAREABLE;
        dtv_l3_lease_state state = DTV_L3_LEASE_FREE;
        std::uint64_t tune_revision = 0;
        std::uint64_t requested_hz = 0;
        std::uint64_t programmed_hz = 0;
        std::uint64_t active_stream = 0;
        bool stop_residual = false;
    };

    struct StreamRecord {
        std::uint64_t generation = 0;
        std::uint32_t lease_id = 0;
        std::uint32_t receiver_id = 0;
        std::uint32_t bridge_id = 0;
        std::uint64_t tune_revision = 0;
        dtv_l3_stream_phase phase = DTV_L3_STREAM_ACTIVE;
        std::deque<std::vector<std::uint8_t>> queue;
        std::vector<std::uint8_t> staging;
        std::size_t read_borrows = 0;
        std::uint64_t dropped_overflow_packets = 0;
        std::uint64_t discarded_unread_packets = 0;
        bool eof_pending = false;
        bool eof_observed = false;
        bool physical_stopped = false;
        bool retire_requested = false;
        dtv_l3_stream_release_mode retire_mode =
            DTV_L3_STREAM_RELEASE_DRAINED;
        bool stopping = false;
        bool close_retire = false;
        std::shared_ptr<StreamSink> sink;
    };

    struct StreamSink {
        OwnerEngine *owner = nullptr;
        std::uint64_t generation = 0;
        Binding *binding = nullptr;
        static void deliver(void *callback_context,
                            const std::uint8_t *data,
                            std::size_t size);
    };

    class Borrow {
    public:
        explicit Borrow(OwnerEngine &owner);
        ~Borrow();
        explicit operator bool() const { return admitted_; }

    private:
        OwnerEngine &owner_;
        bool admitted_;
    };

    static void completion_thunk(Binding *binding,
                                 std::uint64_t id,
                                 const dtv_l3_transfer_completion *completion,
                                 void *callback_context);
    void on_completion(Binding *binding,
                       std::uint64_t id,
                       const dtv_l3_transfer_completion &completion,
                       Operation *operation);
    bool active_streams_locked() const;
    void finish_close();
    void on_ts_deliver(std::uint64_t generation,
                       const std::uint8_t *data,
                       std::size_t size);
    void finalize_retire_locked(std::uint64_t generation);
    void retire_stream_for_close(std::uint64_t generation);
    void queue_cancel_locked(const std::shared_ptr<Operation> &operation);
    void run_cancel(const std::shared_ptr<Operation> &operation,
                    bool before_submit);
    void run_cleanup();
    bool cleanup_claims_locked() const;
    void worker_loop();
    void enqueue_locked(std::function<void()> work);
    bool all_ops_recovered_locked() const;

    Provider &provider_;
    mutable std::mutex mutex_;
    bool retired_gate_ = false;
    bool released_ = false;
    std::size_t active_borrows_ = 0;
    OpenState open_state_ = OpenState::Opening;
    InitializationPhase initialization_phase_ = InitializationPhase::NotStarted;
    bool close_requested_ = false;
    bool physical_quiesced_ = false;
    bool cleanup_complete_ = false;
    bool cleanup_started_ = false;
    bool cleanup_attempted_ = false;
    bool worker_stopping_ = false;
    std::size_t provider_calls_in_flight_ = 0;
    std::size_t cleanup_count_ = 0;
    std::size_t release_count_ = 0;
    std::uint64_t cleanup_epoch_ = 1;
    std::uint32_t claimed_count_ = 0;
    std::vector<ClaimState> claims_;
    std::vector<bool> claim_quiesced_;
    std::vector<dtv_l3_result> claim_cleanup_errors_;
    bool claim_in_flight_ = false;
    dtv_l3_result last_cleanup_error_ = DTV_L3_OK;
    dtv_l3_result open_result_ = DTV_L3_ACCEPTED;
    bool restoration_failed_ = false;
    std::uint64_t next_transfer_id_ = 1;
    std::uint32_t next_lease_id_ = 1;
    std::uint64_t next_stream_generation_ = 1;
    std::shared_ptr<Binding> binding_;
    std::shared_ptr<void> stream_ = std::make_shared<int>(1);
    std::map<std::uint64_t, std::shared_ptr<Operation>> operations_;
    std::map<std::uint32_t, LeaseRecord> leases_;
    std::map<std::uint32_t, BridgeRecord> bridges_;
    std::map<std::uint64_t, std::shared_ptr<StreamRecord>> streams_;
    std::deque<std::function<void()>> work_queue_;
    std::condition_variable work_condition_;
    std::condition_variable state_condition_;
    std::thread worker_;
};

class TestBarrier {
public:
    void arrive_and_wait();
    void wait_until_arrived();
    void release();

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool arrived_ = false;
    bool released_ = false;
};

}  // namespace l3_owner_prototype

#endif
