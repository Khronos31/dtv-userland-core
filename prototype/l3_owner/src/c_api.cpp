/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

#include "owner_engine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace {

class CProvider final : public l3_owner_prototype::Provider {
public:
    explicit CProvider(const dtv_l3_provider &provider) : provider_(provider) {}

    dtv_l3_claim_result claim(l3_owner_prototype::Binding *binding,
                              std::uint32_t index) override {
        return provider_.claim(provider_.provider_context, binding, index);
    }

    dtv_l3_release_result release_claim(
        l3_owner_prototype::Binding *binding,
        std::uint32_t index) override {
        return provider_.release_claim(provider_.provider_context, binding,
                                       index);
    }

    dtv_l3_result submit(l3_owner_prototype::Binding *binding,
                         std::uint64_t id,
                         const dtv_l3_transfer_request &request,
                         dtv_l3_transfer_complete_fn complete,
                         void *callback_context) override {
        return provider_.submit(provider_.provider_context, binding, id,
                                &request, complete, callback_context);
    }

    dtv_l3_result cancel(l3_owner_prototype::Binding *binding,
                         std::uint64_t id) override {
        return provider_.cancel(provider_.provider_context, binding, id);
    }

private:
    dtv_l3_provider provider_;
};

}  // namespace

struct dtv_l3_open_op_state {
    std::mutex mutex;
    bool terminal = false;
    bool cancel_requested = false;
    dtv_l3_op_state state = DTV_L3_OP_RUNNING;
    dtv_l3_result result = DTV_L3_ACCEPTED;
    std::uint64_t cleanup_epoch = 0;
};

struct dtv_l3_context {
    explicit dtv_l3_context(const dtv_l3_provider &provider)
        : adapter(provider), engine(std::make_unique<l3_owner_prototype::OwnerEngine>(
                                         adapter, 1)) {}
    ~dtv_l3_context() {
        if (open_thread.joinable()) {
            open_thread.join();
        }
    }

    CProvider adapter;
    std::unique_ptr<l3_owner_prototype::OwnerEngine> engine;
    std::thread open_thread;
    dtv_l3_open_op_state open_op_state;
    std::atomic<bool> open_thread_done{false};
    std::atomic<std::size_t> outstanding_ops{0};
};

struct dtv_l3_op {
    dtv_l3_context *owner = nullptr;
    dtv_l3_op_kind kind = DTV_L3_OP_KIND_OPEN;
    std::shared_ptr<l3_owner_prototype::OwnerEngine::Operation> operation;
};

