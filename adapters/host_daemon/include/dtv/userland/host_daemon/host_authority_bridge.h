// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_HOST_AUTHORITY_BRIDGE_H
#define PX4_USERLAND_HOST_AUTHORITY_BRIDGE_H

#include "dtv/userland/host_daemon/control_workers.h"
#include "px4/host_authority_owner.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>

namespace px4::userland::ipc::posix {

inline constexpr std::size_t kHostAuthoritySessionCapacity = 32U;

struct HostAuthorityBridgeCounts final {
    std::size_t free = 0U;
    std::size_t active = 0U;
    std::size_t tombstone = 0U;
    std::size_t fatal_unresolved = 0U;
};

struct HostAuthorityDataConnection final {
    std::uint8_t slot = 0U;
    std::uint64_t generation = 0U;
};

struct HostAuthorityAttachResult final {
    Error error = Error::OK;
    dtv::userland::wire::AttachResponse response{};
    bool ready = false;
};

struct HostAuthorityDataReadResult final {
    Error error = Error::OK;
    dtv::userland::wire::TsData data{};
    TunerStreamTerminal terminal = TunerStreamTerminal::none;
    bool eof = false;
    bool timed_out = false;
};

struct HostAuthorityStreamEndResult final {
    Error error = Error::OK;
    dtv::userland::wire::StreamEndEvent event{};
    bool ready = false;
};

struct HostAuthorityDataCounts final {
    std::size_t free = 0U;
    std::size_t active = 0U;
    std::size_t tombstone = 0U;
    std::size_t fatal_unresolved = 0U;
    std::size_t retired = 0U;
};

// Poll-thread owner for the bounded, socketless Authority bridge. The caller
// owns frame parsing and writes reply.frame; response_sent() must receive true
// only after every byte of that frame was written.
class HostAuthorityBridge final : public dtv::userland::detail::ControlAuthorityExecutor {
public:
    using IngressStop = void (*)(void*) noexcept;
    using LegacyCompletionSink = void (*)(void*, const ControlWorkerCompletion&) noexcept;

    HostAuthorityBridge(TunerService& tuner_service, ControlWorkerLanes& workers,
                       const dtv::userland::wire::ServerHello& server,
                       const dtv::userland::wire::ListResponse& canonical,
                       dtv::userland::detail::ControlAuthority::TuneValidator
                           validate_tune = nullptr) noexcept;
    ~HostAuthorityBridge() noexcept = default;

    HostAuthorityBridge(const HostAuthorityBridge&) = delete;
    HostAuthorityBridge& operator=(const HostAuthorityBridge&) = delete;

    Result<std::uint8_t> open_session() noexcept;
    dtv::userland::detail::AuthorityReply accept(
        std::uint8_t slot, const dtv::userland::wire::Frame& frame,
        const dtv::userland::wire::Context& context,
        const dtv::userland::wire::Snapshot& snapshot,
        std::uint64_t accepted_at_ms) noexcept;
    Error response_sent(std::uint8_t slot, bool complete_frame) noexcept;
    dtv::userland::detail::AuthorityReply take_response(std::uint8_t slot) noexcept;
    Error close_session(std::uint8_t slot) noexcept;
    Error set_ingress_stop(IngressStop stop_other_ingress,
                           void* context) noexcept;
    Error set_control_ingress_stop(IngressStop stop_control_ingress,
                                   void* context) noexcept;
    Error set_data_ingress_stop(IngressStop stop_data_ingress,
                                void* context) noexcept;
    Result<HostAuthorityDataConnection> open_data_session() noexcept;
    Error close_data_session(HostAuthorityDataConnection connection) noexcept;
    Error configure_stream_contract(std::uint64_t engine_epoch,
                                    std::uint64_t arm_budget_ms) noexcept;
    Result<dtv::userland::detail::StreamStartResult> start_stream(
        std::uint8_t control_slot, std::uint64_t lease_id,
        std::uint64_t tune_revision, std::uint64_t now_ms,
        std::uint64_t arm_budget_ms) noexcept;
    Error prepare_data_attach(HostAuthorityDataConnection connection,
                              std::uint8_t control_slot,
                              const dtv::userland::wire::AttachRequest& request,
                              std::uint64_t now_ms) noexcept;
    HostAuthorityAttachResult take_data_attach_result(
        HostAuthorityDataConnection connection) noexcept;
    Error data_attach_response_sent(HostAuthorityDataConnection connection,
                                    bool complete_frame) noexcept;
    HostAuthorityDataReadResult read_data(HostAuthorityDataConnection connection,
                                          MutableByteView output,
                                          Timeout timeout) noexcept;
    HostAuthorityStreamEndResult take_data_stream_end(
        HostAuthorityDataConnection connection) noexcept;
    Error data_stream_end_sent(HostAuthorityDataConnection connection,
                               bool complete_frame) noexcept;
    Result<dtv::userland::detail::StreamStopResult> stop_stream(
        std::uint8_t control_slot, std::uint64_t lease_id,
        std::uint64_t stream_id) noexcept;
    Result<dtv::userland::detail::StreamStopResult> stop_stream_v1(
        std::uint64_t client_id, std::uint64_t lease_id,
        std::uint64_t stream_id) noexcept override;
    dtv::userland::wire::Optional<bool> lnb_power_state() noexcept override;
    HostAuthorityDataCounts data_counts() const noexcept;
    Error set_legacy_completion_sink(LegacyCompletionSink sink,
                                     void* context) noexcept;
    bool take_legacy_completion(ControlWorkerCompletion& completion) noexcept;

