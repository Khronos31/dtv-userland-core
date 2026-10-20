// SPDX-License-Identifier: GPL-2.0-only
#include "px4/control_server.h"

#include "dtv/userland/host_daemon/control_workers.h"
#include "dtv/userland/control_connection.h"
#include "dtv/userland/wire.h"
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
#include "dtv/userland/host_daemon/control_server_test_access.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if defined(_MSC_VER)
using ssize_t = SSIZE_T;
#endif
#include <sys/types.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// Map the POSIX poll/send shapes the shared server already uses onto Winsock.
// The helper definitions appear before the object-like macros so their bodies
// still resolve to the real winsock calls; POSIX code below stays in #else.
using pollfd = WSAPOLLFD;
#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif
inline int socket_poll(WSAPOLLFD* descriptors, std::size_t count, int timeout) noexcept
{
    return ::WSAPoll(descriptors, static_cast<ULONG>(count), timeout);
}
inline ssize_t socket_send(std::uintptr_t handle, const void* buffer,
                           std::size_t size, int flags) noexcept
{
    const int amount = size > static_cast<std::size_t>(INT_MAX) ?
                           INT_MAX : static_cast<int>(size);
    return static_cast<ssize_t>(::send(
        static_cast<SOCKET>(handle), static_cast<const char*>(buffer),
        amount, flags));
}
#define poll socket_poll
#define send socket_send
#endif

namespace px4::userland::ipc::posix {
namespace {

namespace wire = dtv::userland::wire;

// Bound for completing one control response once its first byte is queued.
constexpr std::uint32_t kClientWriteTimeoutMs = 250U;
constexpr std::uint32_t kPresencePollIntervalMs = 250U;
constexpr std::size_t kMaxControlConnections = 32U;
constexpr std::size_t kRetiredClientCapacity = 64U;
// Caller-supplied finite armed budget for v1 START.  It is a bounded policy
// value, not a machine-calibrated constant.
constexpr std::uint64_t kStreamArmBudgetMs = 5000U;
// Engine-local epoch tag passed to configure_stream_contract.  The 16-byte
// wire epoch remains the authority checked by the control/data connection.
constexpr std::uint64_t kEngineStreamEpoch = 1U;
constexpr std::size_t kMaxStreamConnections = 32U;
constexpr std::size_t kStreamReadBytes = (65536U / 188U) * 188U;
constexpr std::size_t kStreamWriteBudget = 65536U;

const PathChar* empty_path() noexcept
{
    static const PathChar empty[1] = {0};
    return empty;
}

#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
ControlServerTestHooks* test_hooks = nullptr;

void report_stream_count(std::size_t live, std::size_t retired) noexcept
{
    if (test_hooks != nullptr && test_hooks->stream_count_changed != nullptr)
        test_hooks->stream_count_changed(test_hooks->context, live, retired);
}

void report_stream_partial(std::uint64_t connection_id) noexcept
{
    if (test_hooks != nullptr && test_hooks->stream_partial_frame != nullptr)
        test_hooks->stream_partial_frame(test_hooks->context, connection_id);
}

void report_stream_write_blocked(std::uint64_t connection_id) noexcept
{
    if (test_hooks != nullptr && test_hooks->stream_write_blocked != nullptr)
        test_hooks->stream_write_blocked(test_hooks->context, connection_id);
}

std::size_t control_write_limit() noexcept
{
    return test_hooks != nullptr ? test_hooks->control_write_limit : 0U;
}

void report_control_write_partial(std::uint64_t connection_id) noexcept
{
    if (test_hooks != nullptr && test_hooks->control_write_partial != nullptr)
        test_hooks->control_write_partial(test_hooks->context, connection_id);
}
#endif

std::uint32_t wire_code(Error error) noexcept
{
    return static_cast<std::uint32_t>(error);
}

wire::Scope enclosure_scope() noexcept
{
    return wire::Scope{wire::ScopeKind::enclosure, 0U};
}

wire::Scope receiver_scope(std::uint16_t id) noexcept
{
    return wire::Scope{wire::ScopeKind::receiver, id};
}

wire::Scope reader_scope(std::uint16_t id) noexcept
{
    return wire::Scope{wire::ScopeKind::reader, id};
}

bool is_card_operation(ControlWorkerOperation operation) noexcept
{
    switch (operation) {
    case ControlWorkerOperation::card_status:
    case ControlWorkerOperation::card_handle_status:
    case ControlWorkerOperation::card_status_combined:
    case ControlWorkerOperation::card_presence:
    case ControlWorkerOperation::card_connect:
    case ControlWorkerOperation::card_reconnect:
    case ControlWorkerOperation::card_disconnect:
    case ControlWorkerOperation::card_reset:
    case ControlWorkerOperation::card_transmit:
    case ControlWorkerOperation::card_begin_transaction:
    case ControlWorkerOperation::card_end_transaction:
    case ControlWorkerOperation::card_release_connection:
    case ControlWorkerOperation::card_shutdown:
        return true;
    default:
        return false;
    }
}

const char* card_operation_name(std::uint16_t opcode) noexcept
{
    switch (opcode) {
    case wire::kOpcodeCardStatus: return "card_status";
    case wire::kOpcodeCardConnect: return "card_connect";
    case wire::kOpcodeCardReconnect: return "card_reconnect";
    case wire::kOpcodeCardDisconnect: return "card_disconnect";
    case wire::kOpcodeCardReset: return "card_reset";
    case wire::kOpcodeCardTransmit: return "card_transmit";
    case wire::kOpcodeBeginTransaction: return "begin_transaction";
    case wire::kOpcodeEndTransaction: return "end_transaction";
    case wire::kOpcodeCardHandleStatus: return "card_handle_status";
    default: return "card";
    }
}

wire::System wire_system(dtv::userland::detail::ReceiverSystem system) noexcept
{
    return system == dtv::userland::detail::ReceiverSystem::satellite ? wire::System::isdb_s
                                                                       : wire::System::isdb_t;
}

ipc::System ipc_system(wire::System system) noexcept
{
    return system == wire::System::isdb_s ? ipc::System::ISDB_S : ipc::System::ISDB_T;
}

ipc::System ipc_system(dtv::userland::detail::ReceiverSystem system) noexcept
{
    return system == dtv::userland::detail::ReceiverSystem::satellite
               ? ipc::System::ISDB_S : ipc::System::ISDB_T;
}

// Engine samples are monotonic milliseconds times 10^6 in the engine clock;
// the wire carries nanoseconds from the context start in the same clock.  A
// sample that cannot be placed on that axis becomes unknown, never valid at 0.
wire::Measurement<bool> context_measurement(const wire::Measurement<bool>& sample,
                                            std::uint64_t start_ms,
                                            std::uint64_t observed_ns) noexcept
{
    if (sample.state != wire::MeasureState::valid) return sample;
    const std::uint64_t start_ns = start_ms * 1000000U;
    wire::Measurement<bool> result = sample;
    if (start_ms > std::numeric_limits<std::uint64_t>::max() / 1000000U ||
        sample.sampled_at_ns < start_ns || sample.sampled_at_ns - start_ns > observed_ns) {
        result.state = wire::MeasureState::unknown;
        result.reason = wire::UnknownReason::not_sampled;
        result.value = false;
        result.sampled_at_ns = 0U;
        return result;
    }
    result.sampled_at_ns = sample.sampled_at_ns - start_ns;
    return result;
}

wire::Tune wire_tune(std::uint64_t revision,
                     const dtv::userland::detail::TuneRecord& record) noexcept
{
    wire::Tune tune;
    tune.revision = revision;
    tune.system = wire_system(record.system);
    tune.frequency_hz = record.requested_hz;
    tune.programmed_hz = record.programmed_hz;
    tune.verification = record.verification;
    if (record.stream_id != 0xffffU) {
        tune.selection = wire::Selection::tsid;
        tune.selection_value = record.stream_id;
    } else if (record.slot != 0xffffU) {
        tune.selection = wire::Selection::slot;
        tune.selection_value = record.slot;
    }
    return tune;
}

wire::ReceiverState wire_state(dtv::userland::detail::ReceiverState state) noexcept
{
    switch (state) {
    case dtv::userland::detail::ReceiverState::free: return wire::ReceiverState::free;
    case dtv::userland::detail::ReceiverState::leased: return wire::ReceiverState::leased;
    case dtv::userland::detail::ReceiverState::tuned: return wire::ReceiverState::tuned;
    case dtv::userland::detail::ReceiverState::streaming: return wire::ReceiverState::streaming;
    case dtv::userland::detail::ReceiverState::error: return wire::ReceiverState::error;
    }
    return wire::ReceiverState::error;
}

wire::Operation wire_operation(dtv::userland::detail::ReceiverTuneOperation operation) noexcept
{
    switch (operation) {
    case dtv::userland::detail::ReceiverTuneOperation::idle: return wire::Operation::idle;
    case dtv::userland::detail::ReceiverTuneOperation::tuning: return wire::Operation::tuning;
    case dtv::userland::detail::ReceiverTuneOperation::cleanup:
        return wire::Operation::releasing;
    }
    return wire::Operation::releasing;
}

int timeout_to_poll(Timeout timeout) noexcept
{
    return timeout.milliseconds > static_cast<std::uint32_t>(INT_MAX) ?
               INT_MAX : static_cast<int>(timeout.milliseconds);
}

// The adopted STREAM_END statistics carry exactly the transmitted-packet and
// queue-dropped counter IDs.
wire::StreamCounters wire_stream_counters(const TunerStreamCounters& counters,
                                          std::uint64_t observed_ns) noexcept
{
    wire::StreamCounters result{};
    result.entries.count = 2U;
    result.entries.items[0].id = wire::kCounterIdTransmittedPackets;
    result.entries.items[0].counter.measurement.state = wire::MeasureState::valid;
    result.entries.items[0].counter.measurement.value = counters.packets;
    result.entries.items[0].counter.measurement.sampled_at_ns = observed_ns;
    result.entries.items[1].id = wire::kCounterIdQueueDroppedPackets;
    result.entries.items[1].counter.measurement.state = wire::MeasureState::valid;
    result.entries.items[1].counter.measurement.value = counters.queue_drops;
    result.entries.items[1].counter.measurement.sampled_at_ns = observed_ns;
    return result;
}

wire::StreamEndReason wire_end_reason(TunerStreamTerminal terminal) noexcept
{
    switch (terminal) {
    case TunerStreamTerminal::stopped: return wire::StreamEndReason::stopped;
    case TunerStreamTerminal::slow_consumer:
        return wire::StreamEndReason::slow_consumer;
    case TunerStreamTerminal::disconnected:
    case TunerStreamTerminal::usb_error:
        return wire::StreamEndReason::device_lost;
    case TunerStreamTerminal::sync_error:
    case TunerStreamTerminal::bridge_fatal:
        return wire::StreamEndReason::backend_error;
    case TunerStreamTerminal::none: break;
    }
    return wire::StreamEndReason::stopped;
}

ErrorCode stream_end_error(TunerStreamTerminal terminal) noexcept
{
    switch (terminal) {
    case TunerStreamTerminal::stopped: return ErrorCode::OK;
    case TunerStreamTerminal::slow_consumer: return ErrorCode::SLOW_CONSUMER;
    case TunerStreamTerminal::usb_error: return ErrorCode::USB_IO;
    case TunerStreamTerminal::disconnected: return ErrorCode::DISCONNECTED;
    case TunerStreamTerminal::sync_error: return ErrorCode::PROTOCOL_ERROR;
    case TunerStreamTerminal::bridge_fatal: return ErrorCode::INTERNAL;
    case TunerStreamTerminal::none: break;
    }
    return ErrorCode::INTERNAL;
}

}  // namespace

struct PosixControlServer::Impl final {
    struct LeaseRecord final {
        bool active = false;
        std::uint64_t lease_id = 0U;
        // TunerService receiver and the public u16 ID it was acquired as.
        std::uint8_t receiver = 0U;
        std::uint16_t public_id = 0U;
    };

    struct Outstanding final {
        bool active = false;
        std::uint16_t opcode = 0U;
        ControlWorkerOperation operation = ControlWorkerOperation::tuner_status;
        std::uint32_t request_id = 0U;
        std::uint8_t receiver = 0U;
        std::uint16_t public_id = 0U;
        std::uint64_t lease_id = 0U;
    };

    // One UDTV control connection.  The poll thread owns it; worker tasks
    // never hold a pointer to it or a view into its frame storage.  A request
    // stays outstanding until its whole response frame has been written, so
    // a partial response write blocks the next request like a pending one.
    class Client final : public wire::FrameHandler {
    public:
        Client(Impl& owner, CardClientId id, SocketStream stream) noexcept
            : owner_(owner), id_(id), stream_(std::move(stream)),
              receiver_(MutableByteView{payload_storage_.data(), payload_storage_.size()}),
              connection_(owner.hello)
        {
        }

        Result<void> on_frame(const wire::Frame& frame) noexcept override
        {
            return owner_.on_frame(*this, frame);
        }

        Result<std::size_t> read_ready() noexcept
        {
            const auto received = stream_.read_some(
                MutableByteView{read_buffer_.data(), read_buffer_.size()}, Timeout{0U});
            if (!received) {
                // EOF on a frame boundary is a clean close; inside a frame it
                // is a truncated request that is never dispatched.  Both end
                // the connection.
                if (received.error() == Error::DISCONNECTED) (void)receiver_.finish();
                return Result<std::size_t>::failure(received.error());
            }
            return receiver_.feed(ByteView{read_buffer_.data(), received.value()}, *this);
        }

        // Queues one complete response frame already written to tx_view().
        void queue_response(std::size_t size, bool close_after) noexcept
        {
            tx_size_ = size;
            tx_offset_ = 0U;
            tx_deadline_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(kClientWriteTimeoutMs);
            if (close_after) close_after_flush_ = true;
        }

        MutableByteView tx_view() noexcept
        {
            return MutableByteView{tx_buffer_.data(), tx_buffer_.size()};
        }

        bool tx_pending() const noexcept { return tx_offset_ < tx_size_; }
        bool tx_expired(std::chrono::steady_clock::time_point now) const noexcept
        {
            return tx_pending() && now >= tx_deadline_;
        }
        std::chrono::steady_clock::time_point tx_deadline() const noexcept
        {
            return tx_deadline_;
        }

        // Nonblocking write of the queued response.  When the last byte is
        // written the request is completed and the next one may be accepted.
        Result<void> flush() noexcept
        {
            std::size_t budget = tx_size_ - tx_offset_;
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
            const std::size_t limit = control_write_limit();
            if (limit != 0U && limit < budget) budget = limit;
#endif
            while (tx_offset_ < tx_size_ && budget != 0U) {
#if defined(MSG_NOSIGNAL)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const ssize_t written = ::send(
                    stream_.native_handle(), tx_buffer_.data() + tx_offset_, budget, flags);
                if (written > 0) {
                    tx_offset_ += static_cast<std::size_t>(written);
                    budget -= static_cast<std::size_t>(written);
                    continue;
                }
                if (written == 0) return Result<void>::failure(Error::DISCONNECTED);
#if defined(_WIN32)
                const int socket_error = ::WSAGetLastError();
                if (socket_error == WSAEINTR) continue;
                if (socket_error == WSAEWOULDBLOCK) break;
                return Result<void>::failure(
                    socket_error == WSAECONNRESET || socket_error == WSAENOTCONN ||
                            socket_error == WSAESHUTDOWN ?
                        Error::DISCONNECTED : Error::INTERNAL);
#else
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return Result<void>::failure(
                    errno == EPIPE || errno == ECONNRESET ? Error::DISCONNECTED
                                                          : Error::INTERNAL);
#endif
            }
            if (tx_offset_ < tx_size_) {
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
                report_control_write_partial(id_);
#endif
                return Result<void>::success();
            }
            tx_offset_ = 0U;
            tx_size_ = 0U;
            connection_.complete();
            if (close_after_flush_) return Result<void>::failure(Error::DISCONNECTED);
            return Result<void>::success();
        }

