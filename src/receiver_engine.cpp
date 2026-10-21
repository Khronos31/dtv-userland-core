// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <cstddef>
#include <limits>

namespace dtv::userland::detail {

ReceiverEngine::ReceiverEngine(ReceiverBackend& backend, const ReceiverId* receivers,
                               std::size_t receiver_count,
                               ReceiverTokenSource& token_source, ReceiverClock& clock,
                               ReceiverIdSource* lease_id_source,
                               ReceiverIdSource* attachment_id_source,
                               ReceiverStreamControl* stream_control) noexcept
    : backend_(backend), token_source_(token_source), clock_(clock),
      lease_id_source_(lease_id_source), attachment_id_source_(attachment_id_source),
      stream_control_(stream_control)
{
    states_.fill(ReceiverState::free);
    bool configured = receiver_count <= receivers_.size() &&
                      (receiver_count == 0U || receivers != nullptr);
    for (std::size_t slot = 0U; configured && slot < receiver_count; ++slot) {
        for (std::size_t earlier = 0U; earlier < slot; ++earlier) {
            if (receivers[earlier] == receivers[slot]) configured = false;
        }
        receivers_[slot] = receivers[slot];
    }
    configured_ = configured;
    receiver_count_ = configured ? receiver_count : 0U;
}

ReceiverEngine::~ReceiverEngine() noexcept
{
    (void)shutdown();
}

bool ReceiverEngine::valid_client(std::uint64_t client_id) noexcept
{
    return client_id != 0U;
}

int ReceiverEngine::find_slot(ReceiverId receiver) const noexcept
{
    for (std::size_t slot = 0U; slot < receiver_count_; ++slot) {
        if (receivers_[slot] == receiver) return static_cast<int>(slot);
    }
    return -1;
}

ReceiverSystem ReceiverEngine::initial_system(std::size_t slot) const noexcept
{
    // A dual-system receiver cannot stream before its first tune, which
    // replaces this value with the tuned system.
    const ReceiverId receiver = receivers_[slot];
    return backend_.supports(receiver, ReceiverSystem::satellite) &&
                   !backend_.supports(receiver, ReceiverSystem::terrestrial)
        ? ReceiverSystem::satellite
        : ReceiverSystem::terrestrial;
}

Attachment ReceiverEngine::attachment_for(const Lease& lease,
                                          std::uint64_t attachment_id) const noexcept
{
    return Attachment{lease.client_id, lease.lease_id, attachment_id,
                      receivers_[lease.slot], lease.system, lease.token};
}

int ReceiverEngine::find_lease_locked(std::uint64_t client_id,
                                      std::uint64_t lease_id) const noexcept
{
    if (!valid_client(client_id) || lease_id == 0U) return -1;
    for (std::size_t index = 0U; index < leases_.size(); ++index) {
        const Lease& lease = leases_[index];
        if (lease.active && lease.client_id == client_id &&
            lease.lease_id == lease_id)
            return static_cast<int>(index);
    }
    return -1;
}

bool ReceiverEngine::lease_id_in_use_locked(std::uint64_t lease_id) const noexcept
{
    if (lease_id == 0U) return true;
    for (const Lease& lease : leases_) {
        if (lease.active && lease.lease_id == lease_id) return true;
    }
    for (const std::uint64_t reserved : reserved_lease_ids_) {
        if (reserved == lease_id) return true;
    }
    return false;
}

bool ReceiverEngine::attachment_id_in_use_locked(
    std::uint64_t attachment_id) const noexcept
{
    if (attachment_id == 0U) return true;
    for (const Lease& lease : leases_) {
        if (lease.active && lease.attachment_id == attachment_id) return true;
    }
    for (const std::uint64_t reserved : reserved_attachment_ids_) {
        if (reserved == attachment_id) return true;
    }
    return false;
}

Result<std::uint64_t> ReceiverEngine::allocate_lease_id_locked(std::size_t slot) noexcept
{
    constexpr std::size_t kMaxAttempts = kReceiverEngineCapacity + 8U;
    for (std::size_t attempt = 0U; attempt < kMaxAttempts; ++attempt) {
        std::uint64_t candidate = 0U;
        if (lease_id_source_ != nullptr) {
            const auto next = lease_id_source_->next();
            if (!next) return Result<std::uint64_t>::failure(next.error());
            candidate = next.value();
        } else {
            candidate = next_lease_id_;
            next_lease_id_ = candidate == std::numeric_limits<std::uint64_t>::max()
                ? 1U
                : candidate + 1U;
        }
        if (candidate == 0U) {
            return Result<std::uint64_t>::failure(Error::INVALID_ARGUMENT);
        }
        if (!lease_id_in_use_locked(candidate)) {
            reserved_lease_ids_[slot] = candidate;
            return Result<std::uint64_t>::success(candidate);
        }
    }
    return Result<std::uint64_t>::failure(Error::BUSY);
}

Result<std::uint64_t> ReceiverEngine::allocate_attachment_id_locked(
    std::size_t slot) noexcept
{
    constexpr std::size_t kMaxAttempts = kReceiverEngineCapacity + 8U;
    for (std::size_t attempt = 0U; attempt < kMaxAttempts; ++attempt) {
        std::uint64_t candidate = 0U;
        if (attachment_id_source_ != nullptr) {
            const auto next = attachment_id_source_->next();
            if (!next) return Result<std::uint64_t>::failure(next.error());
            candidate = next.value();
        } else {
            candidate = next_attachment_id_;
            next_attachment_id_ = candidate == std::numeric_limits<std::uint64_t>::max()
                ? 0U
                : candidate + 1U;
        }
        if (candidate == 0U) {
            return Result<std::uint64_t>::failure(Error::BUSY);
        }
        // Attachment identities are never reused during this engine's
        // lifetime.  This remains true even when a deterministic test source
        // offers a stale numeric value after the old stream detached.
        if (candidate > last_attachment_id_ &&
            !attachment_id_in_use_locked(candidate)) {
            reserved_attachment_ids_[slot] = candidate;
            last_attachment_id_ = candidate;
            return Result<std::uint64_t>::success(candidate);
        }
    }
    return Result<std::uint64_t>::failure(Error::BUSY);
}

void ReceiverEngine::set_state_locked(std::size_t slot, ReceiverState state) noexcept
{
    if (states_[slot] != state) {
        states_[slot] = state;
        ++generation_;
    }
}

Result<AcquireResult> ReceiverEngine::acquire(std::uint64_t client_id,
                                              ReceiverId receiver) noexcept
{
    const int found = valid_client(client_id) ? find_slot(receiver) : -1;
    if (found < 0) return Result<AcquireResult>::failure(Error::INVALID_ARGUMENT);
    const auto slot = static_cast<std::size_t>(found);
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    ReceiverToken token_value{};
    std::uint64_t lease_id_value = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (leases_[slot].active)
            return Result<AcquireResult>::failure(Error::BUSY);
        if (states_[slot] == ReceiverState::error)
            return Result<AcquireResult>::failure(Error::NOT_READY);

        const auto token = token_source_.generate();
        if (!token) return Result<AcquireResult>::failure(token.error());
        const auto lease_id = allocate_lease_id_locked(slot);
        if (!lease_id)
            return Result<AcquireResult>::failure(lease_id.error());
        token_value = token.value();
        lease_id_value = lease_id.value();
    }

