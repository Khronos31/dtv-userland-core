// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/packet_stream_session.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define STORE_CHECK(condition)                                                    \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "stream v1 store check failed at line %d: %s\n", \
                         __LINE__, #condition);                                   \
            return false;                                                         \
        }                                                                         \
    } while (false)

Attachment identity(std::uint64_t attachment_id)
{
    Attachment value{};
    value.owner_client_id = 7U;
    value.lease_id = 3U;
    value.attachment_id = attachment_id;
    value.receiver = 1U;
    value.system = ReceiverSystem::terrestrial;
    value.token[0] = static_cast<std::uint8_t>(attachment_id);
    return value;
}

std::array<std::uint8_t, PacketStreamSession::packet_size> packet(std::uint8_t tag)
{
    std::array<std::uint8_t, PacketStreamSession::packet_size> value{};
    value[0] = 0x47U;
    value[1] = tag;
    return value;
}

// Legacy policy keeps the accepted behavior: a full ring drops the incoming
// packet, sets the slow-consumer terminal and counts an incoming drop.
bool test_legacy_full_ring_unchanged()
{
    PacketStreamSession session;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.begin_attach_locked(identity(1U), 2U));
        for (std::uint8_t tag = 1U; tag <= 3U; ++tag) {
            const auto p = packet(tag);
            session.append_locked(ByteView{p.data(), p.size()});
        }
        // Two stored packets, one incoming drop, slow-consumer terminal.
        STORE_CHECK(session.count == 2U);
        STORE_CHECK(session.counters.packets == 3U);
        STORE_CHECK(session.counters.queue_drops == 1U);
        STORE_CHECK(session.terminal == StreamTerminal::slow_consumer);
        STORE_CHECK(session.evicted_packets == 0U);
    }
    // Oldest-first drain of the two stored packets (tags 1 then 2).
    std::array<std::uint8_t, 2U * PacketStreamSession::packet_size> out{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(identity(1U));
        STORE_CHECK(ticket);
        const auto read = session.finish_read_locked(
            identity(1U), ticket.value(),
            MutableByteView{out.data(), out.size()});
        STORE_CHECK(read);
        STORE_CHECK(read.value().bytes == out.size());
        STORE_CHECK(out[1] == 1U);
        STORE_CHECK(out[PacketStreamSession::packet_size + 1U] == 2U);
    }
    return true;
}

// v1 policy evicts the oldest unread complete packet, continues, and counts
// each actual eviction without setting the slow-consumer terminal.
bool test_v1_evict_oldest()
{
    PacketStreamSession session;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.begin_attach_locked(identity(2U), 2U,
                                                StreamQueuePolicy::evict_oldest));
        for (std::uint8_t tag = 1U; tag <= 5U; ++tag) {
            const auto p = packet(tag);
            session.append_locked(ByteView{p.data(), p.size()});
        }
        STORE_CHECK(session.count == 2U);
        STORE_CHECK(session.counters.queue_drops == 0U);
        STORE_CHECK(session.terminal == StreamTerminal::none);
        STORE_CHECK(session.evicted_packets == 3U);
        STORE_CHECK(!session.evicted_overflow);
    }
    // The two surviving packets are the newest ones (tags 4 then 5), in order.
    std::array<std::uint8_t, 2U * PacketStreamSession::packet_size> out{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(identity(2U));
        STORE_CHECK(ticket);
        const auto read = session.finish_read_locked(
            identity(2U), ticket.value(),
            MutableByteView{out.data(), out.size()});
        STORE_CHECK(read);
        STORE_CHECK(out[1] == 4U);
        STORE_CHECK(out[PacketStreamSession::packet_size + 1U] == 5U);
    }
    return true;
}

