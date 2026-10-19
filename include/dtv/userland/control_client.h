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

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CLIENT_H