    const auto opened = backend_.open(receiver);
    if (!opened) {
        observe_disconnect(slot, opened.error());
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_lease_ids_[slot] = 0U;
        return Result<AcquireResult>::failure(opened.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // receiver_lock prevents another operation from changing this slot while
    // the backend open is in flight.
    Lease& lease = leases_[slot];
    reserved_lease_ids_[slot] = 0U;
    lease.active = true;
    lease.client_id = client_id;
    lease.lease_id = lease_id_value;
    lease.slot = slot;
    lease.system = initial_system(slot);
    lease.token = token_value;
    set_state_locked(slot, ReceiverState::leased);
    return Result<AcquireResult>::success(AcquireResult{lease.lease_id, lease.token});
}

void ReceiverEngine::observe_disconnect(std::size_t slot, Error error) noexcept
{
    if (error == Error::DISCONNECTED) {
        backend_.mark_disconnected(receivers_[slot]);
    }
}

Result<void> ReceiverEngine::close_lease(const Lease& lease) noexcept
{
    const auto result = backend_.close(receivers_[lease.slot]);
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::stop_capture_if_needed(const Lease& lease) noexcept
{
    if (lease.backend_disconnected)
        return Result<void>::failure(Error::DISCONNECTED);
    if (!lease.capture_maybe_active) return Result<void>::success();
    const auto result = backend_.stop_capture(receivers_[lease.slot], lease.system);
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::detach_data_plane_if_needed(const Lease& lease) noexcept
{
    if (!lease.stream_maybe_attached || stream_control_ == nullptr)
        return Result<void>::success();
    const auto result = stream_control_->detach(attachment_for(lease, lease.attachment_id));
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::release_lease(std::uint64_t client_id,
                                           std::uint64_t lease_id) noexcept
{
    std::size_t slot = 0U;
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
        // Linearize cleanup with ATTACH before waiting for the receiver
        // operation.  ATTACH will compensate if its finite start is already
        // in progress.
        const StreamState stream = leases_[slot].stream_state;
        if (stream == StreamState::armed || stream == StreamState::attaching ||
            stream == StreamState::active)
            leases_[slot].stream_state = StreamState::revoked;
    }

    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        leases_[static_cast<std::size_t>(index)] = Lease{};
        set_state_locked(slot, ReceiverState::free);
    }

    Error first = Error::OK;
    const auto detached = detach_data_plane_if_needed(lease);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    if (!detach_clean) first = detached.error();
    const bool disconnected = lease.backend_disconnected ||
                              detached.error() == Error::DISCONNECTED;
    const auto stopped = disconnected
        ? Result<void>::failure(Error::DISCONNECTED)
        : stop_capture_if_needed(lease);
    if (!stopped && first == Error::OK) first = stopped.error();
    // A disconnected transport is terminal for this cleanup path; do not
    // issue a later close write to the same device.
    if (!disconnected &&
        stopped.error() != Error::DISCONNECTED) {
        const auto closed = close_lease(lease);
        if (!closed && first == Error::OK) first = closed.error();
    }
    if (!detach_clean || !stopped || disconnected) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_state_locked(slot, ReceiverState::error);
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::release(std::uint64_t client_id,
                                     std::uint64_t lease_id) noexcept
{
    return release_lease(client_id, lease_id);
}

Result<bool> ReceiverEngine::poll_lock(std::size_t slot, ReceiverSystem system,
                                       std::uint64_t start_ms,
                                       std::uint32_t timeout_ms) noexcept
{
    const std::size_t max_polls = static_cast<std::size_t>(timeout_ms / 10U) + 1U;
    for (std::size_t count = 0U; count < max_polls; ++count) {
        const std::uint64_t before = clock_.monotonic_ms();
        if (before - start_ms >= timeout_ms) break;
        const auto locked = backend_.is_locked(receivers_[slot], system);
        if (!locked) {
            observe_disconnect(slot, locked.error());
            return Result<bool>::failure(locked.error());
        }
        if (locked.value()) return Result<bool>::success(true);

        const std::uint64_t now = clock_.monotonic_ms();
        const std::uint64_t elapsed = now - start_ms;
        if (elapsed >= timeout_ms || count + 1U == max_polls)
            break;
        const std::uint64_t remaining = timeout_ms - elapsed;
        if (remaining < 10U) break;
        clock_.sleep_ms(10U);
    }
    return Result<bool>::success(false);
}

Result<void> ReceiverEngine::tune(std::uint64_t client_id,
                                  const TuneRequest& request) noexcept
{
    std::size_t slot = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, request.lease_id);
        if (index < 0)
            return Result<void>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, request.lease_id);
        if (index < 0)
            return Result<void>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        if (states_[lease.slot] == ReceiverState::error)
            return Result<void>::failure(Error::NOT_READY);
    }
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active ||
        lease.stream_state == StreamState::revoked)
        return Result<void>::failure(Error::BUSY);
    const ReceiverId receiver = receivers_[slot];
    if (!backend_.supports(receiver, request.system))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    // The decision travels with this request; nothing else is consulted.
    if (request.validation != Error::OK)
        return Result<void>::failure(request.validation);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_state_locked(slot, ReceiverState::leased);
    }
    const auto power_prepared = backend_.begin_tune_power(
        receiver, request.system, request.lnb_voltage);
    if (!power_prepared) {
        observe_disconnect(slot, power_prepared.error());
        return Result<void>::failure(power_prepared.error());
    }
    const auto fail_after_power = [&](Error primary) noexcept {
        // A transport-loss result is terminal before rollback: rollback may
        // update logical references but must not write GPIO on that bridge.
        observe_disconnect(slot, primary);
        const auto rollback = backend_.rollback_tune_power(receiver);
        observe_disconnect(slot, rollback.error());
        if (!rollback) {
            std::lock_guard<std::mutex> lock(mutex_);
            set_state_locked(slot, ReceiverState::error);
        }
        return Result<void>::failure(primary);
    };
    const std::uint64_t start_ms = clock_.monotonic_ms();
    const auto remaining_before_tune = [&]() noexcept -> std::uint32_t {
        const std::uint64_t elapsed = clock_.monotonic_ms() - start_ms;
        return elapsed >= request.timeout_ms
            ? 0U
            : static_cast<std::uint32_t>(request.timeout_ms - elapsed);
    };
    const bool select_before_tune = request.system == ReceiverSystem::satellite &&
                                    backend_.selects_satellite_stream_before_tune();
    const auto select_satellite_stream = [&](std::uint32_t remaining) noexcept {
        return request.slot != 0xffffU
            ? backend_.select_satellite_slot(
                  receiver, static_cast<std::uint8_t>(request.slot), remaining)
            : backend_.select_satellite_tsid(receiver, request.stream_id, remaining);
    };
    if (select_before_tune) {
        const std::uint32_t select_timeout = remaining_before_tune();
        if (select_timeout == 0U)
            return fail_after_power(Error::TIMEOUT);
        const auto selected = select_satellite_stream(select_timeout);
        if (!selected)
            return fail_after_power(selected.error());
    }
    const std::uint32_t tune_timeout = remaining_before_tune();
    if (tune_timeout == 0U)
        return fail_after_power(Error::TIMEOUT);
    const auto tuned = backend_.tune(receiver, request.system, request.frequency_khz,
                                     tune_timeout);
    if (!tuned)
        return fail_after_power(tuned.error());

    // Keep this origin separate from start_ms: the settle interval starts
    // when the lock check begins, after the frontend tune has completed.
    const std::uint64_t lock_poll_start_ms = clock_.monotonic_ms();
    const auto locked = poll_lock(slot, request.system, start_ms, request.timeout_ms);
    if (!locked) return fail_after_power(locked.error());
    if (!locked.value())
        return fail_after_power(Error::TIMEOUT);

    const std::uint32_t settle_after_lock_ms =
        backend_.lock_settle_ms(receiver, request.system);
    if (settle_after_lock_ms != 0U) {
        const std::uint64_t now = clock_.monotonic_ms();
        const std::uint64_t elapsed_total = now - start_ms;
        if (elapsed_total >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);

        const std::uint64_t lock_poll_elapsed = now - lock_poll_start_ms;
        if (lock_poll_elapsed < settle_after_lock_ms) {
            const std::uint32_t settle_ms = static_cast<std::uint32_t>(
                settle_after_lock_ms - lock_poll_elapsed);
            const std::uint32_t remaining = static_cast<std::uint32_t>(
                request.timeout_ms - elapsed_total);
            if (remaining <= settle_ms)
                return fail_after_power(Error::TIMEOUT);
            clock_.sleep_ms(settle_ms);
        }
    }

    if (request.system == ReceiverSystem::satellite && !select_before_tune) {
        const std::uint64_t elapsed = clock_.monotonic_ms() - start_ms;
        if (elapsed >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);
        const auto remaining = static_cast<std::uint32_t>(
            request.timeout_ms - elapsed);
        const auto selected = select_satellite_stream(remaining);
        if (!selected)
            return fail_after_power(selected.error());
        if (clock_.monotonic_ms() - start_ms >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);
    }

    const auto committed = backend_.commit_tune_power(receiver);
    if (!committed) return fail_after_power(committed.error());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // receiver_lock keeps this lease in place since it was copied above.
        leases_[slot].system = request.system;
        // A successful tune re-arms a lease whose previous stream ended:
        // STOP leaves the lease consumed, and a new stream start must be
        // possible again without release/acquire.  The attach token stays the
        // one issued at acquire; its validity is per arm.
        if (leases_[slot].stream_state == StreamState::consumed)
            leases_[slot].stream_state = StreamState::none;
        set_state_locked(slot, ReceiverState::tuned);
    }
    return Result<void>::success();
}

