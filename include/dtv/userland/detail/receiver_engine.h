// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H
#define DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H

#include "dtv/userland/error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Internal compatibility engine for receiver leases, tuning, and stream
// attachment.  It carries the existing PX4 TunerService lifecycle unchanged so
// that product facades can share one implementation.  It is not the adopted
// v1 receiver contract: tune timing starts after the receiver and power
// preparation as before (no end-to-end deadline), frequencies are kHz, and
// there are no public wire IDs, verification results, or JSON records.  These
// declarations may change without notice and are not a stable API.
namespace dtv::userland::detail {

// Stable receiver identity chosen by the consumer.  It is independent of the
// storage slot that holds the receiver.
using ReceiverId = std::uint16_t;

// Resource ceiling of the fixed slot table, not a receiver ID range.
inline constexpr std::size_t kReceiverEngineCapacity = 8U;

inline constexpr std::size_t kReceiverTokenLength = 16U;
using ReceiverToken = std::array<std::uint8_t, kReceiverTokenLength>;

// The numbering matches the legacy PX4 values so that a facade may pass any
// other legacy value through unchanged to backend queries and identity
// comparisons.  Request validation rejects such values before they are acted
// on.
enum class ReceiverSystem : std::uint8_t {
    terrestrial = 1U,
    satellite = 2U,
};

enum class ReceiverState : std::uint8_t {
    free,
    leased,
    tuned,
    streaming,
    error,
};

// Terminal code a stream reports in its final snapshot after transport loss.
inline constexpr std::uint8_t kStreamTerminalDisconnected = 6U;

// Legacy-shaped tune request.  For satellite exactly one of stream_id/slot is
// in use and 0xffff marks the other.
struct TuneRequest final {
    std::uint64_t lease_id = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    std::uint32_t frequency_khz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t timeout_ms = 0U;
    // The caller's validation decision for this request.  The engine returns
    // it after the lease, receiver-state, stream, and receiver-support checks
    // and before any backend operation.
    Error validation = Error::OK;
};

struct AcquireResult final {
    std::uint64_t lease_id = 0U;
    ReceiverToken token{};
};

struct Attachment final {
    std::uint64_t owner_client_id = 0U;
    std::uint64_t lease_id = 0U;
    std::uint64_t attachment_id = 0U;
    ReceiverId receiver = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    ReceiverToken token{};
};

struct StreamCounters final {
    std::uint64_t packets = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t sync_errors = 0U;
    std::uint64_t tei_packets = 0U;
    std::uint64_t continuity_errors = 0U;
    std::uint64_t queue_drops = 0U;
    std::uint64_t usb_errors = 0U;
    std::uint64_t empty_intervals = 0U;
};

struct StreamFinalSnapshot final {
    StreamCounters counters{};
    std::uint8_t terminal = 0U;
};

struct ReceiverEngineStatus final {
    std::uint64_t generation = 1U;
    // Slots [0, receiver_count) hold the configured receivers in order.
    std::size_t receiver_count = 0U;
    std::array<ReceiverId, kReceiverEngineCapacity> receivers{};
    std::array<ReceiverState, kReceiverEngineCapacity> states{};
};

// Physical receiver seam.  Methods receive the stable receiver ID.
class ReceiverBackend {
public:
    virtual ~ReceiverBackend() noexcept = default;
    virtual bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept = 0;
    virtual bool selects_satellite_stream_before_tune() const noexcept = 0;
    // Post-lock settle measured from the first lock query; 0 disables it.
    virtual std::uint32_t lock_settle_ms(ReceiverId receiver,
                                         ReceiverSystem system) const noexcept = 0;
    virtual Result<void> open(ReceiverId receiver) noexcept = 0;
    virtual Result<void> tune(ReceiverId receiver, ReceiverSystem system,
                              std::uint32_t frequency_khz,
                              std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<bool> is_locked(ReceiverId receiver, ReceiverSystem system) noexcept = 0;
    virtual Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> close(ReceiverId receiver) noexcept = 0;
    // begin_tune_power() precedes the frontend operations; a later failure is
    // paired with rollback_tune_power() and full success with commit.
    virtual Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem system,
                                          std::uint8_t lnb_voltage) noexcept = 0;
    virtual Result<void> commit_tune_power(ReceiverId receiver) noexcept = 0;
    virtual Result<void> rollback_tune_power(ReceiverId receiver) noexcept = 0;
    // Metadata-only transport-loss notification; must not perform I/O.
    virtual void mark_disconnected(ReceiverId receiver) noexcept = 0;
    // Called after all leases have been released.
    virtual Result<void> shutdown() noexcept = 0;
    virtual Result<void> start_capture(ReceiverId receiver,
                                       ReceiverSystem system) noexcept = 0;
    virtual Result<void> stop_capture(ReceiverId receiver,
                                      ReceiverSystem system) noexcept = 0;
};

class ReceiverTokenSource {
public:
    virtual ~ReceiverTokenSource() noexcept = default;
    virtual Result<ReceiverToken> generate() noexcept = 0;
};

class ReceiverIdSource {
public:
    virtual ~ReceiverIdSource() noexcept = default;
    virtual Result<std::uint64_t> next() noexcept = 0;
};

class ReceiverClock {
public:
    virtual ~ReceiverClock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

// Optional data-plane seam, called while the receiver-local lock is held.
// detach() must logically invalidate the identity before it returns, even when
// physical cleanup fails, and retain the final snapshot for final_snapshot().
class ReceiverStreamControl {
public:
    virtual ~ReceiverStreamControl() noexcept = default;
    virtual Result<void> attach(const Attachment& attachment) noexcept = 0;
    virtual Result<void> detach(const Attachment& attachment) noexcept = 0;
    virtual Result<StreamCounters> stats(const Attachment& attachment) const noexcept = 0;
    virtual Result<StreamFinalSnapshot> final_snapshot(
        const Attachment& attachment) const noexcept = 0;
};

class ReceiverEngine final {
public:
    // receivers lists receiver_count distinct IDs in slot order; cleanup visits
    // them in this order.  A longer list or a repeated ID leaves the engine
    // unconfigured with no receivers.  The set is fixed for the engine's
    // lifetime; there is no receiver hot-plug or topology change.
    ReceiverEngine(ReceiverBackend& backend, const ReceiverId* receivers,
                   std::size_t receiver_count, ReceiverTokenSource& token_source,
                   ReceiverClock& clock, ReceiverIdSource* lease_id_source = nullptr,
                   ReceiverIdSource* attachment_id_source = nullptr,
                   ReceiverStreamControl* stream_control = nullptr) noexcept;
    ~ReceiverEngine() noexcept;

