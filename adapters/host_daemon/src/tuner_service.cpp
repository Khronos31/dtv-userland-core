// SPDX-License-Identifier: GPL-2.0-only
#include "px4/tuner_service.h"

#include <dtv/userland/detail/client_id_allocator.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace px4::userland {
namespace {

namespace engine = dtv::userland::detail;

bool valid_legacy_client_id(std::uint64_t client_id) noexcept
{
    return engine::is_legacy_client_id(client_id);
}

// px4_drv/driver/ptx_chrdev.c:356-359, enabled by
// px4_drv/driver/px4_device.c:1258 (PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T).
// The terrestrial lock path settles for 340 ms from check_lock start.
constexpr std::uint32_t kWaitAfterLockTcTMs = 340U;
constexpr std::uint64_t kMaxRequestedHz = (std::uint64_t{1U} << 53U) - 1U;

static_assert(ipc::kNonceLength == engine::kReceiverTokenLength,
              "the attach nonce is carried as the engine token");
static_assert(ipc::kReceiverCount <= engine::kReceiverEngineCapacity,
              "every legacy receiver needs an engine slot");

// ISDB_T and ISDB_S map to the engine systems.  Any other value (the
// capability-only ISDB_T_OR_S or an unknown wire value) keeps its number, so
// backend queries and identity comparisons see exactly the legacy input; the
// engine rejects it through request validation before acting on it.
engine::ReceiverSystem to_engine(ipc::System system) noexcept
{
    switch (system) {
    case ipc::System::ISDB_T:
        return engine::ReceiverSystem::terrestrial;
    case ipc::System::ISDB_S:
        return engine::ReceiverSystem::satellite;
    default:
        return static_cast<engine::ReceiverSystem>(static_cast<std::uint8_t>(system));
    }
}

ipc::System to_legacy(engine::ReceiverSystem system) noexcept
{
    switch (system) {
    case engine::ReceiverSystem::terrestrial:
        return ipc::System::ISDB_T;
    case engine::ReceiverSystem::satellite:
        return ipc::System::ISDB_S;
    default:
        return static_cast<ipc::System>(static_cast<std::uint8_t>(system));
    }
}

ipc::ReceiverState to_legacy(engine::ReceiverState state) noexcept
{
    switch (state) {
    case engine::ReceiverState::free:
        return ipc::ReceiverState::free;
    case engine::ReceiverState::leased:
        return ipc::ReceiverState::leased;
    case engine::ReceiverState::tuned:
        return ipc::ReceiverState::tuned;
    case engine::ReceiverState::streaming:
        return ipc::ReceiverState::streaming;
    case engine::ReceiverState::error:
        break;
    }
    return ipc::ReceiverState::error;
}

// Legacy receiver numbers are the engine IDs 0..7, so the narrowing is exact.
std::uint8_t to_legacy(engine::ReceiverId receiver) noexcept
{
    return static_cast<std::uint8_t>(receiver);
}

TunerAttachment to_legacy(const engine::Attachment& attachment) noexcept
{
    return TunerAttachment{attachment.owner_client_id, attachment.lease_id,
                           attachment.attachment_id, to_legacy(attachment.receiver),
                           to_legacy(attachment.system), attachment.token,
                           attachment.evict_oldest};
}

engine::Attachment to_engine(const TunerAttachment& attachment) noexcept
{
    return engine::Attachment{attachment.owner_client_id, attachment.lease_id,
                              attachment.attachment_id, attachment.receiver,
                              to_engine(attachment.system), attachment.nonce,
                              attachment.evict_oldest};
}

TunerStreamCounters to_legacy(const engine::StreamCounters& counters) noexcept
{
    return TunerStreamCounters{counters.packets, counters.bytes, counters.sync_errors,
                               counters.tei_packets, counters.continuity_errors,
                               counters.queue_drops, counters.usb_errors,
                               counters.empty_intervals, counters.availability};
}

engine::StreamCounters to_engine(const TunerStreamCounters& counters) noexcept
{
    return engine::StreamCounters{counters.packets, counters.bytes, counters.sync_errors,
                                  counters.tei_packets, counters.continuity_errors,
                                  counters.queue_drops, counters.usb_errors,
                                  counters.empty_intervals, counters.availability};
}

std::array<engine::ReceiverId, ipc::kReceiverCount> legacy_receiver_ids() noexcept
{
    std::array<engine::ReceiverId, ipc::kReceiverCount> receivers{};
    for (std::size_t receiver = 0U; receiver < receivers.size(); ++receiver)
        receivers[receiver] = static_cast<engine::ReceiverId>(receiver);
    return receivers;
}

// A native profile must describe a nonempty bounded domain and well-formed
// rows; anything else is a configuration error, not a support claim.
bool valid_strict_profile(const TunerStrictProfile& profile) noexcept
{
    if (!profile.native_hz) return true;
    if (profile.min_hz == 0U || profile.min_hz > profile.max_hz ||
        profile.max_hz > kMaxRequestedHz || profile.row_sets > profile.rows.size() ||
        (profile.lnb_voltage_mask & static_cast<std::uint8_t>(~0x03U)) != 0U)
        return false;
    for (std::size_t index = 0U; index < profile.row_sets; ++index) {
        const TunerFrequencyRows& rows = profile.rows[index];
        if (rows.count == 0U || (rows.count > 1U && rows.step_hz == 0U)) return false;
    }
    return true;
}

// Exact frequency membership in the owned profile facts: inclusive range,
// then exact row membership when rows are declared (no tolerance/rounding).
bool profile_admits_frequency(const TunerStrictProfile& profile, std::uint64_t hz) noexcept
{
    if (hz < profile.min_hz || hz > profile.max_hz) return false;
    if (profile.row_sets == 0U) return true;
    for (std::size_t index = 0U; index < profile.row_sets; ++index) {
        const TunerFrequencyRows& rows = profile.rows[index];
        if (hz < rows.first_hz) continue;
        const std::uint64_t delta = hz - rows.first_hz;
        if (rows.step_hz == 0U) {
            if (delta == 0U) return true;
            continue;
        }
        if (delta % rows.step_hz == 0U && delta / rows.step_hz < rows.count) return true;
    }
    return false;
}

// Satellite selection limits of the profile: slot 0..slot_count-1 or TSID
// 1..65534 (0xffff marks the unused field).
bool profile_admits_selection(const TunerStrictProfile& profile, std::uint16_t stream_id,
                              std::uint16_t slot) noexcept
{
    if (slot != 0xffffU) return slot < profile.slot_count;
    return stream_id != 0U && stream_id != 0xffffU;
}

}  // namespace