Result<void> ReceiverEngine::start_stream(std::uint64_t client_id,
                                          std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (states_[lease.slot] != ReceiverState::tuned)
        return Result<void>::failure(Error::NOT_READY);
    if (lease.stream_state != StreamState::none)
        return Result<void>::failure(Error::BUSY);
    lease.attach_started_ms = clock_.monotonic_ms();
    lease.stream_state = StreamState::armed;
    return Result<void>::success();
}

Result<Attachment> ReceiverEngine::attach_stream(std::uint64_t lease_id,
                                                 const ReceiverToken& token) noexcept
{
    if (lease_id == 0U)
        return Result<Attachment>::failure(Error::NOT_FOUND);

    std::size_t slot = 0U;
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int index = -1;
        for (std::size_t candidate = 0U; candidate < leases_.size(); ++candidate) {
            const Lease& lease = leases_[candidate];
            if (lease.active && lease.lease_id == lease_id && lease.token == token) {
                index = static_cast<int>(candidate);
                break;
            }
        }
        if (index < 0) return Result<Attachment>::failure(Error::NOT_FOUND);
        Lease& lease = leases_[static_cast<std::size_t>(index)];
        slot = lease.slot;
    }

    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    std::uint64_t attachment_id = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int index = -1;
        for (std::size_t candidate = 0U; candidate < leases_.size(); ++candidate) {
            const Lease& lease = leases_[candidate];
            if (lease.active && lease.lease_id == lease_id && lease.token == token) {
                index = static_cast<int>(candidate);
                break;
            }
        }
        if (index < 0) return Result<Attachment>::failure(Error::NOT_FOUND);
        Lease& lease = leases_[static_cast<std::size_t>(index)];
        if (lease.slot != slot || lease.stream_state != StreamState::armed)
            return Result<Attachment>::failure(
                lease.stream_state == StreamState::none
                    ? Error::NOT_READY : Error::NOT_FOUND);
        // Unsigned subtraction is defined modulo 2^64 and therefore remains
        // correct when a monotonic counter wraps during the five-second arm.
        if (clock_.monotonic_ms() - lease.attach_started_ms >= 5000U) {
            lease.stream_state = StreamState::consumed;
            return Result<Attachment>::failure(Error::TIMEOUT);
        }
        const auto allocated = allocate_attachment_id_locked(slot);
        if (!allocated)
            return Result<Attachment>::failure(allocated.error());
        attachment_id = allocated.value();
        lease.stream_state = StreamState::attaching;
        pending = lease;
    }
    const ReceiverId receiver = receivers_[slot];
    const Attachment pending_attachment = attachment_for(pending, attachment_id);
    const auto started = backend_.start_capture(receiver, pending.system);
    observe_disconnect(slot, started.error());
    bool data_attached = false;
    Error data_error = Error::OK;
    if (started && stream_control_ != nullptr) {
        const auto attached = stream_control_->attach(pending_attachment);
        observe_disconnect(slot, attached.error());
        if (!attached) {
            data_error = attached.error();
        } else {
            data_attached = true;
        }
    }
    if (!started || data_error != Error::OK) {
        Error result_error = !started ? started.error() : data_error;
        bool data_maybe_attached = false;
        pending.capture_maybe_active = static_cast<bool>(started);
        pending.backend_disconnected = started.error() == Error::DISCONNECTED ||
                                       data_error == Error::DISCONNECTED;
        if (data_attached || (started && stream_control_ != nullptr)) {
            const auto detached = stream_control_->detach(pending_attachment);
            observe_disconnect(slot, detached.error());
            data_maybe_attached = !detached && detached.error() != Error::NOT_FOUND;
            if (detached.error() == Error::DISCONNECTED)
                pending.backend_disconnected = true;
        }
        if (!pending.backend_disconnected) {
            const auto stopped = backend_.stop_capture(receiver, pending.system);
            observe_disconnect(slot, stopped.error());
            pending.backend_disconnected = stopped.error() == Error::DISCONNECTED;
            pending.capture_maybe_active = !stopped;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            // Keep the identity when compensating detach failed.  The
            // subsequent lease release must retry that exact data session.
            lease.attachment_id = data_maybe_attached ? attachment_id : 0U;
            lease.capture_maybe_active = pending.capture_maybe_active;
            lease.stream_maybe_attached = data_maybe_attached;
            lease.backend_disconnected = pending.backend_disconnected;
            if (pending.capture_maybe_active || data_maybe_attached ||
                pending.backend_disconnected)
                set_state_locked(slot, ReceiverState::error);
        }
        return Result<Attachment>::failure(result_error);
    }

    bool attach_accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            if (lease.stream_state == StreamState::attaching) {
                lease.stream_state = StreamState::active;
                lease.attachment_id = attachment_id;
                lease.capture_maybe_active = true;
                lease.stream_maybe_attached = data_attached;
                set_state_locked(slot, ReceiverState::streaming);
                attach_accepted = true;
            }
        }
    }
    if (attach_accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const Lease& lease = leases_[slot];
        return Result<Attachment>::success(attachment_for(lease, lease.attachment_id));
    }

    // revoke_stream() won the race while start_capture was in flight.  The
    // physical start must be compensated before ATTACH can complete.
    Error result_error = Error::NOT_FOUND;
    const auto detached = data_attached && stream_control_ != nullptr
        ? stream_control_->detach(pending_attachment)
        : Result<void>::success();
    observe_disconnect(slot, detached.error());
    const bool data_maybe_attached =
        !detached && detached.error() != Error::NOT_FOUND;
    if (!detached && detached.error() != Error::NOT_FOUND)
        result_error = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receiver, pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && result_error == Error::NOT_FOUND) result_error = stopped.error();
    if (stopped.error() == Error::DISCONNECTED) result_error = Error::DISCONNECTED;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = data_maybe_attached ? attachment_id : 0U;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = data_maybe_attached;
            lease.backend_disconnected = stopped.error() == Error::DISCONNECTED;
            set_state_locked(slot, stopped && !data_maybe_attached
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return Result<Attachment>::failure(result_error);
}

