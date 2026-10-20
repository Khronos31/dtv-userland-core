// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_CONTROL_CLIENT_H
#define DTV_USERLAND_CONTROL_CLIENT_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

class ClientTransport {
public:
    virtual ~ClientTransport() noexcept = default;
    virtual Result<void> write_all(ByteView bytes, Timeout timeout) noexcept = 0;
    virtual Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept = 0;
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

// Uses one transport and one total monotonic deadline for HELLO, LIST and
// STATUS. Every failure closes the connection; requests are never retried.
StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout) noexcept;

// Same HELLO/LIST/STATUS sequence, but HELLO declares required_features and the
// result is UNSUPPORTED, with the connection closed, when the negotiated
// features do not contain every required bit. The zero-feature overload above
// keeps its original behavior.
StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout,
                               std::uint64_t required_features) noexcept;

// True when the canonical LIST producer.product and STATUS context.instance
// match the supplied expectations. A zero-size view imposes no constraint; a
// positive size with a null data pointer never matches. The product is
// compared as its canonical slug and the instance is a case-preserving ASCII
// byte sequence; neither is rewritten here.
bool status_identity_matches(const StatusSnapshot& snapshot,
                             ByteView expected_product,
                             ByteView expected_instance) noexcept;

struct LnbPowerResult final {
    LnbPowerResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One independent enclosure LNB power request after HELLO/LIST/STATUS.
LnbPowerResult set_lnb_power(ClientTransport& transport, bool enabled,
                             Timeout timeout) noexcept;

// Inputs for one normal recording lease.  epoch/lease_id in tune are assigned
// from the ACQUIRE response, so the caller sets only the tune body.
struct RecordingOptions final {
    std::uint16_t receiver_id = 0U;
    TuneRequest tune{};
};

// One accepted normal recording session.  It carries no attach token to
// STATUS or logs; the token stays in this caller-owned value for the ATTACH.
struct RecordingSession final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    std::uint64_t tune_revision = 0U;
    std::uint64_t stream_id = 0U;
    Uuid attach_token{};
    std::uint32_t expires_after_ms = 0U;
    TuneResult tune_result{};
    // Next strictly increasing control request ID on this connection.
    std::uint32_t next_request_id = 7U;
};

struct RecordingControlResult final {
    RecordingSession session;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

using RecordingStatusValidator = Error (*)(const StatusSnapshot&, void*) noexcept;

// HELLO, LIST, STATUS, ACQUIRE, TUNE and START on one control connection.  The
// transport stays open on success; every failure closes it.  No request is
// retried automatically.
RecordingControlResult begin_recording(ClientTransport& transport,
                                       const RecordingOptions& options,
                                       Timeout timeout) noexcept;
RecordingControlResult begin_recording(ClientTransport& transport,
                                       const RecordingOptions& options,
                                       Timeout timeout,
                                       RecordingStatusValidator validator,
                                       void* validator_context) noexcept;

struct StopControlResult final {
    StopResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed STOP for the session's exact stream.  It does not release the
// lease and performs no retry.
StopControlResult stop_recording(ClientTransport& transport,
                                 RecordingSession& session,
                                 Timeout timeout) noexcept;

struct ReleaseControlResult final {
    ReleaseResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed RELEASE for the session's lease.  It performs no retry.
ReleaseControlResult release_recording(ClientTransport& transport,
                                       RecordingSession& session,
                                       Timeout timeout) noexcept;

// Inputs for one card reader session.  The epoch, owner handle and bound
// generation are assigned from the CARD_STATUS/CONNECT responses, so the
// caller sets only the reader and the connect preferences.
struct CardClientOptions final {
    std::uint16_t reader_id = 0U;
    Share share = Share::shared;
    // Preferred protocol mask; UDTV 1.0 accepts only bit0 (T=1).
    std::uint8_t preferred_protocols = 1U;
    // Optional canonical identity constraint, checked against the LIST/STATUS
    // already fetched before CARD_STATUS/CARD_CONNECT. A zero-size view keeps
    // the original behavior. Positive-size views must stay valid for the call.
    ByteView expected_product{};
    ByteView expected_instance{};
};

// One open card session on a control connection that negotiated CARD_SERVICE.
// The epoch is the daemon epoch bound at HELLO; card_handle and
// bound_generation are the owner handle issued by CARD_CONNECT and the
// generation it was bound to.  A successful CARD_DISCONNECT consumes the
// handle and zeroes both fields; nothing is reconnected automatically.
struct CardClientSession final {
    Uuid epoch{};
    std::uint16_t reader_id = 0U;
    std::uint64_t card_handle = 0U;
    std::uint64_t bound_generation = 0U;
    // Next strictly increasing control request ID on this connection.
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

// HELLO that requires CARD_SERVICE, LIST and STATUS on the same connection,
// validation of the requested reader against the canonical snapshot, then
// CARD_STATUS and CARD_CONNECT.  The transport stays open on success; every
// failure closes it and no request is retried.  A server that does not offer
// CARD_SERVICE is refused honestly with UNSUPPORTED.
CardOpenResult begin_card(ClientTransport& transport, const CardClientOptions& options,
                          Timeout timeout) noexcept;

struct CardStatusResult final {
    CardStatusResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed CARD_STATUS for the session's reader.  It performs no retry.
CardStatusResult card_status(ClientTransport& transport, CardClientSession& session,
                             Timeout timeout) noexcept;

struct CardTransmitResult final {
    CardTransmitResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed CARD_TRANSMIT for the session's exact handle.  A removal, stale
// generation or foreign handle is returned as-is; the APDU is never retried
// and the connection is never re-established.
CardTransmitResult card_transmit(ClientTransport& transport, CardClientSession& session,
                                 CardProtocol protocol, ByteView command,
                                 Timeout timeout) noexcept;

struct CardResetResult final {
    CardSessionResult response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed CARD_RESET for the session's handle.  The returned owner handle and
// generation become the session's binding; the APDU is never retried.
CardResetResult card_reset(ClientTransport& transport, CardClientSession& session,
                           Timeout timeout) noexcept;

struct CardDisconnectResult final {
    CardDisconnectResponse response;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// One typed CARD_DISCONNECT.  It performs no retry.
CardDisconnectResult card_disconnect(ClientTransport& transport, CardClientSession& session,
                                     Disposition disposition, Timeout timeout) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CLIENT_H
