// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CONTROL_WORKERS_H
#define PX4_USERLAND_CONTROL_WORKERS_H

#include "px4/card_service.h"
#include "px4/control_execution_layout.h"
#include "px4/tuner_service.h"

#include <dtv/userland/detail/control_authority.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace px4::userland::ipc::posix {

inline constexpr std::size_t kControlWorkerQueueCapacity = 64U;

// The worker wake handle is an int on POSIX and a pointer-width SOCKET on
// Windows. Callers pass it to the platform poll without narrowing.
#if defined(_WIN32)
using WakeHandle = std::uintptr_t;
inline constexpr WakeHandle kInvalidWakeHandle =
    static_cast<WakeHandle>(~static_cast<std::uintptr_t>(0U));
#else
using WakeHandle = int;
inline constexpr WakeHandle kInvalidWakeHandle = -1;
#endif

#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
// Private construction seam used only by offline tests to exercise startup
// rollback. Production callers use create(), with no failure injection.
struct ControlWorkerStartupOptions final {
    bool fail_wake_pipe = false;
    int fail_lane = -1;
    bool fail_tuner_attach_submit = false;
    // Deterministic injection of the strict TUNE publication signal failure so
    // the post-admission abort path is exercised without relying on a real
    // pthread error.
    bool fail_tune_signal = false;
    int fatal_after_authority_lane = -1;
    bool duplicate_next_authority_completion = false;
    bool mismatch_next_authority_acquire_receiver = false;
    bool reject_next_authority_cleanup_submit = false;
    bool reject_next_authority_stream_cleanup_submit = false;
    void (*before_tuner_attach_completion)(void*) noexcept = nullptr;
    void* before_tuner_attach_completion_context = nullptr;
};
#endif

enum class ControlWorkerOperation : std::uint8_t {
    tuner_acquire,
    tuner_release,
    tuner_tune,
    tuner_start_stream,
    tuner_stop_stream,
    tuner_start_stream_v1,
    tuner_stop_stream_v1,
    tuner_stop_stream_and_cleanup_v1,
    tuner_prepare_attach_v1,
    tuner_run_stream_cleanup_v1,
    tuner_stats,
    tuner_attach_stream,
    tuner_detach_stream,
    tuner_status,
    tuner_lnb_power,
    tuner_shutdown,
    card_status,
    card_handle_status,
    card_status_combined,
    card_presence,
    card_connect,
    card_reconnect,
    card_disconnect,
    card_reset,
    card_transmit,
    card_begin_transaction,
    card_end_transaction,
    card_release_connection,
    card_shutdown,
    tuner_authority_job,
};

// Internal routing/ownership classification. This is separate from
// request_id because zero is a valid wire request ID.
enum class ControlWorkerTaskKind : std::uint8_t {
    client_request,
    cleanup,
    background,
    authority_request,
    authority_cleanup,
};

// All task fields own their data. In particular, no task contains a Client,
// socket, or view into a frame receiver buffer.
struct ControlWorkerTask final {
    // Wire opcode of the request this task answers, carried back unchanged in
    // the completion; the lanes never interpret it.
    std::uint16_t opcode = 0U;
    ControlWorkerTaskKind kind = ControlWorkerTaskKind::client_request;
    ControlWorkerOperation operation = ControlWorkerOperation::tuner_status;
    std::uint64_t client_id = 0U;
    // Stable poll-thread connection identity. For stream clients this is
    // distinct from the control owner in client_id.
    std::uint64_t connection_id = 0U;
    std::uint32_t request_id = 0U;
    std::uint8_t receiver = 0U;
    std::uint64_t lease_id = 0U;
    std::uint64_t card_handle = 0U;
    ShareMode share_mode = ShareMode::shared;
    bool card_generation_bound = false;
    Disposition disposition = Disposition::leave;
    TuneRequestPayload tune{};
    // R2 strict dispatch.  accepted_at_ms is sampled at request acceptance in
    // the engine clock domain; admitted_tune is the callback-free admission
    // result published with the task.
    std::uint64_t accepted_at_ms = 0U;
    // v1 START/STOP dispatch values.  now_ms/arm_budget_ms are supplied by the
    // caller as a finite clock and budget; they are not machine-calibrated
    // constants.
    std::uint64_t tune_revision = 0U;
    std::uint64_t stream_id = 0U;
    std::uint64_t now_ms = 0U;
    std::uint64_t arm_budget_ms = 0U;
    bool tune_admitted = false;
    dtv::userland::detail::AdmittedTune admitted_tune{};
    // R3: owned outcome of the pure strict preparation, made after the
    // acceptance sample and before the lane lock (old-wire or internal Hz
    // ingress).  An unprepared task uses the R2 kHz admission.
    bool tune_prepared = false;
    TunerPreparedTune prepared_tune{};
    std::array<std::uint8_t, kMaxCardPayload> apdu{};
    std::size_t apdu_size = 0U;
    std::array<std::uint8_t, kNonceLength> nonce{};
    TunerAttachment attachment{};
    bool attachment_valid = false;
    // Exact v1 attachment ticket for prepare and the physical cleanup.
    dtv::userland::detail::StreamAttachTicket stream_ticket{};
    std::uint8_t data_slot = 0U;
    std::uint64_t data_generation = 0U;
    bool authority_stream = false;
    dtv::userland::detail::AuthorityJob authority_job{};
    bool lnb_enabled = false;
    dtv::userland::wire::Scope status_scope{};
};