Result<StreamFinalSnapshot> ReceiverEngine::stop_stream(
    std::uint64_t client_id, std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
    std::size_t slot = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
        pending = leases_[static_cast<std::size_t>(index)];
        if (pending.slot != slot ||
            (pending.stream_state != StreamState::active &&
             pending.stream_state != StreamState::revoked) ||
            !pending.capture_maybe_active)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_READY);
    }
    const Attachment attachment = attachment_for(pending, pending.attachment_id);
    const auto detached = detach_data_plane_if_needed(pending);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    Error first = Error::OK;
    if (!detach_clean) first = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receivers_[slot], pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && first == Error::OK) first = stopped.error();
    StreamFinalSnapshot final_snapshot{};
    bool snapshot_disconnected = false;
    if (detach_clean && stream_control_ != nullptr &&
        pending.stream_maybe_attached) {
        const auto snapshot = stream_control_->final_snapshot(attachment);
        if (snapshot) {
            final_snapshot = snapshot.value();
            snapshot_disconnected = final_snapshot.terminal == kStreamTerminalDisconnected;
            if (snapshot_disconnected)
                observe_disconnect(slot, Error::DISCONNECTED);
        } else {
            snapshot_disconnected = snapshot.error() == Error::DISCONNECTED;
            observe_disconnect(slot, snapshot.error());
            if (first == Error::OK) first = snapshot.error();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = detach_clean ? 0U : pending.attachment_id;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = !detach_clean;
            lease.backend_disconnected = detached.error() == Error::DISCONNECTED ||
                                         stopped.error() == Error::DISCONNECTED ||
                                         snapshot_disconnected;
            set_state_locked(slot, stopped && detach_clean && !snapshot_disconnected
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return first == Error::OK
        ? Result<StreamFinalSnapshot>::success(final_snapshot)
        : Result<StreamFinalSnapshot>::failure(first);
}

Result<StreamCounters> ReceiverEngine::stats(std::uint64_t client_id,
                                             std::uint64_t lease_id) const noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<StreamCounters>::failure(Error::NOT_FOUND);
    std::size_t slot = 0U;
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamCounters>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        slot = lease.slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamCounters>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        if (lease.slot != slot || lease.stream_state != StreamState::active ||
            !lease.capture_maybe_active)
            return Result<StreamCounters>::failure(Error::NOT_READY);
        if (stream_control_ == nullptr || !lease.stream_maybe_attached)
            return Result<StreamCounters>::failure(Error::UNSUPPORTED);
    }
    return stream_control_->stats(attachment_for(lease, lease.attachment_id));
}