bool TunerService::BackendAdapter::supports(engine::ReceiverId receiver,
                                            engine::ReceiverSystem system) const noexcept
{
    return backend_.receiver_supports(to_legacy(receiver), to_legacy(system));
}

bool TunerService::BackendAdapter::selects_satellite_stream_before_tune() const noexcept
{
    return backend_.selects_satellite_stream_before_tune();
}

std::uint32_t TunerService::BackendAdapter::lock_settle_ms(
    engine::ReceiverId receiver, engine::ReceiverSystem system) const noexcept
{
    (void)receiver;
    // The post-lock settle is a px4_drv device policy for the terrestrial
    // path only; backends opt in explicitly.
    return system == engine::ReceiverSystem::terrestrial &&
                   backend_.requires_terrestrial_lock_settle()
        ? kWaitAfterLockTcTMs
        : 0U;
}

Result<void> TunerService::BackendAdapter::open(engine::ReceiverId receiver) noexcept
{
    return backend_.open_receiver(to_legacy(receiver));
}

Result<void> TunerService::BackendAdapter::tune(engine::ReceiverId receiver,
                                                engine::ReceiverSystem system,
                                                std::uint32_t frequency_khz,
                                                std::uint32_t timeout_ms) noexcept
{
    return system == engine::ReceiverSystem::terrestrial
        ? backend_.tune_terrestrial(to_legacy(receiver), frequency_khz, timeout_ms)
        : backend_.tune_satellite(to_legacy(receiver), frequency_khz, timeout_ms);
}

Result<engine::TuneProgramming> TunerService::BackendAdapter::program_hz(
    engine::ReceiverId receiver, engine::ReceiverSystem system, std::uint64_t requested_hz,
    std::uint32_t timeout_ms) noexcept
{
    // Exact Hz forwarding; no unit conversion happens in this adapter.
    return system == engine::ReceiverSystem::terrestrial
        ? backend_.tune_terrestrial_hz(to_legacy(receiver), requested_hz, timeout_ms)
        : backend_.tune_satellite_hz(to_legacy(receiver), requested_hz, timeout_ms);
}

Result<bool> TunerService::BackendAdapter::is_locked(
    engine::ReceiverId receiver, engine::ReceiverSystem system) noexcept
{
    return backend_.is_locked(to_legacy(receiver), to_legacy(system));
}

Result<void> TunerService::BackendAdapter::select_satellite_slot(
    engine::ReceiverId receiver, std::uint8_t slot, std::uint32_t timeout_ms) noexcept
{
    return backend_.select_satellite_slot(to_legacy(receiver), slot, timeout_ms);
}

Result<void> TunerService::BackendAdapter::select_satellite_tsid(
    engine::ReceiverId receiver, std::uint16_t tsid, std::uint32_t timeout_ms) noexcept
{
    return backend_.select_satellite_tsid(to_legacy(receiver), tsid, timeout_ms);
}

Result<void> TunerService::BackendAdapter::close(engine::ReceiverId receiver) noexcept
{
    return backend_.close_receiver(to_legacy(receiver));
}

Result<void> TunerService::BackendAdapter::begin_tune_power(
    engine::ReceiverId receiver, engine::ReceiverSystem system,
    std::uint8_t lnb_voltage) noexcept
{
    return backend_.begin_tune_power(to_legacy(receiver), to_legacy(system), lnb_voltage);
}

Result<void> TunerService::BackendAdapter::commit_tune_power(
    engine::ReceiverId receiver) noexcept
{
    return backend_.commit_tune_power(to_legacy(receiver));
}

