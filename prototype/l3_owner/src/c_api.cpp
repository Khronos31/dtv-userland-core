/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

#include "owner_engine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

namespace {

constexpr int kTestOpenLaneBit = 1;

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

    dtv_l3_result scope_query(std::uint32_t receiver_id,
                              dtv_l3_scope_info *scope) override {
        if (provider_.scope_query == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.scope_query(provider_.provider_context, receiver_id,
                                     scope);
    }

    dtv_l3_result bridge_start(std::uint32_t bridge_id,
                               std::uint32_t receiver_id) override {
        if (provider_.bridge_start == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.bridge_start(provider_.provider_context, binding_.get(),
                                      bridge_id, receiver_id);
    }

    dtv_l3_result bridge_stop(std::uint32_t bridge_id) override {
        if (provider_.bridge_stop == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.bridge_stop(provider_.provider_context, binding_.get(),
                                     bridge_id);
    }

    dtv_l3_result stream_attach(std::uint64_t stream_id,
                                dtv_l3_ts_deliver_fn deliver,
                                void *callback_context) override {
        if (provider_.stream_attach == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.stream_attach(provider_.provider_context,
                                       binding_.get(), stream_id, deliver,
                                       callback_context);
    }

    dtv_l3_result stream_stop(std::uint64_t stream_id) override {
        if (provider_.stream_stop == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.stream_stop(provider_.provider_context, binding_.get(),
                                     stream_id);
    }

    dtv_l3_result tune(std::uint32_t receiver_id,
                       std::uint64_t requested_hz,
                       std::uint64_t *programmed_hz) override {
        if (provider_.tune == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.tune(provider_.provider_context, binding_.get(),
                              receiver_id, requested_hz, programmed_hz);
    }

    dtv_l3_result claim_cancel(l3_owner_prototype::Binding *binding,
                               std::uint32_t index) override {
        if (provider_.claim_cancel == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.claim_cancel(provider_.provider_context, binding,
                                      index);
    }

    dtv_l3_cancel_scope cancel_scope() const override {
        return provider_.cancel_scope;
    }

    void set_binding(std::shared_ptr<l3_owner_prototype::Binding> binding) {
        binding_ = std::move(binding);
    }

private:
    dtv_l3_provider provider_;
    std::shared_ptr<l3_owner_prototype::Binding> binding_ =
        std::make_shared<l3_owner_prototype::Binding>(1);
};

class CUsbProvider final : public l3_owner_prototype::Provider {
public:
    explicit CUsbProvider(const dtv_l3_usb_provider &provider)
        : provider_(provider) {}

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

    dtv_l3_result claim_cancel(l3_owner_prototype::Binding *binding,
                               std::uint32_t index) override {
        if (provider_.claim_cancel == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return provider_.claim_cancel(provider_.provider_context, binding,
                                      index);
    }

    dtv_l3_cancel_scope cancel_scope() const override {
        return provider_.cancel_scope;
    }

private:
    dtv_l3_usb_provider provider_;
};

class CProductAdapter final : public l3_owner_prototype::ProductAdapter {
public:
    explicit CProductAdapter(const dtv_l3_product_adapter &adapter)
        : adapter_(adapter) {}

    dtv_l3_result scope_query(std::uint32_t receiver_id,
                              dtv_l3_scope_info *scope) override {
        if (adapter_.scope_query == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.scope_query(adapter_.product_context, receiver_id,
                                    scope);
    }

    dtv_l3_result tune(std::uint32_t receiver_id,
                       std::uint64_t requested_hz,
                       std::uint64_t *programmed_hz) override {
        if (adapter_.tune == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.tune(adapter_.product_context, binding_.get(),
                             receiver_id, requested_hz, programmed_hz);
    }

    dtv_l3_result bridge_start(std::uint32_t bridge_id,
                               std::uint32_t receiver_id) override {
        if (adapter_.bridge_start == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.bridge_start(adapter_.product_context, binding_.get(),
                                     bridge_id, receiver_id);
    }

    dtv_l3_result bridge_stop(std::uint32_t bridge_id) override {
        if (adapter_.bridge_stop == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.bridge_stop(adapter_.product_context, binding_.get(),
                                    bridge_id);
    }

    dtv_l3_result stream_attach(std::uint64_t stream_id,
                                dtv_l3_ts_deliver_fn deliver,
                                void *callback_context) override {
        if (adapter_.stream_attach == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.stream_attach(adapter_.product_context, binding_.get(),
                                      stream_id, deliver, callback_context);
    }

    dtv_l3_result stream_stop(std::uint64_t stream_id) override {
        if (adapter_.stream_stop == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        return adapter_.stream_stop(adapter_.product_context, binding_.get(),
                                    stream_id);
    }

    void set_binding(std::shared_ptr<l3_owner_prototype::Binding> binding) {
        binding_ = std::move(binding);
    }

private:
    dtv_l3_product_adapter adapter_;
    std::shared_ptr<l3_owner_prototype::Binding> binding_ =
        std::make_shared<l3_owner_prototype::Binding>(1);
};

static std::unique_ptr<l3_owner_prototype::OwnerEngine> make_engine_legacy(
    CProvider &adapter,
    const dtv_l3_provider &provider) {
    l3_owner_prototype::maybe_throw_test_exception(
        l3_owner_prototype::kTestInjectOpenContext);
    return std::make_unique<l3_owner_prototype::OwnerEngine>(
        adapter, 1, provider.logger);
}

static std::unique_ptr<l3_owner_prototype::OwnerEngine> make_engine_ex(
    CUsbProvider &usb_provider,
    CProductAdapter &product_adapter,
    const dtv_l3_logger &logger) {
    l3_owner_prototype::maybe_throw_test_exception(
        l3_owner_prototype::kTestInjectOpenContext);
    return std::make_unique<l3_owner_prototype::OwnerEngine>(
        usb_provider, 1, logger, &product_adapter);
}

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
        : legacy(std::make_unique<CProvider>(provider)),
          logger(provider.logger),
          engine(make_engine_legacy(*legacy, provider)) {
        legacy->set_binding(engine->binding());
        engine->set_submit_mode(DTV_L3_SUBMIT_NONBLOCKING);
        capabilities.cancel_scope = provider.cancel_scope;
    }
    dtv_l3_context(const dtv_l3_usb_provider &usb,
                   const dtv_l3_product_adapter &adapter)
        : usb_provider(std::make_unique<CUsbProvider>(usb)),
          product_adapter(std::make_unique<CProductAdapter>(adapter)),
          product_view(&adapter),
          capabilities(adapter.capabilities),
          catalog(adapter.catalog),
          logger(usb.logger),
          engine(make_engine_ex(*usb_provider, *product_adapter, usb.logger)) {
        product_adapter->set_binding(engine->binding());
        engine->set_submit_mode(usb.submit_mode);
    }
    ~dtv_l3_context() {
        if (open_thread.joinable()) {
            open_thread.join();
        }
        if (adapter_release != nullptr) {
            adapter_release(adapter_owner);
        }
    }

    std::unique_ptr<CProvider> legacy;
    std::unique_ptr<CUsbProvider> usb_provider;
    std::unique_ptr<CProductAdapter> product_adapter;
    const dtv_l3_product_adapter *product_view = nullptr;
    dtv_l3_capabilities capabilities{};
    const dtv_l3_capability_catalog *catalog = nullptr;
    dtv_l3_adapter_release_fn adapter_release = nullptr;
    void *adapter_owner = nullptr;
    dtv_l3_logger logger;
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

struct dtv_l3_lease {
    dtv_l3_context *owner = nullptr;
    std::uint32_t id = 0;
};

struct dtv_l3_stream {
    dtv_l3_context *owner = nullptr;
    std::uint64_t generation = 0;
};

namespace {

void log_context(dtv_l3_context *owner,
                 dtv_l3_log_level level,
                 const char *fn,
                 const char *message) noexcept {
    if (owner == nullptr || owner->logger.fn == nullptr ||
        level > owner->logger.min_level) {
        return;
    }
    l3_owner_prototype::CallbackFrame frame;
    try {
        owner->logger.fn(owner->logger.userdata, level, fn, message);
    } catch (...) {
    }
}

void log_logger(const dtv_l3_logger *logger,
                dtv_l3_log_level level,
                const char *fn,
                const char *message) noexcept {
    if (logger == nullptr || logger->fn == nullptr ||
        level > logger->min_level) {
        return;
    }
    l3_owner_prototype::CallbackFrame frame;
    try {
        logger->fn(logger->userdata, level, fn, message);
    } catch (...) {
    }
}

void log_provider(const dtv_l3_provider *provider,
                  dtv_l3_log_level level,
                  const char *fn,
                  const char *message) noexcept {
    if (provider == nullptr) {
        return;
    }
    log_logger(&provider->logger, level, fn, message);
}

void log_provider(const dtv_l3_logger *logger,
                  dtv_l3_log_level level,
                  const char *fn,
                  const char *message) noexcept {
    log_logger(logger, level, fn, message);
}

dtv_l3_result entry_failure(dtv_l3_context *owner,
                            const char *fn) noexcept {
    log_context(owner, DTV_L3_LOG_ERROR, fn, "contained C++ exception");
    return DTV_L3_FAILED;
}

}  // namespace

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

static dtv_l3_open_result finish_open(dtv_l3_context *owner,
                                     uint32_t claim_count) {
    dtv_l3_op *op = nullptr;
    try {
        l3_owner_prototype::maybe_throw_test_exception(
            l3_owner_prototype::kTestInjectOpenOp);
        op = new (std::nothrow) dtv_l3_op;
        if (op == nullptr) {
            log_context(owner, DTV_L3_LOG_ERROR, "dtv_l3_open",
                        "open allocation failed");
            delete owner;
            return {DTV_L3_FAILED, nullptr, nullptr};
        }
        op->owner = owner;
        op->kind = DTV_L3_OP_KIND_OPEN;
        owner->outstanding_ops.store(1);
        if (l3_owner_prototype::test_lane_start_fails(kTestOpenLaneBit)) {
            log_context(owner, DTV_L3_LOG_ERROR, "dtv_l3_open",
                        "lane start failed");
            delete op;
            delete owner;
            return {DTV_L3_FAILED, nullptr, nullptr};
        }
        owner->engine->mark_open_lane_running();
        owner->open_thread = std::thread([owner, claim_count] {
            try {
                owner->engine->initialize_claims(claim_count);
            } catch (...) {
                owner->engine->abort_open_after_lane_exception();
            }
            owner->engine->mark_open_lane_exited();
            owner->open_thread_done.store(true, std::memory_order_release);
        });
    } catch (...) {
        log_context(owner, DTV_L3_LOG_ERROR, "dtv_l3_open",
                    "contained C++ exception");
        delete op;
        delete owner;
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    log_context(owner, DTV_L3_LOG_INFO, "dtv_l3_open", "open accepted");
    return {DTV_L3_ACCEPTED, owner, op};
}

dtv_l3_open_result dtv_l3_open(const dtv_l3_provider *provider,
                               uint32_t claim_count) {
    if (provider == nullptr || provider->claim == nullptr ||
        provider->release_claim == nullptr || provider->submit == nullptr ||
        provider->cancel == nullptr) {
        return {DTV_L3_INVALID, nullptr, nullptr};
    }
    dtv_l3_context *owner = nullptr;
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        owner = new (std::nothrow) dtv_l3_context(*provider);
    } catch (...) {
        delete owner;
        log_provider(provider, DTV_L3_LOG_ERROR, "dtv_l3_open",
                     "contained C++ exception");
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    if (owner == nullptr) {
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    return finish_open(owner, claim_count);
}

dtv_l3_open_result dtv_l3_open_ex(const dtv_l3_usb_provider *usb_provider,
                                  const dtv_l3_product_adapter *adapter,
                                  uint32_t claim_count) {
    if (usb_provider == nullptr || usb_provider->claim == nullptr ||
        usb_provider->release_claim == nullptr ||
        usb_provider->submit == nullptr || usb_provider->cancel == nullptr ||
        adapter == nullptr) {
        return {DTV_L3_INVALID, nullptr, nullptr};
    }
    dtv_l3_context *owner = nullptr;
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        owner = new (std::nothrow) dtv_l3_context(*usb_provider, *adapter);
    } catch (...) {
        delete owner;
        log_provider(&usb_provider->logger, DTV_L3_LOG_ERROR,
                     "dtv_l3_open_ex", "contained C++ exception");
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    if (owner == nullptr) {
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    return finish_open(owner, claim_count);
}

dtv_l3_open_result dtv_l3_open_factory(
    const dtv_l3_usb_provider *usb_provider,
    const dtv_l3_adapter_factory *factory,
    const dtv_l3_open_config *config,
    uint32_t claim_count) {
    if (usb_provider == nullptr || usb_provider->claim == nullptr ||
        usb_provider->release_claim == nullptr ||
        usb_provider->submit == nullptr || usb_provider->cancel == nullptr ||
        factory == nullptr || factory->create == nullptr) {
        return {DTV_L3_INVALID, nullptr, nullptr};
    }
    const dtv_l3_product_adapter *adapter = nullptr;
    void *adapter_owner = nullptr;
    dtv_l3_result created = DTV_L3_FAILED;
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        created = factory->create(factory->factory_context, config, &adapter,
                                  &adapter_owner);
    } catch (...) {
        log_provider(&usb_provider->logger, DTV_L3_LOG_ERROR,
                     "dtv_l3_open_factory", "contained C++ exception");
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    if (created != DTV_L3_OK || adapter == nullptr) {
        if (factory->release != nullptr && adapter_owner != nullptr) {
            factory->release(adapter_owner);
        }
        return {created == DTV_L3_OK ? DTV_L3_FAILED : created, nullptr,
                nullptr};
    }
    const dtv_l3_open_result opened =
        dtv_l3_open_ex(usb_provider, adapter, claim_count);
    if (opened.result != DTV_L3_ACCEPTED) {
        if (factory->release != nullptr && adapter_owner != nullptr) {
            factory->release(adapter_owner);
        }
        return opened;
    }
    opened.owner->adapter_release = factory->release;
    opened.owner->adapter_owner = adapter_owner;
    return opened;
}

dtv_l3_open_result dtv_l3_open_with_args(const dtv_l3_open_args *args) {
    if (args == nullptr || args->usb_provider == nullptr ||
        args->usb_provider->claim == nullptr ||
        args->usb_provider->release_claim == nullptr ||
        args->usb_provider->submit == nullptr ||
        args->usb_provider->cancel == nullptr || args->factory == nullptr ||
        args->factory->create == nullptr ||
        (args->grant_count != 0 && args->grants == nullptr)) {
        return {DTV_L3_INVALID, nullptr, nullptr};
    }
    const dtv_l3_product_adapter *adapter = nullptr;
    uint32_t claim_count = 0;
    void *adapter_owner = nullptr;
    dtv_l3_result created = DTV_L3_FAILED;
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        created = args->factory->create(args->factory->factory_context, args,
                                        &adapter, &claim_count,
                                        &adapter_owner);
    } catch (...) {
        log_provider(&args->usb_provider->logger, DTV_L3_LOG_ERROR,
                     "dtv_l3_open_with_args", "contained C++ exception");
        return {DTV_L3_FAILED, nullptr, nullptr};
    }
    if (created != DTV_L3_OK || adapter == nullptr) {
        if (args->factory->release != nullptr && adapter_owner != nullptr) {
            args->factory->release(adapter_owner);
        }
        return {created == DTV_L3_OK ? DTV_L3_FAILED : created, nullptr,
                nullptr};
    }
    const dtv_l3_open_result opened =
        dtv_l3_open_ex(args->usb_provider, adapter, claim_count);
    if (opened.result != DTV_L3_ACCEPTED) {
        if (args->factory->release != nullptr && adapter_owner != nullptr) {
            args->factory->release(adapter_owner);
        }
        return opened;
    }
    opened.owner->adapter_release = args->factory->release;
    opened.owner->adapter_owner = adapter_owner;
    return opened;
}

dtv_l3_result dtv_l3_capabilities_get_catalog(
    dtv_l3_context *owner,
    const dtv_l3_capability_catalog **catalog) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr || catalog == nullptr) {
            return DTV_L3_INVALID;
        }
        if (owner->catalog == nullptr) {
            return DTV_L3_UNSUPPORTED;
        }
        *catalog = owner->catalog;
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_capabilities_get_catalog");
    }
}

dtv_l3_result dtv_l3_capabilities_get(dtv_l3_context *owner,
                                      dtv_l3_capabilities *capabilities) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr || capabilities == nullptr) {
            return DTV_L3_INVALID;
        }
        *capabilities = owner->capabilities;
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_capabilities_get");
    }
}

dtv_l3_result dtv_l3_open_status_get(dtv_l3_context *owner,
                                    dtv_l3_open_status *status) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
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
        status->disconnected = snapshot.disconnected ? 1 : 0;
        status->reserved[0] = status->reserved[1] = 0;
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_open_status_get");
    }
}

dtv_l3_result dtv_l3_close(dtv_l3_context *owner, uint32_t timeout_ms) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        return owner == nullptr ? DTV_L3_INVALID : owner->engine->close(timeout_ms);
    } catch (...) {
        return entry_failure(owner, "dtv_l3_close");
    }
}

dtv_l3_result dtv_l3_close_status_get(dtv_l3_context *owner,
                                     dtv_l3_close_status *status) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
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
        status->releasable =
            snapshot.releasable && owner->outstanding_ops.load() == 0 ? 1 : 0;
        status->open_lane = snapshot.open_lane;
        status->control_lane = snapshot.control_lane;
        status->live_lanes = snapshot.live_lanes;
        status->disconnected = snapshot.disconnected ? 1 : 0;
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_close_status_get");
    }
}

dtv_l3_result dtv_l3_cleanup_retry(dtv_l3_context *owner) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        return owner == nullptr ? DTV_L3_INVALID : owner->engine->retry_cleanup();
    } catch (...) {
        return entry_failure(owner, "dtv_l3_cleanup_retry");
    }
}

dtv_l3_result dtv_l3_context_release(dtv_l3_context *owner) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr) {
            return DTV_L3_INVALID;
        }
        if (l3_owner_prototype::in_callback_frame()) {
            return DTV_L3_BUSY;
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
    } catch (...) {
        return entry_failure(owner, "dtv_l3_context_release");
    }
}

