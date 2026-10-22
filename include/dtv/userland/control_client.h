// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_CONTROL_CLIENT_H
#define DTV_USERLAND_CONTROL_CLIENT_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

// Caller-owned transport. read_some returns a positive byte count, zero at
// end-of-stream, or an error. The client borrows this object.
class ClientTransport {
public:
    virtual ~ClientTransport() noexcept = default;
    virtual Result<void> write_all(ByteView bytes) noexcept = 0;
    virtual Result<std::size_t> read_some(MutableByteView bytes) noexcept = 0;
    virtual void close() noexcept = 0;
};

// Caller-owned transport for operations with a bounded monotonic deadline.
// The client passes the remaining operation budget to each blocking call.
class TimedClientTransport {
public:
    virtual ~TimedClientTransport() noexcept = default;
    virtual Result<void> write_all(ByteView bytes, Timeout timeout) noexcept = 0;
    virtual Result<std::size_t> read_some(MutableByteView bytes,
                                          Timeout timeout) noexcept = 0;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void close() noexcept = 0;
};

struct StatusSnapshot final {
    ListResponse list;
    StatusResponse status;
};

struct StatusFetchResult final {
    StatusSnapshot snapshot;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Fetches HELLO, enclosure LIST and enclosure STATUS on one transport. On
// failure the transport is closed and snapshot remains default-initialized;
// on success the caller retains the open transport. No request is retried.
StatusFetchResult fetch_status(ClientTransport& transport) noexcept;

// Uses one total monotonic deadline for HELLO, LIST and STATUS. On failure
// the transport is closed and snapshot remains default-initialized; on
// success the caller retains the open transport. No request is retried.
StatusFetchResult fetch_status(TimedClientTransport& transport, Timeout timeout) noexcept;

// Requests the required HELLO feature mask using the same status sequence and
// deadline. Missing required features fail with UNSUPPORTED and close the
// transport; the zero-mask overload above keeps its original behavior.
StatusFetchResult fetch_status(TimedClientTransport& transport, Timeout timeout,
                               std::uint64_t required_features) noexcept;

struct LnbPowerResult final {
    LnbPowerResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Fetches status and sends one independent LNB power request on that
// connection. The response reports the requested control state, not measured
// voltage; failures close the transport and are never retried.
LnbPowerResult set_lnb_power(TimedClientTransport& transport, bool enabled,
                             Timeout timeout) noexcept;

struct CardClientOptions final {
    std::uint16_t reader_id = 0U;
    Share share = Share::shared;
    // UDTV 1.0 currently accepts only the T=1 bit.
    std::uint8_t preferred_protocols = 1U;
    // Optional constraints checked against the fetched status snapshot. Empty
    // views keep the original behavior; positive-size views must stay valid
    // for the call.
    ByteView expected_product{};
    ByteView expected_instance{};
};

struct CardClientSession final {
    Uuid epoch{};
    std::uint16_t reader_id = 0U;
    std::uint64_t card_handle = 0U;
    std::uint64_t bound_generation = 0U;
    Optional<CardProtocol> protocol;
    std::uint32_t next_request_id = 6U;
};

struct CardOpenResult final {
    CardClientSession session;
    CardStatusResponse reader_status;
    CardSessionResult connection;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Requires CARD_SERVICE, verifies the reader in the same connection's status
// snapshot, then sends CARD_STATUS and CARD_CONNECT. The connection stays open
// on success; failures close it and are never retried.
CardOpenResult begin_card(TimedClientTransport& transport,
                          const CardClientOptions& options,
                          Timeout timeout) noexcept;

struct CardStatusResult final {
    CardStatusResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

CardStatusResult card_status(TimedClientTransport& transport,
                             CardClientSession& session,
                             Timeout timeout) noexcept;

struct CardDisconnectResult final {
    // A matching consumed completion clears the session even when cleanup fails;
    // response and remote_error_code preserve the daemon's physical outcome.
    CardDisconnectResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

CardDisconnectResult card_disconnect(TimedClientTransport& transport,
                                     CardClientSession& session,
                                     Disposition disposition,
                                     Timeout timeout) noexcept;

struct RecordingOptions final {
    std::uint16_t receiver_id = 0U;
    TuneRequest tune{};
};

// One accepted recording session. The caller owns this value and uses its
// attach token only for the data connection's ATTACH request.
struct RecordingSession final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    std::uint64_t tune_revision = 0U;
    std::uint64_t stream_id = 0U;
    Uuid attach_token{};
    std::uint32_t expires_after_ms = 0U;
    TuneResult tune_result{};
    std::uint32_t next_request_id = 7U;
};

struct RecordingControlResult final {
    RecordingSession session;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

using RecordingStatusValidator = Error (*)(const StatusSnapshot&, void*) noexcept;

// Performs HELLO/LIST/STATUS with one timeout budget, then ACQUIRE/TUNE/START
// with a fresh budget of the same size for compatibility with the existing
// recording helper. The transport remains open on success and closes on
// failure. A successful START means armed/accepted, not active TS delivery;
// data connection and ATTACH remain caller-owned.
RecordingControlResult begin_recording(TimedClientTransport& transport,
                                       const RecordingOptions& options,
                                       Timeout timeout,
                                       RecordingStatusValidator validator = nullptr,
                                       void* validator_context = nullptr) noexcept;

struct StopControlResult final {
    StopResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Sends one STOP for the session stream. It does not release the lease.
StopControlResult stop_recording(TimedClientTransport& transport,
                                 RecordingSession& session,
                                 Timeout timeout) noexcept;

struct ReleaseControlResult final {
    ReleaseResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Sends one RELEASE for the session lease. No automatic retry is performed.
ReleaseControlResult release_recording(TimedClientTransport& transport,
                                       RecordingSession& session,
                                       Timeout timeout) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CLIENT_H