        CardClientId id() const noexcept { return id_; }
        NativeHandle fd() const noexcept { return stream_.native_handle(); }
        bool closed() const noexcept { return receiver_.failed() || !stream_.valid(); }
        void close() noexcept { stream_.close(); }

        wire::ControlConnection& connection() noexcept { return connection_; }

        bool find_lease(std::uint64_t lease_id, LeaseRecord& found) const noexcept
        {
            if (lease_id == 0U) return false;
            for (const LeaseRecord& lease : leases_) {
                if (lease.active && lease.lease_id == lease_id) {
                    found = lease;
                    return true;
                }
            }
            return false;
        }

        bool add_lease(std::uint8_t receiver, std::uint16_t public_id,
                       std::uint64_t lease_id) noexcept
        {
            if (lease_id == 0U || receiver >= kReceiverCount) return false;
            for (const LeaseRecord& lease : leases_) {
                if (lease.active && lease.lease_id == lease_id) return false;
            }
            for (LeaseRecord& lease : leases_) {
                if (!lease.active) {
                    lease = LeaseRecord{true, lease_id, receiver, public_id};
                    return true;
                }
            }
            return false;
        }

        void remove_lease(std::uint64_t lease_id) noexcept
        {
            for (LeaseRecord& lease : leases_) {
                if (lease.active && lease.lease_id == lease_id) {
                    lease = LeaseRecord{};
                    return;
                }
            }
        }

        struct CardHandleRecord final {
            std::uint64_t handle = 0U;
            std::uint64_t bound_generation = 0U;
        };

        std::array<LeaseRecord, kReceiverCount>& leases() noexcept { return leases_; }

        bool add_card_handle(std::uint64_t handle, std::uint64_t bound_generation) noexcept
        {
            if (handle == 0U || bound_generation == 0U) return false;
            for (const CardHandleRecord& record : card_handles_) {
                if (record.handle == handle) {
                    return record.bound_generation == bound_generation;
                }
            }
            card_handles_.push_back(CardHandleRecord{handle, bound_generation});
            return true;
        }

        bool find_card_handle(std::uint64_t handle,
                              std::uint64_t& bound_generation) const noexcept
        {
            for (const CardHandleRecord& record : card_handles_) {
                if (record.handle == handle) {
                    bound_generation = record.bound_generation;
                    return true;
                }
            }
            return false;
        }

        void rebind_card_handle(std::uint64_t handle,
                                std::uint64_t bound_generation) noexcept
        {
            for (CardHandleRecord& record : card_handles_) {
                if (record.handle == handle) {
                    record.bound_generation = bound_generation;
                    return;
                }
            }
        }

        void remove_card_handle(std::uint64_t handle) noexcept
        {
            for (std::size_t index = card_handles_.size(); index > 0U; --index) {
                if (card_handles_[index - 1U].handle == handle) {
                    card_handles_.erase(card_handles_.begin() +
                                        static_cast<std::ptrdiff_t>(index - 1U));
                    return;
                }
            }
        }

        std::vector<CardHandleRecord>& card_handles() noexcept { return card_handles_; }
        const std::vector<CardHandleRecord>& card_handles() const noexcept
        {
            return card_handles_;
        }

        void set_outstanding(std::uint16_t opcode, ControlWorkerOperation operation,
                             std::uint32_t request_id, std::uint8_t receiver,
                             std::uint16_t public_id, std::uint64_t lease_id) noexcept
        {
            outstanding_ = Outstanding{true, opcode, operation, request_id, receiver,
                                       public_id, lease_id};
        }

        bool matches(const ControlWorkerCompletion& completion) const noexcept
        {
            return outstanding_.active &&
                   outstanding_.request_id == completion.request_id &&
                   outstanding_.opcode == completion.opcode &&
                   outstanding_.operation == completion.operation;
        }

        const Outstanding& outstanding() const noexcept { return outstanding_; }
        void clear_outstanding() noexcept { outstanding_ = Outstanding{}; }

    private:
        Impl& owner_;
        CardClientId id_;
        SocketStream stream_;
        std::array<std::uint8_t, wire::kMaxControlPayload> payload_storage_{};
        std::array<std::uint8_t, 8192U> read_buffer_{};
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> tx_buffer_{};
        wire::FrameReceiver receiver_;
        wire::ControlConnection connection_;
        std::size_t tx_size_ = 0U;
        std::size_t tx_offset_ = 0U;
        std::chrono::steady_clock::time_point tx_deadline_{};
        bool close_after_flush_ = false;
        std::array<LeaseRecord, kReceiverCount> leases_{};
        std::vector<CardHandleRecord> card_handles_;
        Outstanding outstanding_{};
    };

    // Data connection.  Its first frame is the typed UDTV ATTACH; the engine
    // reserves the armed stream, the existing receiver lane prepares it, and
    // only the poll-side writer commits after the whole AttachResponse was
    // sent.
    class StreamClient;

    // Called by the stream writer after the AttachResponse ws fully sent.
    void on_attach_ack_sent(StreamClient& client) noexcept;

    class StreamClient final : public wire::FrameHandler {
    public:
        enum class Phase : std::uint8_t { attaching, active, ending };

        StreamClient(Impl& owner, std::uint64_t id, SocketStream stream) noexcept
            : owner_(owner), id_(id), stream_(std::move(stream)),
              receiver_(MutableByteView{framing_storage_.data(), framing_storage_.size()}),
              phase_(Phase::attaching)
        {
        }

        Result<void> on_frame(const wire::Frame& frame) noexcept override;

        std::uint64_t id() const noexcept { return id_; }
        NativeHandle fd() const noexcept { return stream_.native_handle(); }
        void close() noexcept { stream_.close(); }
        bool valid() const noexcept { return stream_.valid(); }
        bool remove_requested() const noexcept { return remove_requested_; }
        bool tx_pending() const noexcept { return tx_offset_ < tx_size_; }
        bool ack_pending() const noexcept { return ack_pending_; }
        MutableByteView read_buffer() noexcept
        {
            return MutableByteView{read_buffer_.data(), read_buffer_.size()};
        }
        bool can_read() const noexcept
        {
            if (phase_ == Phase::ending || close_after_flush_ || ack_pending_)
                return false;
            return phase_ == Phase::attaching ? !attach_submitted_ : true;
        }
        bool is_attaching() const noexcept { return phase_ == Phase::attaching; }
        bool is_active() const noexcept { return phase_ == Phase::active; }
        void request_remove() noexcept { remove_requested_ = true; }
        Phase phase() const noexcept { return phase_; }
        void set_phase(Phase phase) noexcept { phase_ = phase; }

        Result<std::size_t> read_ready() noexcept
        {
            const auto received = stream_.read_some(
                MutableByteView{read_buffer_.data(), read_buffer_.size()}, Timeout{0U});
            if (!received) return Result<std::size_t>::failure(received.error());
            return receiver_.feed(ByteView{read_buffer_.data(), received.value()}, *this);
        }

        Result<bool> flush(std::size_t budget) noexcept
        {
            while (tx_offset_ < tx_size_ && budget != 0U) {
                const std::size_t remaining = tx_size_ - tx_offset_;
                const std::size_t amount = std::min(remaining, budget);
#if defined(MSG_NOSIGNAL)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const ssize_t written = ::send(
                    stream_.native_handle(), tx_buffer_.data() + tx_offset_, amount, flags);
                if (written > 0) {
                    tx_offset_ += static_cast<std::size_t>(written);
                    budget -= static_cast<std::size_t>(written);
                    continue;
                }
                if (written == 0) {
                    return Result<bool>::failure(Error::DISCONNECTED);
                }
#if defined(_WIN32)
                const int socket_error = ::WSAGetLastError();
                if (socket_error == WSAEINTR) continue;
                if (socket_error == WSAEWOULDBLOCK) {
#else
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
                    report_stream_write_blocked(id_);
#endif
                    return Result<bool>::success(false);
                }
#if defined(_WIN32)
                return Result<bool>::failure(
                    socket_error == WSAECONNRESET || socket_error == WSAENOTCONN ||
                            socket_error == WSAESHUTDOWN ?
                        Error::DISCONNECTED : Error::INTERNAL);
#else
                return Result<bool>::failure(
                    errno == EPIPE || errno == ECONNRESET ? Error::DISCONNECTED
                                                          : Error::INTERNAL);
#endif
            }
            if (tx_offset_ == tx_size_) {
                tx_offset_ = 0U;
                tx_size_ = 0U;
                const bool attach_ack_sent = ack_pending_ && attach_ack_;
                ack_pending_ = false;
                attach_ack_ = false;
                if (close_after_flush_) remove_requested_ = true;
                // COMMIT is the poll-side publication point and happens only
                // after the whole AttachResponse has been written.
                if (attach_ack_sent) owner_.on_attach_ack_sent(*this);
                return Result<bool>::success(true);
            }
            return Result<bool>::success(false);
        }

        // UDTV typed error reply to the ATTACH request, then close after it is
        // sent.
        Result<void> queue_error(std::uint32_t error_request_id,
                                 std::uint32_t code) noexcept
        {
            if (tx_pending()) return Result<void>::failure(Error::BUSY);
            const auto written = wire::encode_error_frame(
                wire::kOpcodeAttachStream, error_request_id, owner_.hello.epoch, code,
                "attach", enclosure_scope(),
                MutableByteView{tx_buffer_.data(), tx_buffer_.size()});
            close_after_flush_ = true;
            if (!written) return Result<void>::failure(written.error());
            tx_size_ = written.value();
            tx_offset_ = 0U;
            ack_pending_ = false;
            attach_ack_ = false;
            return Result<void>::success();
        }

        Result<void> queue_attach_response() noexcept
        {
            if (tx_pending() || !ticket_valid)
                return Result<void>::failure(Error::BUSY);
            wire::AttachResponse value;
            value.epoch = owner_.hello.epoch;
            value.stream_id = stream_id;
            const auto written = wire::encode_record_frame(
                wire::Kind::response, wire::kOpcodeAttachStream, request_id, value,
                &wire::encode_attach_response,
                MutableByteView{tx_buffer_.data(), tx_buffer_.size()});
            if (!written) return Result<void>::failure(written.error());
            tx_size_ = written.value();
            tx_offset_ = 0U;
            ack_pending_ = true;
            attach_ack_ = true;
            return Result<void>::success();
        }

        Result<void> queue_data(std::uint64_t data_sequence, std::uint64_t drop_count,
                                ByteView bytes) noexcept
        {
            wire::TsData value;
            value.stream_id = stream_id;
            value.sequence = data_sequence;
            value.cumulative_queue_dropped_packets = drop_count;
            value.packets = bytes;
            return queue_event(wire::kOpcodeTsDataEvent, value, &wire::encode_ts_data);
        }

        Result<void> queue_end(const wire::StreamFinal& final) noexcept
        {
            wire::StreamEndEvent value;
            value.final = final;
            const auto result =
                queue_event(wire::kOpcodeStreamEndEvent, value,
                            &wire::encode_stream_end_event);
            if (result) close_after_flush_ = true;
            return result;
        }

        template <typename Record>
        Result<void> queue_event(std::uint16_t opcode, const Record& value,
                                 Result<std::size_t> (*encode)(const Record&,
                                                               MutableByteView) noexcept) noexcept
        {
            if (tx_pending()) return Result<void>::failure(Error::BUSY);
            const auto written = wire::encode_record_frame(
                wire::Kind::event, opcode, 0U, value, encode,
                MutableByteView{tx_buffer_.data(), tx_buffer_.size()});
            if (!written) return Result<void>::failure(written.error());
            tx_size_ = written.value();
            tx_offset_ = 0U;
            ack_pending_ = false;
            attach_ack_ = false;
            return Result<void>::success();
        }

        void begin_attach(const dtv::userland::detail::StreamAttachTicket& value,
                          const TunerAttachment& identity) noexcept
        {
            ticket = value;
            ticket_valid = true;
            stream_id = value.stream_id;
            owner_client_id = value.client_id;
            lease_id = identity.lease_id;
            receiver = identity.receiver;
            attachment = identity;
            attachment_valid = true;
            prepared = false;
            committed = false;
            attach_completion_seen = true;
        }

        Impl& owner() noexcept { return owner_; }
        std::uint32_t request_id = 0U;
        std::uint64_t owner_client_id = 0U;
        std::uint64_t lease_id = 0U;
        std::uint8_t receiver = 0U;
        dtv::userland::detail::StreamAttachTicket ticket{};
        bool ticket_valid = false;
        bool prepared = false;
        bool committed = false;
        std::uint64_t stream_id = 0U;
        TunerAttachment attachment{};
        bool attachment_valid = false;
        bool cleanup_pending = false;
        bool cleanup_queued = false;
        bool cleanup_complete = false;
        bool final_snapshot_valid = false;
        TunerStreamFinalSnapshot final_snapshot{};
        std::uint64_t sequence = 0U;
        std::uint64_t last_queue_drops = 0U;
        bool end_queued = false;
        bool data_eof = false;
        bool attach_completion_seen = false;

        void mark_attach_submitted() noexcept { attach_submitted_ = true; }
        bool attach_was_submitted() const noexcept { return attach_submitted_; }
        void mark_attach_completion_seen() noexcept { attach_completion_seen = true; }

    private:
        Impl& owner_;
        std::uint64_t id_;
        SocketStream stream_;
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxTsDataPayload>
            framing_storage_{};
        std::array<std::uint8_t, kStreamReadBytes> read_buffer_{};
        std::array<std::uint8_t, wire::kMaxFrameSize> tx_buffer_{};
        wire::FrameReceiver receiver_;
        Phase phase_;
        std::size_t tx_size_ = 0U;
        std::size_t tx_offset_ = 0U;
        bool ack_pending_ = false;
        bool attach_ack_ = false;
        bool attach_submitted_ = false;
        bool close_after_flush_ = false;
        bool remove_requested_ = false;
    };

    struct RetiredStream final {
        bool active = false;
        bool waiting_attach = false;
        bool cleanup_queued = false;
        bool cleanup_complete = false;
        std::uint64_t connection_id = 0U;
        std::uint64_t owner_client_id = 0U;
        std::uint64_t lease_id = 0U;
        std::uint8_t receiver = 0U;
        dtv::userland::detail::StreamAttachTicket ticket{};
        bool ticket_valid = false;
        TunerAttachment attachment{};
        bool attachment_valid = false;
    };

    struct RetiredLease final {
        bool active = false;
        bool queued = false;
        std::uint64_t lease_id = 0U;
        std::uint8_t receiver = 0U;
    };

    struct RetiredClient final {
        bool active = false;
        std::uint64_t client_id = 0U;
        // Keep this record until every in-flight client request has been
        // classified, not only an ACQUIRE.
        bool waiting_request = false;
        bool card_pending = false;
        bool card_queued = false;
        std::array<RetiredLease, kReceiverCount> leases{};
    };

    Impl(SocketListener listener_value, SocketListener stream_listener_value,
         CardService& service,
         TunerService& tuner, std::unique_ptr<ControlWorkerLanes> workers_value,
         TunerStreamControl* stream_control_value) noexcept
        : listener(std::move(listener_value)), stream_listener(std::move(stream_listener_value)),
          card_service(service),
          tuner_service(tuner), workers(std::move(workers_value)),
          stream_control(stream_control_value),
          next_presence_poll(std::chrono::steady_clock::now())
    {
    }

    void record_error(Error error) noexcept
    {
        if (error != Error::OK && cleanup_error == Error::OK) cleanup_error = error;
    }

    void schedule_remove(CardClientId id) noexcept
    {
        if (std::find(pending_removals.begin(), pending_removals.end(), id) ==
            pending_removals.end()) {
            pending_removals.push_back(id);
        }
    }

    Result<void> on_stream_frame(StreamClient& client,
                                 const wire::Frame& frame) noexcept;
    Result<void> accept_stream_ready() noexcept;
    void process_stream_clients(const std::vector<pollfd>& descriptors,
                                std::size_t descriptor_base,
                                std::size_t count) noexcept;
    void remove_stream_clients() noexcept;
    void remove_stream_client(std::size_t index) noexcept;
    void schedule_stream_remove(std::uint64_t connection_id) noexcept;
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
    void report_stream_capacity() noexcept;
