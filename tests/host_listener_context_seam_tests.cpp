// SPDX-License-Identifier: GPL-2.0-only
#include "detail/listener_io.h"
#include "dtv/userland/host_listener_context/enclosure_context.h"

#include <type_traits>

using namespace dtv::userland::host_listener_context::detail;
using namespace dtv::userland;

static_assert(kListenerIoEventCapacity == 67U);
static_assert(!std::is_copy_constructible<ListenerIo>::value);
static_assert(!std::is_copy_constructible<
              dtv::userland::host_listener_context::EnclosureContext>::value);

class FakeListenerIo final : public ListenerIo {
public:
    Error reserve_pair(const dtv::userland::host_runtime::EndpointConfig&,
                       const dtv::userland::host_runtime::EndpointConfig&) noexcept override
    {
        return Error::OK;
    }
    Error publish_pair(ListenerPair&) noexcept override { return Error::OK; }
    Error poll(const ::px4::userland::ipc::posix::WakeHandle&, Timeout,
               ReadyEvent*, std::size_t, std::size_t& count) noexcept override
    {
        count = 0U;
        return Error::OK;
    }
    AcceptResult accept(ListenerToken) noexcept override { return {}; }
    Error set_connection_interest(ConnectionToken, std::uint8_t) noexcept override
    {
        return Error::OK;
    }
    IoResult read_some(ConnectionToken, MutableByteView) noexcept override { return {}; }
    IoResult write_some(ConnectionToken, ByteView) noexcept override { return {}; }
    Error close_connection(ConnectionToken) noexcept override { return Error::OK; }
    Error unpublish_pair(ListenerPair) noexcept override { return Error::OK; }
    Error release_namespace() noexcept override { return Error::OK; }
};

int main()
{
    FakeListenerIo io;
    ListenerPair pair;
    return io.publish_pair(pair) == Error::OK ? 0 : 1;
}
