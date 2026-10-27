// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/packet_stream_session.h"

#include <cstring>
#include <limits>
#include <new>

namespace dtv::userland::detail {

bool PacketStreamSession::same_identity(const Attachment& left,
                                        const Attachment& right) noexcept
{
    return left.owner_client_id == right.owner_client_id &&
           left.lease_id == right.lease_id &&
           left.attachment_id == right.attachment_id &&
           left.receiver == right.receiver && left.system == right.system &&
           left.token == right.token;
}

Result<void> PacketStreamSession::begin_attach_locked(
    const Attachment& new_identity, std::size_t queue_packets,
    StreamQueuePolicy policy) noexcept
{
    if (attached || draining) return Result<void>::failure(Error::BUSY);
    if (incarnation == std::numeric_limits<std::uint64_t>::max())
        return Result<void>::failure(Error::RESOURCE_LIMIT);
    if (queue_packets == 0U) return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (packets == nullptr) {
        if (queue_packets > std::numeric_limits<std::size_t>::max() / packet_size)
            return Result<void>::failure(Error::INTERNAL);
        packets.reset(new (std::nothrow) std::uint8_t[queue_packets * packet_size]);
        if (!packets) return Result<void>::failure(Error::INTERNAL);
        capacity = queue_packets;
    }
    head = 0U;
    count = 0U;
    attached = true;
    draining = false;
    identity = new_identity;
    final_snapshot_valid = false;
    final_identity = Attachment{};
    final_snapshot = StreamFinalSnapshot{};
    terminal = StreamTerminal::none;
    counters = StreamCounters{};
    counters.availability = {
        wire::MeasureState::valid, wire::MeasureState::valid,
        wire::MeasureState::unsupported, wire::MeasureState::unsupported,
        wire::MeasureState::unsupported, wire::MeasureState::valid,
        wire::MeasureState::unsupported, wire::MeasureState::unsupported};
    queue_policy = policy;
    evicted_packets = 0U;
    evicted_overflow = false;
    ++incarnation;
    return Result<void>::success();
}

void PacketStreamSession::abandon_attach_locked() noexcept
{
    attached = false;
    terminal = StreamTerminal::stopped;
    count = 0U;
    changed.notify_all();
}

Result<void> PacketStreamSession::detach_locked(
    const Attachment& detach_identity) noexcept
{
    if (!attached || !same_identity(identity, detach_identity))
        return Result<void>::failure(Error::NOT_FOUND);
    const StreamTerminal final_terminal =
        terminal == StreamTerminal::none ? StreamTerminal::stopped : terminal;
    final_snapshot.counters = counters;
    final_snapshot.terminal = static_cast<std::uint8_t>(final_terminal);
    final_identity = identity;
    final_snapshot_valid = true;
    attached = false;
    terminal = final_terminal;
    draining = true;
    changed.notify_all();
    return Result<void>::success();
}

void PacketStreamSession::append_locked(ByteView packet) noexcept
{
    ++counters.packets;
    counters.bytes += packet_size;
    if (count == capacity) {
        if (queue_policy == StreamQueuePolicy::legacy) {
            ++counters.queue_drops;
            terminal = StreamTerminal::slow_consumer;
            changed.notify_all();
            return;
        }
        head = (head + 1U) % capacity;
        --count;
        if (evicted_packets == std::numeric_limits<std::uint64_t>::max())
            evicted_overflow = true;
        else
            ++evicted_packets;
    }
    const std::size_t tail = (head + count) % capacity;
    std::memcpy(packets.get() + tail * packet_size, packet.data, packet_size);
    ++count;
    changed.notify_all();
}

void PacketStreamSession::amend_final_terminal_locked(
    const Attachment& amend_identity, StreamTerminal reason) noexcept
{
    if (final_snapshot_valid && same_identity(final_identity, amend_identity))
        final_snapshot.terminal = static_cast<std::uint8_t>(reason);
}

void PacketStreamSession::amend_final_terminal_any_locked(
    StreamTerminal reason) noexcept
{
    if (final_snapshot_valid)
        final_snapshot.terminal = static_cast<std::uint8_t>(reason);
}

void PacketStreamSession::shutdown_detach_locked() noexcept
{
    if (!attached) return;
    const StreamTerminal final_terminal =
        terminal == StreamTerminal::none ? StreamTerminal::stopped : terminal;
    final_identity = identity;
    final_snapshot.counters = counters;
    final_snapshot.terminal = static_cast<std::uint8_t>(final_terminal);
    final_snapshot_valid = true;
    attached = false;
    terminal = final_terminal;
    draining = false;
    count = 0U;
    changed.notify_all();
}

Result<PacketStreamSession::ReadTicket> PacketStreamSession::begin_read_locked(
    const Attachment& read_identity) const noexcept
{
    const bool active = attached && same_identity(identity, read_identity);
    const bool drain = draining && same_identity(final_identity, read_identity);
    if (!active && !drain) return Result<ReadTicket>::failure(Error::NOT_FOUND);
    return Result<ReadTicket>::success(ReadTicket{incarnation});
}

bool PacketStreamSession::read_wake_locked(const ReadTicket& ticket) const noexcept
{
    return count != 0U || terminal != StreamTerminal::none ||
           incarnation != ticket.incarnation;
}

Result<StreamReadResult> PacketStreamSession::finish_read_locked(
    const Attachment& read_identity, const ReadTicket& ticket,
    MutableByteView output) noexcept
{
    (void)read_identity;
    if (incarnation != ticket.incarnation)
        return Result<StreamReadResult>::failure(Error::NOT_FOUND);
    if (count == 0U) {
        if (terminal != StreamTerminal::none) {
            const StreamTerminal final_terminal = terminal;
            draining = false;
            changed.notify_all();
            return Result<StreamReadResult>::success(
                StreamReadResult{0U, true, false, final_terminal});
        }
        return Result<StreamReadResult>::success(
            StreamReadResult{0U, false, true, StreamTerminal::none});
    }
    std::size_t packets_wanted = output.size / packet_size;
    packets_wanted = packets_wanted > count ? count : packets_wanted;
    const std::size_t first = head;
    const std::size_t first_count =
        packets_wanted > capacity - first ? capacity - first : packets_wanted;
    if (first_count != 0U)
        std::memcpy(output.data, packets.get() + first * packet_size,
                    first_count * packet_size);
    if (packets_wanted > first_count)
        std::memcpy(output.data + first_count * packet_size, packets.get(),
                    (packets_wanted - first_count) * packet_size);
    head = (head + packets_wanted) % capacity;
    count -= packets_wanted;
    return Result<StreamReadResult>::success(
        StreamReadResult{packets_wanted * packet_size, false, false,
                         StreamTerminal::none});
}

Result<StreamCounters> PacketStreamSession::stats(
    const Attachment& query_identity) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!attached || !same_identity(identity, query_identity))
        return Result<StreamCounters>::failure(Error::NOT_FOUND);
    return Result<StreamCounters>::success(counters);
}

Result<StreamFinalSnapshot> PacketStreamSession::final_snapshot_query(
    const Attachment& query_identity) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!final_snapshot_valid || !same_identity(final_identity, query_identity))
        return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
    return Result<StreamFinalSnapshot>::success(final_snapshot);
}

Result<void> PacketStreamSession::release_final(
    const Attachment& release_identity) noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!final_snapshot_valid || !same_identity(final_identity, release_identity))
        return Result<void>::failure(Error::NOT_FOUND);
    draining = false;
    count = 0U;
    changed.notify_all();
    return Result<void>::success();
}

Result<StreamTerminal> PacketStreamSession::terminal_query(
    const Attachment& query_identity) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!attached || !same_identity(identity, query_identity))
        return Result<StreamTerminal>::failure(Error::NOT_FOUND);
    return Result<StreamTerminal>::success(terminal);
}

}  // namespace dtv::userland::detail
