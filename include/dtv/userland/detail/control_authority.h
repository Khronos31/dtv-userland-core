// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_CONTROL_AUTHORITY_H
#define DTV_USERLAND_DETAIL_CONTROL_AUTHORITY_H

#include <dtv/userland/control_connection.h>
#include <dtv/userland/detail/client_id_allocator.h>
#include <dtv/userland/detail/receiver_engine.h>
#include <dtv/userland/detail/status_projection.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dtv::userland::detail {

struct SessionKey final {
    std::uint64_t client_id = 0U;
    std::uint64_t request_id = 0U;
};

enum class AuthorityJobKind : std::uint8_t {
    acquire, tune, lnb_power, release, rollback_release, disconnect, card,
    internal_card_status
};

struct AuthorityCompletion final {
    SessionKey key{};
    AuthorityJobKind kind = AuthorityJobKind::tune;
    Error error = Error::OK;
    std::uint64_t lease_id = 0U;
    ReceiverId receiver = 0U;
    wire::ControlRequestKind card_kind = wire::ControlRequestKind::card_status;
    wire::CardStatusResponse card_status{};
    wire::CardSessionResult card_session{};
    wire::CardDisconnectResponse card_disconnect{};
    wire::CardTransmitResponse card_transmit{};
    wire::BeginTransactionResponse card_transaction{};
    bool lnb_enabled = false;
    wire::Optional<bool> internal_card_inserted{};
};

struct AuthorityJob final {
    AuthorityJobKind kind = AuthorityJobKind::tune;
    SessionKey key{};
    AdmittedTune tune{};
    std::uint64_t client_id = 0U;
    std::uint64_t lease_id = 0U;
    ReceiverId receiver = 0U;
    bool lnb_enabled = false;
    Error rollback_error = Error::OK;
    wire::ControlRequestKind card_kind = wire::ControlRequestKind::card_status;
    wire::ControlRequest card_request{};
    // A deferred disconnect is only a close intent; completion must first
    // return the runnable cleanup value.
    bool deferred = false;
};

struct AuthorityCleanup final {
    AuthorityJobKind kind = AuthorityJobKind::tune;
    SessionKey key{};
    std::uint64_t client_id = 0U;
    std::uint64_t lease_id = 0U;
    ReceiverId receiver = 0U;
    Error rollback_error = Error::OK;
    bool deferred = false;

    AuthorityCleanup& operator=(const AuthorityJob& job) noexcept
    {
        kind = job.kind;
        key = job.key;
        client_id = job.client_id;
        lease_id = job.lease_id;
        receiver = job.receiver;
        rollback_error = job.rollback_error;
        deferred = job.deferred;
        return *this;
    }

    void copy_to(AuthorityJob& job) const noexcept
    {
        job = AuthorityJob{};
        job.kind = kind;
        job.key = key;
        job.client_id = client_id;
        job.lease_id = lease_id;
        job.receiver = receiver;
        job.rollback_error = rollback_error;
        job.deferred = deferred;
    }

    operator AuthorityJob() const noexcept
    {
        AuthorityJob job{};
        copy_to(job);
        return job;
    }
};

class ControlAuthorityExecutor {
public:
    virtual ~ControlAuthorityExecutor() noexcept = default;
    // false means no worker owns the job; true transfers the value to it.
    virtual bool submit(const AuthorityJob& job) noexcept = 0;
    bool submit(const AuthorityCleanup& job) noexcept
    {
        AuthorityJob value{};
        job.copy_to(value);
        return submit(value);
    }
    // Handles only the short metadata STOP entrance; implementations retain
    // physical cleanup ownership when publication is rejected.
    virtual Result<StreamStopResult> stop_stream_v1(std::uint64_t client_id,
                                                    std::uint64_t lease_id,
                                                    std::uint64_t stream_id) noexcept
    {
        (void)client_id;
        (void)lease_id;
        (void)stream_id;
        return Result<StreamStopResult>::failure(Error::UNSUPPORTED);
    }
    // Returns cached enclosure state only; STATUS must not trigger hardware I/O.
    virtual wire::Optional<bool> lnb_power_state() noexcept { return {}; }
    virtual bool supports_internal_card_observation() const noexcept { return false; }
};