Result<void> ReceiverEngine::detach_stream(const Attachment& attachment) noexcept
{
    const int found = find_slot(attachment.receiver);
    if (attachment.owner_client_id == 0U || attachment.lease_id == 0U ||
        attachment.attachment_id == 0U || found < 0)
        return Result<void>::failure(Error::NOT_FOUND);
    const auto slot = static_cast<std::size_t>(found);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Lease& lease = leases_[slot];
        if (!lease.active || lease.client_id != attachment.owner_client_id ||
            lease.lease_id != attachment.lease_id ||
            lease.attachment_id != attachment.attachment_id ||
            lease.system != attachment.system || lease.token != attachment.token ||
            (lease.stream_state != StreamState::active &&
             lease.stream_state != StreamState::revoked))
            return Result<void>::failure(Error::NOT_FOUND);
        leases_[slot].stream_state = StreamState::revoked;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Lease& lease = leases_[slot];
        if (!lease.active || lease.client_id != attachment.owner_client_id ||
            lease.lease_id != attachment.lease_id ||
            lease.attachment_id != attachment.attachment_id ||
            lease.system != attachment.system || lease.token != attachment.token ||
            (lease.stream_state != StreamState::active &&
             lease.stream_state != StreamState::revoked))
            return Result<void>::failure(Error::NOT_FOUND);
        pending = lease;
    }
    const auto detached = detach_data_plane_if_needed(pending);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    Error first = Error::OK;
    if (!detach_clean) first = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receivers_[slot], pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && first == Error::OK) first = stopped.error();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(attachment.owner_client_id,
                                            attachment.lease_id);
        if (index >= 0 && leases_[static_cast<std::size_t>(index)].attachment_id ==
                              attachment.attachment_id) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = detach_clean ? 0U : attachment.attachment_id;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = !detach_clean;
            lease.backend_disconnected = detached.error() == Error::DISCONNECTED ||
                                         stopped.error() == Error::DISCONNECTED;
            set_state_locked(slot, stopped && detach_clean
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::revoke_stream(std::uint64_t client_id,
                                           std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active)
        lease.stream_state = StreamState::revoked;
    return Result<void>::success();
}