Result<void> TunerService::BackendAdapter::rollback_tune_power(
    engine::ReceiverId receiver) noexcept
{
    return backend_.rollback_tune_power(to_legacy(receiver));
}

void TunerService::BackendAdapter::mark_disconnected(engine::ReceiverId receiver) noexcept
{
    backend_.mark_receiver_disconnected(to_legacy(receiver));
}

Result<void> TunerService::BackendAdapter::shutdown() noexcept
{
    return backend_.shutdown();
}

Result<void> TunerService::BackendAdapter::set_independent_lnb_power(bool enabled) noexcept
{
    return backend_.set_independent_lnb_power(enabled);
}

dtv::userland::wire::Optional<bool>
TunerService::BackendAdapter::independent_lnb_power_state() noexcept
{
    return backend_.independent_lnb_power_state();
}

Result<void> TunerService::BackendAdapter::start_capture(
    engine::ReceiverId receiver, engine::ReceiverSystem system) noexcept
{
    return backend_.start_capture(to_legacy(receiver), to_legacy(system));
}

Result<void> TunerService::BackendAdapter::stop_capture(
    engine::ReceiverId receiver, engine::ReceiverSystem system) noexcept
{
    return backend_.stop_capture(to_legacy(receiver), to_legacy(system));
}

Result<engine::ReceiverToken> TunerService::NonceAdapter::generate() noexcept
{
    const auto nonce = source_.generate();
    if (!nonce) return Result<engine::ReceiverToken>::failure(nonce.error());
    return Result<engine::ReceiverToken>::success(nonce.value());
}

Result<std::uint64_t> TunerService::LeaseIdAdapter::next() noexcept
{
    return source_->next();
}

Result<std::uint64_t> TunerService::AttachmentIdAdapter::next() noexcept
{
    return source_->next();
}

std::uint64_t TunerService::TimeAdapter::monotonic_ms() noexcept
{
    return time_.monotonic_ms();
}

void TunerService::TimeAdapter::sleep_ms(std::uint32_t milliseconds) noexcept
{
    time_.sleep_ms(milliseconds);
}

Result<void> TunerService::StreamAdapter::attach(
    const engine::Attachment& attachment) noexcept
{
    return stream_->attach(to_legacy(attachment));
}

Result<void> TunerService::StreamAdapter::detach(
    const engine::Attachment& attachment) noexcept
{
    return stream_->detach(to_legacy(attachment));
}

Result<engine::StreamCounters> TunerService::StreamAdapter::stats(
    const engine::Attachment& attachment) const noexcept
{
    const auto counters = stream_->stats(to_legacy(attachment));
    if (!counters) return Result<engine::StreamCounters>::failure(counters.error());
    return Result<engine::StreamCounters>::success(to_engine(counters.value()));
}

Result<engine::StreamFinalSnapshot> TunerService::StreamAdapter::final_snapshot(
    const engine::Attachment& attachment) const noexcept
{
    const auto snapshot = stream_->final_snapshot(to_legacy(attachment));
    if (!snapshot)
        return Result<engine::StreamFinalSnapshot>::failure(snapshot.error());
    return Result<engine::StreamFinalSnapshot>::success(engine::StreamFinalSnapshot{
        to_engine(snapshot.value().counters), snapshot.value().terminal});
}

Result<engine::StreamReadResult> TunerService::StreamAdapter::read(
    const engine::Attachment& attachment, MutableByteView output,
    Timeout timeout) noexcept
{
    const auto result = stream_->read(to_legacy(attachment), output, timeout);
    if (!result) return Result<engine::StreamReadResult>::failure(result.error());
    return Result<engine::StreamReadResult>::success(engine::StreamReadResult{
        result.value().bytes, result.value().eof, result.value().timed_out,
        static_cast<engine::StreamTerminal>(result.value().terminal)});
}

Result<void> TunerService::StreamAdapter::release_final(
    const engine::Attachment& attachment) noexcept
{
    return stream_->release_final(to_legacy(attachment));
}

TunerService::TunerService(TunerServiceBackend& backend,
                           TunerNonceSource& nonce_source,
                           TunerServiceTime& time,
                           TunerLeaseIdSource* lease_id_source,
                           TunerAttachmentIdSource* attachment_id_source,
                           TunerStreamControl* stream_control) noexcept
    : backend_(backend), nonce_source_(nonce_source), time_(time),
      lease_id_source_(lease_id_source), attachment_id_source_(attachment_id_source),
      stream_control_(stream_control), receiver_count_(legacy_receiver_count(backend)),
      receiver_ids_(legacy_receiver_ids()),
      engine_(backend_, receiver_ids_.data(), receiver_count_, nonce_source_, time_,
              lease_id_source != nullptr ? &lease_id_source_ : nullptr,
              attachment_id_source != nullptr ? &attachment_id_source_ : nullptr,
              stream_control != nullptr ? &stream_control_ : nullptr)
{
}

TunerService::~TunerService() noexcept
{
    // engine_ is destroyed first and shuts the receivers down exactly once.
}