#endif
    bool stream_retired_capacity_available() const noexcept;
    void retry_stream_cleanup() noexcept;
    void handle_stream_completion(const ControlWorkerCompletion& completion) noexcept;
    void handle_stream_cleanup_completion(
        const ControlWorkerCompletion& completion) noexcept;
    void notify_stream_detached(std::uint64_t owner_client_id,
                                std::uint64_t lease_id,
                                const TunerStreamFinalSnapshot& snapshot) noexcept;
    void notify_stream_detached_by_identity(std::uint64_t owner_client_id,
                                            std::uint64_t lease_id) noexcept;
    void begin_stream_cleanup(StreamClient& client) noexcept;
    void begin_stream_end(StreamClient& client,
                          const TunerStreamFinalSnapshot& snapshot) noexcept;
    RetiredStream* allocate_retired_stream(std::uint64_t connection_id) noexcept;
    RetiredStream* find_retired_stream(std::uint64_t connection_id) noexcept;
    const RetiredStream* find_retired_stream(std::uint64_t connection_id) const noexcept;
    bool retired_stream_cleanup_done(const RetiredStream& stream) const noexcept;
    bool all_retired_stream_cleanup_done() const noexcept;
    void reap_retired_streams() noexcept;
    void queue_live_stream_cleanup(StreamClient& client) noexcept;
    void queue_v1_stream_cleanup(StreamClient& client) noexcept;
    StreamClient* find_live_stream(std::uint64_t connection_id) noexcept;
    bool find_owner_lease(std::uint64_t lease_id, CardClientId& owner,
                          std::uint8_t& receiver) const noexcept;

    // Queues an error reply for the current request.  A reply that cannot be
    // encoded ends the connection instead of leaving the request unanswered.
    void reply_error(Client& client, std::uint16_t opcode, std::uint32_t request_id,
                     std::uint32_t code, const char* operation,
                     const wire::Scope& target) noexcept
    {
        const auto written = wire::encode_error_frame(opcode, request_id, hello.epoch, code,
                                                      operation, target, client.tx_view());
        if (!written) {
            client.close();
            schedule_remove(client.id());
            return;
        }
        client.queue_response(written.value(), false);
    }

    template <typename Record>
    void reply_record(Client& client, std::uint16_t opcode, std::uint32_t request_id,
                      const Record& value,
                      Result<std::size_t> (*encode)(const Record&, MutableByteView) noexcept,
                      const char* operation, const wire::Scope& target) noexcept
    {
        const auto written = wire::encode_record_frame(wire::Kind::response, opcode,
                                                       request_id, value, encode,
                                                       client.tx_view());
        if (!written) {
            // Never publish a partial or unvalidated record.
            reply_error(client, opcode, request_id,
                        wire_code(written.error() == Error::RESOURCE_LIMIT
                                      ? Error::RESOURCE_LIMIT : Error::INTERNAL),
                        operation, target);
            return;
        }
        client.queue_response(written.value(), false);
    }

    // Context-relative clock: nanoseconds since create() in the engine clock.
    std::uint64_t context_now_ns() const noexcept
    {
        const std::uint64_t now_ms = tuner_service.monotonic_ms();
        const std::uint64_t elapsed = now_ms >= context_start_ms ? now_ms - context_start_ms
                                                                 : 0U;
        constexpr std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() / 1000000U;
        return elapsed > limit ? std::numeric_limits<std::uint64_t>::max()
                               : elapsed * 1000000U;
    }

    wire::Snapshot next_snapshot() noexcept
    {
        return wire::Snapshot{++snapshot_sequence, context_now_ns()};
    }

    // Public ID -> list position, or -1.  IDs may be sparse and include 0.
    int find_public_receiver(std::uint16_t id) const noexcept
    {
        const auto& receivers = context->list.receivers;
        if (!receivers.present) return -1;
        for (std::size_t index = 0U; index < receivers.value.count; ++index) {
            if (receivers.value.items[index].id == id) return static_cast<int>(index);
        }
        return -1;
    }

    // True when id names a reader in the canonical same-epoch LIST.  The core
    // already refused a non-daemon epoch; the caller refuses an unknown reader.
    bool find_reader(std::uint16_t id) const noexcept
    {
        const auto& readers = context->list.readers;
        if (!readers.present) return false;
        for (std::size_t index = 0U; index < readers.value.count; ++index) {
            if (readers.value.items[index].id == id) return true;
        }
        return false;
    }

    static ipc::ShareMode to_share(wire::Share share) noexcept
    {
        return share == wire::Share::exclusive ? ipc::ShareMode::exclusive
                                                : ipc::ShareMode::shared;
    }

    static ipc::Disposition to_disposition(wire::Disposition disposition) noexcept
    {
        return disposition == wire::Disposition::reset ? ipc::Disposition::reset
                                                       : ipc::Disposition::leave;
    }

    // ATR is carried only when this exact handle has a live initialized
    // session; a zero-length ATR never becomes a present one.
    static wire::Optional<wire::BBytes<wire::kMaxAtrBytes>> wire_atr(
        const CardAtrBytes& atr) noexcept
    {
        wire::Optional<wire::BBytes<wire::kMaxAtrBytes>> out;
        if (atr.length == 0U) return out;
        const std::size_t length =
            std::min<std::size_t>(atr.length, wire::kMaxAtrBytes);
        out.present = true;
        out.value.length = static_cast<std::uint32_t>(length);
        std::memcpy(out.value.bytes.data(), atr.bytes.data(), length);
        return out;
    }

    // CardSessionResult describes one handle.  usable requires this handle's
    // bound generation to still be current; a stale or uninitialized handle
    // never claims a protocol or ATR.
    void build_card_session_result(wire::CardSessionResult& result,
                                   std::uint16_t reader_id,
                                   std::uint64_t card_handle,
                                   std::uint64_t bound_generation,
                                   std::uint64_t current_generation,
                                   bool present, bool initialized,
                                   const CardAtrBytes& atr) const noexcept
    {
        result = wire::CardSessionResult{};
        result.epoch = hello.epoch;
        result.reader_id = reader_id;
        result.card_handle = card_handle;
        result.bound_generation = bound_generation;
        result.current_generation = current_generation;
        result.present = present;
        result.initialized = initialized;
        // usable requires a current binding *and* a real (non-empty) ATR for
        // this exact handle.  A handle that cannot present an ATR is reported
        // not usable, never with a fabricated protocol or ATR.
        const auto atr_value = wire_atr(atr);
        result.usable = present && initialized &&
                        bound_generation == current_generation && atr_value.present;
        if (result.usable) {
            result.protocol.present = true;
            result.protocol.value = wire::CardProtocol::t1;
            result.atr = atr_value;
        }
    }

    // Handle ownership and generation check for every HKey request.  Returns a
    // wire error code (0 = accepted), matching the adopted rule: an unknown or
    // foreign handle is NOT_FOUND and a stored-generation mismatch is
    // STALE_HANDLE.  The bound generation is read back for the reply.
    std::uint32_t check_card_handle(Client& client, const wire::CardHandleKey& key,
                                    std::uint64_t& bound_generation) const noexcept
    {
        if (!find_reader(key.reader_id)) return wire_code(Error::NOT_FOUND);
        if (!client.find_card_handle(key.card_handle, bound_generation)) {
            return wire_code(Error::NOT_FOUND);
        }
        if (key.bound_generation != bound_generation) {
            return wire::kWireErrorStaleHandle;
        }
        return 0U;
    }

    // Serializes one CardSessionResult only after validate_card_session_result
    // accepts it; an internally inconsistent record is an internal error and is
    // never published on the wire.
    void reply_card_session(Client& client, std::uint16_t opcode,
                            std::uint32_t request_id,
                            const wire::CardSessionResult& value,
                            const char* operation,
                            const wire::Scope& target) noexcept
    {
        if (wire::validate_card_session_result(value) != Error::OK) {
            reply_error(client, opcode, request_id, wire_code(Error::INTERNAL),
                        operation, target);
            return;
        }
        reply_record(client, opcode, request_id, value,
                     &wire::encode_card_session_result, operation, target);
    }

    Result<void> submit(Client& client, std::uint16_t opcode, std::uint32_t request_id,
                        std::uint16_t public_id, ControlWorkerTask task) noexcept
    {
        task.client_id = client.id();
        task.request_id = request_id;
        task.opcode = opcode;
        task.kind = ControlWorkerTaskKind::client_request;
        client.set_outstanding(opcode, task.operation, request_id, task.receiver, public_id,
                               task.lease_id);
        const ControlWorkerLane lane = lane_for(task);
        const auto submitted = workers->submit(lane, task);
        if (submitted) return Result<void>::success();
        client.clear_outstanding();
        if (is_card_operation(task.operation)) {
            reply_error(client, opcode, request_id, wire_code(submitted.error()),
                        card_operation_name(opcode), reader_scope(public_id));
            return Result<void>::success();
        }
        if (task.operation == ControlWorkerOperation::tuner_status) {
            reply_error(client, opcode, request_id, wire_code(submitted.error()),
                        "status", task.status_scope);
            return Result<void>::success();
        }
        reply_error(client, opcode, request_id, wire_code(submitted.error()),
                    opcode == wire::kOpcodeTune ? "tune"
                    : opcode == wire::kOpcodeRelease ? "release"
                    : opcode == wire::kOpcodeStartStream ? "start_stream"
                    : opcode == wire::kOpcodeStopStream ? "stop_stream" : "acquire",
                    receiver_scope(public_id));
        return Result<void>::success();
    }

    // Product lane routing: the P bridge of the TunerService receiver.  It
    // is not derived from the public receiver ID.
    ControlWorkerLane lane_for(const ControlWorkerTask& task) const noexcept
    {
        if (task.operation == ControlWorkerOperation::card_status ||
            task.operation == ControlWorkerOperation::card_handle_status ||
            task.operation == ControlWorkerOperation::card_status_combined ||
            task.operation == ControlWorkerOperation::card_presence ||
            task.operation == ControlWorkerOperation::card_connect ||
            task.operation == ControlWorkerOperation::card_reconnect ||
            task.operation == ControlWorkerOperation::card_disconnect ||
            task.operation == ControlWorkerOperation::card_reset ||
            task.operation == ControlWorkerOperation::card_transmit ||
            task.operation == ControlWorkerOperation::card_begin_transaction ||
            task.operation == ControlWorkerOperation::card_end_transaction ||
            task.operation == ControlWorkerOperation::card_release_connection) {
            return context->execution.reader_lane;
        }
        if (task.operation == ControlWorkerOperation::card_shutdown)
            return context->execution.card_shutdown_lane;
        if (task.operation == ControlWorkerOperation::tuner_status)
            return context->execution.status_lane;
        if (task.operation == ControlWorkerOperation::tuner_lnb_power)
            return context->execution.lnb_lane;
        if (task.operation == ControlWorkerOperation::tuner_shutdown)
            return context->execution.tuner_shutdown_lane;
        return context->execution.receiver_lane[task.receiver];
    }

    void serve_list(Client& client, const wire::ControlRequest& request) noexcept
    {
        wire::ListResponse& response = *list_scratch;
        response = context->list;
        response.epoch = hello.epoch;
        response.snapshot = next_snapshot();
        if (wire::validate_list_response(response) != Error::OK) {
            reply_error(client, request.opcode, request.request_id, wire_code(Error::INTERNAL),
                        "list", enclosure_scope());
            return;
        }
        reply_record(client, request.opcode, request.request_id, response,
                     &wire::encode_list_response, "list", enclosure_scope());
    }

    // Normal STATUS is served on the poll thread from cached engine state and
    // immutable descriptors. Internal-card products sample enclosure state
    // in a worker before reaching this cached response path.
    void serve_status(Client& client, const wire::ControlRequest& request) noexcept
    {
        const wire::Scope scope = request.status.scope;
        wire::Scope shell_scope = scope;
        if (scope.kind == wire::ScopeKind::stream && context->list.receivers.present) {
            // Resolve only this context's issued current/retained stream ID.
            // The engine snapshot below still proves that its lease is live.
            for (std::size_t i = 0; i < context->list.receivers.value.count; ++i) {
                const auto& observed = stream_observations[context->receiver_index[i]];
                if ((observed.current.present && observed.current.value.id == scope.id) ||
                    (observed.last.present && observed.last.value.id == scope.id)) {
                    shell_scope = receiver_scope(context->list.receivers.value.items[i].id);
                    break;
                }
            }
        }
        const Error membership = wire::check_status_scope(context->list, shell_scope);
        if (membership != Error::OK) {
            reply_error(client, request.opcode, request.request_id, wire_code(membership),
                        "status",
                        membership == Error::INVALID_ARGUMENT ? enclosure_scope() : scope);
            return;
        }
        const auto engine = tuner_service.engine_status();
        if (!engine) {
            reply_error(client, request.opcode, request.request_id,
                        wire_code(engine.error()), "status", scope);
            return;
        }
        wire::Context status_context;
        status_context.epoch = hello.epoch;
        status_context.instance = context->instance;
        status_context.state = wire::ContextState::ready;
        const wire::Snapshot snapshot = next_snapshot();
        wire::StatusResponse& response = *status_scratch;
        const Error shell = wire::build_status_shell(context->list, status_context, snapshot,
                                                     shell_scope, response);
        if (shell != Error::OK) {
            reply_error(client, request.opcode, request.request_id, wire_code(shell), "status",
                        scope);
            return;
        }
        if (response.receivers.present) {
            for (std::size_t index = 0U; index < response.receivers.value.count; ++index) {
                project_receiver(engine.value(), snapshot,
                                 response.receivers.value.items[index]);
            }
        }
        if (scope.kind == wire::ScopeKind::stream) {
            const auto& receiver = response.receivers.value.items[0];
            if (!receiver.leased ||
                !((receiver.stream.present && receiver.stream.value.id == scope.id) ||
                  (receiver.last_stream.present && receiver.last_stream.value.id == scope.id))) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "status", scope);
                return;
            }
            response.scope = scope;
        }
        if (scope.kind == wire::ScopeKind::enclosure && response.enclosure.present) {
            response.lnb_15v_enabled = tuner_service.independent_lnb_power_state();
            response.card_inserted = internal_card_inserted;
        } else {
            response.card_inserted = wire::Optional<bool>{};
        }
        if (response.readers.present) {
            for (std::size_t index = 0U; index < response.readers.value.count; ++index) {
                wire::ReaderStatus& reader = response.readers.value.items[index];
                // The card is observed only by explicit card operations, which
                // this endpoint does not serve yet: there is no cached sample.
                reader.present = true;
                reader.card.state = wire::CardState::unknown;
                reader.card.sampled_at_ns.present = false;
                reader.card.reason = static_cast<std::uint8_t>(wire::UnknownReason::not_sampled);
                reader.operation = wire::CardOperation::idle;
                reader.handles_open = 0U;
                reader.transaction_active = false;
            }
        }
        const auto written = wire::encode_status_frame(request.request_id, response,
                                                       context->list, client.tx_view());
        if (!written) {
            reply_error(client, request.opcode, request.request_id,
                        wire_code(written.error() == Error::RESOURCE_LIMIT
                                      ? Error::RESOURCE_LIMIT : Error::INTERNAL),
                        "status", scope);
            return;
        }
        client.queue_response(written.value(), false);
    }

    void project_receiver(const dtv::userland::detail::ReceiverEngineStatus& engine,
                          const wire::Snapshot& snapshot,
                          wire::ReceiverStatus& status) const noexcept
    {
        const int position = find_public_receiver(status.descriptor.id);
        const std::uint8_t receiver =
            position >= 0 ? context->receiver_index[static_cast<std::size_t>(position)] : 0U;
        std::size_t slot = engine.receiver_count;
        for (std::size_t index = 0U; index < engine.receiver_count; ++index) {
            if (engine.receivers[index] == receiver) {
                slot = index;
                break;
            }
        }
        if (slot == engine.receiver_count) {
            // Validated at create(); a missing slot would be an internal fault.
            status.present = false;
            status.state = wire::ReceiverState::absent;
            return;
        }
        const auto& metadata = engine.tune_metadata[slot];
        status.present = true;
        status.state = wire_state(engine.states[slot]);
        status.operation = wire_operation(engine.operations[slot]);
        // A receiver under cleanup still holds its resources: leased stays
        // true until the release finished.
        status.leased = engine.leased[slot] ||
                        engine.operations[slot] ==
                            dtv::userland::detail::ReceiverTuneOperation::cleanup;
        status.tune_revision = metadata.revision;
        status.current_tune.present = metadata.current_valid;
        if (metadata.current_valid)
            status.current_tune.value = wire_tune(metadata.current_revision, metadata.current);
        status.last_successful_tune.present = metadata.last_successful_valid;
        if (metadata.last_successful_valid)
            status.last_successful_tune.value =
                wire_tune(metadata.last_successful_revision, metadata.last_successful);
        // The poll-owned observation carries identities/statistics published
        // by the real typed lifecycle. STATUS itself performs no stream/backend
        // callback; the same engine snapshot decides whether the stream lives.
        const auto& observed = stream_observations[receiver];
        const auto phase = static_cast<dtv::userland::detail::StreamPhase>(
            engine.stream_phases[slot]);
        status.stream.present = observed.current.present &&
            phase != dtv::userland::detail::StreamPhase::none &&
            phase != dtv::userland::detail::StreamPhase::ended;
        if (status.stream.present) {
            status.stream.value = observed.current.value;
            status.stream.value.phase = static_cast<wire::StreamPhase>(phase);
        }
        status.last_stream = observed.last;
        status.last_stream.present = status.leased && observed.last.present;
        status.measurements.entries.count = 0U;
        status.receiver_statistics.entries.count = 0U;
        status.stream_statistics.present = status.stream.present && observed.statistics.present;
        if (status.stream_statistics.present)
            status.stream_statistics.value = observed.statistics.value;
        status.last_error.present = false;
        (void)snapshot;
    }

    void dispatch(Client& client, const wire::ControlRequest& request) noexcept
    {
        if (context->verbose) {
            std::fprintf(stderr, "px4d verbose: control opcode=0x%04x request_id=%u\n",
                         static_cast<unsigned int>(request.opcode),
                         static_cast<unsigned int>(request.request_id));
        }
        switch (request.kind) {
        case wire::ControlRequestKind::list:
            serve_list(client, request);
            return;
        case wire::ControlRequestKind::status:
            if (request.status.scope.kind == wire::ScopeKind::enclosure &&
                tuner_service.supports_internal_card_observation()) {
                ControlWorkerTask task{};
                task.operation = ControlWorkerOperation::tuner_status;
                task.status_scope = request.status.scope;
                (void)submit(client, request.opcode, request.request_id, 0U, task);
                return;
            }
            serve_status(client, request);
            return;
        case wire::ControlRequestKind::acquire: {
            const std::uint16_t public_id = request.acquire.receiver_id;
            const int position = find_public_receiver(public_id);
            if (position < 0) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "acquire", receiver_scope(public_id));
                return;
            }
            const auto engine = tuner_service.engine_status();
            if (!engine) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(engine.error()), "acquire", receiver_scope(public_id));
                return;
            }
            const std::uint8_t receiver =
                context->receiver_index[static_cast<std::size_t>(position)];
            bool found = false;
            bool busy = false;
            for (std::size_t index = 0U; index < engine.value().receiver_count; ++index) {
                if (engine.value().receivers[index] != receiver) continue;
                found = true;
                busy = engine.value().leased[index] ||
                       engine.value().operations[index] !=
                           dtv::userland::detail::ReceiverTuneOperation::idle;
                break;
            }
            if (!found) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::INTERNAL), "acquire", receiver_scope(public_id));
                return;
            }
            if (busy) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::BUSY), "acquire", receiver_scope(public_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_acquire;
            task.receiver = receiver;
            (void)submit(client, request.opcode, request.request_id, public_id, task);
            return;
        }
        case wire::ControlRequestKind::release: {
            LeaseRecord lease{};
            if (!client.find_lease(request.release.lease_id, lease)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "release", enclosure_scope());
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_release;
            task.receiver = lease.receiver;
            task.lease_id = lease.lease_id;
            (void)submit(client, request.opcode, request.request_id, lease.public_id, task);
            return;
        }
        case wire::ControlRequestKind::tune: {
            // Another owner's lease is unknown here and never reaches hardware.
            LeaseRecord lease{};
            if (!client.find_lease(request.tune.lease_id, lease)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "tune", enclosure_scope());
                return;
            }
            if (!tuner_service.strict_admission_enabled()) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::UNSUPPORTED), "tune",
                            receiver_scope(lease.public_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_tune;
            task.receiver = lease.receiver;
            task.lease_id = lease.lease_id;
            // Accepted time in the engine clock domain before the pure
            // preparation and before queue publication, so the admitted
            // deadline covers queue wait.  It is not resampled later.
            task.accepted_at_ms = tuner_service.monotonic_ms();
            TunerHzTuneRequest hz{};
            hz.lease_id = request.tune.lease_id;
            hz.system = ipc_system(request.tune.system);
            hz.frequency_hz = request.tune.frequency_hz;
            hz.selection = request.tune.selection;
            hz.selection_value = request.tune.selection_value;
            hz.lnb_voltage = request.tune.lnb_voltage;
            hz.lna = request.tune.lna;
            hz.pid_subset = request.tune.pids.present;
            hz.timeout_ms = request.tune.timeout_ms;
            task.prepared_tune = tuner_service.prepare_tune_hz(lease.receiver, hz);
            task.tune_prepared = true;
            (void)submit(client, request.opcode, request.request_id, lease.public_id, task);
            return;
        }
        case wire::ControlRequestKind::start_stream: {
            LeaseRecord lease{};
            if (!client.find_lease(request.start_stream.lease_id, lease)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "start_stream", enclosure_scope());
                return;
            }
            // START binds to the receiver's successful current tune revision.
            const auto metadata = tuner_service.tune_metadata(lease.receiver);
            if (!metadata || !metadata.value().current_valid) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_READY), "start_stream",
                            receiver_scope(lease.public_id));
                return;
            }
            if (request.start_stream.tune_revision !=
                metadata.value().current_revision) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::INVALID_ARGUMENT), "start_stream",
                            receiver_scope(lease.public_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_start_stream_v1;
            task.receiver = lease.receiver;
            task.lease_id = lease.lease_id;
            task.tune_revision = metadata.value().current_revision;
            task.now_ms = tuner_service.monotonic_ms();
            task.arm_budget_ms = kStreamArmBudgetMs;
            (void)submit(client, request.opcode, request.request_id, lease.public_id, task);
            return;
        }
        case wire::ControlRequestKind::stop_stream: {
            LeaseRecord lease{};
            if (!client.find_lease(request.stop_stream.lease_id, lease)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "stop_stream", enclosure_scope());
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_stop_stream_v1;
            task.receiver = lease.receiver;
            task.lease_id = lease.lease_id;
            task.stream_id = request.stop_stream.stream_id;
            // A live data connection owns the exact ticket.  Run STOP and the
            // physical cleanup on the same receiver lane so the typed response
            // carries the settled final and STREAM_END follows it.
            for (std::size_t index = 0U; index < stream_client_count; ++index) {
                const StreamClient& stream = *stream_clients[index];
                if (!stream.ticket_valid || stream.lease_id != lease.lease_id ||
                    stream.stream_id != request.stop_stream.stream_id)
                    continue;
                task.operation =
                    ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1;
                task.stream_ticket = stream.ticket;
                break;
            }
            (void)submit(client, request.opcode, request.request_id, lease.public_id, task);
            return;
        }
        case wire::ControlRequestKind::card_status: {
            const std::uint16_t reader_id = request.card_status.key.reader_id;
            if (!find_reader(reader_id)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "card_status",
                            reader_scope(reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_status;
            (void)submit(client, request.opcode, request.request_id, reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_connect: {
            const std::uint16_t reader_id = request.card_connect.key.reader_id;
            if (!find_reader(reader_id)) {
                reply_error(client, request.opcode, request.request_id,
                            wire_code(Error::NOT_FOUND), "card_connect",
                            reader_scope(reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_connect;
            task.share_mode = to_share(request.card_connect.share);
            // Every UDTV control handle is bound to the reader generation it
            // was created in; a later card change invalidates it as stale.
            task.card_generation_bound = true;
            (void)submit(client, request.opcode, request.request_id, reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_reconnect: {
            const wire::CardHandleKey& key = request.card_reconnect.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "card_reconnect", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_reconnect;
            task.card_handle = key.card_handle;
            task.share_mode = to_share(request.card_reconnect.share);
            task.disposition = to_disposition(request.card_reconnect.disposition);
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_disconnect: {
            const wire::CardHandleKey& key = request.card_disconnect.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "card_disconnect", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_disconnect;
            task.card_handle = key.card_handle;
            task.disposition = to_disposition(request.card_disconnect.disposition);
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_reset: {
            const wire::CardHandleKey& key = request.card_reset.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "card_reset", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_reset;
            task.card_handle = key.card_handle;
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_transmit: {
            const wire::CardHandleKey& key = request.card_transmit.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "card_transmit", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_transmit;
            task.card_handle = key.card_handle;
            task.apdu_size = std::min<std::size_t>(request.card_transmit.command.length,
                                                   task.apdu.size());
            std::memcpy(task.apdu.data(), request.card_transmit.command.bytes.data(),
                        task.apdu_size);
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::begin_transaction: {
            const wire::CardHandleKey& key = request.begin_transaction.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "begin_transaction", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_begin_transaction;
            task.card_handle = key.card_handle;
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::end_transaction: {
            const wire::CardHandleKey& key = request.end_transaction.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "end_transaction", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_end_transaction;
            task.card_handle = key.card_handle;
            task.disposition = to_disposition(request.end_transaction.disposition);
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::card_handle_status: {
            const wire::CardHandleKey& key = request.card_handle_status.key;
            std::uint64_t bound_generation = 0U;
            const std::uint32_t rejection =
                check_card_handle(client, key, bound_generation);
            if (rejection != 0U) {
                reply_error(client, request.opcode, request.request_id, rejection,
                            "card_handle_status", reader_scope(key.reader_id));
                return;
            }
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::card_handle_status;
            task.card_handle = key.card_handle;
            (void)submit(client, request.opcode, request.request_id, key.reader_id, task);
            return;
        }
        case wire::ControlRequestKind::lnb_power: {
            ControlWorkerTask task{};
            task.operation = ControlWorkerOperation::tuner_lnb_power;
            task.lnb_enabled = request.lnb_power.enabled;
            (void)submit(client, request.opcode, request.request_id, 0U, task);
            return;
        }
        }
        reply_error(client, request.opcode, request.request_id, wire_code(Error::INTERNAL),
                    "request", enclosure_scope());
    }

    Result<void> on_frame(Client& client, const wire::Frame& frame) noexcept
    {
        const wire::Inbound inbound =
            client.connection().accept(frame, *request_scratch, client.tx_view());
        switch (inbound.action) {
        case wire::InboundAction::close:
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        case wire::InboundAction::respond:
            client.queue_response(inbound.reply_size, inbound.close_after_reply);
            return Result<void>::success();
        case wire::InboundAction::dispatch:
            dispatch(client, *request_scratch);
            return Result<void>::success();
        }
        return Result<void>::failure(Error::INTERNAL);
    }

    RetiredClient* find_retired(CardClientId id) noexcept
    {
        for (RetiredClient& retired : retired_clients) {
            if (retired.active && retired.client_id == id) return &retired;
        }
        return nullptr;
    }

    RetiredClient* allocate_retired(CardClientId id) noexcept
    {
        for (RetiredClient& retired : retired_clients) {
            if (!retired.active) {
                retired = RetiredClient{};
                retired.active = true;
                retired.client_id = id;
                return &retired;
            }
        }
        return nullptr;
    }

    bool retired_cleanup_done(const RetiredClient& retired) const noexcept
    {
        if (retired.waiting_request || retired.card_pending) return false;
        for (const RetiredLease& lease : retired.leases) {
            if (lease.active) return false;
        }
        return true;
    }

    bool all_retired_cleanup_done() const noexcept
    {
        for (const RetiredClient& retired : retired_clients) {
            if (retired.active && !retired_cleanup_done(retired)) return false;
        }
        return all_retired_stream_cleanup_done();
    }

    void reap_retired() noexcept
    {
        for (RetiredClient& retired : retired_clients) {
            if (retired.active && retired_cleanup_done(retired)) {
                retired = RetiredClient{};
            }
        }
        reap_retired_streams();
    }

    void queue_cleanup_task(ControlWorkerTask task, bool& queued) noexcept
    {
        if (queued) return;
        const auto submitted = workers->submit(lane_for(task), task);
        if (submitted) {
            queued = true;
        } else if (submitted.error() != Error::BUSY) {
            record_error(submitted.error());
        }
    }

    void retry_retired_cleanup() noexcept
    {
        for (RetiredClient& retired : retired_clients) {
            if (!retired.active) continue;
            for (RetiredLease& lease : retired.leases) {
                if (!lease.active || lease.queued) continue;
                ControlWorkerTask task{};
                task.kind = ControlWorkerTaskKind::cleanup;
                task.operation = ControlWorkerOperation::tuner_release;
                task.client_id = retired.client_id;
                task.request_id = 0U;
                task.receiver = lease.receiver;
                task.lease_id = lease.lease_id;
                queue_cleanup_task(task, lease.queued);
            }
            if (retired.card_pending && !retired.card_queued) {
                ControlWorkerTask task{};
                task.kind = ControlWorkerTaskKind::cleanup;
                task.operation = ControlWorkerOperation::card_release_connection;
                task.client_id = retired.client_id;
                task.request_id = 0U;
                queue_cleanup_task(task, retired.card_queued);
            }
        }
        retry_stream_cleanup();
    }

    void remove_client(std::size_t index) noexcept
    {
        if (index >= clients.size()) return;
        Client& client = *clients[index];
        const CardClientId id = client.id();
        client.close();
        RetiredClient* retired = allocate_retired(id);
        if (retired == nullptr) {
            // Admission reserves a retired slot before accepting a socket;
            // reaching this branch indicates an internal invariant failure.
            record_error(Error::INTERNAL);
        } else {
            retired->waiting_request = client.outstanding().active;
            for (const LeaseRecord& lease : client.leases()) {
                if (lease.active) {
                    // Synchronously revoke any admitted TUNE ticket at the
                    // logical-teardown point, before the queued release runs.
                    (void)tuner_service.revoke_tune_ticket(id, lease.lease_id);
                    for (RetiredLease& target : retired->leases) {
                        if (!target.active) {
                            target = RetiredLease{true, false, lease.lease_id,
                                                  lease.receiver};
                            break;
                        }
                    }
                }
            }
            // Always enqueue this fence. It also catches a card connect that
            // completes after the socket has already been removed.
            retired->card_pending = true;
        }
        clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        client_count.store(clients.size(), std::memory_order_release);
        retry_retired_cleanup();
    }

    void remove_pending_clients() noexcept
    {
        for (const CardClientId id : pending_removals) {
            for (std::size_t index = clients.size(); index > 0U; --index) {
                if (clients[index - 1U]->id() == id) {
                    remove_client(index - 1U);
                    break;
                }
            }
        }
        pending_removals.clear();
    }

    Result<void> accept_ready() noexcept
    {
        while (true) {
            auto accepted = listener.accept(Timeout{0U});
            if (!accepted) {
                return accepted.error() == Error::TIMEOUT ? Result<void>::success() :
                                                            Result<void>::failure(accepted.error());
            }
            const bool capacity = clients.size() < kMaxControlConnections &&
                                  retired_slot_available();
            if (!capacity || client_ids_exhausted) {
                accepted.value().close();
                continue;
            }
            const CardClientId id = next_client_id;
            if (next_client_id == std::numeric_limits<CardClientId>::max()) {
                client_ids_exhausted = true;
            } else {
                ++next_client_id;
            }
            std::unique_ptr<Client> client(
                new (std::nothrow) Client(*this, id, std::move(accepted.value())));
            if (!client) {
                accepted.value().close();
                record_error(Error::INTERNAL);
                continue;
            }
            clients.push_back(std::move(client));
            client_count.store(clients.size(), std::memory_order_release);
        }
    }

    bool retired_slot_available() const noexcept
    {
        std::size_t active = 0U;
        for (const RetiredClient& retired : retired_clients) {
            if (retired.active) ++active;
        }
        return clients.size() + active < kRetiredClientCapacity;
    }

    void handle_cleanup_completion(const ControlWorkerCompletion& completion) noexcept
    {
        RetiredClient* retired = find_retired(completion.client_id);
        if (retired == nullptr) return;
        if (completion.operation == ControlWorkerOperation::tuner_release) {
            if (completion.error == Error::NOT_READY) {
                // The v1 engine still owns physical stream cleanup. Keep the
                // retired lease and let the existing retry path release it
                // after its exact-ticket cleanup; do not lose its last owner.
                for (RetiredLease& lease : retired->leases) {
                    if (lease.active && lease.lease_id == completion.lease_id)
                        lease.queued = false;
                }
                return;
            }
            notify_stream_detached_by_identity(completion.client_id,
                                               completion.lease_id);
            for (RetiredLease& lease : retired->leases) {
                if (lease.active && lease.lease_id == completion.lease_id) {
                    lease = RetiredLease{};
                    break;
                }
            }
            if (completion.error != Error::OK && completion.error != Error::NOT_FOUND) {
                record_error(completion.error);
            }
        } else if (completion.operation == ControlWorkerOperation::card_release_connection) {
            retired->card_pending = false;
            if (completion.error != Error::OK && completion.error != Error::NOT_FOUND) {
                record_error(completion.error);
            }
        }
    }

    void add_orphan_lease(RetiredClient& retired, std::uint8_t receiver,
                          std::uint64_t lease_id) noexcept
    {
        for (RetiredLease& lease : retired.leases) {
            if (!lease.active) {
                lease = RetiredLease{true, false, lease_id, receiver};
                return;
            }
        }
        record_error(Error::INTERNAL);
    }

    void handle_orphan_completion(const ControlWorkerCompletion& completion,
                                  RetiredClient& retired) noexcept
    {
        // The record covers every client request, not just ACQUIRE. Classify
        // the completion before allowing the retired record to be reaped.
        retired.waiting_request = false;
        if (completion.operation == ControlWorkerOperation::tuner_acquire) {
            if (completion.error == Error::OK) {
                add_orphan_lease(retired, completion.receiver,
                                 completion.tuner_acquire.lease_id);
            }
        }
        handle_cleanup_completion(completion);
    }

    void send_completion(Client& client,
                         const ControlWorkerCompletion& completion) noexcept
    {
        const Outstanding outstanding = client.outstanding();
        const wire::Scope target = receiver_scope(outstanding.public_id);
        client.clear_outstanding();
        switch (completion.operation) {
        case ControlWorkerOperation::tuner_status: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "status", completion.status_scope);
                return;
            }
            internal_card_inserted = completion.internal_card_inserted;
            wire::ControlRequest request{};
            request.opcode = outstanding.opcode;
            request.request_id = completion.request_id;
            request.status.scope = completion.status_scope;
            serve_status(client, request);
            return;
        }
        case ControlWorkerOperation::tuner_lnb_power: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "lnb_power", enclosure_scope());
                return;
            }
            wire::LnbPowerResponse response{};
            response.epoch = hello.epoch;
            response.enabled = completion.lnb_enabled;
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_lnb_power_response, "lnb_power", enclosure_scope());
            return;
        }
        case ControlWorkerOperation::tuner_acquire: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "acquire", target);
                return;
            }
            if (!client.add_lease(completion.receiver, outstanding.public_id,
                                  completion.tuner_acquire.lease_id)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "acquire", target);
                auto retired = allocate_retired(client.id());
                if (retired != nullptr)
                    add_orphan_lease(*retired, completion.receiver,
                                     completion.tuner_acquire.lease_id);
                retry_retired_cleanup();
                return;
            }
            wire::AcquireResponse response;
            stream_observations[completion.receiver] = StreamObservation{};
            response.epoch = hello.epoch;
            response.lease_id = completion.tuner_acquire.lease_id;
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_acquire_response, "acquire", target);
            return;
        }
        case ControlWorkerOperation::tuner_release: {
            // NOT_READY is a precondition refusal while a v1 physical owner
            // remains in the engine. Other outcomes follow release_lease's
            // logical removal before its backend cleanup.
            if (completion.error != Error::NOT_READY)
                client.remove_lease(outstanding.lease_id);
            if (completion.error == Error::NOT_FOUND) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "release", target);
                return;
            }
            wire::ReleaseResponse response;
            response.epoch = hello.epoch;
            response.lease_id = outstanding.lease_id;
            // A cleanup failure leaves the receiver excluded: neither released
            // nor known to be quiesced, with the primary error.
            response.released = completion.error == Error::OK;
            response.quiesced = completion.error == Error::OK;
            response.error.present = completion.error != Error::OK;
            if (response.error.present)
                response.error.value =
                    wire::make_error_record(wire_code(completion.error), "release");
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_release_response, "release", target);
            return;
        }
        case ControlWorkerOperation::tuner_tune: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "tune", target);
                return;
            }
            if (!completion.tune_record_valid) {
                // The engine committed, but no committed record of this exact
                // ticket is observable: never report a reconstructed result.
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "tune", target);
                return;
            }
            const auto& record = completion.tune_metadata.current;
            wire::TuneResult result;
            result.epoch = hello.epoch;
            result.tune_revision = completion.tune_metadata.current_revision;
            result.requested_hz = record.requested_hz;
            result.programmed_hz = record.programmed_hz;
            result.verification = record.verification;
            result.lock = context_measurement(record.lock, context_start_ms, context_now_ns());
            if (record.stream_id != 0xffffU) {
                result.selected_tsid.present = true;
                result.selected_tsid.value = record.stream_id;
            } else if (record.slot != 0xffffU) {
                result.selected_slot.present = true;
                result.selected_slot.value = record.slot;
            }
            reply_record(client, outstanding.opcode, completion.request_id, result,
                         &wire::encode_tune_result, "tune", target);
            return;
        }
        case ControlWorkerOperation::tuner_start_stream_v1: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "start_stream", target);
                return;
            }
            if (!completion.v1_start_valid) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "start_stream", target);
                return;
            }
            wire::StartResponse response;
            response.epoch = hello.epoch;
            response.stream_id = completion.v1_start.stream_id;
            response.attach_token = completion.v1_start.token;
            response.expires_after_ms = completion.v1_expires_after_ms;
            auto& observed = stream_observations[outstanding.receiver];
            const auto metadata = tuner_service.tune_metadata(outstanding.receiver);
            if (!metadata || !metadata.value().current_valid) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "start_stream", target);
                return;
            }
            observed.current.present = true;
            observed.current.value = wire::Stream{response.stream_id,
                metadata.value().current_revision,
                wire::StreamPhase::armed};
            observed.statistics.present = false;
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_start_response, "start_stream", target);
            return;
        }
        case ControlWorkerOperation::tuner_stop_stream_v1:
        case ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1: {
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "stop_stream", target);
                return;
            }
            if (!completion.v1_stop_valid) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "stop_stream", target);
                return;
            }
            wire::StopResponse response;
            response.epoch = hello.epoch;
            response.final.id = completion.v1_stop.stream_id;
            response.final.tune_revision = completion.v1_stop.revision;
            response.final.reason = wire::StreamEndReason::stopped;
            if (completion.v1_stop.cleanup_error != Error::OK) {
                response.final.error.present = true;
                response.final.error.value = wire::make_error_record(
                    wire_code(completion.v1_stop.cleanup_error), "stop_stream");
            }
            response.quiesced = completion.v1_stop.quiesced;
            // The wire StreamFinal requires the two adopted stream counters.
            // Report the live data-plane observation when this STOP has a
            // data connection; an armed STOP has no physical stream and uses
            // the zero-valued observation.
            TunerStreamCounters stop_counters{};
            for (std::size_t index = 0U; index < stream_client_count; ++index) {
                const StreamClient& stream = *stream_clients[index];
                if (!stream.ticket_valid || stream.lease_id != outstanding.lease_id ||
                    stream.stream_id != completion.v1_stop.stream_id ||
                    !stream.attachment_valid || stream_control == nullptr)
                    continue;
                const auto stats = stream_control->stats(stream.attachment);
                if (stats) {
                    stop_counters = stats.value();
                } else {
                    const auto snapshot =
                        stream_control->final_snapshot(stream.attachment);
                    if (snapshot) stop_counters = snapshot.value().counters;
                }
                break;
            }
            response.final.statistics =
                wire_stream_counters(stop_counters, context_now_ns());
            auto& observed = stream_observations[outstanding.receiver];
            observed.last.present = true;
            observed.last.value = response.final;
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_stop_response, "stop_stream", target);
            if (completion.operation ==
                ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1) {
                // The same lane already ran the physical cleanup.  Stop the
                // writer and publish STREAM_END from the retained final.
                for (std::size_t index = 0U; index < stream_client_count; ++index) {
                    StreamClient& stream = *stream_clients[index];
                    if (!stream.ticket_valid ||
                        stream.lease_id != outstanding.lease_id ||
                        stream.stream_id != completion.v1_stop.stream_id)
                        continue;
                    stream.cleanup_pending = false;
                    stream.cleanup_queued = false;
                    stream.cleanup_complete = true;
                    stream.set_phase(StreamClient::Phase::ending);
                    if (stream_control == nullptr || !stream.attachment_valid) {
                        stream.request_remove();
                        break;
                    }
                    const auto snapshot =
                        stream_control->final_snapshot(stream.attachment);
                    if (snapshot) begin_stream_end(stream, snapshot.value());
                     else stream.request_remove();
                    break;
                }
            }
            return;
        }
        case ControlWorkerOperation::card_status: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_status", reader);
                return;
            }
            wire::CardStatusResponse response;
            response.key.epoch = hello.epoch;
            response.key.reader_id = outstanding.public_id;
            response.current_generation = completion.card_status.reader_generation;
            response.present = completion.card_status.present;
            const auto atr = wire_atr(completion.card_status.atr);
            response.initialized = completion.card_status.present &&
                                   completion.card_status.initialized && atr.present;
            if (response.initialized) {
                response.protocol.present = true;
                response.protocol.value = wire::CardProtocol::t1;
                response.atr = atr;
            }
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_card_status_response, "card_status", reader);
            return;
        }
        case ControlWorkerOperation::card_connect: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_connect", reader);
                return;
            }
            const std::uint64_t handle = completion.card_connect.handle;
            const std::uint64_t generation = completion.card_generation;
            if (handle == 0U || generation == 0U ||
                !client.add_card_handle(handle, generation)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "card_connect", reader);
                return;
            }
            wire::CardSessionResult response;
            build_card_session_result(response, outstanding.public_id, handle,
                                      generation, generation, true, true,
                                      completion.card_connect.atr);
            reply_card_session(client, outstanding.opcode, completion.request_id,
                               response, "card_connect", reader);
            return;
        }
        case ControlWorkerOperation::card_reconnect: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_reconnect", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            if (!client.find_card_handle(completion.card_handle, bound_generation)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::NOT_FOUND), "card_reconnect", reader);
                return;
            }
            const std::uint64_t generation = completion.card_generation != 0U
                                                 ? completion.card_generation
                                                 : bound_generation;
            client.rebind_card_handle(completion.card_handle, generation);
            wire::CardSessionResult response;
            build_card_session_result(response, outstanding.public_id,
                                      completion.card_handle, generation, generation,
                                      true, true, completion.atr);
            reply_card_session(client, outstanding.opcode, completion.request_id,
                               response, "card_reconnect", reader);
            return;
        }
        case ControlWorkerOperation::card_disconnect: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            // Ownership and arguments were accepted before the task ran, so
            // the logical handle is consumed even when cleanup fails.  Only a
            // pre-acceptance rejection is a generic error reply.
            if (completion.error == Error::NOT_FOUND ||
                completion.error == Error::BUSY ||
                completion.error == Error::INVALID_ARGUMENT) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_disconnect", reader);
                return;
            }
            client.remove_card_handle(completion.card_handle);
            wire::CardDisconnectResponse response;
            response.key.epoch = hello.epoch;
            response.key.reader_id = outstanding.public_id;
            response.card_handle = completion.card_handle;
            response.consumed = true;
            response.quiesced = completion.error == Error::OK;
            response.error.present = completion.error != Error::OK;
            if (response.error.present) {
                response.error.value = wire::make_error_record(
                    wire_code(completion.error), "card_disconnect");
            }
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_card_disconnect_response, "card_disconnect",
                         reader);
            return;
        }
        case ControlWorkerOperation::card_reset: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_reset", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            if (!client.find_card_handle(completion.card_handle, bound_generation)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::NOT_FOUND), "card_reset", reader);
                return;
            }
            // An explicit RESET may rebind the handle to the reader's current
            // generation; the successful reply carries the new binding.
            const std::uint64_t generation = completion.card_generation != 0U
                                                 ? completion.card_generation
                                                 : bound_generation;
            client.rebind_card_handle(completion.card_handle, generation);
            wire::CardSessionResult response;
            build_card_session_result(response, outstanding.public_id,
                                      completion.card_handle, generation, generation,
                                      true, true, completion.atr);
            reply_card_session(client, outstanding.opcode, completion.request_id,
                               response, "card_reset", reader);
            return;
        }
        case ControlWorkerOperation::card_transmit: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_transmit", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            (void)client.find_card_handle(completion.card_handle, bound_generation);
            wire::CardTransmitResponse response;
            response.key.epoch = hello.epoch;
            response.key.reader_id = outstanding.public_id;
            response.key.bound_generation = bound_generation;
            response.key.card_handle = completion.card_handle;
            const std::size_t length = std::min<std::size_t>(
                completion.card_response_size, response.response.bytes.size());
            response.response.length = static_cast<std::uint32_t>(length);
            std::memcpy(response.response.bytes.data(), completion.card_response.data(),
                        length);
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_card_transmit_response, "card_transmit", reader);
            return;
        }
        case ControlWorkerOperation::card_begin_transaction: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "begin_transaction", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            (void)client.find_card_handle(completion.card_handle, bound_generation);
            wire::BeginTransactionResponse response;
            response.key.epoch = hello.epoch;
            response.key.reader_id = outstanding.public_id;
            response.key.bound_generation = bound_generation;
            response.key.card_handle = completion.card_handle;
            reply_record(client, outstanding.opcode, completion.request_id, response,
                         &wire::encode_begin_transaction_response, "begin_transaction",
                         reader);
            return;
        }
        case ControlWorkerOperation::card_end_transaction: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "end_transaction", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            if (!client.find_card_handle(completion.card_handle, bound_generation)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::NOT_FOUND), "end_transaction", reader);
                return;
            }
            std::uint64_t current = completion.card_generation != 0U
                                        ? completion.card_generation
                                        : bound_generation;
            bool present = false;
            bool initialized = false;
            CardAtrBytes atr{};
            if (completion.card_status_valid) {
                current = completion.card_status.reader_generation != 0U
                              ? completion.card_status.reader_generation
                              : current;
                present = completion.card_status.present;
                initialized = completion.card_status.initialized;
                atr = completion.card_status.atr;
            }
            wire::CardSessionResult response;
            build_card_session_result(response, outstanding.public_id,
                                      completion.card_handle, bound_generation,
                                      current, present, initialized, atr);
            reply_card_session(client, outstanding.opcode, completion.request_id,
                               response, "end_transaction", reader);
            return;
        }
        case ControlWorkerOperation::card_handle_status: {
            const wire::Scope reader = reader_scope(outstanding.public_id);
            if (completion.error != Error::OK) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(completion.error), "card_handle_status", reader);
                return;
            }
            if (!completion.card_status_valid) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::INTERNAL), "card_handle_status", reader);
                return;
            }
            std::uint64_t bound_generation = 0U;
            if (!client.find_card_handle(completion.card_handle, bound_generation)) {
                reply_error(client, outstanding.opcode, completion.request_id,
                            wire_code(Error::NOT_FOUND), "card_handle_status", reader);
                return;
            }
            wire::CardSessionResult response;
            build_card_session_result(response, outstanding.public_id,
                                      completion.card_handle, bound_generation,
                                       completion.card_status.reader_generation,
                                      completion.card_status.present,
                                      completion.card_status.initialized,
                                      completion.card_status.atr);
            reply_card_session(client, outstanding.opcode, completion.request_id,
                               response, "card_handle_status", reader);
            return;
        }
        default:
            break;
        }
        reply_error(client, outstanding.opcode, completion.request_id,
                    wire_code(Error::INTERNAL), "request", target);
    }

    void handle_completion(const ControlWorkerCompletion& completion) noexcept
    {
        if (completion.kind == ControlWorkerTaskKind::background &&
            completion.operation == ControlWorkerOperation::card_presence) {
            presence_poll_pending = false;
            if (completion.error != Error::OK) record_error(completion.error);
            next_presence_poll = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(kPresencePollIntervalMs);
            return;
        }
        if (completion.kind == ControlWorkerTaskKind::background &&
            completion.operation == ControlWorkerOperation::tuner_shutdown) {
            tuner_shutdown_pending = false;
            tuner_shutdown_complete = true;
            if (completion.error != Error::OK) record_error(completion.error);
            return;
        }
        if (completion.kind == ControlWorkerTaskKind::background &&
            completion.operation == ControlWorkerOperation::card_shutdown) {
            card_shutdown_pending = false;
            card_shutdown_complete = true;
            if (completion.error != Error::OK) record_error(completion.error);
            return;
        }
        if (completion.operation == ControlWorkerOperation::tuner_attach_stream ||
            completion.operation == ControlWorkerOperation::tuner_prepare_attach_v1) {
            handle_stream_completion(completion);
            return;
        }
        if (completion.kind == ControlWorkerTaskKind::cleanup) {
            if (completion.operation == ControlWorkerOperation::tuner_detach_stream ||
                completion.operation == ControlWorkerOperation::tuner_run_stream_cleanup_v1) {
                handle_stream_cleanup_completion(completion);
            } else if (completion.operation == ControlWorkerOperation::tuner_release ||
                completion.operation == ControlWorkerOperation::card_release_connection) {
                handle_cleanup_completion(completion);
            }
            return;
        }

        RetiredClient* retired = find_retired(completion.client_id);
        if (retired != nullptr) {
            handle_orphan_completion(completion, *retired);
            retry_retired_cleanup();
            return;
        }

        for (const auto& client : clients) {
            if (client->id() != completion.client_id) continue;
            if (!client->matches(completion)) return;
            send_completion(*client, completion);
            if (completion.operation == ControlWorkerOperation::tuner_release &&
                completion.error != Error::NOT_READY) {
                notify_stream_detached_by_identity(completion.client_id,
                                                   completion.lease_id);
            }
            return;
        }
        // A stale successful ACQUIRE must not leak a newly created lease. In
        // normal operation a removed client has a retired record; this guard
        // only records an invariant failure if that record was lost.
        if (completion.operation == ControlWorkerOperation::tuner_acquire &&
            completion.error == Error::OK) {
            record_error(Error::INTERNAL);
        }
    }

    void process_completions() noexcept
    {
        workers->drain_wake();
        for (const ControlWorkerLane lane : {ControlWorkerLane::tuner_dev1,
                                             ControlWorkerLane::tuner_dev2,
                                             ControlWorkerLane::card}) {
            ControlWorkerCompletion completion{};
            while (workers->try_pop(lane, completion)) handle_completion(completion);
        }
        retry_retired_cleanup();
        reap_retired();
    }

    void schedule_presence_if_due(std::chrono::steady_clock::time_point now) noexcept
    {
        if (!any_card_handles() || presence_poll_pending || now < next_presence_poll ||
            shutting_down) return;
        ControlWorkerTask task{};
        task.kind = ControlWorkerTaskKind::background;
        task.operation = ControlWorkerOperation::card_presence;
        const auto submitted = workers->submit(ControlWorkerLane::card, task);
        if (submitted) {
            presence_poll_pending = true;
        } else if (submitted.error() != Error::BUSY) {
            record_error(submitted.error());
            next_presence_poll = now + std::chrono::milliseconds(kPresencePollIntervalMs);
        }
    }

    bool any_card_handles() const noexcept
    {
        for (const auto& client : clients) {
            if (!client->card_handles().empty()) return true;
        }
        return false;
    }

    // One nonblocking write attempt per pending response; a response that
    // is not complete within its deadline ends the connection.
    void flush_clients() noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& client : clients) {
            if (!client->tx_pending() || client->closed()) continue;
            if (client->tx_expired(now)) {
                client->close();
                schedule_remove(client->id());
                continue;
            }
            const auto flushed = client->flush();
            if (!flushed) {
                client->close();
                schedule_remove(client->id());
            }
        }
    }

    Result<void> poll_once(Timeout timeout) noexcept
    {
        if (shutdown_complete) return Result<void>::failure(Error::NOT_READY);
        process_completions();
        if (cleanup_error != Error::OK) {
            const Error error = cleanup_error;
            cleanup_error = Error::OK;
            return Result<void>::failure(error);
        }
        schedule_presence_if_due(std::chrono::steady_clock::now());

        const std::size_t control_count = clients.size();
        const std::size_t stream_count = stream_client_count;
        std::vector<pollfd> descriptors;
        descriptors.reserve(control_count + stream_count + 3U);
        descriptors.push_back(pollfd{listener.native_handle(), POLLIN, 0});
        descriptors.push_back(pollfd{stream_listener.native_handle(), POLLIN, 0});
        descriptors.push_back(pollfd{workers->wake_fd(), POLLIN, 0});
        for (std::size_t index = 0U; index < control_count; ++index) {
            const auto& client = clients[index];
            short events = POLLIN;
            if (client->tx_pending()) events = static_cast<short>(events | POLLOUT);
            descriptors.push_back(pollfd{client->fd(), events, 0});
        }
        for (std::size_t index = 0U; index < stream_count; ++index) {
            const auto& client = stream_clients[index];
            short events = POLLIN;
            if (client->tx_pending()) events = static_cast<short>(events | POLLOUT);
            descriptors.push_back(pollfd{client->fd(), events, 0});
        }

        std::uint32_t wait_ms = timeout.milliseconds;
        if (any_card_handles()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_presence_poll && !presence_poll_pending) {
                wait_ms = 0U;
            } else if (now < next_presence_poll) {
                const auto until_poll = std::chrono::duration_cast<std::chrono::milliseconds>(
                    next_presence_poll - now).count();
                wait_ms = std::min<std::uint32_t>(
                    wait_ms, static_cast<std::uint32_t>(std::max<std::int64_t>(0, until_poll)));
            }
        }
        {
            // Wake for the earliest partial-response deadline.
            const auto now = std::chrono::steady_clock::now();
            for (const auto& client : clients) {
                if (!client->tx_pending()) continue;
                const auto until = std::chrono::duration_cast<std::chrono::milliseconds>(
                    client->tx_deadline() - now).count();
                wait_ms = std::min<std::uint32_t>(
                    wait_ms, static_cast<std::uint32_t>(std::max<std::int64_t>(0, until)));
            }
        }
        const int result = ::poll(descriptors.data(), descriptors.size(),
                                  timeout_to_poll(Timeout{wait_ms}));
        if (result < 0) {
#if defined(_WIN32)
            return ::WSAGetLastError() == WSAEINTR ? Result<void>::success() :
                                                      Result<void>::failure(Error::INTERNAL);
#else
            return errno == EINTR ? Result<void>::success() :
                                    Result<void>::failure(Error::INTERNAL);
#endif
        }

        // Drain completions before socket callbacks as well as after them.
        // This keeps the poll thread as the only Client owner while making a
        // worker wake immediately visible without a second poll cycle.
        if (descriptors[2].revents != 0) process_completions();
        if ((descriptors[0].revents & POLLIN) != 0) {
            const auto accepted = accept_ready();
            if (!accepted) return accepted;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            const auto accepted = accept_stream_ready();
            if (!accepted) return accepted;
        }
        for (std::size_t index = control_count; index > 0U; --index) {
            const std::size_t client_index = index - 1U;
            const std::size_t descriptor_index = 3U + client_index;
            const short events = descriptors[descriptor_index].revents;
            if (client_index >= clients.size() || events == 0) continue;
            bool remove = (events & POLLNVAL) != 0;
            if (!remove && (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
                const auto read = clients[client_index]->read_ready();
                remove = !read || clients[client_index]->closed();
            }
            if (remove) {
                clients[client_index]->close();
                schedule_remove(clients[client_index]->id());
            }
        }
        // Responses are written only after every frame of this cycle has been
        // read, so a request pipelined behind an unanswered one is refused.
        flush_clients();
        process_stream_clients(descriptors, 3U + control_count, stream_count);
        remove_pending_clients();
        remove_stream_clients();
        process_completions();
        schedule_presence_if_due(std::chrono::steady_clock::now());
        flush_clients();
        remove_pending_clients();
        if (cleanup_error != Error::OK) {
            const Error error = cleanup_error;
            cleanup_error = Error::OK;
            return Result<void>::failure(error);
        }
        return Result<void>::success();
    }

    Result<void> shutdown() noexcept
    {
        if (shutdown_complete) return Result<void>::success();
        shutting_down = true;
        tuner_service.request_stop();
        card_service.request_stop();
        if (stream_control != nullptr) stream_control->request_stop();
        stream_listener.close();
        // The control listener is the first listener to create the shared
        // product/instance directories.  Close the stream endpoint first so
        // its socket cannot prevent the owning listener from removing those
        // now-empty directories.
        listener.close();
        for (std::size_t index = clients.size(); index > 0U; --index) {
            remove_client(index - 1U);
        }
        for (std::size_t index = stream_client_count; index > 0U; --index) {
            remove_stream_client(index - 1U);
        }

        // Backend operations are finite by contract (tuner tune <=30 s,
        // card APDU <=3 s, and finite USB primitive timeouts). Arbitrarily
        // hung backend code is outside this contract; it cannot be safely
        // recovered with pthread cancellation.
        while (!all_retired_cleanup_done() || !tuner_shutdown_complete ||
               !card_shutdown_complete || stream_client_count != 0U) {
            process_completions();
            retry_retired_cleanup();
            // remove_stream_client() deliberately retains a client if no
            // retired slot can be reserved.  Retry after completions have
            // had a chance to reap an older record; never drop its cleanup
            // ownership merely to make shutdown progress.
            for (std::size_t index = stream_client_count; index > 0U; --index) {
                remove_stream_client(index - 1U);
            }
            if (all_retired_cleanup_done() && workers->idle()) {
                if (!tuner_shutdown_complete && !tuner_shutdown_pending) {
                    ControlWorkerTask task{};
                    task.kind = ControlWorkerTaskKind::background;
                    task.operation = ControlWorkerOperation::tuner_shutdown;
                    const auto submitted = workers->submit(lane_for(task), task);
                    if (submitted) tuner_shutdown_pending = true;
                    else record_error(submitted.error());
                }
                if (!card_shutdown_complete && !card_shutdown_pending) {
                    ControlWorkerTask task{};
                    task.kind = ControlWorkerTaskKind::background;
                    task.operation = ControlWorkerOperation::card_shutdown;
                    const auto submitted = workers->submit(lane_for(task), task);
                    if (submitted) card_shutdown_pending = true;
                    else record_error(submitted.error());
                }
            }
            if (workers->live_thread_count() == 0U) break;
            pollfd descriptor{workers->wake_fd(), POLLIN, 0};
            const int result = ::poll(&descriptor, 1U, 10);
#if defined(_WIN32)
            if (result < 0 && ::WSAGetLastError() != WSAEINTR) {
                record_error(Error::INTERNAL);
            }
#else
            if (result < 0 && errno != EINTR) record_error(Error::INTERNAL);
#endif
        }
        const auto joined = workers->stop_and_join();
        if (!joined && cleanup_error == Error::OK) cleanup_error = joined.error();
        reap_retired();
        shutdown_complete = true;
        if (cleanup_error != Error::OK) {
            const Error error = cleanup_error;
            cleanup_error = Error::OK;
            return Result<void>::failure(error);
        }
        return Result<void>::success();
    }

    SocketListener listener;
    SocketListener stream_listener;
    CardService& card_service;
    TunerService& tuner_service;
    std::unique_ptr<ControlWorkerLanes> workers;
    TunerStreamControl* stream_control;
    // Immutable context: the epoch and producer offered by HELLO, and the
    // canonical descriptors with their receiver mapping.  The scratch records
    // are poll-thread working storage for one request at a time.
    wire::ServerHello hello{};
    std::unique_ptr<ControlContext> context;
    std::unique_ptr<wire::ControlRequest> request_scratch;
    std::unique_ptr<wire::ListResponse> list_scratch;
    std::unique_ptr<wire::StatusResponse> status_scratch;
    std::uint64_t context_start_ms = 0U;
    std::uint64_t snapshot_sequence = 0U;
    struct StreamObservation final {
        wire::Optional<wire::Stream> current;
        wire::Optional<wire::StreamCounters> statistics;
        wire::Optional<wire::StreamFinal> last;
    };
    std::array<StreamObservation, kReceiverCount> stream_observations{};
    wire::Optional<bool> internal_card_inserted{};
    std::vector<std::unique_ptr<Client>> clients;
    std::atomic<std::size_t> client_count{0U};
    // Data clients are fixed-capacity so an accepted socket never depends on
    // an exception-throwing vector growth while the daemon runs.
    std::array<std::unique_ptr<StreamClient>, kMaxStreamConnections> stream_clients{};
    std::size_t stream_client_count = 0U;
    std::vector<CardClientId> pending_removals;
    std::array<std::uint64_t, kMaxStreamConnections> pending_stream_removals{};
    std::size_t pending_stream_removal_count = 0U;
    std::array<RetiredClient, kRetiredClientCapacity> retired_clients{};
    std::array<RetiredStream, kRetiredClientCapacity> retired_streams{};
    CardClientId next_client_id = 1U;
    bool client_ids_exhausted = false;
    bool presence_poll_pending = false;
    bool shutting_down = false;
    bool shutdown_complete = false;
    bool tuner_shutdown_pending = false;
    bool tuner_shutdown_complete = false;
    bool card_shutdown_pending = false;
    bool card_shutdown_complete = false;
    Error cleanup_error = Error::OK;
    std::chrono::steady_clock::time_point next_presence_poll;
};