Result<void> ReceiverEngine::revoke_stream(std::uint64_t client_id,
                                           std::uint64_t lease_id,
                                           const ReceiverToken& token) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (lease.token != token) return Result<void>::failure(Error::NOT_FOUND);
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active)
        lease.stream_state = StreamState::revoked;
    return Result<void>::success();
}

Result<ReceiverEngineStatus> ReceiverEngine::status() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Result<ReceiverEngineStatus>::success(
        ReceiverEngineStatus{generation_, receiver_count_, receivers_, states_});
}

Result<void> ReceiverEngine::cleanup_client(std::uint64_t client_id) noexcept
{
    if (!valid_client(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    std::array<std::uint64_t, kReceiverEngineCapacity> lease_ids{};
    // Revoke the complete connection atomically before any receiver I/O.  A
    // slow cleanup on one bridge must not leave another lease attachable.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t slot = 0U; slot < leases_.size(); ++slot) {
            Lease& lease = leases_[slot];
            if (!lease.active || lease.client_id != client_id) continue;
            lease_ids[slot] = lease.lease_id;
            if (lease.stream_state == StreamState::armed ||
                lease.stream_state == StreamState::attaching ||
                lease.stream_state == StreamState::active)
                lease.stream_state = StreamState::revoked;
        }
    }
    Error first = Error::OK;
    for (std::size_t slot = 0U; slot < lease_ids.size(); ++slot) {
        if (lease_ids[slot] == 0U) continue;
        const auto result = release_lease(client_id, lease_ids[slot]);
        if (first == Error::OK && !result) first = result.error();
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::disconnect_client(std::uint64_t client_id) noexcept
{
    return cleanup_client(client_id);
}

Result<void> ReceiverEngine::cleanup_all() noexcept
{
    std::array<std::uint64_t, kReceiverEngineCapacity> client_ids{};
    std::array<std::uint64_t, kReceiverEngineCapacity> lease_ids{};
    // Same two-phase rule as connection cleanup: revoke all logical stream
    // credentials before the first potentially blocking backend operation.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t slot = 0U; slot < leases_.size(); ++slot) {
            Lease& lease = leases_[slot];
            if (!lease.active) continue;
            client_ids[slot] = lease.client_id;
            lease_ids[slot] = lease.lease_id;
            if (lease.stream_state == StreamState::armed ||
                lease.stream_state == StreamState::attaching ||
                lease.stream_state == StreamState::active)
                lease.stream_state = StreamState::revoked;
        }
    }
    Error first = Error::OK;
    for (std::size_t slot = 0U; slot < client_ids.size(); ++slot) {
        if (client_ids[slot] == 0U) continue;
        const auto result = release_lease(client_ids[slot], lease_ids[slot]);
        if (first == Error::OK && !result) first = result.error();
    }
    const auto backend_shutdown = backend_.shutdown();
    if (first == Error::OK && !backend_shutdown) first = backend_shutdown.error();
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::shutdown() noexcept
{
    return cleanup_all();
}

}  // namespace dtv::userland::detail