dtv_l3_result dtv_l3_transfer_start(dtv_l3_context *owner,
                                    const dtv_l3_transfer_request *request,
                                    dtv_l3_op **op_out) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
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
    } catch (...) {
        return entry_failure(owner, "dtv_l3_transfer_start");
    }
}

dtv_l3_result dtv_l3_op_cancel(dtv_l3_op *op) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
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
    } catch (...) {
        return entry_failure(op != nullptr ? op->owner : nullptr, "dtv_l3_op_cancel");
    }
}

dtv_l3_result dtv_l3_op_poll(dtv_l3_op *op, dtv_l3_op_result *result) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (op == nullptr || op->owner == nullptr || result == nullptr) {
            return DTV_L3_INVALID;
        }
        result->kind = op->kind;
        result->cleanup_epoch = 0;
        result->cancel_cause = DTV_L3_CANCEL_CAUSE_NONE;
        result->provider_abort_resubmits = 0;
        if (op->kind == DTV_L3_OP_KIND_OPEN) {
            return poll_open_op(op->owner, result);
        }
        return op->owner->engine->poll_result(
            op->operation, &result->state, &result->result, &result->data,
            &result->data_size, &result->cancel_cause,
            &result->provider_abort_resubmits);
    } catch (...) {
        return entry_failure(op != nullptr ? op->owner : nullptr, "dtv_l3_op_poll");
    }
}

