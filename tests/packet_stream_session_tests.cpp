// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/packet_stream_session.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "packet session check failed at %d: %s\n", __LINE__, #value); \
    return false; \
} } while (false)

using Packet = std::array<std::uint8_t, PacketStreamSession::packet_size>;

Attachment identity(std::uint64_t id)
{
    Attachment value{};
    value.owner_client_id = 7U;
    value.lease_id = 3U;
    value.attachment_id = id;
    value.receiver = 1U;
    value.token[0] = static_cast<std::uint8_t>(id);
    return value;
}

Packet packet(std::uint8_t marker)
{
    Packet value{};
    value[0] = 0x47U;
    value[1] = marker;
    for (std::size_t i = 2U; i < value.size(); ++i)
        value[i] = static_cast<std::uint8_t>(marker + i);
    return value;
}

void append(PacketStreamSession& session, const Packet& value)
{
    std::lock_guard<std::mutex> lock(session.mutex);
    session.append_locked(ByteView{value.data(), value.size()});
}

bool attach(PacketStreamSession& session, const Attachment& id,
            std::size_t capacity,
            StreamQueuePolicy policy = StreamQueuePolicy::legacy)
{
    std::lock_guard<std::mutex> lock(session.mutex);
    return static_cast<bool>(session.begin_attach_locked(id, capacity, policy));
}

bool detach(PacketStreamSession& session, const Attachment& id)
{
    std::lock_guard<std::mutex> lock(session.mutex);
    return static_cast<bool>(session.detach_locked(id));
}

bool read(PacketStreamSession& session, const Attachment& id,
          std::uint8_t* output, std::size_t packet_count, std::size_t expected_bytes)
{
    std::lock_guard<std::mutex> lock(session.mutex);
    const auto ticket = session.begin_read_locked(id);
    if (!ticket) return false;
    const auto result = session.finish_read_locked(
        id, ticket.value(), MutableByteView{output, packet_count * PacketStreamSession::packet_size});
    return result && result.value().bytes == expected_bytes;
}

bool test_legacy_overflow()
{
    PacketStreamSession session;
    const Attachment id = identity(1U);
    CHECK(attach(session, id, 2U));
    const Packet first = packet(1U);
    const Packet second = packet(2U);
    const Packet third = packet(3U);
    append(session, first);
    append(session, second);
    append(session, third);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        CHECK(session.count == 2U && session.counters.packets == 3U);
        CHECK(session.counters.bytes == 3U * PacketStreamSession::packet_size);
        CHECK(session.counters.queue_drops == 1U);
        CHECK(session.evicted_packets == 0U);
        CHECK(session.terminal == StreamTerminal::slow_consumer);
    }
    std::array<std::uint8_t, 2U * PacketStreamSession::packet_size> output{};
    CHECK(read(session, id, output.data(), 2U, output.size()));
    CHECK(std::memcmp(output.data(), first.data(), first.size()) == 0);
    CHECK(std::memcmp(output.data() + first.size(), second.data(), second.size()) == 0);
    return true;
}

bool test_v1_evict_and_wrap()
{
    PacketStreamSession session;
    const Attachment id = identity(2U);
    CHECK(attach(session, id, 3U, StreamQueuePolicy::evict_oldest));
    const Packet a = packet(10U);
    const Packet b = packet(11U);
    const Packet c = packet(12U);
    const Packet d = packet(13U);
    const Packet e = packet(14U);
    append(session, a);
    append(session, b);
    append(session, c);
    append(session, d);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        CHECK(session.count == 3U && session.evicted_packets == 1U);
        CHECK(session.counters.queue_drops == 0U);
        CHECK(session.terminal == StreamTerminal::none);
    }
    std::array<std::uint8_t, PacketStreamSession::packet_size> one{};
    CHECK(read(session, id, one.data(), 1U, one.size()));
    CHECK(std::memcmp(one.data(), b.data(), b.size()) == 0);
    append(session, e);
    std::array<std::uint8_t, 3U * PacketStreamSession::packet_size> output{};
    CHECK(read(session, id, output.data(), 3U, output.size()));
    CHECK(std::memcmp(output.data(), c.data(), c.size()) == 0);
    CHECK(std::memcmp(output.data() + c.size(), d.data(), d.size()) == 0);
    CHECK(std::memcmp(output.data() + 2U * c.size(), e.data(), e.size()) == 0);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        CHECK(session.evicted_packets == 1U);
    }
    return true;
}