struct AuthorityReply final {
    wire::InboundAction action = wire::InboundAction::close;
    ByteView frame{nullptr, 0U};
    bool close_after_reply = false;
    bool queued = false;
    // The caller owns cleanup until submit acceptance and must retain it when
    // submit rejects; a returned completion never submits this value itself.
    bool cleanup_required = false;
    AuthorityCleanup cleanup{};
};

class ControlAuthority;
class ControlServiceSession;

struct AuthorityPollResult final {
    ControlServiceSession* session = nullptr;
    AuthorityReply reply{};
    // attempted is false when the queue was empty; reply_ready distinguishes a
    // wire response from a retained request or a newly published worker job.
    bool attempted = false;
    bool reply_ready = false;
    bool close_required = false;
};

struct FunctionPresenceObservation final {
    // Identity fields must be from the observer's confirmed mapping.
    wire::UsbFunction function;
    // Required only while present; do not derive it from VID/PID or location.
    wire::Optional<std::uint64_t> connection_generation;
};

struct FunctionPresenceSnapshot final {
    wire::Uuid epoch{};
    std::uint64_t sequence = 0U;
    wire::BArray<FunctionPresenceObservation, wire::kMaxFunctions> functions;
};

class ControlServiceSession final {
public:
    explicit ControlServiceSession(ControlAuthority& authority) noexcept;
    ControlServiceSession(const ControlServiceSession&) = delete;
    ControlServiceSession& operator=(const ControlServiceSession&) = delete;
    ~ControlServiceSession() noexcept;

    bool connected() const noexcept { return client_id_ != 0U && !closed_; }
    std::uint64_t client_id() const noexcept { return client_id_; }

private:
    friend class ControlAuthority;
    static constexpr std::size_t kFrameCapacity = wire::kHeaderSize + wire::kMaxControlPayload;

    ControlAuthority& authority_;
    wire::ControlConnection connection_;
    std::array<std::uint8_t, kFrameCapacity> response_{};
    wire::StatusResponse status_response_{};
    ControlServiceSession* next_ = nullptr;
    std::array<std::uint64_t, kReceiverEngineCapacity> lease_ids_{};
    std::array<ReceiverId, kReceiverEngineCapacity> lease_receivers_{};
    wire::ControlRequest connection_request_{};
    std::size_t lease_count_ = 0U;
    std::uint64_t client_id_ = 0U;
    std::uint64_t pending_request_id_ = 0U;
    std::uint64_t pending_lease_id_ = 0U;
    std::uint64_t pending_tune_revision_ = 0U;
    Error pending_error_ = Error::OK;
    ReceiverId pending_receiver_ = 0U;
    bool pending_lnb_enabled_ = false;
    AuthorityJobKind pending_kind_ = AuthorityJobKind::tune;
    bool used_ = false;
    bool close_after_reply_ = false;
    std::uint64_t pending_requested_hz_ = 0U;
    HzTuneRequest pending_admission_request_{};
    std::uint64_t pending_admission_accepted_at_ms_ = 0U;
    wire::Selection pending_selection_ = wire::Selection::automatic;
    std::uint16_t pending_selection_value_ = 0U;
    bool pending_ = false;
    bool pending_admission_ = false;
    bool closed_ = false;
    wire::ControlRequestKind pending_card_kind_ = wire::ControlRequestKind::card_status;
    std::uint16_t pending_card_opcode_ = 0U;
    std::uint16_t pending_status_opcode_ = 0U;
};

class ControlAuthority final {
public:
    using TuneValidator = Error (*)(ReceiverId, const wire::TuneRequest&) noexcept;

    ControlAuthority(const wire::ServerHello& server, const wire::ListResponse& canonical,
                     ReceiverEngine& engine, TuneValidator validate_tune = nullptr) noexcept;
    ControlAuthority(const ControlAuthority&) = delete;
    ControlAuthority& operator=(const ControlAuthority&) = delete;
    ~ControlAuthority() noexcept;