dtv_l3_result dtv_l3_op_wait(dtv_l3_op *op, uint32_t timeout_ms) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (op == nullptr || op->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        if (op->kind == DTV_L3_OP_KIND_OPEN) {
            dtv_l3_op_result result{};
            const auto call_result = poll_open_op(op->owner, &result);
            if (call_result != DTV_L3_OK) {
                return call_result;
            }
            if (result.state != DTV_L3_OP_RUNNING) {
                return DTV_L3_OK;
            }
            const auto wait_result = op->owner->engine->wait_open(timeout_ms);
            if (wait_result == DTV_L3_OK) {
                (void)poll_open_op(op->owner, &result);
            }
            return wait_result;
        }
        return op->owner->engine->wait_operation(op->operation, timeout_ms);
    } catch (...) {
        return entry_failure(op != nullptr ? op->owner : nullptr, "dtv_l3_op_wait");
    }
}

dtv_l3_result dtv_l3_op_release(dtv_l3_op *op) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
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
    } catch (...) {
        return entry_failure(op != nullptr ? op->owner : nullptr, "dtv_l3_op_release");
    }
}

dtv_l3_result dtv_l3_lease_acquire(dtv_l3_context *owner,
                                   uint32_t receiver_id,
                                   dtv_l3_lease **lease_out) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr || lease_out == nullptr) {
            return DTV_L3_INVALID;
        }
        *lease_out = nullptr;
        auto lease = std::make_unique<dtv_l3_lease>();
        std::uint32_t id = 0;
        const auto result = owner->engine->acquire_lease(receiver_id, &id);
        if (result != DTV_L3_OK) {
            return result;
        }
        lease->owner = owner;
        lease->id = id;
        *lease_out = lease.release();
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_lease_acquire");
    }
}