bool test_detach_drain_final()
{
    PacketStreamSession session;
    const Attachment id = identity(3U);
    CHECK(attach(session, id, 2U));
    const Packet item = packet(21U);
    append(session, item);
    CHECK(detach(session, id));
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        CHECK(session.draining && session.final_snapshot_valid);
    }
    const auto final = session.final_snapshot_query(id);
    CHECK(final && final.value().counters.packets == 1U);
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    CHECK(read(session, id, output.data(), 1U, output.size()));
    CHECK(std::memcmp(output.data(), item.data(), item.size()) == 0);
    bool eof_ok = false;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(id);
        CHECK(ticket);
        const auto eof = session.finish_read_locked(
            id, ticket.value(), MutableByteView{output.data(), output.size()});
        eof_ok = eof && eof.value().eof && !session.draining;
    }
    CHECK(eof_ok);
    CHECK(session.release_final(id));
    return true;
}

bool test_old_ticket_cannot_read_replacement()
{
    PacketStreamSession session;
    const Attachment old_id = identity(4U);
    const Attachment new_id = identity(5U);
    CHECK(attach(session, old_id, 2U));
    PacketStreamSession::ReadTicket saved_ticket{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(old_id);
        CHECK(ticket);
        saved_ticket = ticket.value();
    }
    CHECK(detach(session, old_id));
    CHECK(session.release_final(old_id));
    CHECK(attach(session, new_id, 2U));
    const Packet replacement = packet(31U);
    append(session, replacement);
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    output.fill(0xa5U);
    Result<StreamReadResult> stale = Result<StreamReadResult>::failure(Error::INTERNAL);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        stale = session.finish_read_locked(
            old_id, saved_ticket, MutableByteView{output.data(), output.size()});
        CHECK(session.count == 1U && session.counters.packets == 1U);
    }
    CHECK(!stale && stale.error() == Error::NOT_FOUND);
    for (const std::uint8_t byte : output) CHECK(byte == 0xa5U);
    CHECK(read(session, new_id, output.data(), 1U, output.size()));
    CHECK(std::memcmp(output.data(), replacement.data(), replacement.size()) == 0);
    return true;
}

bool test_blocked_reader_wakes_on_detach()
{
    PacketStreamSession session;
    const Attachment old_id = identity(6U);
    const Attachment new_id = identity(7U);
    CHECK(attach(session, old_id, 2U));
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool reader_waiting = false;
    bool reader_ok = false;
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    std::thread reader([&]() {
        std::unique_lock<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(old_id);
        if (!ticket) return;
        {
            std::lock_guard<std::mutex> gate_lock(gate_mutex);
            reader_waiting = true;
        }
        gate_changed.notify_one();
        session.changed.wait(lock, [&]() { return session.read_wake_locked(ticket.value()); });
        const auto result = session.finish_read_locked(
            old_id, ticket.value(), MutableByteView{output.data(), output.size()});
        reader_ok = result && result.value().eof &&
                    result.value().terminal == StreamTerminal::stopped;
    });
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        started = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                        [&]() { return reader_waiting; });
    }
    if (started) {
        std::lock_guard<std::mutex> lock(session.mutex);
        (void)session.detach_locked(old_id);
    } else {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.shutdown_detach_locked();
    }
    session.changed.notify_all();
    reader.join();
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        CHECK(started && reader_ok && session.count == 0U && !session.draining);
    }
    CHECK(session.release_final(old_id));
    CHECK(attach(session, new_id, 2U));
    return true;
}

}  // namespace

int main()
{
    return test_legacy_overflow() && test_v1_evict_and_wrap() &&
                   test_detach_drain_final() &&
                   test_old_ticket_cannot_read_replacement() &&
                   test_blocked_reader_wakes_on_detach()
               ? 0
               : 1;
}