// A saturated eviction counter sets the overflow flag and never wraps.
bool test_eviction_saturation()
{
    PacketStreamSession session;
    std::lock_guard<std::mutex> lock(session.mutex);
    STORE_CHECK(session.begin_attach_locked(identity(3U), 1U,
                                            StreamQueuePolicy::evict_oldest));
    const auto p1 = packet(1U);
    session.append_locked(ByteView{p1.data(), p1.size()});
    session.evicted_packets = std::numeric_limits<std::uint64_t>::max();
    const auto p2 = packet(2U);
    session.append_locked(ByteView{p2.data(), p2.size()});
    STORE_CHECK(session.evicted_packets == std::numeric_limits<std::uint64_t>::max());
    STORE_CHECK(session.evicted_overflow);
    // The surviving packet is the newest one.
    STORE_CHECK(session.count == 1U);
    return true;
}

// A held reader of the old attachment must not consume or retire the
// replacement session, even with the v1 policy selected on the replacement.
bool test_held_old_read_isolated_from_replacement()
{
    PacketStreamSession session;
    PacketStreamSession::ReadTicket old_ticket{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.begin_attach_locked(identity(4U), 2U));
        const auto p = packet(9U);
        session.append_locked(ByteView{p.data(), p.size()});
        const auto ticket = session.begin_read_locked(identity(4U));
        STORE_CHECK(ticket);
        old_ticket = ticket.value();
    }
    // Detach the old attachment (self-locking helpers are called outside the
    // session mutex).
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.detach_locked(identity(4U)));
    }
    // Retire the drained old final before the replacement publication.
    STORE_CHECK(session.release_final(identity(4U)));
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.begin_attach_locked(identity(5U), 2U,
                                                StreamQueuePolicy::evict_oldest));
        const auto p = packet(50U);
        session.append_locked(ByteView{p.data(), p.size()});
    }
    // The delayed reader observes NOT_FOUND and leaves the replacement intact.
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        std::array<std::uint8_t, PacketStreamSession::packet_size> out{};
        const auto read = session.finish_read_locked(
            identity(4U), old_ticket, MutableByteView{out.data(), out.size()});
        STORE_CHECK(!read);
        STORE_CHECK(read.error() == Error::NOT_FOUND);
        STORE_CHECK(session.attached);
        STORE_CHECK(session.count == 1U);
        STORE_CHECK(session.queue_policy == StreamQueuePolicy::evict_oldest);
    }
    return true;
}

// A failed publication is abandoned without breaking the immutable per-attach
// policy or the old-read replacement protection.
bool test_failed_attach_policy_and_old_read()
{
    PacketStreamSession session;
    PacketStreamSession::ReadTicket old_ticket{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        STORE_CHECK(session.begin_attach_locked(identity(6U), 2U,
                                                StreamQueuePolicy::evict_oldest));
        const auto p = packet(60U);
        session.append_locked(ByteView{p.data(), p.size()});
        const auto ticket = session.begin_read_locked(identity(6U));
        STORE_CHECK(ticket);
        old_ticket = ticket.value();
        // Abandon the publication (failed product startup) and republish.
        session.abandon_attach_locked();
        STORE_CHECK(!session.attached);
    }
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        // A new attachment selects the policy at attach time; it is not
        // mutable afterwards.
        STORE_CHECK(session.begin_attach_locked(identity(7U), 1U,
                                                StreamQueuePolicy::evict_oldest));
        STORE_CHECK(session.queue_policy == StreamQueuePolicy::evict_oldest);
        const auto p = packet(70U);
        session.append_locked(ByteView{p.data(), p.size()});
        // A delayed reader of the abandoned attachment observes NOT_FOUND and
        // leaves the replacement intact.
        std::array<std::uint8_t, PacketStreamSession::packet_size> out{};
        const auto read = session.finish_read_locked(
            identity(6U), old_ticket, MutableByteView{out.data(), out.size()});
        STORE_CHECK(!read);
        STORE_CHECK(read.error() == Error::NOT_FOUND);
        STORE_CHECK(session.count == 1U);
    }
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_legacy_full_ring_unchanged() && test_v1_evict_oldest() &&
                    test_eviction_saturation() &&
                    test_held_old_read_isolated_from_replacement() &&
                    test_failed_attach_policy_and_old_read();
    if (!ok) return 1;
    std::printf("stream v1 store tests: PASS\n");
    return 0;
}