dtv_l3_result dtv_l3_lease_tune(dtv_l3_lease *lease,
                                uint64_t requested_hz,
                                uint64_t *programmed_hz,
                                uint64_t *tune_revision) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (lease == nullptr || lease->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return lease->owner->engine->tune_lease(lease->id, requested_hz,
                                                programmed_hz, tune_revision);
    } catch (...) {
        return entry_failure(lease != nullptr ? lease->owner : nullptr, "dtv_l3_lease_tune");
    }
}

dtv_l3_result dtv_l3_lease_start(dtv_l3_lease *lease) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (lease == nullptr || lease->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return lease->owner->engine->start_lease(lease->id);
    } catch (...) {
        return entry_failure(lease != nullptr ? lease->owner : nullptr, "dtv_l3_lease_start");
    }
}

dtv_l3_result dtv_l3_lease_attach(dtv_l3_lease *lease,
                                  dtv_l3_stream **stream_out) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (lease == nullptr || lease->owner == nullptr || stream_out == nullptr) {
            return DTV_L3_INVALID;
        }
        *stream_out = nullptr;
        auto stream = std::make_unique<dtv_l3_stream>();
        std::uint64_t generation = 0;
        const auto result =
            lease->owner->engine->attach_lease(lease->id, &generation);
        if (result != DTV_L3_OK) {
            return result;
        }
        stream->owner = lease->owner;
        stream->generation = generation;
        *stream_out = stream.release();
        return DTV_L3_OK;
    } catch (...) {
        return entry_failure(lease != nullptr ? lease->owner : nullptr, "dtv_l3_lease_attach");
    }
}

