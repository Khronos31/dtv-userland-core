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
    w::DiscoveryReceiver receivers[2]{};
    w::DiscoveryReader readers[1]{};
    w::DiscoveryCandidate candidates[1]{};
    w::DiscoveryUsbObservation ungrouped[1]{};
    w::DiscoveryScanIssue issues[1]{};
};

void make_scan(Fixture& fixture)
{
    auto& scan = fixture.scan;
    scan.complete = true;
    scan.product = text<32>("px4");
    scan.version = text<64>("1.0.0");
    scan.enclosure_count = 2U;
    scan.enclosures = fixture.enclosures;
    scan.ungrouped_usb_device_count = 1U;
    scan.ungrouped_usb_devices = fixture.ungrouped;
    fixture.ungrouped[0].model.present = true;
    fixture.ungrouped[0].model.value = text<128>("PX-Q3U4");
    fixture.ungrouped[0].vid = 0x0511U;
    fixture.ungrouped[0].pid = 0x084aU;
    fixture.ungrouped[0].status = w::DiscoveryObservationStatus::open_failed;
    fixture.ungrouped[0].location.address.present = true;
    fixture.ungrouped[0].location.address.value = 9U;
    auto& first = fixture.enclosures[0];
    first.enclosure = 4U;
    first.status = w::DiscoveryEnclosureStatus::ready;
    first.model.present = true;
    first.model.value = text<128>("PX-Q3U4");
    first.serial.present = true;
    first.serial.value = text<dtv::userland::detail::kMaxSerialBytes>("serial-a");
    first.lnb_15v_supported.present = true;
    first.lnb_15v_supported.value = true;
    first.readers_known = true;
    first.reader_count = 1U;
    first.readers = fixture.readers;
    fixture.readers[0] = {6U, 2U};
    first.device_count = 1U;
    first.devices = &fixture.devices[0];
    fixture.devices[0].device = 2U;
    fixture.devices[0].vid = 0x0511U;
    fixture.devices[0].pid = 0x084aU;
    fixture.devices[0].serial.present = true;
    fixture.devices[0].serial.value = text<dtv::userland::detail::kMaxSerialBytes>("device-a");
    fixture.devices[0].location.bus.present = true;
    fixture.devices[0].location.bus.value = 1U;
    fixture.devices[0].location.address.present = true;
    fixture.devices[0].location.address.value = 6U;
    fixture.devices[0].location.port.present = true;
    fixture.devices[0].location.port.value = text<64>("1-2.3");
    fixture.devices[0].receivers_known = true;
    fixture.devices[0].receiver_count = 1U;
    fixture.devices[0].receivers = fixture.receivers;
    fixture.receivers[0] = {3U, 2U, 7U, {3U}};

    auto& second = fixture.enclosures[1];
    second.enclosure = 5U;
    second.status = w::DiscoveryEnclosureStatus::incomplete;
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
    CHECK(json.find("\"enclosures\":[{\"id\":4") != std::string::npos);
    CHECK(json.find("\"id\":2,\"usb\":\"0511:084a\"") != std::string::npos);
    CHECK(json.find("\"receiver\":3,\"device\":2,\"local\":7,\"systems\":[\"ISDB-T\",\"ISDB-S\"]") != std::string::npos);
    CHECK(json.find("\"serial_unique\":true") != std::string::npos);
    CHECK(json.find("\"serial_unique\":false") == std::string::npos);
    CHECK(json.find("\"receivers\":[]") == std::string::npos);
    CHECK(json.find(
              "\"lnb_15v_supported\":true,\"readers\":[{\"reader\":6,\"device\":2}]") !=
          std::string::npos);
    CHECK(json.find("\"scan\":{\"complete\":true,\"issues\":[]}") != std::string::npos);
    CHECK(json.find("\"status\":\"ready\"") != std::string::npos);
    CHECK(json.find("\"serial\":\"device-a\",\"bus\":1,\"address\":6,\"port\":\"1-2.3\"") !=
          std::string::npos);
    CHECK(json.find("\"ungrouped_usb_devices\":[{\"serial\":null,\"model\":\"PX-Q3U4\"") !=
          std::string::npos);
    CHECK(json.find("\"status\":\"open_failed\",\"bus\":null,\"address\":9,\"port\":null") !=
          std::string::npos);

    fixture.enclosures[0].model.value = text<128>("PX\\\"Q3U4");
    const auto escaped = encode(fixture.scan);
    CHECK(escaped.find("PX\\\\\\\"Q3U4") != std::string::npos);
}

