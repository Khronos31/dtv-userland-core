// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace w = dtv::userland::wire;

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> out;
    out.length = static_cast<std::uint16_t>(std::strlen(value));
    std::memcpy(out.bytes.data(), value, out.length);
    return out;
}

int main()
{
    w::StatusResponse status;
    status.producer.product = text<32>("px4");
    status.producer.version = text<64>("1.0.0");
    status.context.state = w::ContextState::starting;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology.state = w::TopologyState::unknown;
    status.functions.present = true;

    w::ListResponse list;
    list.producer = status.producer;
    list.topology.state = w::TopologyState::unknown;
    const w::StatusContext context{&list};
    const auto size = w::status_json_size(status, context);
    if (!size) return 1;
    std::array<std::uint8_t, 1024> output{};
    const auto written = w::encode_status_json(status, context,
                                               {output.data(), output.size()});
    if (!written || written.value() != size.value()) return 2;
    return std::fwrite(output.data(), 1U, written.value(), stdout) == written.value() ? 0 : 3;
}