dtv_l3_result dtv_l3_lease_status_get(dtv_l3_lease *lease,
                                      dtv_l3_lease_status *status) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (lease == nullptr || lease->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return lease->owner->engine->lease_status(lease->id, status);
    } catch (...) {
        return entry_failure(lease != nullptr ? lease->owner : nullptr, "dtv_l3_lease_status_get");
    }
}

dtv_l3_result dtv_l3_lease_release(dtv_l3_lease *lease) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (lease == nullptr || lease->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        const auto result = lease->owner->engine->release_lease(lease->id);
        if (result == DTV_L3_OK) {
            delete lease;
        }
        return result;
    } catch (...) {
        return entry_failure(lease != nullptr ? lease->owner : nullptr, "dtv_l3_lease_release");
    }
}

dtv_l3_result dtv_l3_stream_stop(dtv_l3_stream *stream, uint32_t timeout_ms) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return stream->owner->engine->stop_stream(stream->generation, timeout_ms);
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_stop");
    }
}

dtv_l3_result dtv_l3_stream_read_ts(dtv_l3_stream *stream,
                                    uint8_t *buffer,
                                    size_t capacity_bytes,
                                    size_t *bytes_read,
                                    dtv_l3_read_state *state) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return stream->owner->engine->read_stream(stream->generation, buffer,
                                                  capacity_bytes, bytes_read, state);
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_read_ts");
    }
}