void test_scan_issues_are_separate_from_enclosure_status()
{
    Fixture fixture;
    make_scan(fixture);
    fixture.scan.complete = false;
    fixture.scan.issue_count = 1U;
    fixture.scan.issues = fixture.issues;
    fixture.issues[0] = {text<64>("enumeration_failed"), text<256>("one function unavailable")};
    const auto json = encode(fixture.scan);
    CHECK(json.find("\"scan\":{\"complete\":false,\"issues\":[{\"code\":\"enumeration_failed\",\"detail\":\"one function unavailable\"}]}") !=
          std::string::npos);
    CHECK(json.find("\"status\":\"ready\"") != std::string::npos);
}

void test_duplicate_candidates_and_status_vocabulary()
{
    Fixture fixture;
    make_scan(fixture);
    auto& enclosure = fixture.enclosures[0];
    enclosure.status = w::DiscoveryEnclosureStatus::duplicate;
    enclosure.device_count = 0U;
    enclosure.devices = nullptr;
    enclosure.readers_known = false;
    enclosure.reader_count = 0U;
    enclosure.readers = nullptr;
    enclosure.candidate_count = 1U;
    enclosure.candidates = fixture.candidates;
    fixture.candidates[0].device = 1U;
    fixture.candidates[0].serial.present = true;
    fixture.candidates[0].serial.value = text<dtv::userland::detail::kMaxSerialBytes>("candidate-a");
    fixture.candidates[0].location.bus.present = true;
    fixture.candidates[0].location.bus.value = 2U;
    fixture.candidates[0].location.port.present = true;
    fixture.candidates[0].location.port.value = text<64>("2-1.4");
    const auto json = encode(fixture.scan);
    CHECK(json.find("\"status\":\"duplicate\",\"lnb_15v_supported\"") != std::string::npos);
    CHECK(json.find("\"candidates\":[{\"device\":1,\"serial\":\"candidate-a\",\"bus\":2") !=
          std::string::npos);
    CHECK(json.find("\"port\":\"2-1.4\"}") != std::string::npos);
}

void test_shared_capability_presence_and_reader_references()
{
    Fixture fixture;
    make_scan(fixture);
    fixture.enclosures[0].lnb_15v_supported.present = false;
    fixture.enclosures[0].readers_known = true;
    fixture.enclosures[0].reader_count = 0U;
    fixture.enclosures[0].readers = nullptr;
    auto json = encode(fixture.scan);
    CHECK(json.find("\"lnb_15v_supported\":null,\"readers\":[]") != std::string::npos);

    fixture.enclosures[0].readers_known = false;
    json = encode(fixture.scan);
    CHECK(json.find("\"lnb_15v_supported\":null,\"readers\"") == std::string::npos);

    fixture.enclosures[0].readers_known = true;
    fixture.enclosures[0].reader_count = 1U;
    fixture.enclosures[0].readers = fixture.readers;
    fixture.readers[0].device = 99U;
    CHECK(!w::discovery_json_size(fixture.scan));
}

void test_receiver_number_is_unique_within_device()
{
    Fixture fixture;
    make_scan(fixture);
    fixture.devices[0].receiver_count = 2U;
    fixture.receivers[1] = {3U, 2U, 8U, {1U}};
    CHECK(!w::discovery_json_size(fixture.scan));
}

void test_serial_uniqueness_and_unknown_receivers()
{
    Fixture fixture;
    make_scan(fixture);
    auto& scan = fixture.scan;
    scan.complete = false;
    auto json = encode(scan);
    CHECK(json.find("\"serial_unique\":null") != std::string::npos);
    CHECK(json.find("\"scan\":{\"complete\":false") != std::string::npos);

    fixture.enclosures[1].serial.value = fixture.enclosures[0].serial.value;
    json = encode(scan);
    CHECK(json.find("\"serial_unique\":false") != std::string::npos);

    fixture.enclosures[1].serial.present = false;
    fixture.devices[1].receivers_known = false;
    json = encode(scan);
    CHECK(json.find("\"serial\":null,\"serial_unique\":null") != std::string::npos);
    const auto second = json.find("\"id\":5");
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
    test_scan_issues_are_separate_from_enclosure_status();
    test_duplicate_candidates_and_status_vocabulary();
    test_shared_capability_presence_and_reader_references();
    test_receiver_number_is_unique_within_device();
    test_serial_uniqueness_and_unknown_receivers();
    test_atomic_failures();
    return failures == 0 ? 0 : 1;
}
