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

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CLIENT_H