    const wire::ServerHello& server() const noexcept { return server_; }
    const wire::ListResponse& canonical_list() const noexcept { return canonical_; }
    Error open(ControlServiceSession& session) noexcept;
    // Enable before opening sessions; updates and requests share the event-loop owner.
    Error enable_presence_cache() noexcept;
    Error update_presence(const FunctionPresenceSnapshot& snapshot) noexcept;
    Error invalidate_presence_epoch() noexcept;
    // The event-loop owner checks before writing; on failure it discards the
    // frame and closes the session. response_sent() is called only after send.
    Error response_owner_check(const ControlServiceSession& session) const noexcept;
    Error configure_stream_contract(std::uint64_t engine_epoch,
                                   std::uint64_t arm_budget_ms) noexcept;
    AuthorityReply accept(ControlServiceSession& session, const wire::Frame& frame,
                          const wire::Context& context, const wire::Snapshot& snapshot,
                          std::uint64_t accepted_at_ms,
                          ControlAuthorityExecutor& executor) noexcept;
    // Attempts one retained TUNE admission without waiting.  The event-loop
    // owner supplies now_ms in the Engine monotonic clock domain and routes a
    // reply only when reply_ready is true.
    AuthorityPollResult retry_pending_admission(std::uint64_t now_ms,
                                                ControlAuthorityExecutor& executor) noexcept;
    std::uint32_t admission_wait_limit_ms(std::uint64_t now_ms,
                                         std::uint32_t max_wait_ms) const noexcept;
    void cancel_pending_admission(ControlServiceSession& session) noexcept;
    AuthorityJob close(ControlServiceSession& session) noexcept;
    AuthorityReply complete(const AuthorityCompletion& completion) noexcept;
    AuthorityReply complete(const SessionKey& key, Error result) noexcept;
    // Records acceptance for a retained disconnect value after rollback/close.
    Error cleanup_submitted(const AuthorityJob& job, bool accepted) noexcept;
    Error cleanup_submitted(const AuthorityCleanup& job, bool accepted) noexcept;
    Error response_sent(ControlServiceSession& session, bool complete_frame,
                        ControlAuthorityExecutor& executor,
                        AuthorityJob* rejected_cleanup) noexcept;

    static Error execute_job(ReceiverEngine& engine, const AuthorityJob& job) noexcept;
    static AuthorityCompletion execute_job_value(ReceiverEngine& engine,
                                                  const AuthorityJob& job) noexcept;

private:
    friend class ControlServiceSession;
    ControlServiceSession* find_session(std::uint64_t client_id) const noexcept;
    AuthorityReply dispatch(ControlServiceSession& session, const wire::ControlRequest& request,
                            const wire::Context& context, const wire::Snapshot& snapshot,
                            std::uint64_t accepted_at_ms,
                            ControlAuthorityExecutor& executor) noexcept;
    AuthorityReply error_reply(ControlServiceSession& session, std::uint16_t opcode,
                               std::uint32_t request_id, Error error,
                               const char* operation) noexcept;
    AuthorityReply reply(ControlServiceSession& session, std::size_t size,
                         bool close_after = false) noexcept;
    bool remember_lease(ControlServiceSession& session, std::uint64_t lease_id,
                        ReceiverId receiver) noexcept;
    void forget_lease(ControlServiceSession& session, std::uint64_t lease_id) noexcept;
    bool queue_admission(ControlServiceSession& session) noexcept;
    void remove_admission(ControlServiceSession& session) noexcept;
    AuthorityReply submit_admitted_tune(ControlServiceSession& session,
                                        const wire::ControlRequest& request,
                                        const AdmittedTune& ticket,
                                        ControlAuthorityExecutor& executor) noexcept;
    AuthorityCleanup close_cleanup(ControlServiceSession& session) noexcept;
    AuthorityJob disconnect_job(ControlServiceSession& session) noexcept;

    wire::ServerHello server_;
    wire::ListResponse canonical_;
    ReceiverEngine& engine_;
    TuneValidator validate_tune_;
    ControlServiceSession* sessions_ = nullptr;
    std::array<ControlServiceSession*, kReceiverEngineCapacity> admission_queue_{};
    std::size_t admission_queue_head_ = 0U;
    std::size_t admission_queue_count_ = 0U;
    std::uint64_t presence_sequence_ = 0U;
    std::array<wire::Optional<std::uint64_t>, wire::kMaxFunctions>
        presence_generations_{};
    ClientIdAllocator client_ids_{kAuthorityClientIdFirst, UINT64_MAX};
    bool valid_ = false;
    bool presence_cache_enabled_ = false;
    bool presence_cache_ready_ = false;
    bool presence_epoch_invalidated_ = false;
    bool session_opened_ = false;
    std::uint64_t stream_engine_epoch_ = 0U;
    std::uint64_t stream_arm_budget_ms_ = 0U;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_CONTROL_AUTHORITY_H