struct ControlWorkerCompletion final {
    std::uint16_t opcode = 0U;
    ControlWorkerTaskKind kind = ControlWorkerTaskKind::client_request;
    ControlWorkerOperation operation = ControlWorkerOperation::tuner_status;
    std::uint64_t client_id = 0U;
    std::uint64_t connection_id = 0U;
    std::uint32_t request_id = 0U;
    std::uint8_t receiver = 0U;
    std::uint64_t lease_id = 0U;
    dtv::userland::detail::SessionKey authority_key{};
    dtv::userland::detail::AuthorityJobKind authority_job_kind =
        dtv::userland::detail::AuthorityJobKind::tune;
    dtv::userland::detail::AuthorityCompletion authority_completion{};
    dtv::userland::detail::StreamAttachTicket stream_ticket{};
    std::uint8_t data_slot = 0U;
    std::uint64_t data_generation = 0U;
    bool authority_stream = false;
    std::uint64_t card_handle = 0U;
    Error error = Error::OK;
    TunerAcquireResult tuner_acquire{};
    TunerStatus tuner_status{};
    bool tuner_status_valid = false;
    dtv::userland::wire::Optional<bool> internal_card_inserted{};
    dtv::userland::wire::Scope status_scope{};
    bool lnb_enabled = false;
    TuneResponsePayload tune_response{};
    // Admitted TUNE only: the committed tune metadata of task.receiver, read on
    // the worker after execution.  tune_record_valid is set only when its
    // current entry is this ticket's revision.
    std::uint64_t tune_revision = 0U;
    dtv::userland::detail::TuneMetadata tune_metadata{};
    bool tune_record_valid = false;
    TunerStreamCounters stream_counters{};
    TunerStreamFinalSnapshot stream_final_snapshot{};
    bool stream_snapshot_valid = false;
    TunerAttachment tuner_attachment{};
    bool tuner_attachment_valid = false;
    // v1 typed outcomes for START and armed STOP.
    dtv::userland::detail::StreamStartResult v1_start{};
    bool v1_start_valid = false;
    std::uint32_t v1_expires_after_ms = 0U;
    dtv::userland::detail::StreamStopResult v1_stop{};
    bool v1_stop_valid = false;
    CardServiceStatus card_status{};
    bool card_status_valid = false;
    CardPresenceChange card_presence{};
    CardServiceConnectResult card_connect{};
    // Reader generation sampled on the card lane after this operation, so the
    // poll thread never reads service state directly.  Zero only for
    // non-card operations.
    std::uint64_t card_generation = 0U;
    CardAtrBytes atr{};
    std::array<std::uint8_t, kMaxCardPayload> card_response{};
    std::size_t card_response_size = 0U;
};

class ControlWorkerLanes final {
public:
    static Result<std::unique_ptr<ControlWorkerLanes>> create(
        CardService& card_service, TunerService& tuner_service,
        const char* runtime_directory = nullptr,
        ControlExecutionLayout execution = ControlExecutionLayout{}) noexcept;
    // Windows wake sockets use this UTF-8 runtime parent. Only nullptr selects
    // LOCALAPPDATA (for standalone callers); POSIX wake pipes ignore it.
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
    static Result<std::unique_ptr<ControlWorkerLanes>> create_for_test(
        CardService& card_service, TunerService& tuner_service,
        const ControlWorkerStartupOptions& options,
        const char* runtime_directory = nullptr,
        ControlExecutionLayout execution = ControlExecutionLayout{}) noexcept;
    static void set_test_startup_options(
        ControlWorkerStartupOptions* options) noexcept;
#endif
    ~ControlWorkerLanes() noexcept;

    ControlWorkerLanes(const ControlWorkerLanes&) = delete;
    ControlWorkerLanes& operator=(const ControlWorkerLanes&) = delete;

    Result<void> submit(ControlWorkerLane lane,
                        const ControlWorkerTask& task) noexcept;
    Result<ControlWorkerLane> authority_lane(std::uint8_t receiver,
                                             bool cleanup) const noexcept;
    bool try_pop(ControlWorkerLane lane, ControlWorkerCompletion& completion) noexcept;
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
    std::size_t pending_completions_for_test(ControlWorkerLane lane) const noexcept;
#endif

    WakeHandle wake_fd() const noexcept;
    void drain_wake() noexcept;
    Error error() const noexcept;
    bool idle() const noexcept;
    std::size_t live_thread_count() const noexcept;
    Result<void> request_stop() noexcept;
    // request_stop() drains accepted tasks but does not consume completions.
    // Callers using join() must drain every completion first. This high-level
    // shutdown helper safely drains/discards completions before joining.
    Result<void> stop_and_join() noexcept;
    Result<void> join() noexcept;

private:
    static dtv::userland::detail::AuthorityCompletion execute_authority_job(
        TunerService& tuner_service,
        const dtv::userland::detail::AuthorityJob& job) noexcept;
    struct Impl;
    explicit ControlWorkerLanes(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_CONTROL_WORKERS_H
