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

enum class AuthorityJobKind : std::uint8_t { tune, release, disconnect };

struct AuthorityJob final {
    AuthorityJobKind kind = AuthorityJobKind::tune;
    SessionKey key{};
    AdmittedTune tune{};
    std::uint64_t client_id = 0U;
    std::uint64_t lease_id = 0U;
    ReceiverId receiver = 0U;
};

class ControlAuthorityExecutor {
public:
    virtual ~ControlAuthorityExecutor() noexcept = default;
    virtual bool submit(const AuthorityJob& job) noexcept = 0;
};

struct AuthorityReply final {
    wire::InboundAction action = wire::InboundAction::close;
    ByteView frame{nullptr, 0U};
    bool close_after_reply = false;
    bool queued = false;
    bool cleanup_required = false;
    AuthorityJob cleanup{};
};

class ControlAuthority;

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
    ReceiverId pending_receiver_ = 0U;
    AuthorityJobKind pending_kind_ = AuthorityJobKind::tune;
    bool used_ = false;
    bool close_after_reply_ = false;
    std::uint64_t pending_requested_hz_ = 0U;
    wire::Selection pending_selection_ = wire::Selection::automatic;
    std::uint16_t pending_selection_value_ = 0U;
    bool pending_ = false;
    bool closed_ = false;
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
    AuthorityReply accept(ControlServiceSession& session, const wire::Frame& frame,
                          const wire::Context& context, const wire::Snapshot& snapshot,
                          std::uint64_t accepted_at_ms,
                          ControlAuthorityExecutor& executor) noexcept;
    AuthorityJob close(ControlServiceSession& session) noexcept;
    AuthorityReply complete(const SessionKey& key, Error result) noexcept;
    Error response_sent(ControlServiceSession& session, bool complete_frame,
                        ControlAuthorityExecutor& executor,
                        AuthorityJob* rejected_cleanup) noexcept;

    static Error execute_job(ReceiverEngine& engine, const AuthorityJob& job) noexcept;

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

    wire::ServerHello server_;
    wire::ListResponse canonical_;
    ReceiverEngine& engine_;
    TuneValidator validate_tune_;
    ControlServiceSession* sessions_ = nullptr;
    ClientIdAllocator client_ids_{kAuthorityClientIdFirst, UINT64_MAX};
    bool valid_ = false;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_CONTROL_AUTHORITY_H