dtv_l3_result dtv_l3_stream_read_ts_wait(dtv_l3_stream *stream,
                                         uint8_t *buffer,
                                         size_t capacity_bytes,
                                         size_t *bytes_read,
                                         dtv_l3_read_state *state,
                                         uint32_t timeout_ms) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return stream->owner->engine->wait_read_stream(
            stream->generation, buffer, capacity_bytes, bytes_read, state,
            timeout_ms);
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_read_ts_wait");
    }
}

dtv_l3_result dtv_l3_stream_status_get(dtv_l3_stream *stream,
                                       dtv_l3_stream_status *status) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return stream->owner->engine->stream_status(stream->generation, status);
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_status_get");
    }
}

dtv_l3_result dtv_l3_stream_release(dtv_l3_stream *stream,
                                    dtv_l3_stream_release_mode mode) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        return stream->owner->engine->release_stream(stream->generation, mode);
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_release");
    }
}

dtv_l3_result dtv_l3_stream_destroy(dtv_l3_stream *stream) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (stream == nullptr || stream->owner == nullptr) {
            return DTV_L3_INVALID;
        }
        const auto result =
            stream->owner->engine->destroy_stream(stream->generation);
        if (result == DTV_L3_OK || result == DTV_L3_STALE) {
            delete stream;
            return DTV_L3_OK;
        }
        return result;
    } catch (...) {
        return entry_failure(stream != nullptr ? stream->owner : nullptr, "dtv_l3_stream_destroy");
    }
}

