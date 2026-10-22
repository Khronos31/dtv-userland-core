#include <dtv/userland/control_client.h>

class DisconnectedTransport final : public dtv::userland::wire::ClientTransport {
public:
    dtv::userland::Result<void> write_all(dtv::userland::ByteView) noexcept override
    {
        return dtv::userland::Result<void>::failure(dtv::userland::Error::DISCONNECTED);
    }

    dtv::userland::Result<std::size_t> read_some(
        dtv::userland::MutableByteView) noexcept override
    {
        return dtv::userland::Result<std::size_t>::success(0U);
    }

    void close() noexcept override { closed = true; }

    bool closed = false;
};

int main()
{
    DisconnectedTransport transport;
    const auto result = dtv::userland::wire::fetch_status(transport);
    return !result && result.error == dtv::userland::Error::DISCONNECTED && transport.closed ? 0 : 1;
}
