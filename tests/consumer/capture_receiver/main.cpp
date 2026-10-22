#include <dtv/userland/detail/packet_stream_session.h>
#include <dtv/userland/detail/receiver_engine.h>

#include <array>

int main()
{
    using namespace dtv::userland;
    using namespace dtv::userland::detail;
    static_assert(sizeof(ReceiverEngine) != 0U, "receiver component header is installed");
    PacketStreamSession session;
    Attachment id{};
    id.receiver = 513U;
    id.attachment_id = 2U;
    if (!session.begin_attach_locked(id, 1U)) return 1;
    std::array<std::uint8_t, PacketStreamSession::packet_size> packet{};
    packet[0] = 0x47U;
    session.append_locked(ByteView{packet.data(), packet.size()});
    return session.counters.packets == 1U &&
                   id.receiver == static_cast<ReceiverId>(513U)
               ? 0
               : 2;
}