dtv_l3_result dtv_l3_reader_acquire(dtv_l3_context *owner,
                                    uint32_t reader_id,
                                    dtv_l3_reader **reader_out) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr || reader_out == nullptr) {
            return DTV_L3_INVALID;
        }
        if (owner->product_view != nullptr &&
            owner->product_view->reader_acquire != nullptr) {
            *reader_out = nullptr;
            return owner->product_view->reader_acquire(
                owner->product_view->product_context, reader_id, reader_out);
        }
        return DTV_L3_UNSUPPORTED;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_reader_acquire");
    }
}

dtv_l3_result dtv_l3_lnb_set(dtv_l3_context *owner,
                             uint32_t receiver_id,
                             uint32_t mode) {
    try {
        l3_owner_prototype::maybe_throw_test_entry_exception();
        if (owner == nullptr) {
            return DTV_L3_INVALID;
        }
        if (owner->product_view != nullptr &&
            owner->product_view->lnb_set != nullptr) {
            return owner->product_view->lnb_set(
                owner->product_view->product_context, receiver_id, mode);
        }
        return DTV_L3_UNSUPPORTED;
    } catch (...) {
        return entry_failure(owner, "dtv_l3_lnb_set");
    }
}

/* Prototype test seam; not part of the C contract. */
void l3_test_set_lane_start_failure_mask(int mask) {
    l3_owner_prototype::set_test_lane_start_failure_mask(mask);
}

/* Prototype test seam; not part of the C contract. */
void l3_test_set_exception_mask(unsigned int mask) {
    l3_owner_prototype::set_test_exception_mask(mask);
}

}  // extern "C"
