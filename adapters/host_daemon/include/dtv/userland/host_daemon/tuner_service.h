// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_DAEMON_TUNER_SERVICE_H
#define DTV_USERLAND_HOST_DAEMON_TUNER_SERVICE_H

#include "dtv/userland/detail/receiver_engine.h"
#include "px4/error.h"
#include "px4/ipc.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace px4::userland {

class HostAuthorityOwner;
namespace ipc::posix { class ControlWorkerLanes; }
namespace ipc::posix { class HostAuthorityBridge; }
class HostAuthorityOwnerTestAccess;

// R3 strict frequency facts, data only.  A backend reports them per receiver
// and system; TunerService copies them once before workers start and the pure
// strict preparation evaluates only this owned copy (no backend call).
struct TunerFrequencyRows final {
    std::uint64_t first_hz = 0U;
    std::uint64_t step_hz = 0U;
    std::uint8_t count = 0U;
};

struct TunerStrictProfile final {
    // The backend implements the strict Hz programming entrance for this
    // receiver/system.  Without it the receiver keeps the R2 kHz entrance.
    bool native_hz = false;
    // Readiness policy of the native Hz entrance.  The default preserves the
    // polling behaviour of every existing product backend.
    dtv::userland::detail::TuneReadiness readiness =
        dtv::userland::detail::TuneReadiness::lock_poll;
    // Inclusive admitted requested-Hz domain (satellite: IF).
    std::uint64_t min_hz = 0U;
    std::uint64_t max_hz = 0U;
    // When row_sets is nonzero the request must equal one row exactly.
    std::array<TunerFrequencyRows, 2U> rows{};
    std::uint8_t row_sets = 0U;
    // Explicit satellite LNB controls the profile supports: bit 0 = 0 V,
    // bit 1 = 15 V. Absence remains distinct from an explicit 0 V request.
    std::uint8_t lnb_voltage_mask = 0U;
    // Satellite slot indexes 0..slot_count-1 are supported (TSID 1..65534).
    std::uint8_t slot_count = 0U;
};

// Small stack-local strict programming result (optional command Hz and
// typed evidence); the shared engine type.
using TunerHzProgramming = dtv::userland::detail::TuneProgramming;

// Internal strict Hz tune request (not a wire record).  frequency_hz is the
// exact canonical request in the system's public domain (satellite: IF).
struct TunerHzTuneRequest final {
    std::uint64_t lease_id = 0U;
    ipc::System system = ipc::System::ISDB_T;
    std::uint64_t frequency_hz = 0U;
    dtv::userland::wire::Selection selection = dtv::userland::wire::Selection::automatic;
    std::uint16_t selection_value = 0U;
    dtv::userland::wire::Optional<std::uint8_t> lnb_voltage{};
    dtv::userland::wire::Optional<bool> lna{};
    bool pid_subset = false;
    std::uint32_t timeout_ms = 0U;
};

// Owned bounded outcome of the pure strict preparation.  It is produced after
// the acceptance clock sample and before any lane lock, copied into the task,
// and consumed at the engine's validation position.  hz_entrance selects the
// engine request; it is set from the receiver's immutable profile, never from
// a numeric value.
struct TunerPreparedTune final {
    bool hz_entrance = false;
    dtv::userland::detail::TuneRequest legacy{};
    dtv::userland::detail::HzTuneRequest hz{};
};