    // Drains all worker completions and advances cleanup/stop ownership once.
    Error poll() noexcept;
    Error begin_shutdown(IngressStop stop_other_ingress = nullptr,
                         void* context = nullptr) noexcept;
    bool stopped() const noexcept { return stopped_; }
    Error shutdown_result() const noexcept;
    HostAuthorityBridgeCounts counts() const noexcept;
    std::uint64_t client_id(std::uint8_t slot) const noexcept;
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
    bool fatal_unresolved_for_test(std::uint8_t slot) const noexcept
    {
        return slots_ && slot < kHostAuthoritySessionCapacity &&
               slots_[slot].state == SlotState::fatal_unresolved;
    }
    bool session_connected_for_test(std::uint8_t slot) const noexcept
    {
        return slots_ && slot < kHostAuthoritySessionCapacity && slots_[slot].session &&
               slots_[slot].session->connected();
    }
    bool stream_owned_for_test(std::uint8_t slot,
                               std::uint64_t stream_id) const noexcept
    {
        if (!slots_ || slot >= kHostAuthoritySessionCapacity) return false;
        for (const auto& stream : slots_[slot].streams)
            if (stream.active && stream.stream_id == stream_id) return true;
        return false;
    }
    bool close_pending_for_test(std::uint8_t slot) const noexcept
    {
        return slots_ && slot < kHostAuthoritySessionCapacity &&
               slots_[slot].close_pending;
    }
    bool close_pending_before_accept_for_test() const noexcept
    {
        return close_pending_before_accept_;
    }
    bool accept_preserved_close_pending_for_test() const noexcept
    {
        return accept_preserved_close_pending_;
    }
    bool retained_disconnect_for_test(std::uint8_t slot,
                                      std::uint64_t client) const noexcept
    {
        if (!slots_ || slot >= kHostAuthoritySessionCapacity) return false;
        const Slot& value = slots_[slot];
        return value.cleanup_pending &&
               value.cleanup.kind == dtv::userland::detail::AuthorityJobKind::disconnect &&
               value.cleanup.key.client_id == client && value.cleanup.client_id == client;
    }
    void force_next_terminal_unquiesced_for_test() noexcept
    {
        force_next_terminal_unquiesced_ = true;
    }
    void force_terminal_unquiesced_for_test(bool force) noexcept
    {
        force_terminal_unquiesced_ = force;
    }
    bool terminal_observed_for_test() const noexcept { return terminal_observed_; }
    bool terminal_quiesced_for_test() const noexcept { return terminal_quiesced_; }
    bool forced_terminal_unquiesced_for_test() const noexcept
    {
        return forced_terminal_unquiesced_;
    }
    bool quiesced_guard_rejected_for_test() const noexcept
    {
        return quiesced_guard_rejected_;
    }
    void close_session_for_test(std::uint8_t slot) noexcept;
#endif