extern "C" {

static dtv_l3_result poll_open_op(dtv_l3_context *owner,
                                  dtv_l3_op_result *result) {
    l3_owner_prototype::OwnerEngine::Snapshot snapshot{};
    const auto call_result = owner->engine->close_status(&snapshot);
    if (call_result != DTV_L3_OK) {
        return call_result;
    }
    std::lock_guard<std::mutex> lock(owner->open_op_state.mutex);
    if (!owner->open_op_state.terminal &&
        (snapshot.open_state ==
             l3_owner_prototype::OwnerEngine::OpenState::Ready ||
         snapshot.open_state ==
             l3_owner_prototype::OwnerEngine::OpenState::Failed ||
         snapshot.open_result == DTV_L3_OK)) {
        owner->open_op_state.terminal = true;
        owner->open_op_state.state =
            snapshot.open_result == DTV_L3_OK ? DTV_L3_OP_DONE
                                               : DTV_L3_OP_FAILED;
        owner->open_op_state.result = snapshot.open_result;
        owner->open_op_state.cleanup_epoch = snapshot.cleanup_epoch;
    } else if (!owner->open_op_state.terminal &&
               snapshot.open_state ==
                   l3_owner_prototype::OwnerEngine::OpenState::Closed) {
        owner->open_op_state.terminal = true;
        owner->open_op_state.state = DTV_L3_OP_FAILED;
        owner->open_op_state.result = owner->open_op_state.cancel_requested
                                          ? DTV_L3_CANCELLED
                                          : DTV_L3_FAILED;
        owner->open_op_state.cleanup_epoch = snapshot.cleanup_epoch;
    }
    result->kind = DTV_L3_OP_KIND_OPEN;
    result->state = owner->open_op_state.terminal
                        ? owner->open_op_state.state
                        : DTV_L3_OP_RUNNING;
    result->result = owner->open_op_state.terminal
                         ? owner->open_op_state.result
                         : DTV_L3_ACCEPTED;
    result->cleanup_epoch = owner->open_op_state.terminal
                                ? owner->open_op_state.cleanup_epoch
                                : snapshot.cleanup_epoch;
    result->data = nullptr;
    result->data_size = 0;
    return DTV_L3_OK;
}

dtv_l3_open_result dtv_l3_open(const dtv_l3_provider *provider,
                               uint32_t claim_count) {
    if (provider == nullptr || provider->claim == nullptr ||
        provider->release_claim == nullptr || provider->submit == nullptr ||
        provider->cancel == nullptr) {
        return {DTV_L3_INVALID, nullptr, nullptr};
    }
    auto *owner = new (std::nothrow) dtv_l3_context(*provider);
    if (owner == nullptr) {
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    auto *op = new (std::nothrow) dtv_l3_op;
    if (op == nullptr) {
        delete owner;
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    op->owner = owner;
    op->kind = DTV_L3_OP_KIND_OPEN;
    owner->outstanding_ops.store(1);
    try {
        owner->open_thread = std::thread([owner, claim_count] {
            owner->engine->initialize_claims(claim_count);
            owner->open_thread_done.store(true, std::memory_order_release);
        });
    } catch (...) {
        delete op;
        delete owner;
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    return {DTV_L3_ACCEPTED, owner, op};
}

dtv_l3_result dtv_l3_open_status_get(dtv_l3_context *owner,
                                    dtv_l3_open_status *status) {
    if (owner == nullptr || status == nullptr) {
        return DTV_L3_INVALID;
    }
    l3_owner_prototype::OwnerEngine::Snapshot snapshot{};
    const auto result = owner->engine->close_status(&snapshot);
    if (result != DTV_L3_OK) {
        return result;
    }
    switch (snapshot.open_state) {
    case l3_owner_prototype::OwnerEngine::OpenState::Opening:
        status->state = DTV_L3_OPEN_OPENING;
        break;
    case l3_owner_prototype::OwnerEngine::OpenState::Ready:
        status->state = DTV_L3_OPEN_READY;
        break;
    case l3_owner_prototype::OwnerEngine::OpenState::Failed:
        status->state = DTV_L3_OPEN_FAILED;
        break;
    case l3_owner_prototype::OwnerEngine::OpenState::Closing:
        status->state = DTV_L3_OPEN_CLOSING;
        break;
    case l3_owner_prototype::OwnerEngine::OpenState::Closed:
        status->state = DTV_L3_OPEN_CLOSED;
        break;
    }
    status->claims_acquired = static_cast<uint32_t>(snapshot.transfer_ready_claims);
    status->result = snapshot.open_result;
    status->claims_attempted = static_cast<uint32_t>(snapshot.claims_attempted);
    status->cleanup_claims_remaining =
        static_cast<uint32_t>(snapshot.cleanup_claims_remaining);
    status->last_cleanup_error = snapshot.last_cleanup_error;
    status->restoration_failed = snapshot.restoration_failed ? 1 : 0;
    status->reserved[0] = status->reserved[1] = status->reserved[2] = 0;
    return DTV_L3_OK;
}

dtv_l3_result dtv_l3_close(dtv_l3_context *owner, uint32_t timeout_ms) {
    return owner == nullptr ? DTV_L3_INVALID : owner->engine->close(timeout_ms);
}

dtv_l3_result dtv_l3_close_status_get(dtv_l3_context *owner,
                                     dtv_l3_close_status *status) {
    if (owner == nullptr || status == nullptr) {
        return DTV_L3_INVALID;
    }
    l3_owner_prototype::OwnerEngine::Snapshot snapshot{};
    const auto result = owner->engine->close_status(&snapshot);
    if (result != DTV_L3_OK) {
        return result;
    }
    status->result = snapshot.cleanup_complete ? snapshot.last_cleanup_error
                                               : DTV_L3_BUSY;
    status->cleanup_epoch = snapshot.cleanup_epoch;
    status->physical_quiesced = snapshot.physical_quiesced ? 1 : 0;
    status->all_ops_recovered = snapshot.all_ops_recovered ? 1 : 0;
    status->cleanup_complete = snapshot.cleanup_complete ? 1 : 0;
    status->reserved = 0;
    return DTV_L3_OK;
}

dtv_l3_result dtv_l3_cleanup_retry(dtv_l3_context *owner) {
    return owner == nullptr ? DTV_L3_INVALID : owner->engine->retry_cleanup();
}

dtv_l3_result dtv_l3_context_release(dtv_l3_context *owner) {
    if (owner == nullptr) {
        return DTV_L3_INVALID;
    }
    if (owner->outstanding_ops.load() != 0) {
        return DTV_L3_BUSY;
    }
    if (!owner->open_thread_done.load(std::memory_order_acquire)) {
        return DTV_L3_BUSY;
    }
    if (owner->open_thread.joinable()) {
        owner->open_thread.join();
    }
    const auto result = owner->engine->release();
    if (result == DTV_L3_OK) {
        delete owner;
    }
    return result;
}

dtv_l3_result dtv_l3_transfer_start(dtv_l3_context *owner,
                                    const dtv_l3_transfer_request *request,
                                    dtv_l3_op **op_out) {
    if (owner == nullptr || request == nullptr || op_out == nullptr) {
        return DTV_L3_INVALID;
    }
    *op_out = nullptr;
    auto op = std::make_unique<dtv_l3_op>();
    op->owner = owner;
    op->kind = DTV_L3_OP_KIND_TRANSFER;
    const auto result = owner->engine->start(*request, &op->operation);
    if (result != DTV_L3_ACCEPTED) {
        if (op->operation) {
            const auto release_result = owner->engine->release_operation(
                op->operation);
            if (release_result != DTV_L3_OK) {
                owner->outstanding_ops.fetch_add(1);
                *op_out = op.release();
                return release_result;
            }
        }
        return result;
    }
    owner->outstanding_ops.fetch_add(1);
    *op_out = op.release();
    return result;
}

dtv_l3_result dtv_l3_op_cancel(dtv_l3_op *op) {
    if (op == nullptr || op->owner == nullptr) {
        return DTV_L3_INVALID;
    }
    if (op->kind == DTV_L3_OP_KIND_OPEN) {
        dtv_l3_op_result result{};
        const auto poll_result = poll_open_op(op->owner, &result);
        if (poll_result != DTV_L3_OK) {
            return poll_result;
        }
        if (result.state != DTV_L3_OP_RUNNING) {
            return DTV_L3_OK;
        }
        dtv_l3_result cancel_result;
        {
            std::lock_guard<std::mutex> lock(op->owner->open_op_state.mutex);
            if (op->owner->open_op_state.terminal) {
                return DTV_L3_OK;
            }
            cancel_result = op->owner->engine->cancel_open_if_opening();
            if (cancel_result == DTV_L3_ACCEPTED) {
                op->owner->open_op_state.cancel_requested = true;
            }
        }
        if (cancel_result == DTV_L3_OK) {
            return poll_open_op(op->owner, &result);
        }
        return cancel_result;
    }
    return op->owner->engine->cancel(op->operation);
}

dtv_l3_result dtv_l3_op_poll(dtv_l3_op *op, dtv_l3_op_result *result) {
    if (op == nullptr || op->owner == nullptr || result == nullptr) {
        return DTV_L3_INVALID;
    }
    result->kind = op->kind;
    result->cleanup_epoch = 0;
    if (op->kind == DTV_L3_OP_KIND_OPEN) {
        return poll_open_op(op->owner, result);
    }
    return op->owner->engine->poll_result(op->operation, &result->state,
                                          &result->result, &result->data,
                                          &result->data_size);
}

dtv_l3_result dtv_l3_op_release(dtv_l3_op *op) {
    if (op == nullptr || op->owner == nullptr) {
        return DTV_L3_INVALID;
    }
    auto *owner = op->owner;
    if (op->kind == DTV_L3_OP_KIND_OPEN) {
        dtv_l3_op_result result{};
        const auto call_result = dtv_l3_op_poll(op, &result);
        if (call_result != DTV_L3_OK || result.state == DTV_L3_OP_RUNNING) {
            return call_result == DTV_L3_OK ? DTV_L3_BUSY : call_result;
        }
    } else {
        const auto call_result = owner->engine->release_operation(op->operation);
        if (call_result != DTV_L3_OK) {
            return call_result;
        }
    }
    owner->outstanding_ops.fetch_sub(1);
    delete op;
    return DTV_L3_OK;
}

}  // extern "C"