Result<void> PosixControlServer::Impl::StreamClient::on_frame(
    const wire::Frame& frame) noexcept
{
    return owner_.on_stream_frame(*this, frame);
}

PosixControlServer::Impl::RetiredStream*
PosixControlServer::Impl::allocate_retired_stream(
    std::uint64_t connection_id) noexcept
{
    for (RetiredStream& stream : retired_streams) {
        if (!stream.active) {
            stream = RetiredStream{};
            stream.active = true;
            stream.connection_id = connection_id;
            return &stream;
        }
    }
    return nullptr;
}

PosixControlServer::Impl::RetiredStream*
PosixControlServer::Impl::find_retired_stream(
    std::uint64_t connection_id) noexcept
{
    for (RetiredStream& stream : retired_streams) {
        if (stream.active && stream.connection_id == connection_id) return &stream;
    }
    return nullptr;
}

const PosixControlServer::Impl::RetiredStream*
PosixControlServer::Impl::find_retired_stream(
    std::uint64_t connection_id) const noexcept
{
    for (const RetiredStream& stream : retired_streams) {
        if (stream.active && stream.connection_id == connection_id) return &stream;
    }
    return nullptr;
}

bool PosixControlServer::Impl::retired_stream_cleanup_done(
    const PosixControlServer::Impl::RetiredStream& stream) const noexcept
{
    return !stream.waiting_attach &&
           (!stream.attachment_valid || stream.cleanup_complete);
}