// Portable seam for the physical receiver implementation.  It deliberately
// contains no transport, operating-system, or frontend implementation types.
class TunerServiceBackend {
public:
    virtual ~TunerServiceBackend() noexcept = default;
    // Enclosure shape.  The defaults are the fixed PX-Q3U4 layout: eight
    // receivers, 0/1/4/5 ISDB-S and 2/3/6/7 ISDB-T.  A backend whose
    // receivers accept both systems keeps the system of the last successful
    // tune for capture and stop.
    virtual std::uint8_t receiver_count() const noexcept
    {
        return static_cast<std::uint8_t>(ipc::kReceiverCount);
    }
    virtual bool receiver_supports(std::uint8_t receiver,
                                   ipc::System system) const noexcept
    {
        const bool satellite =
            receiver == 0U || receiver == 1U || receiver == 4U || receiver == 5U;
        return system == (satellite ? ipc::System::ISDB_S : ipc::System::ISDB_T);
    }
    // PX-MLT boards select the ISDB-S slot/TSID before the frontend tune
    // (px4_drv PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE); Q3U4 selects it
    // after lock.
    virtual bool selects_satellite_stream_before_tune() const noexcept
    {
        return false;
    }
    virtual bool supports_internal_card_observation() const noexcept
    {
        return false;
    }
    virtual dtv::userland::wire::Optional<bool> sample_internal_card_inserted() noexcept
    {
        return {};
    }
    virtual Result<void> open_receiver(std::uint8_t receiver) noexcept = 0;
    virtual Result<void> tune_terrestrial(std::uint8_t receiver,
                                          std::uint32_t frequency_khz,
                                          std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> tune_satellite(std::uint8_t receiver,
                                        std::uint32_t frequency_khz,
                                        std::uint32_t timeout_ms) noexcept = 0;
    // R3 strict Hz programming entrances.  frequency_hz is the exact request
    // (satellite: IF).  The defaults are UNSUPPORTED: there is no shared kHz
    // fallback.  Production backends override them with native programming.
    virtual Result<TunerHzProgramming> tune_terrestrial_hz(std::uint8_t receiver,
                                                           std::uint64_t frequency_hz,
                                                           std::uint32_t timeout_ms) noexcept
    {
        (void)receiver;
        (void)frequency_hz;
        (void)timeout_ms;
        return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    }
    virtual Result<TunerHzProgramming> tune_satellite_hz(std::uint8_t receiver,
                                                         std::uint64_t frequency_hz,
                                                         std::uint32_t timeout_ms) noexcept
    {
        (void)receiver;
        (void)frequency_hz;
        (void)timeout_ms;
        return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    }
    // Immutable strict profile facts for one receiver/system, queried once by
    // configure_strict_capabilities() before workers start.  The default has
    // no native Hz entrance.
    virtual TunerStrictProfile strict_profile(std::uint8_t receiver,
                                              ipc::System system) const noexcept
    {
        (void)receiver;
        (void)system;
        return TunerStrictProfile{};
    }
    virtual Result<bool> is_locked(std::uint8_t receiver,
                                   ipc::System system) noexcept = 0;
    // The post-lock settle is a px4_drv device policy, not a generic
    // tuner-service requirement. Production Q3U4, MLT, and one-receiver
    // backends enable it explicitly; compatibility/test backends retain
    // their existing timing contract.
    virtual bool requires_terrestrial_lock_settle() const noexcept
    {
        return false;
    }
    virtual Result<void> select_satellite_slot(std::uint8_t receiver,
                                               std::uint8_t slot,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> select_satellite_tsid(std::uint8_t receiver,
                                               std::uint16_t tsid,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> close_receiver(std::uint8_t receiver) noexcept = 0;
    // Transactional LNB seam. begin_tune_power() runs before any frontend
    // tune operation. A later failure is paired with rollback_tune_power();
    // only a fully successful tune is paired with commit_tune_power(). The
    // default keeps non-Q3U4/test backends at 0 V and rejects 15 V safely.
    virtual Result<void> begin_tune_power(std::uint8_t receiver,
                                          ipc::System system,
                                          std::uint8_t lnb_voltage) noexcept
    {
        (void)receiver;
        (void)system;
        return lnb_voltage == 0U ? Result<void>::success()
                                 : Result<void>::failure(Error::UNSUPPORTED);
    }
    virtual Result<void> commit_tune_power(std::uint8_t receiver) noexcept
    {
        (void)receiver;
        return Result<void>::success();
    }
    virtual Result<void> rollback_tune_power(std::uint8_t receiver) noexcept
    {
        (void)receiver;
        return Result<void>::success();
    }
    virtual Result<void> set_independent_lnb_power(bool enabled) noexcept
    {
        (void)enabled;
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    virtual dtv::userland::wire::Optional<bool> independent_lnb_power_state() noexcept
    {
        return {};
    }
    // Metadata-only transport-loss notification. It must not perform USB or
    // frontend I/O. Production uses it to make per-bridge power authorities
    // terminal before any later rollback/shutdown path can issue a write.
    virtual void mark_receiver_disconnected(std::uint8_t receiver) noexcept
    {
        (void)receiver;
    }
    // Called after all logical leases have been released. Production uses it
    // to retry any ambiguous zero-reference LNB shutdown before transports
    // are destroyed.
    virtual Result<void> shutdown() noexcept { return Result<void>::success(); }
    virtual void request_stop() noexcept {}
    // Capture is deliberately a per-receiver lifecycle seam.  The stream
    // transport is owned by a later increment; these calls only control the
    // frontend TS pin.
    virtual Result<void> start_capture(std::uint8_t receiver,
                                       ipc::System system) noexcept
    {
        (void)receiver;
        (void)system;
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    virtual Result<void> stop_capture(std::uint8_t receiver,
                                      ipc::System system) noexcept
    {
        (void)receiver;
        (void)system;
        return Result<void>::failure(Error::UNSUPPORTED);
    }
};

class TunerNonceSource {
public:
    virtual ~TunerNonceSource() noexcept = default;
    virtual Result<std::array<std::uint8_t, ipc::kNonceLength>> generate()
        noexcept = 0;
};

// The service uses its internal nonzero sequence by default.  This optional
// source is for deterministic platform policy and collision/wrap tests.
class TunerLeaseIdSource {
public:
    virtual ~TunerLeaseIdSource() noexcept = default;
    virtual Result<std::uint64_t> next() noexcept = 0;
};

class TunerAttachmentIdSource {
public:
    virtual ~TunerAttachmentIdSource() noexcept = default;
    virtual Result<std::uint64_t> next() noexcept = 0;
};

class TunerServiceTime {
public:
    virtual ~TunerServiceTime() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

struct TunerAcquireResult final {
    std::uint64_t lease_id = 0U;
    std::array<std::uint8_t, ipc::kNonceLength> nonce{};
};

struct TunerAttachment final {
    std::uint64_t owner_client_id = 0U;
    std::uint64_t lease_id = 0U;
    std::uint64_t attachment_id = 0U;
    std::uint8_t receiver = 0U;
    ipc::System system = ipc::System::ISDB_T;
    std::array<std::uint8_t, ipc::kNonceLength> nonce{};
    // v1 attachment selects the evict-oldest data-queue policy.  It is not
    // part of the attachment identity.
    bool evict_oldest = false;
};

// Transport-independent final stream accounting.  A stream implementation
// may retain one snapshot per stale-safe attachment identity after detach;
// this keeps the final STATS/STREAM_END value independent of a stats-before-
// detach race in a later control increment.
struct TunerStreamCounters final {
    std::uint64_t packets = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t sync_errors = 0U;
    std::uint64_t tei_packets = 0U;
    std::uint64_t continuity_errors = 0U;
    std::uint64_t queue_drops = 0U;
    std::uint64_t usb_errors = 0U;
    std::uint64_t empty_intervals = 0U;
    std::array<dtv::userland::wire::MeasureState, 8U> availability{
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown,
        dtv::userland::wire::MeasureState::unknown};
};

struct TunerStreamFinalSnapshot final {
    TunerStreamCounters counters{};
    std::uint8_t terminal = 0U;
};

enum class TunerStreamTerminal : std::uint8_t {
    none = 0,
    slow_consumer = 1,
    usb_error = 2,
    sync_error = 3,
    bridge_fatal = 4,
    stopped = 5,
    disconnected = 6,
};

struct TunerStreamReadResult final {
    std::size_t bytes = 0U;
    bool eof = false;
    bool timed_out = false;
    TunerStreamTerminal terminal = TunerStreamTerminal::none;
    std::uint64_t cumulative_queue_dropped_packets = 0U;
};

struct TunerStatus final {
    std::uint64_t generation = 1U;
    std::array<ipc::ReceiverState, ipc::kReceiverCount> receiver_states{};
};

// Optional data-plane seam.  It is called while the receiver-local service
// lock is held, so registration and frontend pin lifetime are one operation.
// Implementations must logically invalidate the identity and wake readers
// before detach returns, even when physical cleanup returns an error.  They
// may retain the final counters by this exact identity for a later
// final_snapshot() query; callers must not reconstruct them with a
// stats-before-detach sequence.
class TunerStreamControl {
public:
    virtual ~TunerStreamControl() noexcept = default;
    virtual void request_stop() noexcept {}
    virtual Result<void> attach(const TunerAttachment& attachment) noexcept = 0;
    virtual Result<void> detach(const TunerAttachment& attachment) noexcept = 0;
    // For an attached identity this is the live counter snapshot.  A stream
    // implementation must retain the exact final snapshot at its detach
    // linearization point; the service never emulates STOP with a
    // stats-before-detach query.
    // A stream control that returns UNSUPPORTED does not provide the required
    // final snapshot and therefore cannot complete STOP successfully.
    virtual Result<TunerStreamCounters> stats(
        const TunerAttachment& attachment) const noexcept
    {
        (void)attachment;
        return Result<TunerStreamCounters>::failure(Error::UNSUPPORTED);
    }
    virtual Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept
    {
        (void)attachment;
        return Result<TunerStreamFinalSnapshot>::failure(Error::UNSUPPORTED);
    }
    // Releases the retained post-detach queue/session after the data endpoint
    // has flushed STREAM_END.  It never performs hardware I/O.
    virtual Result<void> release_final(const TunerAttachment& attachment) noexcept
    {
        (void)attachment;
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    virtual Result<TunerStreamReadResult> read(
        const TunerAttachment& attachment, MutableByteView output,
        Timeout timeout) noexcept
    {
        (void)attachment;
        (void)output;
        (void)timeout;
        return Result<TunerStreamReadResult>::failure(Error::UNSUPPORTED);
    }
    virtual Result<TunerStreamTerminal> terminal(
        const TunerAttachment& attachment) const noexcept
    {
        (void)attachment;
        return Result<TunerStreamTerminal>::failure(Error::UNSUPPORTED);
    }
};

class TunerService final {
public:
    TunerService(TunerServiceBackend& backend, TunerNonceSource& nonce_source,
                 TunerServiceTime& time,
                 TunerLeaseIdSource* lease_id_source = nullptr,
                 TunerAttachmentIdSource* attachment_id_source = nullptr,
                 TunerStreamControl* stream_control = nullptr) noexcept;
    ~TunerService() noexcept;

    TunerService(const TunerService&) = delete;
    TunerService& operator=(const TunerService&) = delete;

    Result<TunerAcquireResult> acquire(std::uint64_t client_id,
                                      std::uint8_t receiver) noexcept;
    Result<void> release(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<ipc::TuneResponsePayload> tune(
        std::uint64_t client_id, const ipc::TuneRequestPayload& request) noexcept;
    Result<void> start_stream(std::uint64_t client_id,
                              std::uint64_t lease_id) noexcept;
    Result<TunerAttachment> attach_stream(
        std::uint64_t lease_id,
        const std::array<std::uint8_t, ipc::kNonceLength>& nonce) noexcept;
    Result<TunerStreamFinalSnapshot> stop_stream(
        std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<TunerStreamCounters> stats(std::uint64_t client_id,
                                      std::uint64_t lease_id) const noexcept;
    Result<void> detach_stream(const TunerAttachment& attachment) noexcept;
    // This operation is metadata-only and never waits for receiver I/O.
    Result<void> revoke_stream(std::uint64_t client_id,
                               std::uint64_t lease_id) noexcept;
    // Data-endpoint cleanup variant.  The nonce is checked while the same
    // registry mutex is held, so an invalid ATTACH cannot revoke a valid
    // credential for the lease it guessed.
    Result<void> revoke_stream(
        std::uint64_t client_id, std::uint64_t lease_id,
        const std::array<std::uint8_t, ipc::kNonceLength>& nonce) noexcept;
    Result<TunerStatus> status() const noexcept;
    bool supports_internal_card_observation() const noexcept;
    dtv::userland::wire::Optional<bool> sample_internal_card_inserted() noexcept;
    Result<void> disconnect_client(std::uint64_t client_id) noexcept;
    Result<void> shutdown() noexcept;
    void request_stop() noexcept { backend_.legacy().request_stop(); }

    // R2 strict admission.  The real daemon enables the immutable capability
    // table once before its workers start, built from the audited backend
    // model/count metadata.  Tests that do not opt in keep the legacy direct
    // tune entrance.
    using TunerAdmittedTune = dtv::userland::detail::AdmittedTune;
    Result<void> configure_strict_capabilities() noexcept;
    bool strict_admission_enabled() const noexcept;
    // Same monotonic domain as the engine clock, sampled at request acceptance
    // before queue submission.
    std::uint64_t monotonic_ms() const noexcept;
    // R2 compatibility admission of an unprepared old-wire request: the
    // legacy kHz entrance with the old validation decision.
    Result<TunerAdmittedTune> try_admit_tune(
        std::uint64_t client_id, const ipc::TuneRequestPayload& request,
        std::uint64_t accepted_at_ms) noexcept;
    // R3 pure strict preparation.  It reads only the owned profile copy and
    // performs no backend, clock, engine or lock operation, so callers run it
    // after the acceptance sample and before any lane lock.  The old-wire
    // form keeps the old validation decision, converts the u64 kHz scalar to
    // Hz with a checked multiplication, and selects the strict Hz entrance
    // only for a receiver whose profile is native.
    TunerPreparedTune prepare_tune(std::uint8_t receiver,
                                   const ipc::TuneRequestPayload& request) const noexcept;
    TunerPreparedTune prepare_tune_hz(std::uint8_t receiver,
                                      const TunerHzTuneRequest& request) const noexcept;
    // Callback-free admission of a prepared request (same engine body).
    Result<TunerAdmittedTune> try_admit_prepared_tune(
        std::uint64_t client_id, const TunerPreparedTune& prepared,
        std::uint64_t accepted_at_ms) noexcept;
    Result<void> execute_admitted_tune(const TunerAdmittedTune& ticket) noexcept;
    Result<void> abort_unstarted_tune(const TunerAdmittedTune& ticket,
                                      Error error) noexcept;
    // Synchronous logical-teardown invalidation of an admitted ticket.
    Result<void> revoke_tune_ticket(std::uint64_t client_id,
                                    std::uint64_t lease_id) noexcept;
    // Internal diagnostic/test view of the R2 tune metadata for one legacy
    // receiver.  It carries only permitted tune fields (no lease/client/token)
    // and is not part of the wire contract.
    Result<dtv::userland::detail::TuneMetadata> tune_metadata(
        std::uint8_t receiver) const noexcept;

    // Token-free engine snapshot (states, operations, tune metadata and lease
    // presence) taken under the engine registry lock.  It performs no backend
    // I/O.  Slot order is the legacy receiver order 0..receiver_count()-1.
    Result<dtv::userland::detail::ReceiverEngineStatus> engine_status() const noexcept;
    std::uint64_t monotonic_ms() noexcept;
    Result<void> set_independent_lnb_power(bool enabled) noexcept;
    dtv::userland::wire::Optional<bool> independent_lnb_power_state() noexcept;
    // Fixed enclosure shape.  receiver_supports() is the backend's pure
    // metadata predicate (no I/O) for receivers below receiver_count().
    std::uint8_t receiver_count() const noexcept { return receiver_count_; }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept;
    // Owned strict profile copied by configure_strict_capabilities(); an empty
    // profile when the receiver/system has none.  No backend call.
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept;

    // --- v1 stream authority facade (the same common engine) ---
    // The daemon selects the immutable contract once before its workers start;
    // the legacy stream entrances then return UNSUPPORTED on that engine.  These
    // forwarders add no second lease/stream authority.
    using TunerStreamContract = dtv::userland::detail::StreamContract;
    using TunerStreamStart = dtv::userland::detail::StreamStartResult;
    using TunerStreamTicket = dtv::userland::detail::StreamAttachTicket;
    using TunerStreamStop = dtv::userland::detail::StreamStopResult;
    Result<void> configure_stream_contract(TunerStreamContract contract,
                                           std::uint64_t context_epoch) noexcept;
    Result<TunerStreamStart> start_stream_v1(std::uint64_t client_id,
                                             std::uint64_t lease_id,
                                             std::uint64_t revision,
                                             std::uint64_t now_ms,
                                             std::uint64_t arm_budget_ms) noexcept;
    Result<TunerStreamTicket> reserve_attach_v1(
        std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t epoch,
        std::uint64_t stream_id,
        const dtv::userland::detail::ReceiverToken& token,
        std::uint64_t now_ms) noexcept;
    Result<void> prepare_attach_v1(const TunerStreamTicket& ticket) noexcept;
    Result<void> commit_attach_v1(const TunerStreamTicket& ticket) noexcept;
    Result<void> abort_attach_v1(const TunerStreamTicket& ticket, Error cause) noexcept;
    Result<TunerStreamStop> stop_stream_v1(std::uint64_t client_id,
                                           std::uint64_t lease_id,
                                           std::uint64_t stream_id) noexcept;
    Result<TunerStreamStop> expire_armed_v1(std::uint64_t client_id,
                                            std::uint64_t lease_id,
                                            std::uint64_t stream_id,
                                            std::uint64_t now_ms) noexcept;
    Result<void> run_stream_cleanup_v1(const TunerStreamTicket& ticket) noexcept;
    Result<void> complete_stream_cleanup_v1(const TunerStreamTicket& ticket,
                                            Error result, bool quiesced) noexcept;
    Result<TunerStreamStop> stream_last_terminal_v1(std::uint64_t client_id,
                                                    std::uint64_t lease_id) const noexcept;
    Result<TunerStreamStart> authority_start_stream_v1(
        std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t revision,
        std::uint64_t now_ms, std::uint64_t arm_budget_ms) noexcept;
    Result<TunerStreamTicket> authority_reserve_attach_v1(
        std::uint64_t client_id, std::uint64_t lease_id, std::uint64_t epoch,
        std::uint64_t stream_id, const dtv::userland::detail::ReceiverToken& token,
        std::uint64_t now_ms) noexcept;
    Result<TunerStreamStop> authority_stop_stream_v1(
        std::uint64_t client_id, std::uint64_t lease_id,
        std::uint64_t stream_id) noexcept;
    Result<TunerStreamStop> authority_stream_last_terminal_v1(
        std::uint64_t client_id, std::uint64_t lease_id) const noexcept;
    Result<void> authority_prepare_attach_v1(const TunerStreamTicket& ticket) noexcept;
    Result<void> authority_commit_attach_v1(const TunerStreamTicket& ticket) noexcept;
    Result<void> authority_abort_attach_v1(const TunerStreamTicket& ticket,
                                           Error cause) noexcept;
    Result<void> authority_run_stream_cleanup_v1(
        const TunerStreamTicket& ticket) noexcept;
    Result<TunerStreamReadResult> authority_read_stream_v1(
        const TunerStreamTicket& ticket, MutableByteView output,
        Timeout timeout) noexcept;
    Result<TunerStreamCounters> authority_stream_stats_v1(
        const TunerStreamTicket& ticket) const noexcept;
    Result<TunerStreamFinalSnapshot> authority_final_snapshot_v1(
        const TunerStreamTicket& ticket) const noexcept;
    Result<void> authority_release_stream_final_v1(
        const TunerStreamTicket& ticket) noexcept;

private:
    friend class HostAuthorityOwner;
    friend class ipc::posix::ControlWorkerLanes;
    friend class ipc::posix::HostAuthorityBridge;
    friend class HostAuthorityOwnerTestAccess;

    // The lease, tune, and stream lifecycle runs in the common receiver
    // engine.  These adapters only translate the PX4 seams and types; the
    // PX-Q3U4 receiver defaults, the settle policy, and request validation
    // stay in this product.
    class BackendAdapter final : public dtv::userland::detail::ReceiverBackend {
    public:
        explicit BackendAdapter(TunerServiceBackend& backend) noexcept
            : backend_(backend)
        {
        }
        TunerServiceBackend& legacy() const noexcept { return backend_; }
        bool supports(dtv::userland::detail::ReceiverId receiver,
                      dtv::userland::detail::ReceiverSystem system) const noexcept override;
        bool selects_satellite_stream_before_tune() const noexcept override;
        std::uint32_t lock_settle_ms(
            dtv::userland::detail::ReceiverId receiver,
            dtv::userland::detail::ReceiverSystem system) const noexcept override;
        Result<void> open(dtv::userland::detail::ReceiverId receiver) noexcept override;
        Result<void> tune(dtv::userland::detail::ReceiverId receiver,
                          dtv::userland::detail::ReceiverSystem system,
                          std::uint32_t frequency_khz,
                          std::uint32_t timeout_ms) noexcept override;
        Result<dtv::userland::detail::TuneProgramming> program_hz(
            dtv::userland::detail::ReceiverId receiver,
            dtv::userland::detail::ReceiverSystem system, std::uint64_t requested_hz,
            std::uint32_t timeout_ms) noexcept override;
        Result<bool> is_locked(dtv::userland::detail::ReceiverId receiver,
                               dtv::userland::detail::ReceiverSystem system) noexcept override;
        Result<void> select_satellite_slot(dtv::userland::detail::ReceiverId receiver,
                                           std::uint8_t slot,
                                           std::uint32_t timeout_ms) noexcept override;
        Result<void> select_satellite_tsid(dtv::userland::detail::ReceiverId receiver,
                                           std::uint16_t tsid,
                                           std::uint32_t timeout_ms) noexcept override;
        Result<void> close(dtv::userland::detail::ReceiverId receiver) noexcept override;
        Result<void> begin_tune_power(dtv::userland::detail::ReceiverId receiver,
                                      dtv::userland::detail::ReceiverSystem system,
                                      std::uint8_t lnb_voltage) noexcept override;
        Result<void> commit_tune_power(
            dtv::userland::detail::ReceiverId receiver) noexcept override;
        Result<void> rollback_tune_power(
            dtv::userland::detail::ReceiverId receiver) noexcept override;
        Result<void> set_independent_lnb_power(bool enabled) noexcept;
        dtv::userland::wire::Optional<bool> independent_lnb_power_state() noexcept;
        void mark_disconnected(dtv::userland::detail::ReceiverId receiver) noexcept override;
        Result<void> shutdown() noexcept override;
        Result<void> start_capture(dtv::userland::detail::ReceiverId receiver,
                                   dtv::userland::detail::ReceiverSystem system) noexcept override;
        Result<void> stop_capture(dtv::userland::detail::ReceiverId receiver,
                                  dtv::userland::detail::ReceiverSystem system) noexcept override;

    private:
        TunerServiceBackend& backend_;
    };

    class NonceAdapter final : public dtv::userland::detail::ReceiverTokenSource {
    public:
        explicit NonceAdapter(TunerNonceSource& source) noexcept : source_(source) {}
        Result<dtv::userland::detail::ReceiverToken> generate() noexcept override;

    private:
        TunerNonceSource& source_;
    };

    class LeaseIdAdapter final : public dtv::userland::detail::ReceiverIdSource {
    public:
        explicit LeaseIdAdapter(TunerLeaseIdSource* source) noexcept : source_(source) {}
        Result<std::uint64_t> next() noexcept override;

    private:
        TunerLeaseIdSource* source_;
    };

    class AttachmentIdAdapter final : public dtv::userland::detail::ReceiverIdSource {
    public:
        explicit AttachmentIdAdapter(TunerAttachmentIdSource* source) noexcept
            : source_(source)
        {
        }
        Result<std::uint64_t> next() noexcept override;

    private:
        TunerAttachmentIdSource* source_;
    };

    class TimeAdapter final : public dtv::userland::detail::ReceiverClock {
    public:
        explicit TimeAdapter(TunerServiceTime& time) noexcept : time_(time) {}
        std::uint64_t monotonic_ms() noexcept override;
        void sleep_ms(std::uint32_t milliseconds) noexcept override;

    private:
        TunerServiceTime& time_;
    };

    class StreamAdapter final : public dtv::userland::detail::ReceiverStreamControl {
    public:
        explicit StreamAdapter(TunerStreamControl* stream) noexcept : stream_(stream) {}
        Result<void> attach(
            const dtv::userland::detail::Attachment& attachment) noexcept override;
        Result<void> detach(
            const dtv::userland::detail::Attachment& attachment) noexcept override;
        Result<dtv::userland::detail::StreamCounters> stats(
            const dtv::userland::detail::Attachment& attachment) const noexcept override;
        Result<dtv::userland::detail::StreamFinalSnapshot> final_snapshot(
            const dtv::userland::detail::Attachment& attachment) const noexcept override;
        Result<void> release_final(
            const dtv::userland::detail::Attachment& attachment) noexcept override;
        Result<dtv::userland::detail::StreamReadResult> read(
            const dtv::userland::detail::Attachment& attachment,
            MutableByteView output, Timeout timeout) noexcept;

    private:
        TunerStreamControl* stream_;
    };

    static bool valid_tune(const ipc::TuneRequestPayload& request) noexcept;
    static std::uint8_t legacy_receiver_count(
        const TunerServiceBackend& backend) noexcept;
    const TunerStrictProfile* strict_profile_for(std::uint8_t receiver,
                                                 ipc::System system) const noexcept;

    BackendAdapter backend_;
    NonceAdapter nonce_source_;
    TimeAdapter time_;
    LeaseIdAdapter lease_id_source_;
    AttachmentIdAdapter attachment_id_source_;
    StreamAdapter stream_control_;
    // Legacy contiguous receiver IDs 0..receiver_count_-1 in engine slot order,
    // taken from the backend once at construction.  Supported backends (Q3U4,
    // W3U4, MLT, single receiver, and ASICEN after profile selection) keep a
    // fixed count for the service's lifetime; a backend whose count grows
    // later is not supported, and its added receivers are not acquirable.
    std::uint8_t receiver_count_;
    std::array<dtv::userland::detail::ReceiverId, ipc::kReceiverCount> receiver_ids_;
    // Owned immutable strict profiles [receiver][0: ISDB-T, 1: ISDB-S],
    // written once by configure_strict_capabilities() before workers start.
    std::array<std::array<TunerStrictProfile, 2U>, ipc::kReceiverCount> strict_profiles_{};
    // Declared last: destroyed first, while the adapters are still alive.  Its
    // destructor performs the shutdown that ~TunerService() used to call.
    dtv::userland::detail::ReceiverEngine engine_;
};

}  // namespace px4::userland

#endif  // DTV_USERLAND_HOST_DAEMON_TUNER_SERVICE_H