std::uint8_t TunerService::legacy_receiver_count(
    const TunerServiceBackend& backend) noexcept
{
    // Receivers beyond the backend count report `absent` in STATUS.
    return static_cast<std::uint8_t>(std::min<std::size_t>(
        backend.receiver_count(), ipc::kReceiverCount));
}

bool TunerService::valid_tune(const ipc::TuneRequestPayload& request) noexcept
{
    if (request.frequency_khz > std::numeric_limits<std::uint32_t>::max())
        return false;
    if (request.timeout_ms < 100U || request.timeout_ms > 30000U) return false;

    if (request.system == ipc::System::ISDB_T) {
        return request.frequency_khz >= 40000U &&
               request.frequency_khz <= 1002000U &&
               request.stream_id == 0xffffU && request.slot == 0xffffU &&
               request.bandwidth_hz == 6000000U && request.lnb_voltage == 0U;
    }
    if (request.system != ipc::System::ISDB_S || request.frequency_khz < 146875U ||
        request.frequency_khz > 2350000U || request.bandwidth_hz != 0U)
        return false;
    if (request.lnb_voltage != 0U && request.lnb_voltage != 15U) return false;
    const bool has_stream_id = request.stream_id != 0xffffU;
    const bool has_slot = request.slot != 0xffffU;
    return has_stream_id != has_slot && (!has_slot || request.slot < 12U);
}

Result<TunerAcquireResult> TunerService::acquire(std::uint64_t client_id,
                                                 std::uint8_t receiver) noexcept
{
    // The backend count is read on each ACQUIRE, as before the extraction.
    if (!valid_legacy_client_id(client_id) || receiver >= ipc::kReceiverCount ||
        receiver >= backend_.legacy().receiver_count())
        return Result<TunerAcquireResult>::failure(Error::INVALID_ARGUMENT);
    const auto acquired = engine_.acquire(client_id, receiver);
    if (!acquired) return Result<TunerAcquireResult>::failure(acquired.error());
    return Result<TunerAcquireResult>::success(
        TunerAcquireResult{acquired.value().lease_id, acquired.value().token});
}

Result<void> TunerService::release(std::uint64_t client_id,
                                   std::uint64_t lease_id) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.release(client_id, lease_id);
}

Result<ipc::TuneResponsePayload> TunerService::tune(
    std::uint64_t client_id, const ipc::TuneRequestPayload& request) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<ipc::TuneResponsePayload>::failure(Error::INVALID_ARGUMENT);
    engine::TuneRequest neutral{};
    neutral.lease_id = request.lease_id;
    neutral.system = to_engine(request.system);
    // A frequency above 32 bits is invalid as a legacy request; the engine
    // returns that decision before the truncated value could be used.
    neutral.frequency_khz = static_cast<std::uint32_t>(request.frequency_khz);
    neutral.stream_id = request.stream_id;
    neutral.slot = request.slot;
    neutral.lnb_voltage = request.lnb_voltage;
    neutral.timeout_ms = request.timeout_ms;
    neutral.validation = valid_tune(request) ? Error::OK : Error::INVALID_ARGUMENT;
    const auto tuned = engine_.tune(client_id, neutral);
    if (!tuned) return Result<ipc::TuneResponsePayload>::failure(tuned.error());
    return Result<ipc::TuneResponsePayload>::success(
        ipc::TuneResponsePayload{1U, std::numeric_limits<std::int32_t>::min()});
}

Result<void> TunerService::start_stream(std::uint64_t client_id,
                                        std::uint64_t lease_id) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.start_stream(client_id, lease_id);
}

Result<TunerAttachment> TunerService::attach_stream(
    std::uint64_t lease_id,
    const std::array<std::uint8_t, ipc::kNonceLength>& nonce) noexcept
{
    const auto attached = engine_.attach_stream(lease_id, nonce);
    if (!attached) return Result<TunerAttachment>::failure(attached.error());
    return Result<TunerAttachment>::success(to_legacy(attached.value()));
}

Result<TunerStreamFinalSnapshot> TunerService::stop_stream(
    std::uint64_t client_id, std::uint64_t lease_id) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamFinalSnapshot>::failure(Error::INVALID_ARGUMENT);
    const auto stopped = engine_.stop_stream(client_id, lease_id);
    if (!stopped) return Result<TunerStreamFinalSnapshot>::failure(stopped.error());
    return Result<TunerStreamFinalSnapshot>::success(TunerStreamFinalSnapshot{
        to_legacy(stopped.value().counters), stopped.value().terminal});
}

Result<TunerStreamCounters> TunerService::stats(
    std::uint64_t client_id, std::uint64_t lease_id) const noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamCounters>::failure(Error::INVALID_ARGUMENT);
    const auto counters = engine_.stats(client_id, lease_id);
    if (!counters) return Result<TunerStreamCounters>::failure(counters.error());
    return Result<TunerStreamCounters>::success(to_legacy(counters.value()));
}