    bool submit(const dtv::userland::detail::AuthorityJob& job) noexcept override;

private:
    enum class SlotState : std::uint8_t { free, active, tombstone, fatal_unresolved };
    enum class DataSlotState : std::uint8_t {
        free, active, tombstone, fatal_unresolved, retired
    };
    struct Slot final {
        struct StreamOwner final {
            std::uint64_t lease_id = 0U;
            std::uint64_t stream_id = 0U;
            bool active = false;
        };
        std::optional<dtv::userland::detail::ControlServiceSession> session{};
        std::array<StreamOwner, dtv::userland::detail::kReceiverEngineCapacity> streams{};
        dtv::userland::detail::AuthorityJob cleanup{};
        dtv::userland::detail::AuthorityReply response{};
        std::uint64_t client_id = 0U;
        std::size_t inflight = 0U;
        dtv::userland::detail::SessionKey inflight_key{};
        dtv::userland::detail::AuthorityJobKind inflight_kind =
            dtv::userland::detail::AuthorityJobKind::tune;
        dtv::userland::detail::SessionKey last_key{};
        dtv::userland::detail::AuthorityJobKind last_kind =
            dtv::userland::detail::AuthorityJobKind::tune;
        SlotState state = SlotState::free;
        bool request_inflight = false;
        bool last_completion_valid = false;
        bool cleanup_pending = false;
        bool cleanup_accepted = false;
        bool cleanup_inflight = false;
        bool card_cleanup_done = false;
        bool card_cleanup_inflight = false;
        bool deferred_session = false;
        bool close_pending = false;
        bool response_ready = false;
        Error quiesced_sticky_cleanup_error = Error::OK;
    };
    struct DataSlot final {
        std::uint64_t generation = 0U;
        DataSlotState state = DataSlotState::free;
        std::uint64_t lease_id = 0U;
        dtv::userland::detail::StreamAttachTicket ticket{};
        HostAuthorityAttachResult attach_result{};
        bool attach_pending = false;
        bool attach_failed = false;
        bool prepared = false;
        bool cleanup_quiesced = false;
        bool final_available = false;
        bool cleanup_pending = false;
        bool cleanup_inflight = false;
        bool connection_open = false;
        bool drained = false;
        std::uint64_t next_sequence = 0U;
        HostAuthorityStreamEndResult stream_end{};
    };

    std::size_t find_slot(std::uint64_t client_id) const noexcept;
    bool queue_job(const dtv::userland::detail::AuthorityJob& job,
                   std::size_t slot) noexcept;
    void retain_cleanup(const dtv::userland::detail::AuthorityJob& job,
                        std::size_t slot) noexcept;
    void close_all() noexcept;
    void close_all_data() noexcept;
    bool queue_stream_cleanup(std::size_t slot) noexcept;
    Error finish_failed_attach_cleanup(std::size_t slot) noexcept;
    Result<dtv::userland::detail::StreamStopResult> stop_stream_impl(
        std::uint8_t control_slot, std::uint64_t lease_id,
        std::uint64_t stream_id, bool retain_submit_rejection) noexcept;
    void close_owned_streams(std::size_t slot) noexcept;
    bool has_owned_streams(std::size_t slot) const noexcept;
    void finish_control_close(std::size_t slot) noexcept;
    void forget_stream(std::uint64_t client_id, std::uint64_t stream_id) noexcept;
    bool remember_stream(std::size_t slot, std::uint64_t lease_id,
                         std::uint64_t stream_id) noexcept;
    void stop_ingresses() noexcept;
    void enter_fatal() noexcept;
    void advance() noexcept;
    void consume(const ControlWorkerCompletion& completion,
                 std::size_t lane) noexcept;
    void finish_slot(std::size_t slot) noexcept;

    HostAuthorityOwner authority_owner_;
    TunerService& tuner_service_;
    ControlWorkerLanes& workers_;
    std::unique_ptr<Slot[]> slots_{};
    std::array<DataSlot, kHostAuthoritySessionCapacity> data_slots_{};
    std::uint64_t stream_engine_epoch_ = 0U;
    bool accepting_ = true;
    bool shutting_down_ = false;
    bool fatal_ = false;
    bool stop_requested_ = false;
    bool stopped_ = false;
#if defined(PX4_HOST_AUTHORITY_BRIDGE_TEST_ACCESS)
    bool force_next_terminal_unquiesced_ = false;
    bool force_terminal_unquiesced_ = false;
    bool terminal_observed_ = false;
    bool terminal_quiesced_ = false;
    bool forced_terminal_unquiesced_ = false;
    bool quiesced_guard_rejected_ = false;
    bool close_pending_before_accept_ = false;
    bool accept_preserved_close_pending_ = false;
#endif
    IngressStop stop_other_ingress_ = nullptr;
    void* stop_other_ingress_context_ = nullptr;
    IngressStop stop_control_ingress_ = nullptr;
    void* stop_control_ingress_context_ = nullptr;
    IngressStop stop_data_ingress_ = nullptr;
    void* stop_data_ingress_context_ = nullptr;
    bool other_ingress_stopped_ = false;
    bool control_ingress_stopped_ = false;
    bool data_ingress_stopped_ = false;
    LegacyCompletionSink legacy_completion_sink_ = nullptr;
    void* legacy_completion_context_ = nullptr;
    std::array<ControlWorkerCompletion, 3U> deferred_legacy_{};
    std::array<bool, 3U> deferred_legacy_valid_{};
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_HOST_AUTHORITY_BRIDGE_H