bool PosixControlServer::Impl::all_retired_stream_cleanup_done() const noexcept
{
    for (const RetiredStream& stream : retired_streams) {
        if (stream.active && !retired_stream_cleanup_done(stream)) return false;
    }
    return true;
}

void PosixControlServer::Impl::reap_retired_streams() noexcept
{
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
    bool changed = false;
#endif
    for (RetiredStream& stream : retired_streams) {
        if (stream.active && retired_stream_cleanup_done(stream)) {
            stream = RetiredStream{};
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
            changed = true;
#endif
        }
    }
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
    if (changed) report_stream_capacity();
#endif
}

// The first and only accepted data-connection frame is the typed UDTV
// ATTACH_STREAM request.  It reserves the exact armed stream in the same
// engine, hands the physical prepare to the existing receiver lane, and queues
// the typed response only after that prepare completes.  No old-wire attach is
// accepted or reinterpreted.
Result<void> PosixControlServer::Impl::on_stream_frame(
    StreamClient& client, const wire::Frame& frame) noexcept
{
    if (frame.header.kind != wire::Kind::request ||
        frame.header.opcode != wire::kOpcodeAttachStream ||
        client.attach_completion_seen) {
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    }
    const auto request = wire::decode_attach_request(frame.payload);
    if (!request && request.error() == Error::PROTOCOL_ERROR)
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    client.request_id = frame.header.request_id;
    client.mark_attach_completion_seen();
    if (!request) {
        const auto queued = client.queue_error(client.request_id,
                                               wire_code(request.error()));
        if (!queued) client.request_remove();
        return Result<void>::success();
    }
    if (request.value().epoch != hello.epoch) {
        const auto queued =
            client.queue_error(client.request_id, wire::kWireErrorStaleHandle);
        if (!queued) client.request_remove();
        return Result<void>::success();
    }
    CardClientId owner = 0U;
    std::uint8_t receiver = 0U;
    if (!find_owner_lease(request.value().lease_id, owner, receiver)) {
        const auto queued = client.queue_error(client.request_id,
                                               wire_code(Error::NOT_FOUND));
        if (!queued) client.request_remove();
        return Result<void>::success();
    }
    const auto metadata = tuner_service.tune_metadata(receiver);
    if (!metadata || !metadata.value().current_valid) {
        const auto queued = client.queue_error(client.request_id,
                                               wire_code(Error::NOT_READY));
        if (!queued) client.request_remove();
        return Result<void>::success();
    }
    const auto reserved = tuner_service.reserve_attach_v1(
        owner, request.value().lease_id, kEngineStreamEpoch,
        request.value().stream_id, request.value().attach_token,
        tuner_service.monotonic_ms());
    if (!reserved) {
        const auto queued = client.queue_error(client.request_id,
                                               wire_code(reserved.error()));
        if (!queued) client.request_remove();
        return Result<void>::success();
    }
    // The data plane identity is the exact ticket: owner, lease, reservation,
    // receiver, tune system and the consumed START token.
    TunerAttachment identity{};
    identity.owner_client_id = owner;
    identity.lease_id = request.value().lease_id;
    identity.attachment_id = reserved.value().reservation;
    identity.receiver = receiver;
    identity.system = ipc_system(metadata.value().current.system);
    identity.nonce = reserved.value().token;
    identity.evict_oldest = true;
    client.begin_attach(reserved.value(), identity);
    client.mark_attach_submitted();

    ControlWorkerTask task{};
    task.kind = ControlWorkerTaskKind::client_request;
    task.operation = ControlWorkerOperation::tuner_prepare_attach_v1;
    task.client_id = owner;
    task.connection_id = client.id();
    task.request_id = client.request_id;
    task.receiver = receiver;
    task.lease_id = request.value().lease_id;
    task.stream_id = reserved.value().stream_id;
    task.stream_ticket = reserved.value();
    const auto submitted = workers->submit(lane_for(task), task);
    if (!submitted) {
        // The reservation belongs to this data connection.  Revoke its
        // metadata now; the normal removal path runs the exact-ticket cleanup.
        (void)tuner_service.abort_attach_v1(reserved.value(), submitted.error());
        const auto queued = client.queue_error(client.request_id,
                                               wire_code(submitted.error()));
        if (!queued) client.request_remove();
    }
    return Result<void>::success();
}