Result<void> TunerService::detach_stream(
    const TunerAttachment& attachment) noexcept
{
    if (!valid_legacy_client_id(attachment.owner_client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.detach_stream(to_engine(attachment));
}

Result<void> TunerService::revoke_stream(std::uint64_t client_id,
                                         std::uint64_t lease_id) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.revoke_stream(client_id, lease_id);
}

Result<void> TunerService::revoke_stream(
    std::uint64_t client_id, std::uint64_t lease_id,
    const std::array<std::uint8_t, ipc::kNonceLength>& nonce) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.revoke_stream(client_id, lease_id, nonce);
}

Result<TunerStatus> TunerService::status() const noexcept
{
    const auto status = engine_.status();
    if (!status) return Result<TunerStatus>::failure(status.error());
    TunerStatus legacy{};
    legacy.generation = status.value().generation;
    for (std::size_t receiver = 0U; receiver < legacy.receiver_states.size(); ++receiver) {
        legacy.receiver_states[receiver] = receiver < status.value().receiver_count
            ? to_legacy(status.value().states[receiver])
            : ipc::ReceiverState::absent;
    }
    return Result<TunerStatus>::success(legacy);
}

Result<void> TunerService::disconnect_client(std::uint64_t client_id) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.disconnect_client(client_id);
}

Result<void> TunerService::shutdown() noexcept
{
    return engine_.shutdown();
}

Result<void> TunerService::configure_strict_capabilities() noexcept
{
    // Build the immutable descriptor from the audited backend metadata once,
    // outside all engine and worker locks.  The fixed legacy receiver IDs are
    // the engine slot IDs 0..receiver_count_-1.
    std::array<engine::ReceiverCapability, ipc::kReceiverCount> capabilities{};
    std::array<std::array<TunerStrictProfile, 2U>, ipc::kReceiverCount> profiles{};
    for (std::size_t index = 0U; index < receiver_count_; ++index) {
        const std::uint8_t receiver = to_legacy(receiver_ids_[index]);
        capabilities[index].receiver = receiver_ids_[index];
        capabilities[index].supports_terrestrial =
            backend_.legacy().receiver_supports(receiver, ipc::System::ISDB_T);
        capabilities[index].supports_satellite =
            backend_.legacy().receiver_supports(receiver, ipc::System::ISDB_S);
        // R3 profile facts are copied here, once, outside every lock.  The
        // native Hz entrance requires a native profile for every supported
        // system of the receiver.
        bool native = capabilities[index].supports_terrestrial ||
                      capabilities[index].supports_satellite;
        if (capabilities[index].supports_terrestrial) {
            profiles[receiver][0] =
                backend_.legacy().strict_profile(receiver, ipc::System::ISDB_T);
            native = native && profiles[receiver][0].native_hz;
        }
        if (capabilities[index].supports_satellite) {
            profiles[receiver][1] =
                backend_.legacy().strict_profile(receiver, ipc::System::ISDB_S);
            native = native && profiles[receiver][1].native_hz;
        }
        if (!valid_strict_profile(profiles[receiver][0]) ||
            !valid_strict_profile(profiles[receiver][1]))
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        // Carry the backend's readiness policy through the facade.  The
        // default (lock_poll) reproduces the pre-seam behaviour.
        capabilities[index].readiness =
            (capabilities[index].supports_terrestrial ? profiles[receiver][0]
                                                      : profiles[receiver][1])
                .readiness;
        if (!native) profiles[receiver] = {};
        capabilities[index].native_hz = native;
    }
    strict_profiles_ = profiles;
    const auto configured =
        engine_.configure_strict_capabilities(capabilities.data(), receiver_count_);
    if (!configured) strict_profiles_ = {};
    return configured;
}

const TunerStrictProfile* TunerService::strict_profile_for(
    std::uint8_t receiver, ipc::System system) const noexcept
{
    if (receiver >= receiver_count_) return nullptr;
    if (system == ipc::System::ISDB_T) return &strict_profiles_[receiver][0];
    if (system == ipc::System::ISDB_S) return &strict_profiles_[receiver][1];
    return nullptr;
}

TunerPreparedTune TunerService::prepare_tune(
    std::uint8_t receiver, const ipc::TuneRequestPayload& request) const noexcept
{
    TunerPreparedTune prepared{};
    const Error decision = valid_tune(request) ? Error::OK : Error::INVALID_ARGUMENT;
    const TunerStrictProfile* profile = strict_profile_for(receiver, request.system);
    if (profile == nullptr || !profile->native_hz) {
        // No native Hz profile: exactly the R2 kHz admission request.
        engine::TuneRequest& legacy = prepared.legacy;
        legacy.lease_id = request.lease_id;
        legacy.system = to_engine(request.system);
        legacy.frequency_khz = static_cast<std::uint32_t>(request.frequency_khz);
        legacy.stream_id = request.stream_id;
        legacy.slot = request.slot;
        legacy.lnb_voltage = request.lnb_voltage;
        legacy.timeout_ms = request.timeout_ms;
        legacy.validation = decision;
        return prepared;
    }
    prepared.hz_entrance = true;
    engine::HzTuneRequest& hz = prepared.hz;
    hz.lease_id = request.lease_id;
    hz.system = to_engine(request.system);
    // The old-wire scalar is u64 kHz: check it before multiplying; nothing
    // is narrowed first.  An out-of-range value is already invalid above.
    hz.requested_hz = request.frequency_khz <= kMaxRequestedHz / 1000U
        ? request.frequency_khz * 1000U
        : 0U;
    hz.stream_id = request.stream_id;
    hz.slot = request.slot;
    hz.lnb_voltage = request.lnb_voltage;
    hz.timeout_ms = request.timeout_ms;
    if (decision != Error::OK) {
        hz.validation = decision;
    } else if (!profile_admits_frequency(*profile, hz.requested_hz) ||
               (request.system == ipc::System::ISDB_S &&
                !profile_admits_selection(*profile, request.stream_id, request.slot))) {
        hz.validation = Error::UNSUPPORTED;
    }
    return prepared;
}