    ReceiverEngine(const ReceiverEngine&) = delete;
    ReceiverEngine& operator=(const ReceiverEngine&) = delete;

    bool configured() const noexcept { return configured_; }

    Result<AcquireResult> acquire(std::uint64_t client_id, ReceiverId receiver) noexcept;
    Result<void> release(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<void> tune(std::uint64_t client_id, const TuneRequest& request) noexcept;
    Result<void> start_stream(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<Attachment> attach_stream(std::uint64_t lease_id,
                                     const ReceiverToken& token) noexcept;
    Result<StreamFinalSnapshot> stop_stream(std::uint64_t client_id,
                                            std::uint64_t lease_id) noexcept;
    Result<StreamCounters> stats(std::uint64_t client_id,
                                 std::uint64_t lease_id) const noexcept;
    Result<void> detach_stream(const Attachment& attachment) noexcept;
    // Metadata-only; never waits for receiver I/O.
    Result<void> revoke_stream(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    // The token is checked under the same registry lock, so an invalid attach
    // cannot revoke a valid credential for the lease it guessed.
    Result<void> revoke_stream(std::uint64_t client_id, std::uint64_t lease_id,
                               const ReceiverToken& token) noexcept;
    Result<ReceiverEngineStatus> status() const noexcept;
    Result<void> disconnect_client(std::uint64_t client_id) noexcept;
    Result<void> shutdown() noexcept;

private:
    enum class StreamState : std::uint8_t {
        none,
        armed,
        attaching,
        active,
        revoked,
        consumed,
    };

    struct Lease final {
        bool active = false;
        std::uint64_t client_id = 0U;
        std::uint64_t lease_id = 0U;
        std::size_t slot = 0U;
        ReceiverSystem system = ReceiverSystem::terrestrial;
        ReceiverToken token{};
        std::uint64_t attachment_id = 0U;
        StreamState stream_state = StreamState::none;
        std::uint64_t attach_started_ms = 0U;
        // A failed or revoked start may have partially enabled the pin.  Keep
        // this flag until physical cleanup has been attempted.
        bool capture_maybe_active = false;
        bool stream_maybe_attached = false;
        bool backend_disconnected = false;
    };

    static bool valid_client(std::uint64_t client_id) noexcept;
    int find_slot(ReceiverId receiver) const noexcept;
    ReceiverSystem initial_system(std::size_t slot) const noexcept;
    Attachment attachment_for(const Lease& lease,
                              std::uint64_t attachment_id) const noexcept;
    int find_lease_locked(std::uint64_t client_id,
                          std::uint64_t lease_id) const noexcept;
    bool lease_id_in_use_locked(std::uint64_t lease_id) const noexcept;
    bool attachment_id_in_use_locked(std::uint64_t attachment_id) const noexcept;
    Result<std::uint64_t> allocate_lease_id_locked(std::size_t slot) noexcept;
    Result<std::uint64_t> allocate_attachment_id_locked(std::size_t slot) noexcept;
    void set_state_locked(std::size_t slot, ReceiverState state) noexcept;
    Result<void> close_lease(const Lease& lease) noexcept;
    Result<void> stop_capture_if_needed(const Lease& lease) noexcept;
    Result<void> detach_data_plane_if_needed(const Lease& lease) noexcept;
    Result<void> release_lease(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<void> cleanup_client(std::uint64_t client_id) noexcept;
    Result<void> cleanup_all() noexcept;
    void observe_disconnect(std::size_t slot, Error error) noexcept;
    Result<bool> poll_lock(std::size_t slot, ReceiverSystem system, std::uint64_t start_ms,
                           std::uint32_t timeout_ms) noexcept;

    ReceiverBackend& backend_;
    ReceiverTokenSource& token_source_;
    ReceiverClock& clock_;
    ReceiverIdSource* lease_id_source_;
    ReceiverIdSource* attachment_id_source_;
    ReceiverStreamControl* stream_control_;
    bool configured_ = false;
    std::size_t receiver_count_ = 0U;
    std::array<ReceiverId, kReceiverEngineCapacity> receivers_{};
    std::uint64_t next_lease_id_ = 1U;
    std::uint64_t next_attachment_id_ = 1U;
    std::uint64_t last_attachment_id_ = 0U;
    std::uint64_t generation_ = 1U;
    std::array<Lease, kReceiverEngineCapacity> leases_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> reserved_lease_ids_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> reserved_attachment_ids_{};
    std::array<ReceiverState, kReceiverEngineCapacity> states_{};
    mutable std::mutex mutex_;
    mutable std::array<std::mutex, kReceiverEngineCapacity> receiver_mutexes_{};
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H