PosixControlServer::Impl::StreamClient*
PosixControlServer::Impl::find_live_stream(std::uint64_t connection_id) noexcept
{
    for (std::size_t index = 0U; index < stream_client_count; ++index) {
        if (stream_clients[index]->id() == connection_id)
            return stream_clients[index].get();
    }
    return nullptr;
}

bool PosixControlServer::Impl::find_owner_lease(std::uint64_t lease_id,
                                                CardClientId& owner,
                                                std::uint8_t& receiver) const noexcept
{
    if (lease_id == 0U) return false;
    for (const auto& client : clients) {
        for (const LeaseRecord& lease : client->leases()) {
            if (!lease.active || lease.lease_id != lease_id) continue;
            owner = client->id();
            receiver = lease.receiver;
            return true;
        }
    }
    return false;
}

void PosixControlServer::Impl::begin_stream_end(
    StreamClient& client, const TunerStreamFinalSnapshot& snapshot) noexcept
{
    if (client.end_queued || client.remove_requested()) return;
    client.final_snapshot = snapshot;
    client.final_snapshot_valid = true;
    auto& observed = stream_observations[client.receiver];
    // A delayed old connection cannot overwrite the current stream's cache.
    if (observed.current.present && observed.current.value.id == client.stream_id) {
        observed.last.present = true;
        auto& final = observed.last.value;
        final = wire::StreamFinal{};
        final.id = client.stream_id;
        final.tune_revision = client.ticket.revision;
        const auto terminal = static_cast<TunerStreamTerminal>(snapshot.terminal);
        final.reason = wire_end_reason(terminal);
        if (final.reason != wire::StreamEndReason::stopped &&
            final.reason != wire::StreamEndReason::shutdown) {
            final.error.present = true;
            final.error.value = wire::make_error_record(
                static_cast<std::uint32_t>(stream_end_error(terminal)), "stream");
        }
        final.statistics = wire_stream_counters(snapshot.counters, context_now_ns());
    }
    client.data_eof = false;
    client.set_phase(StreamClient::Phase::ending);
}