TunerPreparedTune TunerService::prepare_tune_hz(
    std::uint8_t receiver, const TunerHzTuneRequest& request) const noexcept
{
    using dtv::userland::wire::Selection;
    TunerPreparedTune prepared{};
    prepared.hz_entrance = true;
    engine::HzTuneRequest& hz = prepared.hz;
    hz.lease_id = request.lease_id;
    hz.system = to_engine(request.system);
    hz.requested_hz = request.frequency_hz;
    hz.timeout_ms = request.timeout_ms;
    hz.lnb_voltage = request.lnb_voltage.present ? request.lnb_voltage.value : 0U;
    const bool satellite = request.system == ipc::System::ISDB_S;
    if (satellite && request.selection == Selection::tsid) hz.stream_id = request.selection_value;
    if (satellite && request.selection == Selection::slot) hz.slot = request.selection_value;

    // Invalid common scalar/enum/coupling first (INVALID_ARGUMENT) ...
    Error decision = Error::OK;
    const bool known_selection = request.selection == Selection::automatic ||
                                 request.selection == Selection::tsid ||
                                 request.selection == Selection::slot;
    if (request.frequency_hz == 0U || request.frequency_hz > kMaxRequestedHz ||
        request.timeout_ms < 100U || request.timeout_ms > 30000U || !known_selection) {
        decision = Error::INVALID_ARGUMENT;
    } else if (request.system == ipc::System::ISDB_T) {
        if (request.selection != Selection::automatic || request.selection_value != 0U ||
            hz.lnb_voltage != 0U)
            decision = Error::INVALID_ARGUMENT;
    } else if (satellite) {
        if ((request.selection == Selection::automatic && request.selection_value != 0U) ||
            (request.selection != Selection::automatic && request.selection_value == 0xffffU) ||
            (hz.lnb_voltage != 0U && hz.lnb_voltage != 15U))
            decision = Error::INVALID_ARGUMENT;
    } else {
        decision = Error::INVALID_ARGUMENT;
    }
    // ... then capabilities the receiver profile does not provide.
    const TunerStrictProfile* profile = strict_profile_for(receiver, request.system);
    if (decision == Error::OK) {
        if (profile == nullptr || !profile->native_hz || request.lna.present ||
            request.pid_subset || (satellite && request.selection == Selection::automatic) ||
            (request.lnb_voltage.present &&
             (profile->lnb_voltage_mask &
              (request.lnb_voltage.value == 0U ? 0x01U : 0x02U)) == 0U) ||
            !profile_admits_frequency(*profile, request.frequency_hz) ||
            (satellite && !profile_admits_selection(*profile, hz.stream_id, hz.slot)))
            decision = Error::UNSUPPORTED;
    }
    hz.validation = decision;
    return prepared;
}

Result<TunerService::TunerAdmittedTune> TunerService::try_admit_prepared_tune(
    std::uint64_t client_id, const TunerPreparedTune& prepared,
    std::uint64_t accepted_at_ms) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerAdmittedTune>::failure(Error::INVALID_ARGUMENT);
    return prepared.hz_entrance
        ? engine_.try_admit_tune(client_id, prepared.hz, accepted_at_ms)
        : engine_.try_admit_tune(client_id, prepared.legacy, accepted_at_ms);
}

bool TunerService::strict_admission_enabled() const noexcept
{
    return engine_.strict_capabilities_configured();
}

std::uint64_t TunerService::monotonic_ms() const noexcept
{
    return const_cast<TunerService*>(this)->time_.monotonic_ms();
}

Result<TunerService::TunerAdmittedTune> TunerService::try_admit_tune(
    std::uint64_t client_id, const ipc::TuneRequestPayload& request,
    std::uint64_t accepted_at_ms) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerAdmittedTune>::failure(Error::INVALID_ARGUMENT);
    engine::TuneRequest neutral{};
    neutral.lease_id = request.lease_id;
    neutral.system = to_engine(request.system);
    neutral.frequency_khz = static_cast<std::uint32_t>(request.frequency_khz);
    neutral.stream_id = request.stream_id;
    neutral.slot = request.slot;
    neutral.lnb_voltage = request.lnb_voltage;
    neutral.timeout_ms = request.timeout_ms;
    neutral.validation = valid_tune(request) ? Error::OK : Error::INVALID_ARGUMENT;
    return engine_.try_admit_tune(client_id, neutral, accepted_at_ms);
}

