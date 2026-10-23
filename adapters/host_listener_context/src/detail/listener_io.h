// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_LISTENER_IO_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_LISTENER_IO_H

#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/host_daemon/control_workers.h"
#include "dtv/userland/transport.h"

#include <cstddef>
#include <cstdint>

namespace dtv::userland::host_listener_context::detail {

inline constexpr std::size_t kListenerIoEventCapacity = 67U;

struct ListenerToken final {
    std::uint64_t value = 0U;
};

struct ConnectionToken final {
    std::uint64_t value = 0U;
};

struct ListenerPair final {
    ListenerToken control{};
    ListenerToken data{};
};

enum class ListenerKind : std::uint8_t {
    control,
    data,
};

enum class IoStatus : std::uint8_t {
    progress,
    would_block,
    eof,
    failure,
};

enum class ReadyKind : std::uint8_t {
    listener,
    connection,
    worker_wake,
};

enum ReadyFlag : std::uint8_t {
    ready_read = 1U,
    ready_write = 2U,
    ready_error = 4U,
    ready_hangup = 8U,
};

struct ReadyEvent final {
    ReadyKind kind = ReadyKind::listener;
    ListenerKind listener_kind = ListenerKind::control;
    ListenerToken listener{};
    ConnectionToken connection{};
    std::uint8_t flags = 0U;
};

struct AcceptResult final {
    IoStatus status = IoStatus::would_block;
    ConnectionToken connection{};
    Error error = Error::OK;
};

struct IoResult final {
    IoStatus status = IoStatus::would_block;
    std::size_t transferred = 0U;
    Error error = Error::OK;
};

class ListenerIo {
public:
    virtual ~ListenerIo() noexcept = default;

    // Reserves only the shared namespace. publish_pair() publishes both
    // endpoints atomically and rolls back only paths created by this owner.
    virtual Error reserve_pair(const host_runtime::EndpointConfig& control,
                               const host_runtime::EndpointConfig& data) noexcept = 0;
    virtual Error publish_pair(ListenerPair& pair) noexcept = 0;
    // Returns readiness only; all following accept/read/write calls are
    // nonblocking. event_count is set to the number of initialized entries.
    virtual Error poll(const ::px4::userland::ipc::posix::WakeHandle& worker_wake,
                       Timeout timeout, ReadyEvent* events,
                       std::size_t event_capacity,
                       std::size_t& event_count) noexcept = 0;
    virtual AcceptResult accept(ListenerToken listener) noexcept = 0;
    // progress carries a partial byte count; would_block carries no progress,
    // eof is a clean peer EOF, and failure carries a typed Error.
    virtual IoResult read_some(ConnectionToken connection,
                               MutableByteView output) noexcept = 0;
    virtual IoResult write_some(ConnectionToken connection,
                                ByteView input) noexcept = 0;
    virtual Error close_connection(ConnectionToken connection) noexcept = 0;
    virtual Error unpublish_pair(ListenerPair pair) noexcept = 0;
    virtual Error release_namespace() noexcept = 0;
};

}  // namespace dtv::userland::host_listener_context::detail

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_DETAIL_LISTENER_IO_H