void PosixControlServer::Impl::on_attach_ack_sent(StreamClient& client) noexcept
{
    if (!client.ticket_valid || !client.prepared) {
        client.request_remove();
        return;
    }
    const auto committed = tuner_service.commit_attach_v1(client.ticket);
    if (committed) {
        client.committed = true;
        client.set_phase(StreamClient::Phase::active);
        if (stream_control != nullptr) {
            const auto stats = stream_control->stats(client.attachment);
            auto& observed = stream_observations[client.receiver];
            if (stats && observed.current.present &&
                observed.current.value.id == client.stream_id) {
                observed.statistics.present = true;
                observed.statistics.value = wire_stream_counters(stats.value(), context_now_ns());
            }
        }
        return;
    }
    // STOP/owner teardown linearized before the publication point: never
    // publish active; finish through the same exact-ticket cleanup.
    queue_live_stream_cleanup(client);
}

void PosixControlServer::Impl::notify_stream_detached(
    std::uint64_t owner_client_id, std::uint64_t lease_id,
    const TunerStreamFinalSnapshot& snapshot) noexcept
{
    for (std::size_t index = 0U; index < stream_client_count; ++index) {
        StreamClient& stream = *stream_clients[index];
        if (stream.attachment_valid &&
            stream.attachment.owner_client_id == owner_client_id &&
            stream.attachment.lease_id == lease_id) {
            begin_stream_end(stream, snapshot);
        }
    }
}

void PosixControlServer::Impl::notify_stream_detached_by_identity(
    std::uint64_t owner_client_id, std::uint64_t lease_id) noexcept
{
    for (std::size_t index = 0U; index < stream_client_count; ++index) {
        StreamClient& stream = *stream_clients[index];
        if (!stream.attachment_valid ||
            stream.attachment.owner_client_id != owner_client_id ||
            stream.attachment.lease_id != lease_id)
            continue;
        if (stream_control == nullptr) {
            record_error(Error::INTERNAL);
            stream.request_remove();
            continue;
        }
        const auto snapshot = stream_control->final_snapshot(stream.attachment);
        if (snapshot) {
            begin_stream_end(stream, snapshot.value());
        } else {
            record_error(snapshot.error());
            stream.request_remove();
        }
    }
}

void PosixControlServer::Impl::handle_stream_completion(
    const ControlWorkerCompletion& completion) noexcept
{
    if (completion.operation != ControlWorkerOperation::tuner_prepare_attach_v1)
        return;
    StreamClient* live = find_live_stream(completion.connection_id);
    if (live != nullptr && live->ticket_valid &&
        live->request_id == completion.request_id) {
        live->mark_attach_completion_seen();
        if (completion.error == Error::OK) {
            live->prepared = true;
            const auto queued = live->queue_attach_response();
            if (!queued) live->request_remove();
        } else {
            const auto queued =
                live->queue_error(live->request_id, wire_code(completion.error));
            if (!queued) live->request_remove();
        }
        return;
    }
    RetiredStream* retired = find_retired_stream(completion.connection_id);
    if (retired == nullptr) {
        if (completion.error == Error::OK) record_error(Error::INTERNAL);
        return;
    }
    retired->waiting_attach = false;
    // The data connection closed while the physical prepare was in flight.
    // The exact ticket cleanup is still this server's responsibility.
    retry_stream_cleanup();
}

void PosixControlServer::Impl::handle_stream_cleanup_completion(
    const ControlWorkerCompletion& completion) noexcept
{
    StreamClient* live = nullptr;
    for (std::size_t index = 0U; index < stream_client_count; ++index) {
        if (stream_clients[index]->id() == completion.connection_id) {
            live = stream_clients[index].get();
            break;
        }
    }
    if (completion.error != Error::OK && completion.error != Error::NOT_FOUND)
        record_error(completion.error);
    if (live != nullptr) {
        live->cleanup_queued = false;
        live->cleanup_pending = false;
        live->cleanup_complete = true;
        if (stream_control == nullptr) {
            live->request_remove();
            return;
        }
        const auto snapshot = stream_control->final_snapshot(live->attachment);
        if (snapshot) begin_stream_end(*live, snapshot.value());
        else {
            record_error(snapshot.error());
            live->request_remove();
        }
        return;
    }
    RetiredStream* retired = find_retired_stream(completion.connection_id);
    if (retired == nullptr) return;
    if (retired->attachment_valid && stream_control != nullptr) {
        const auto released = stream_control->release_final(retired->attachment);
        if (!released && released.error() != Error::UNSUPPORTED &&
            released.error() != Error::NOT_FOUND)
            record_error(released.error());
    }
    retired->cleanup_queued = false;
    retired->cleanup_complete = true;
    retired->active = false;
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
    report_stream_capacity();
#endif
}

void PosixControlServer::Impl::retry_stream_cleanup() noexcept
{
    for (RetiredStream& retired : retired_streams) {
        if (!retired.active || retired.waiting_attach ||
            retired.cleanup_queued || !retired.ticket_valid)
            continue;
        ControlWorkerTask task{};
        task.kind = ControlWorkerTaskKind::cleanup;
        task.operation = ControlWorkerOperation::tuner_run_stream_cleanup_v1;
        task.client_id = retired.ticket.client_id;
        task.connection_id = retired.connection_id;
        task.receiver = retired.receiver;
        task.lease_id = retired.lease_id;
        task.stream_id = retired.ticket.stream_id;
        task.stream_ticket = retired.ticket;
        queue_cleanup_task(task, retired.cleanup_queued);
    }
    for (std::size_t index = 0U; index < stream_client_count; ++index) {
        StreamClient& stream = *stream_clients[index];
        if (!stream.cleanup_pending || stream.cleanup_queued ||
            !stream.ticket_valid)
            continue;
        ControlWorkerTask task{};
        task.kind = ControlWorkerTaskKind::cleanup;
        task.operation = ControlWorkerOperation::tuner_run_stream_cleanup_v1;
        task.client_id = stream.ticket.client_id;
        task.connection_id = stream.id();
        task.receiver = stream.receiver;
        task.lease_id = stream.lease_id;
        task.stream_id = stream.stream_id;
        task.stream_ticket = stream.ticket;
        queue_cleanup_task(task, stream.cleanup_queued);
    }
}

void PosixControlServer::Impl::queue_live_stream_cleanup(StreamClient& client) noexcept
{
    if (client.cleanup_complete || client.cleanup_pending) return;
    if (client.ticket_valid) {
        // A metadata entrance issues the single exact-ticket cleanup; the
        // worker below performs the physical stop/detach.
        if (!client.committed) {
            const auto aborted =
                tuner_service.abort_attach_v1(client.ticket, Error::DISCONNECTED);
            if (!aborted && aborted.error() != Error::NOT_FOUND)
                record_error(aborted.error());
        } else {
            const auto stopped = tuner_service.stop_stream_v1(
                client.ticket.client_id, client.lease_id, client.stream_id);
            if (!stopped && stopped.error() != Error::NOT_FOUND)
                record_error(stopped.error());
        }
    }
    client.cleanup_pending = true;
    if (!client.ticket_valid) {
        client.cleanup_complete = true;
        return;
    }
    ControlWorkerTask task{};
    task.kind = ControlWorkerTaskKind::cleanup;
    task.operation = ControlWorkerOperation::tuner_run_stream_cleanup_v1;
    task.client_id = client.ticket.client_id;
    task.connection_id = client.id();
    task.receiver = client.receiver;
    task.lease_id = client.lease_id;
    task.stream_id = client.stream_id;
    task.stream_ticket = client.ticket;
    queue_cleanup_task(task, client.cleanup_queued);
}

