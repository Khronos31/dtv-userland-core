/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <chrono>

namespace l3_owner_prototype {

OwnerEngine::OwnerEngine(Provider &provider, std::uint64_t binding_value)
    : provider_(provider), binding_(std::make_shared<Binding>(binding_value)),
      worker_([this] { worker_loop(); }) {}

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
        open_state_ = OpenState::Opening;
    }

    bool failed = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::shared_ptr<Binding> binding;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (close_requested_) {
                break;
            }
            binding = binding_;
        }

        const auto result = provider_.claim(binding.get(), index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result == DTV_L3_OK) {
                ++claimed_count_;
            } else {
                failed = true;
                open_state_ = OpenState::Failed;
                close_requested_ = true;
            }
            if (close_requested_) {
                break;
            }
        }
    }

    bool must_finish_close = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (close_requested_) {
            if (open_state_ != OpenState::Failed) {
                open_state_ = OpenState::Closing;
            }
        } else if (!failed) {
            open_state_ = OpenState::Ready;
        }
        must_finish_close = failed || close_requested_;
    }

    if (must_finish_close) {
        finish_close();
        std::unique_lock<std::mutex> lock(mutex_);
        state_condition_.wait(lock, [this] { return cleanup_complete_; });
    }
}

dtv_l3_result OwnerEngine::start(
    const dtv_l3_transfer_request &request,
    std::shared_ptr<Operation> *operation) {
    if (operation == nullptr ||
        (request.out_size != 0 && request.out_data == nullptr)) {
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
        if (open_state_ != OpenState::Ready || close_requested_) {
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

    std::vector<std::shared_ptr<Operation>> pending;
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
    }

    finish_close();

    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout_ms != 0 && !physical_quiesced_) {
        state_condition_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this] { return physical_quiesced_; });
    }
    return physical_quiesced_ ? DTV_L3_OK : DTV_L3_TIMEOUT;
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
    return DTV_L3_OK;
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (released_) {
            return DTV_L3_INVALID;
        }
        if (!physical_quiesced_ || !all_ops_recovered_locked() ||
            !cleanup_complete_) {
            return DTV_L3_BUSY;
        }
        retired_gate_ = true;
    }

    if (after_retire_gate) {
        after_retire_gate();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (active_borrows_ != 0) {
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
    if (!close_requested_ || cleanup_started_ || cleanup_complete_ ||
        open_state_ == OpenState::Opening || provider_calls_in_flight_ != 0) {
        return;
    }
    for (const auto &entry : operations_) {
        if (entry.second->state == DTV_L3_OP_OPENING ||
            entry.second->state == DTV_L3_OP_RUNNING) {
            return;
        }
    }
    cleanup_started_ = true;
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
    std::uint32_t claim_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        binding = binding_;
        claim_count = claimed_count_;
    }

    for (std::uint32_t index = claim_count; index > 0; --index) {
        provider_.release_claim(binding.get(), index - 1);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        claimed_count_ = 0;
        ++cleanup_count_;
        physical_quiesced_ = true;
        cleanup_complete_ = true;
        open_state_ = OpenState::Closed;
        state_condition_.notify_all();
    }
}

void OwnerEngine::worker_loop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_condition_.wait(lock, [this] {
                return worker_stopping_ || !work_queue_.empty();
            });
            if (work_queue_.empty() && worker_stopping_) {
                return;
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

bool OwnerEngine::all_ops_recovered_locked() const {
    return operations_.empty();
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

}  // namespace l3_owner_prototype
