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

class DisconnectedTimedTransport final : public dtv::userland::wire::TimedClientTransport {
public:
    dtv::userland::Result<void> write_all(dtv::userland::ByteView,
                                          dtv::userland::Timeout) noexcept override
    {
        return dtv::userland::Result<void>::failure(dtv::userland::Error::DISCONNECTED);
    }

    dtv::userland::Result<std::size_t> read_some(dtv::userland::MutableByteView,
                                                  dtv::userland::Timeout) noexcept override
    {
        return dtv::userland::Result<std::size_t>::success(0U);
    }

    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void close() noexcept override { closed = true; }

    bool closed = false;
};

int main()
{
    DisconnectedTransport transport;
    const auto result = dtv::userland::wire::fetch_status(transport);
    if (result || result.error != dtv::userland::Error::DISCONNECTED || !transport.closed)
        return 1;

    DisconnectedTimedTransport timed;
    const auto timed_status = dtv::userland::wire::fetch_status(
        timed, dtv::userland::Timeout{100U});
    if (timed_status || timed_status.error != dtv::userland::Error::DISCONNECTED ||
        !timed.closed)
        return 1;

    timed.closed = false;
    const auto required_status = dtv::userland::wire::fetch_status(
        timed, dtv::userland::Timeout{100U},
        dtv::userland::wire::kFeatureCardService);
    if (required_status || required_status.error != dtv::userland::Error::DISCONNECTED ||
        !timed.closed)
        return 1;

    timed.closed = false;
    const auto lnb = dtv::userland::wire::set_lnb_power(
        timed, false, dtv::userland::Timeout{100U});
    if (lnb || lnb.error != dtv::userland::Error::DISCONNECTED || !timed.closed)
        return 1;

    timed.closed = false;
    const auto recording = dtv::userland::wire::begin_recording(
        timed, dtv::userland::wire::RecordingOptions{}, dtv::userland::Timeout{100U});
    if (recording || recording.error != dtv::userland::Error::DISCONNECTED || !timed.closed)
        return 1;

    dtv::userland::wire::RecordingSession session{};
    session.epoch[0] = 1U;
    session.lease_id = 1U;
    session.stream_id = 1U;
    timed.closed = false;
    const auto stopped = dtv::userland::wire::stop_recording(
        timed, session, dtv::userland::Timeout{100U});
    if (stopped || stopped.error != dtv::userland::Error::DISCONNECTED || !timed.closed)
        return 1;

    timed.closed = false;
    const auto released = dtv::userland::wire::release_recording(
        timed, session, dtv::userland::Timeout{100U});
    return !released && released.error == dtv::userland::Error::DISCONNECTED && timed.closed
               ? 0
               : 1;
}