void PosixControlServer::Impl::begin_stream_cleanup(StreamClient& client) noexcept
{
    if (client.cleanup_complete) return;
    if (!client.cleanup_pending && client.ticket_valid) {
        // A data disconnect is the owner teardown for this exact stream: issue
        // the one metadata cleanup before the retired record is published.
        if (!client.committed) {
            const auto aborted =
                tuner_service.abort_attach_v1(client.ticket, Error::DISCONNECTED);
            if (!aborted && aborted.error() != Error::NOT_FOUND)
                record_error(aborted.error());
        } else {
            const auto stopped = tuner_service.stop_stream_v1(
                client.ticket.client_id, client.lease_id, client.stream_id);
            if (!stopped && stopped.error() != Error::NOT_FOUND)
                record_error(stopped.error());
        }
    }
    client.cleanup_pending = true;
    if (!client.ticket_valid) {
        client.cleanup_complete = true;
        return;
    }
    // An attached or in-flight client must not be removed until its retired
    // cleanup record has been reserved.  Admission maintains this invariant;
    // keep the defensive check here so a future capacity change cannot orphan
    // physical stream ownership.
    if (!stream_retired_capacity_available()) {
        record_error(Error::INTERNAL);
        return;
    }
    RetiredStream* retired = allocate_retired_stream(client.id());
    if (retired == nullptr) {
        record_error(Error::INTERNAL);
        return;
    }
    retired->owner_client_id = client.owner_client_id;
    retired->lease_id = client.lease_id;
    retired->receiver = client.receiver;
    retired->ticket = client.ticket;
    retired->ticket_valid = true;
    retired->attachment_valid = client.attachment_valid;
    retired->attachment = client.attachment;
    retired->cleanup_queued = client.cleanup_queued;
    if (!retired->cleanup_queued) {
        ControlWorkerTask task{};
        task.kind = ControlWorkerTaskKind::cleanup;
        task.operation = ControlWorkerOperation::tuner_run_stream_cleanup_v1;
        task.client_id = client.ticket.client_id;
        task.connection_id = client.id();
        task.receiver = client.receiver;
        task.lease_id = client.lease_id;
        task.stream_id = client.stream_id;
        task.stream_ticket = client.ticket;
        queue_cleanup_task(task, retired->cleanup_queued);
    }
}

void PosixControlServer::Impl::schedule_stream_remove(
    std::uint64_t connection_id) noexcept
{
    for (std::size_t index = 0U; index < pending_stream_removal_count; ++index) {
        if (pending_stream_removals[index] == connection_id) return;
    }
    if (pending_stream_removal_count < pending_stream_removals.size()) {
        pending_stream_removals[pending_stream_removal_count++] = connection_id;
    } else {
        record_error(Error::INTERNAL);
    }
}

bool PosixControlServer::Impl::stream_retired_capacity_available() const noexcept
{
    std::size_t retired_count = 0U;
    for (const RetiredStream& stream : retired_streams) {
        if (stream.active) ++retired_count;
    }
    return stream_client_count + retired_count < retired_streams.size();
}

#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
void PosixControlServer::Impl::report_stream_capacity() noexcept
{
    std::size_t retired_count = 0U;
    for (const RetiredStream& stream : retired_streams) {
        if (stream.active) ++retired_count;
    }
    report_stream_count(stream_client_count, retired_count);
}
#endif

void PosixControlServer::Impl::remove_stream_client(std::size_t index) noexcept
{
    if (index >= stream_client_count) return;
    StreamClient& client = *stream_clients[index];
    client.close();
    if (client.cleanup_complete && client.attachment_valid && stream_control != nullptr) {
        const auto released = stream_control->release_final(client.attachment);
        if (!released && released.error() != Error::UNSUPPORTED &&
            released.error() != Error::NOT_FOUND)
            record_error(released.error());
    }
    if (!client.cleanup_complete &&
        (client.attachment_valid || client.is_attaching() || client.ticket_valid))
        begin_stream_cleanup(client);
    for (std::size_t current = index; current + 1U < stream_client_count; ++current)
        stream_clients[current] = std::move(stream_clients[current + 1U]);
    stream_clients[stream_client_count - 1U].reset();
    --stream_client_count;
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
    report_stream_capacity();
#endif
    retry_stream_cleanup();
}

void PosixControlServer::Impl::remove_stream_clients() noexcept
{
    for (std::size_t pending = 0U; pending < pending_stream_removal_count; ++pending) {
        const std::uint64_t id = pending_stream_removals[pending];
        for (std::size_t index = stream_client_count; index > 0U; --index) {
            if (stream_clients[index - 1U]->id() == id) {
                remove_stream_client(index - 1U);
                break;
            }
        }
    }
    pending_stream_removal_count = 0U;
}

Result<void> PosixControlServer::Impl::accept_stream_ready() noexcept
{
    while (true) {
        auto accepted = stream_listener.accept(Timeout{0U});
        if (!accepted) {
            return accepted.error() == Error::TIMEOUT
                ? Result<void>::success() : Result<void>::failure(accepted.error());
        }
        if (stream_client_count >= kMaxStreamConnections ||
            !stream_retired_capacity_available() || client_ids_exhausted) {
            accepted.value().close();
            continue;
        }
        const std::uint64_t id = next_client_id;
        if (next_client_id == std::numeric_limits<std::uint64_t>::max())
            client_ids_exhausted = true;
        else
            ++next_client_id;
        std::unique_ptr<StreamClient> client(
            new (std::nothrow) StreamClient(*this, id, std::move(accepted.value())));
        if (!client) {
            accepted.value().close();
            record_error(Error::INTERNAL);
            continue;
        }
        stream_clients[stream_client_count++] = std::move(client);
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
        report_stream_capacity();
#endif
    }
}

void PosixControlServer::Impl::process_stream_clients(
    const std::vector<pollfd>& descriptors, std::size_t descriptor_base,
    std::size_t count) noexcept
{
    for (std::size_t index = 0U; index < count && index < stream_client_count; ++index) {
        StreamClient& client = *stream_clients[index];
        const short events = descriptors[descriptor_base + index].revents;
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            client.request_remove();
        } else {
            // At most one bounded write attempt is made for this client in a
            // poll cycle.  In particular, do not spend a second budget after
            // queueing TS_DATA or STREAM_END; POLLOUT schedules the next
            // bounded flush on the following cycle.
            if (client.tx_pending()) {
                const auto flushed = client.flush(kStreamWriteBudget);
                if (!flushed) {
                    client.request_remove();
                }
            }
            if (!client.remove_requested() && client.can_read() &&
                (events & POLLIN) != 0) {
                const auto read = client.read_ready();
#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
                if (read && read.value() == 0U)
                    report_stream_partial(client.id());
#endif
                if (!read && read.error() != Error::TIMEOUT) {
                    client.request_remove();
                }
            }
            if (!client.remove_requested() &&
                (client.is_active() || (client.phase() == StreamClient::Phase::ending &&
                                        client.final_snapshot_valid)) &&
                !client.tx_pending() && !client.ack_pending()) {
                if (stream_control == nullptr) {
                    client.request_remove();
                } else {
                    const auto read = stream_control->read(
                        client.attachment, client.read_buffer(), Timeout{0U});
                    if (read) {
                        if (read.value().bytes != 0U) {
                            const auto stats = stream_control->stats(client.attachment);
                            if (stats) {
                                client.last_queue_drops = stats.value().queue_drops;
                                auto& observed = stream_observations[client.receiver];
                                if (observed.current.present &&
                                    observed.current.value.id == client.stream_id) {
                                    observed.statistics.present = true;
                                    observed.statistics.value = wire_stream_counters(
                                        stats.value(), context_now_ns());
                                }
                            }
                            const auto queued = client.queue_data(
                                client.sequence, client.last_queue_drops,
                                ByteView{client.read_buffer().data,
                                         read.value().bytes});
                            if (!queued) {
                                client.request_remove();
                            }
                            else ++client.sequence;
                        } else if (read.value().eof ||
                                   read.value().terminal != TunerStreamTerminal::none) {
                            if (client.is_active()) {
                                client.set_phase(StreamClient::Phase::ending);
                                queue_live_stream_cleanup(client);
                            } else {
                                client.data_eof = true;
                            }
                        }
                    } else if (read.error() == Error::NOT_FOUND) {
                        if (client.phase() == StreamClient::Phase::ending) {
                            client.data_eof = true;
                        } else {
                            notify_stream_detached_by_identity(
                                client.attachment.owner_client_id,
                                client.attachment.lease_id);
                        }
                    } else if (read.error() != Error::TIMEOUT) {
                        record_error(read.error());
                        client.request_remove();
                    }
                }
            }
            if (!client.remove_requested() && client.phase() == StreamClient::Phase::ending &&
                client.final_snapshot_valid && client.data_eof && !client.end_queued &&
                !client.tx_pending()) {
                wire::StreamFinal end{};
                end.id = client.stream_id;
                end.tune_revision =
                    client.ticket_valid ? client.ticket.revision : 0U;
                const auto terminal = static_cast<TunerStreamTerminal>(
                    client.final_snapshot.terminal);
                end.reason = wire_end_reason(terminal);
                if (end.reason != wire::StreamEndReason::stopped &&
                    end.reason != wire::StreamEndReason::shutdown) {
                    end.error.present = true;
                    end.error.value = wire::make_error_record(
                        static_cast<std::uint32_t>(stream_end_error(terminal)), "stream");
                }
                end.statistics =
                    wire_stream_counters(client.final_snapshot.counters,
                                         context_now_ns());
                const auto queued = client.queue_end(end);
                if (!queued) client.request_remove();
                else client.end_queued = true;
            }
        }
        if (client.remove_requested()) schedule_stream_remove(client.id());
    }
}

namespace {

// The context must be a valid canonical LIST whose receivers each map to a
// distinct receiver of the tuner service; nothing is padded or guessed.
bool valid_context(const ControlContext& context, const TunerService& tuner_service) noexcept
{
    namespace wire = dtv::userland::wire;
    const auto valid_tuner_lane = [](ControlWorkerLane lane) noexcept {
        return lane == ControlWorkerLane::tuner_dev1 ||
               lane == ControlWorkerLane::tuner_dev2;
    };
    if (wire::validate_list_response(context.list) != Error::OK) return false;
    if (context.execution.receiver_count > context.execution.receiver_lane.size()) return false;
    if (context.execution.reader_lane != ControlWorkerLane::card ||
        !valid_tuner_lane(context.execution.status_lane) ||
        !valid_tuner_lane(context.execution.lnb_lane) ||
        !valid_tuner_lane(context.execution.tuner_shutdown_lane) ||
        context.execution.card_shutdown_lane != ControlWorkerLane::card)
        return false;
    if (context.instance.present &&
        (context.instance.value.length == 0U ||
         context.instance.value.length > context.instance.value.bytes.size()))
        return false;
    if (!context.list.receivers.present) return true;
    std::array<bool, kReceiverCount> used{};
    for (std::size_t index = 0U; index < context.list.receivers.value.count; ++index) {
        const std::uint8_t receiver = context.receiver_index[index];
        if (receiver >= context.execution.receiver_count ||
            receiver >= tuner_service.receiver_count() || receiver >= used.size() ||
            used[receiver] || !valid_tuner_lane(context.execution.receiver_lane[receiver]))
            return false;
        used[receiver] = true;
    }
    return true;
}

}  // namespace

Result<std::unique_ptr<PosixControlServer>> PosixControlServer::create(
    const EndpointConfig& endpoint, CardService& card_service,
    TunerService& tuner_service, const ControlContext& context,
    TunerStreamControl* stream_control) noexcept
{
    if (!valid_context(context, tuner_service)) {
        return Result<std::unique_ptr<PosixControlServer>>::failure(
            Error::INVALID_ARGUMENT);
    }
    std::unique_ptr<ControlContext> owned(new (std::nothrow) ControlContext(context));
    std::unique_ptr<dtv::userland::wire::ControlRequest> request(
        new (std::nothrow) dtv::userland::wire::ControlRequest());
    std::unique_ptr<dtv::userland::wire::ListResponse> list(
        new (std::nothrow) dtv::userland::wire::ListResponse());
    std::unique_ptr<dtv::userland::wire::StatusResponse> status(
        new (std::nothrow) dtv::userland::wire::StatusResponse());
    if (!owned || !request || !list || !status) {
        return Result<std::unique_ptr<PosixControlServer>>::failure(Error::INTERNAL);
    }
    owned->list.epoch = context.epoch;
    auto listener = SocketListener::listen(endpoint);
    if (!listener) {
        return Result<std::unique_ptr<PosixControlServer>>::failure(listener.error());
    }
    const EndpointConfig stream_endpoint{
        endpoint.runtime_directory, endpoint.instance, kStreamEndpointName,
        endpoint.access};
    auto stream_listener = SocketListener::listen(stream_endpoint);
    if (!stream_listener) {
        return Result<std::unique_ptr<PosixControlServer>>::failure(
            stream_listener.error());
    }
    // The v1 stream contract is selected once before any ACQUIRE and before the
    // worker lanes start.  The engine-local u64 epoch is an internal ticket
    // tag; the 16-byte wire epoch remains the authority validated by the
    // control connection, not this value.
    const auto contract = tuner_service.configure_stream_contract(
        TunerService::TunerStreamContract::v1, kEngineStreamEpoch);
    if (!contract) {
        return Result<std::unique_ptr<PosixControlServer>>::failure(contract.error());
    }
    auto workers = ControlWorkerLanes::create(card_service, tuner_service,
                                             endpoint.runtime_directory,
                                             context.execution);
    if (!workers) {
        // listener remains owned here and therefore closes/unlinks the endpoint
        // transactionally before returning the worker startup error.
        return Result<std::unique_ptr<PosixControlServer>>::failure(workers.error());
    }
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl(
        std::move(listener.value()), std::move(stream_listener.value()), card_service,
        tuner_service, std::move(workers.value()), stream_control));
    if (!impl) return Result<std::unique_ptr<PosixControlServer>>::failure(Error::INTERNAL);
    impl->hello.epoch = context.epoch;
    impl->hello.producer = context.list.producer;
    // CARD_SERVICE is offered only because every adopted card request is now
    // decoded and served: CARD_STATUS/CONNECT/RECONNECT/DISCONNECT/RESET/
    // TRANSMIT/BEGIN_TRANSACTION/END_TRANSACTION/CARD_HANDLE_STATUS all reach
    // the card lane.  STATE_EVENT and PID_FILTER remain unconnected.
    impl->hello.features = wire::kFeatureCardService;
    impl->context = std::move(owned);
    impl->request_scratch = std::move(request);
    impl->list_scratch = std::move(list);
    impl->status_scratch = std::move(status);
    // Origin of the context-relative clock, in the engine clock domain.
    impl->context_start_ms = tuner_service.monotonic_ms();
    std::unique_ptr<PosixControlServer> server(
        new (std::nothrow) PosixControlServer(std::move(impl)));
    if (!server) return Result<std::unique_ptr<PosixControlServer>>::failure(Error::INTERNAL);
    return Result<std::unique_ptr<PosixControlServer>>::success(std::move(server));
}

PosixControlServer::PosixControlServer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

PosixControlServer::~PosixControlServer() noexcept
{
    (void)shutdown();
}

Result<void> PosixControlServer::poll_once(Timeout timeout) noexcept
{
    if (!impl_) return Result<void>::failure(Error::NOT_READY);
    return impl_->poll_once(timeout);
}

Result<void> PosixControlServer::shutdown() noexcept
{
    if (!impl_) return Result<void>::success();
    return impl_->shutdown();
}

const PathChar* PosixControlServer::endpoint_path() const noexcept
{
    return impl_ ? impl_->listener.endpoint_path() : empty_path();
}

const PathChar* PosixControlServer::stream_endpoint_path() const noexcept
{
    return impl_ ? impl_->stream_listener.endpoint_path() : empty_path();
}

std::size_t PosixControlServer::connection_count() const noexcept
{
    return impl_ ? impl_->client_count.load(std::memory_order_acquire) : 0U;
}

#if defined(PX4_CONTROL_SERVER_TEST_ACCESS)
void set_control_server_test_hooks(ControlServerTestHooks* hooks) noexcept
{
    test_hooks = hooks;
}
#endif

}  // namespace px4::userland::ipc::posix