Result<void> TunerService::execute_admitted_tune(
    const TunerAdmittedTune& ticket) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.execute_admitted_tune(ticket);
}

Result<void> TunerService::abort_unstarted_tune(const TunerAdmittedTune& ticket,
                                               Error error) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.abort_unstarted_tune(ticket, error);
}

Result<void> TunerService::revoke_tune_ticket(std::uint64_t client_id,
                                              std::uint64_t lease_id) noexcept
{
    if (!valid_legacy_client_id(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.revoke_tune_ticket(client_id, lease_id);
}

Result<engine::TuneMetadata> TunerService::tune_metadata(
    std::uint8_t receiver) const noexcept
{
    const auto status = engine_.status();
    if (!status) return Result<engine::TuneMetadata>::failure(status.error());
    for (std::size_t slot = 0U; slot < status.value().receiver_count; ++slot) {
        if (status.value().receivers[slot] != receiver) continue;
        return Result<engine::TuneMetadata>::success(
            status.value().tune_metadata[slot]);
    }
    return Result<engine::TuneMetadata>::failure(Error::NOT_FOUND);
}

Result<engine::ReceiverEngineStatus> TunerService::engine_status() const noexcept
{
    return engine_.status();
}

std::uint64_t TunerService::monotonic_ms() noexcept
{
    return time_.monotonic_ms();
}

bool TunerService::supports_internal_card_observation() const noexcept
{
    return backend_.legacy().supports_internal_card_observation();
}

dtv::userland::wire::Optional<bool>
TunerService::sample_internal_card_inserted() noexcept
{
    return backend_.legacy().sample_internal_card_inserted();
}

Result<void> TunerService::set_independent_lnb_power(bool enabled) noexcept
{
    return backend_.set_independent_lnb_power(enabled);
}

dtv::userland::wire::Optional<bool>
TunerService::independent_lnb_power_state() noexcept
{
    return backend_.independent_lnb_power_state();
}

bool TunerService::receiver_supports(std::uint8_t receiver,
                                     ipc::System system) const noexcept
{
    return receiver < receiver_count_ &&
           backend_.legacy().receiver_supports(receiver, system);
}

TunerStrictProfile TunerService::strict_profile(std::uint8_t receiver,
                                                ipc::System system) const noexcept
{
    const TunerStrictProfile* profile = strict_profile_for(receiver, system);
    return profile != nullptr ? *profile : TunerStrictProfile{};
}

Result<void> TunerService::configure_stream_contract(
    TunerStreamContract contract, std::uint64_t context_epoch) noexcept
{
    return engine_.configure_stream_contract(contract, context_epoch);
}

Result<TunerService::TunerStreamStart> TunerService::start_stream_v1(
    std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t revision,
    std::uint64_t now_ms, std::uint64_t arm_budget_ms) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamStart>::failure(Error::INVALID_ARGUMENT);
    return engine_.start_stream_v1(client_id, lease_id, revision, now_ms,
                                   arm_budget_ms);
}

Result<TunerService::TunerStreamTicket> TunerService::reserve_attach_v1(
    std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t epoch,
    std::uint64_t stream_id, const dtv::userland::detail::ReceiverToken& token,
    std::uint64_t now_ms) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamTicket>::failure(Error::INVALID_ARGUMENT);
    return engine_.reserve_attach_v1(client_id, lease_id, epoch, stream_id, token,
                                     now_ms);
}

Result<void> TunerService::prepare_attach_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.prepare_attach_v1(ticket);
}

Result<void> TunerService::commit_attach_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.commit_attach_v1(ticket);
}

Result<void> TunerService::abort_attach_v1(const TunerStreamTicket& ticket,
                                           Error cause) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.abort_attach_v1(ticket, cause);
}

Result<TunerService::TunerStreamStop> TunerService::stop_stream_v1(
    std::uint64_t client_id, std::uint64_t lease_id,
    std::uint64_t stream_id) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamStop>::failure(Error::INVALID_ARGUMENT);
    return engine_.stop_stream_v1(client_id, lease_id, stream_id);
}

Result<TunerService::TunerStreamStop> TunerService::expire_armed_v1(
    std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t stream_id,
    std::uint64_t now_ms) noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamStop>::failure(Error::INVALID_ARGUMENT);
    return engine_.expire_armed_v1(client_id, lease_id, stream_id, now_ms);
}

Result<void> TunerService::run_stream_cleanup_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.run_stream_cleanup_v1(ticket);
}

Result<void> TunerService::complete_stream_cleanup_v1(
    const TunerStreamTicket& ticket, Error result, bool quiesced) noexcept
{
    if (!valid_legacy_client_id(ticket.client_id))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.complete_stream_cleanup_v1(ticket, result, quiesced);
}

Result<TunerService::TunerStreamStop> TunerService::stream_last_terminal_v1(
    std::uint64_t client_id, std::uint64_t lease_id) const noexcept
{
    if (!valid_legacy_client_id(client_id))
        return Result<TunerStreamStop>::failure(Error::INVALID_ARGUMENT);
    return engine_.stream_last_terminal_v1(client_id, lease_id);
}

