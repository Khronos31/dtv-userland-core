// SPDX-License-Identifier: GPL-2.0-only
#include "px4/enclosure_context.h"

#include <new>
#if defined(_WIN32) && defined(_MSC_VER)
#include "windows_pthread_compat.h"
#else
#include <pthread.h>
#endif
#include <utility>

namespace px4::userland::ipc::posix {

struct EnclosureCleanupThread final {
    pthread_t handle{};
};

Result<std::unique_ptr<EnclosureContext>> EnclosureContext::assemble(
    std::unique_ptr<EnclosureFamily> family,
    std::unique_ptr<TunerNonceSource> nonce_source) noexcept
{
    using Created = Result<std::unique_ptr<EnclosureContext>>;
    if (!family || !nonce_source) return Created::failure(Error::INVALID_ARGUMENT);
    std::unique_ptr<EnclosureContext> context(new (std::nothrow) EnclosureContext());
    std::unique_ptr<EnclosureCleanupThread> thread(new (std::nothrow) EnclosureCleanupThread());
    if (!context || !thread) return Created::failure(Error::INTERNAL);
    context->family_ = std::move(family);
    context->nonce_ = std::move(nonce_source);
    EnclosureFamily& graph = *context->family_;
    context->card_.reset(new (std::nothrow) CardService(graph.card_backend(),
                                                        graph.card_protocol()));
    context->tuner_.reset(new (std::nothrow) TunerService(
        graph.tuner_backend(), *context->nonce_, graph.time(), nullptr, nullptr,
        graph.stream_control()));
    if (!context->card_ || !context->tuner_) return Created::failure(Error::INTERNAL);
    const auto strict = context->tuner_->configure_strict_capabilities();
    if (!strict) return Created::failure(strict.error());
    // The cleanup thread exists before the context can be published, so a
    // later close never depends on creating a thread.
    context->thread_ = std::move(thread);
    if (pthread_create(&context->thread_->handle, nullptr, &EnclosureContext::cleanup_entry,
                       context.get()) != 0) {
        context->thread_.reset();
        return Created::failure(Error::INTERNAL);
    }
    return Created::success(std::move(context));
}

EnclosureContext::~EnclosureContext() noexcept
{
    if (!thread_) return;
    closing_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        cleanup_requested_ = true;
        state_changed_.notify_all();
        // An unquiesced graph is never freed early: wait for the retained
        // cleanup rather than destroy objects a returning task may reach.
        state_changed_.wait(lock, [&]() noexcept { return quiesced_; });
    }
    (void)pthread_join(thread_->handle, nullptr);
}

void* EnclosureContext::cleanup_entry(void* context) noexcept
{
    static_cast<EnclosureContext*>(context)->run_cleanup();
    return nullptr;
}

void EnclosureContext::run_cleanup() noexcept
{
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        state_changed_.wait(lock, [&]() noexcept { return cleanup_requested_; });
    }
    // close() never waits for the serving thread.  The cleanup worker waits
    // here so shutdown cannot race an in-progress poll or setup operation.
    {
        std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    }
    // Blocking work only on this thread.  The server shutdown returns after
    // every retired client/stream cleanup, the tuner and card shutdown tasks
    // and the worker join; each release below then runs in reverse dependency
    // order after everything that can reach the released object.
    Error primary = Error::OK;
    if (server_) {
        const auto shutdown = server_->shutdown();
        if (!shutdown) primary = shutdown.error();
        server_.reset();
    } else if (card_) {
        const auto shutdown = card_->shutdown();
        if (!shutdown) primary = shutdown.error();
    }
    tuner_.reset();
    card_.reset();
    nonce_.reset();
    if (family_) {
        const auto stopped = family_->shutdown();
        if (!stopped && primary == Error::OK) primary = stopped.error();
    }
    family_.reset();
    lease_.close();
    std::lock_guard<std::mutex> lock(state_mutex_);
    cleanup_error_ = primary;
    quiesced_ = true;
    state_changed_.notify_all();
}

namespace {

EndpointConfig control_endpoint(const EnclosurePublication& publication) noexcept
{
    return EndpointConfig{
        publication.runtime_directory.empty() ? nullptr : publication.runtime_directory.c_str(),
        publication.instance.c_str(), kControlEndpointName, publication.access};
}

}  // namespace

Result<void> EnclosureContext::reserve_namespace(
    const EnclosurePublication& publication) noexcept
{
    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    if (closing_.load(std::memory_order_acquire) || server_ || lease_.valid())
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    publication_ = publication;
    auto lease = SerialEndpointLease::acquire(control_endpoint(publication_),
                                              publication_.lease_identity);
    if (!lease) return Result<void>::failure(lease.error());
    lease_ = std::move(lease.value());
    return Result<void>::success();
}

Result<void> EnclosureContext::adopt_namespace(
    const EnclosurePublication& publication, SerialEndpointLease lease) noexcept
{
    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    if (closing_.load(std::memory_order_acquire) || server_ || lease_.valid() || !lease.valid())
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    publication_ = publication;
    lease_ = std::move(lease);
    return Result<void>::success();
}

Result<void> EnclosureContext::publish(const ControlContext& context) noexcept
{
    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    if (closing_.load(std::memory_order_acquire) || server_ || !lease_.valid())
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    // The lease stays held on failure; it is released with the graph.
    auto server = PosixControlServer::create(control_endpoint(publication_), *card_, *tuner_,
                                             context, family_->stream_control());
    if (!server) return Result<void>::failure(server.error());
    server_ = std::move(server.value());
    return Result<void>::success();
}

const PathChar* EnclosureContext::endpoint_path() const noexcept
{
    static const PathChar empty[1] = {0};
    return server_ ? server_->endpoint_path() : empty;
}

Result<void> EnclosureContext::poll_once(Timeout timeout) noexcept
{
    std::lock_guard<std::mutex> poll_lock(poll_mutex_);
    if (closing_.load(std::memory_order_acquire) || !server_)
        return Result<void>::failure(Error::NOT_READY);
    return server_->poll_once(timeout);
}

ContextCloseResult EnclosureContext::close(std::chrono::milliseconds budget) noexcept
{
    // Overflow-safe finite deadline: the budget is clamped before it is added
    // to the current steady time.
    if (budget < std::chrono::milliseconds::zero()) budget = std::chrono::milliseconds::zero();
    if (budget > kMaxCloseBudget) budget = kMaxCloseBudget;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    // Publish closing before requesting cleanup.  Neither action waits for a
    // poll that may be blocked in the operating system.
    closing_.store(true, std::memory_order_release);
    ContextCloseResult result;
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!thread_) return result;
    if (!cleanup_requested_) {
        cleanup_requested_ = true;
        state_changed_.notify_all();
    }
    result.quiesced =
        state_changed_.wait_until(lock, deadline, [&]() noexcept { return quiesced_; });
    result.deadline_expired = !result.quiesced;
    if (result.quiesced) result.error = cleanup_error_;
    return result;
}

}  // namespace px4::userland::ipc::posix
