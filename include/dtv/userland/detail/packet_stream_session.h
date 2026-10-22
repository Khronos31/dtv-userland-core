// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H
#define DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H

#include "dtv/userland/detail/stream_session_types.h"
#include "dtv/userland/error.h"
#include "dtv/userland/transport.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dtv::userland::detail {

enum class StreamQueuePolicy : std::uint8_t {
    // Reject the incoming packet and mark a full queue slow-consumer.
    legacy = 0U,
    // Evict the oldest unread complete packet and continue accepting.
    evict_oldest = 1U,
};

// One bounded complete-packet store.  It owns no producer, socket, or device;
// callers share its mutex when combining packet state with their own policy.
// This internal compatibility component is not a stable API.
class PacketStreamSession {
public:
    static constexpr std::size_t packet_size = 188U;

    PacketStreamSession() noexcept = default;
    ~PacketStreamSession() noexcept = default;
    PacketStreamSession(const PacketStreamSession&) = delete;
    PacketStreamSession& operator=(const PacketStreamSession&) = delete;

    static bool same_identity(const Attachment& left,
                              const Attachment& right) noexcept;

    // The caller holds mutex. Capacity is fixed by the first successful
    // allocation and must be nonzero.
    Result<void> begin_attach_locked(
        const Attachment& identity, std::size_t queue_packets,
        StreamQueuePolicy policy = StreamQueuePolicy::legacy) noexcept;
    void abandon_attach_locked() noexcept;
    // Detach snapshots final state and leaves queued packets drainable.
    Result<void> detach_locked(const Attachment& identity) noexcept;
    // The caller holds mutex; packet points to exactly packet_size bytes.
    void append_locked(ByteView packet) noexcept;
    void amend_final_terminal_locked(const Attachment& identity,
                                     StreamTerminal reason) noexcept;
    void amend_final_terminal_any_locked(StreamTerminal reason) noexcept;
    void shutdown_detach_locked() noexcept;

    struct ReadTicket final {
        std::uint64_t incarnation = 0U;
    };
    // Locked helpers; the caller owns the wait strategy and lock lifetime.
    Result<ReadTicket> begin_read_locked(const Attachment& identity) const noexcept;
    bool read_wake_locked(const ReadTicket& ticket) const noexcept;
    Result<StreamReadResult> finish_read_locked(const Attachment& identity,
                                                const ReadTicket& ticket,
                                                MutableByteView output) noexcept;

    Result<StreamCounters> stats(const Attachment& identity) const noexcept;
    Result<StreamFinalSnapshot> final_snapshot_query(
        const Attachment& identity) const noexcept;
    Result<void> release_final(const Attachment& identity) noexcept;
    Result<StreamTerminal> terminal_query(const Attachment& identity) const noexcept;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<std::uint8_t[]> packets;
    std::size_t capacity = 0U;
    std::size_t head = 0U;
    std::size_t count = 0U;
    bool attached = false;
    bool draining = false;
    Attachment identity{};
    bool final_snapshot_valid = false;
    Attachment final_identity{};
    StreamFinalSnapshot final_snapshot{};
    StreamTerminal terminal = StreamTerminal::none;
    StreamCounters counters{};
    StreamQueuePolicy queue_policy = StreamQueuePolicy::legacy;
    // v1 evictions are tracked separately from legacy queue_drops.
    std::uint64_t evicted_packets = 0U;
    bool evicted_overflow = false;
    std::uint64_t incarnation = 0U;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H