Result<TunerService::TunerStreamStart> TunerService::authority_start_stream_v1(
    std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t revision,
    std::uint64_t now_ms, std::uint64_t arm_budget_ms) noexcept
{
    if ((client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamStart>::failure(Error::INVALID_ARGUMENT);
    return engine_.start_stream_v1(client_id, lease_id, revision, now_ms,
                                   arm_budget_ms);
}

Result<TunerService::TunerStreamTicket> TunerService::authority_reserve_attach_v1(
    std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t epoch,
    std::uint64_t stream_id, const dtv::userland::detail::ReceiverToken& token,
    std::uint64_t now_ms) noexcept
{
    if ((client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamTicket>::failure(Error::INVALID_ARGUMENT);
    return engine_.reserve_attach_v1(client_id, lease_id, epoch, stream_id, token,
                                     now_ms);
}

Result<TunerService::TunerStreamStop> TunerService::authority_stop_stream_v1(
    std::uint64_t client_id, std::uint64_t lease_id,
    std::uint64_t stream_id) noexcept
{
    if ((client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamStop>::failure(Error::INVALID_ARGUMENT);
    return engine_.stop_stream_v1(client_id, lease_id, stream_id);
}

Result<TunerService::TunerStreamStop> TunerService::authority_stream_last_terminal_v1(
    std::uint64_t client_id, std::uint64_t lease_id) const noexcept
{
    if ((client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamStop>::failure(Error::INVALID_ARGUMENT);
    return engine_.stream_last_terminal_v1(client_id, lease_id);
}

Result<void> TunerService::authority_prepare_attach_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.prepare_attach_v1(ticket);
}

Result<void> TunerService::authority_commit_attach_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.commit_attach_v1(ticket);
}

Result<void> TunerService::authority_abort_attach_v1(
    const TunerStreamTicket& ticket, Error cause) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.abort_attach_v1(ticket, cause);
}

Result<void> TunerService::authority_run_stream_cleanup_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.run_stream_cleanup_v1(ticket);
}

Result<TunerStreamReadResult> TunerService::authority_read_stream_v1(
    const TunerStreamTicket& ticket, MutableByteView output,
    Timeout timeout) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamReadResult>::failure(Error::INVALID_ARGUMENT);
    const auto attachment = engine_.stream_attachment_v1(ticket);
    if (!attachment) return Result<TunerStreamReadResult>::failure(attachment.error());
    const auto result = stream_control_.read(attachment.value(), output, timeout);
    if (!result) return Result<TunerStreamReadResult>::failure(result.error());
    std::uint64_t queue_drops = 0U;
    if (result.value().bytes != 0U) {
        const auto counters = stream_control_.stats(attachment.value());
        if (counters) {
            queue_drops = counters.value().queue_drops;
        } else {
            if (counters.error() != Error::NOT_FOUND)
                return Result<TunerStreamReadResult>::failure(counters.error());
            const auto final = stream_control_.final_snapshot(attachment.value());
            if (!final) return Result<TunerStreamReadResult>::failure(final.error());
            queue_drops = final.value().counters.queue_drops;
        }
    }
    return Result<TunerStreamReadResult>::success(TunerStreamReadResult{
        result.value().bytes, result.value().eof, result.value().timed_out,
        static_cast<TunerStreamTerminal>(result.value().terminal), queue_drops});
}

Result<TunerStreamCounters> TunerService::authority_stream_stats_v1(
    const TunerStreamTicket& ticket) const noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamCounters>::failure(Error::INVALID_ARGUMENT);
    const auto attachment = engine_.stream_attachment_v1(ticket);
    if (!attachment) return Result<TunerStreamCounters>::failure(attachment.error());
    const auto counters = stream_control_.stats(attachment.value());
    if (counters)
        return Result<TunerStreamCounters>::success(to_legacy(counters.value()));
    if (counters.error() != Error::NOT_FOUND)
        return Result<TunerStreamCounters>::failure(counters.error());
    const auto final = stream_control_.final_snapshot(attachment.value());
    if (!final) return Result<TunerStreamCounters>::failure(final.error());
    return Result<TunerStreamCounters>::success(to_legacy(final.value().counters));
}

Result<TunerStreamFinalSnapshot> TunerService::authority_final_snapshot_v1(
    const TunerStreamTicket& ticket) const noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<TunerStreamFinalSnapshot>::failure(Error::INVALID_ARGUMENT);
    const auto snapshot = engine_.stream_final_snapshot_v1(ticket);
    if (!snapshot)
        return Result<TunerStreamFinalSnapshot>::failure(snapshot.error());
    return Result<TunerStreamFinalSnapshot>::success(TunerStreamFinalSnapshot{
        to_legacy(snapshot.value().counters), snapshot.value().terminal});
}

Result<void> TunerService::authority_release_stream_final_v1(
    const TunerStreamTicket& ticket) noexcept
{
    if ((ticket.client_id & dtv::userland::detail::kAuthorityClientIdFirst) == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return engine_.release_stream_final_v1(ticket);
}

}  // namespace px4::userland
