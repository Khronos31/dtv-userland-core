#include <dtv/userland/detail/packet_stream_session.h>

#include <array>

int main()
{
    using namespace dtv::userland;
    using namespace dtv::userland::detail;
    PacketStreamSession session;
    Attachment id{};
    id.attachment_id = 1U;
    if (!session.begin_attach_locked(id, 1U, StreamQueuePolicy::evict_oldest)) return 1;
    std::array<std::uint8_t, PacketStreamSession::packet_size> packet{};
    packet[0] = 0x47U;
    session.append_locked(ByteView{packet.data(), packet.size()});
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    const auto ticket = session.begin_read_locked(id);
    if (!ticket) return 2;
    const auto result = session.finish_read_locked(
        id, ticket.value(), MutableByteView{output.data(), output.size()});
    return result && result.value().bytes == output.size() && output == packet ? 0 : 3;
}
