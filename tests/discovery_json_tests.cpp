// SPDX-License-Identifier: MIT
#include <dtv/userland/discovery_json.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace w = dtv::userland::wire;
using dtv::userland::Error;

namespace {

int failures = 0;

#define CHECK(value) do { if (!(value)) { ++failures; std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #value); } } while (false)

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> out;
    out.length = static_cast<std::uint16_t>(std::strlen(value));
    std::memcpy(out.bytes.data(), value, out.length);
    return out;
}

struct Fixture final {
    w::DiscoveryScan scan;
    w::DiscoveryEnclosure enclosures[2]{};
    w::DiscoveryDevice devices[2]{};
    w::DiscoveryReceiver receivers[1]{};
};

void make_scan(Fixture& fixture)
{
    auto& scan = fixture.scan;
    scan.complete = true;
    scan.product = text<32>("px4");
    scan.version = text<64>("1.0.0");
    scan.enclosure_count = 2U;
    scan.enclosures = fixture.enclosures;
    auto& first = fixture.enclosures[0];
    first.enclosure = 4U;
    first.model.present = true;
    first.model.value = text<128>("PX-Q3U4");
    first.serial.present = true;
    first.serial.value = text<dtv::userland::detail::kMaxSerialBytes>("serial-a");
    first.device_count = 1U;
    first.devices = &fixture.devices[0];
    fixture.devices[0].device = 2U;
    fixture.devices[0].vid = 0x0511U;
    fixture.devices[0].pid = 0x084aU;
    fixture.devices[0].receivers_known = true;
    fixture.devices[0].receiver_count = 1U;
    fixture.devices[0].receivers = fixture.receivers;
    fixture.receivers[0] = {3U, 2U, 7U, {3U}};

    auto& second = fixture.enclosures[1];
    second.enclosure = 5U;
    second.serial.present = true;
    second.serial.value = text<dtv::userland::detail::kMaxSerialBytes>("serial-b");
    second.device_count = 1U;
    second.devices = &fixture.devices[1];
    fixture.devices[1].device = 8U;
    fixture.devices[1].vid = 0x1234U;
    fixture.devices[1].pid = 0x00abU;
    fixture.devices[1].receivers_known = false;
}

std::string encode(const w::DiscoveryScan& scan)
{
    const auto size = w::discovery_json_size(scan);
    if (!size) return {};
    std::array<std::uint8_t, 4096> output{};
    const auto written = w::encode_discovery_json(scan, {output.data(), output.size()});
    if (!written || written.value() != size.value()) return {};
    return std::string(reinterpret_cast<const char*>(output.data()), written.value());
}

void test_scan_projection()
{
    Fixture fixture;
    make_scan(fixture);
    const auto json = encode(fixture.scan);
    CHECK(json.find("\"enclosures\":[{\"enclosure\":4") != std::string::npos);
    CHECK(json.find("\"device\":2,\"usb\":\"0511:084a\"") != std::string::npos);
    CHECK(json.find("\"receiver\":3,\"device\":2,\"local\":7,\"systems\":[\"ISDB-T\",\"ISDB-S\"]") != std::string::npos);
    CHECK(json.find("\"serial_unique\":true") != std::string::npos);
    CHECK(json.find("\"serial_unique\":false") == std::string::npos);
    CHECK(json.find("\"receivers\":[]") == std::string::npos);

    fixture.enclosures[0].model.value = text<128>("PX\\\"Q3U4");
    const auto escaped = encode(fixture.scan);
    CHECK(escaped.find("PX\\\\\\\"Q3U4") != std::string::npos);
}

void test_serial_uniqueness_and_unknown_receivers()
{
    Fixture fixture;
    make_scan(fixture);
    auto& scan = fixture.scan;
    scan.complete = false;
    auto json = encode(scan);
    CHECK(json.find("\"serial_unique\":null") != std::string::npos);
    CHECK(json.find("\"scan_complete\":false") != std::string::npos);

    fixture.enclosures[1].serial.value = fixture.enclosures[0].serial.value;
    json = encode(scan);
    CHECK(json.find("\"serial_unique\":false") != std::string::npos);

    fixture.enclosures[1].serial.present = false;
    fixture.devices[1].receivers_known = false;
    json = encode(scan);
    CHECK(json.find("\"serial\":null,\"serial_unique\":null") != std::string::npos);
    const auto second = json.find("\"enclosure\":5");
    CHECK(second != std::string::npos && json.find("\"receivers\"", second) == std::string::npos);
}

void test_atomic_failures()
{
    Fixture fixture;
    make_scan(fixture);
    const auto& scan = fixture.scan;
    std::array<std::uint8_t, 32> output{};
    output.fill(0x5aU);
    const auto short_result = w::encode_discovery_json(scan, {output.data(), output.size()});
    CHECK(!short_result && short_result.error() == Error::BUFFER_TOO_SMALL);
    for (const auto byte : output) CHECK(byte == 0x5aU);

    auto invalid = scan;
    fixture.receivers[0].systems.mask = 0U;
    const auto bad_result = w::encode_discovery_json(invalid, {output.data(), output.size()});
    CHECK(!bad_result && bad_result.error() == Error::INVALID_ARGUMENT);
    for (const auto byte : output) CHECK(byte == 0x5aU);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--json") == 0) {
        Fixture fixture;
        make_scan(fixture);
        std::puts(encode(fixture.scan).c_str());
        return failures == 0 ? 0 : 1;
    }
    test_scan_projection();
    test_serial_uniqueness_and_unknown_receivers();
    test_atomic_failures();
    return failures == 0 ? 0 : 1;
}
