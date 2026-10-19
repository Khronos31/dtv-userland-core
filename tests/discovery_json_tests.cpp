// SPDX-License-Identifier: MIT
// Bounded userland.discovery v1.0 serialization: ordinary empty, cold,
// grouped, partial and contradictory snapshots. No hardware or USB access.
#include <dtv/userland/json.h>
#include <dtv/userland/error.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace w = dtv::userland::wire;
using dtv::userland::Error;
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::fprintf(stderr, \
    "CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#expr); } } while (false)

template <std::size_t N>
w::WString<N> str(const char* value)
{
    w::WString<N> result{};
    const std::size_t length=std::strlen(value);
    if(length>N){CHECK(false);return result;}
    result.length=static_cast<std::uint16_t>(length);
    std::memcpy(result.bytes.data(),value,length);
    return result;
}

// Small owned test fixture. Production serialization takes non-owning views
// so a scan can contain multiple eight-function enclosures without copying.
struct FixtureDocument final {
    w::Producer producer;
    w::Uuid scan_id{};
    bool complete = true;
    w::BArray<w::DiscoveryScanIssue,w::kMaxFunctions> issues;
    w::BArray<w::DiscoveryFunction,w::kMaxFunctions> usb_functions;
    w::BArray<w::DiscoveryEnclosure,w::kMaxFunctions> enclosures;

    w::DiscoveryResponse view() const noexcept
    {
        w::DiscoveryResponse result{};
        result.producer=producer;
        result.scan_id=scan_id;
        result.complete=complete;
        result.issues={issues.items.data(),issues.count};
        result.usb_functions={usb_functions.items.data(),usb_functions.count};
        result.enclosures={enclosures.items.data(),enclosures.count};
        return result;
    }
};

FixtureDocument empty_scan()
{
    FixtureDocument result{};
    result.producer.product=str<32>("example-userland");
    result.producer.version=str<64>("0.0.0-test");
    result.scan_id[0]=0x10U;
    return result;
}
std::string json(const FixtureDocument& fixture)
{
    const auto document=fixture.view();
    auto size=w::discovery_json_size(document);
    CHECK(size);
    if(!size) { std::fprintf(stderr,"discovery_json_size error=%u\n",static_cast<unsigned>(size.error())); return {}; }
    std::vector<std::uint8_t> output(size.value(),0xa5U);
    auto result=w::encode_discovery_json(document,{output.data(),output.size()});
    CHECK(result&&result.value()==size.value());
    if(!result)return {};
    CHECK(output.back()=='\n');
    return std::string(reinterpret_cast<const char*>(output.data()),output.size());
}
bool has(const std::string& text,const char* part)
{ return text.find(part)!=std::string::npos; }

FixtureDocument cold_scan()
{
    auto scan=empty_scan();
    scan.usb_functions.count=1U;
    auto& record=scan.usb_functions.items[0];
    auto& f=record.observation;
    f.id=0U;
    f.vid=0x1738U;f.pid=0x5211U;
    f.mode=w::Mode::cold;
    f.location.usb_path.present=true;
    f.location.usb_path.value=str<dtv::userland::detail::kMaxUsbPathBytes>("1-2");
    f.location.bus.present=true;f.location.bus.value=1U;
    f.location.address.present=true;f.location.address.value=4U;
    f.serial_state=w::SerialState::not_queried;
    f.identification_reason=w::IdentificationReason::cold_model;
    record.grouping=w::DiscoveryGroupingState::unresolved;
    record.grouping_reason.present=true;
    record.grouping_reason.value=str<64>("cold_model");
    record.selector_candidates.count=1U;
    auto& candidate=record.selector_candidates.items[0];
    candidate.value=str<w::kMaxDiscoverySelectorBytes>("usb:1-2");
    candidate.state=w::DiscoveryCandidateState::requires_resolution;
    candidate.reason.present=true;
    candidate.reason.value=str<64>("cold_model");
    return scan;
}
FixtureDocument bound_scan()
{
    auto scan=cold_scan();
    auto& function=scan.usb_functions.items[0];
    function.observation.vid=0x06b8U;
    function.observation.pid=0x102bU;
    function.observation.mode=w::Mode::runtime;
    function.observation.model.present=true;
    function.observation.model.value.id=str<64>("synthetic-receiver");
    function.observation.model.value.name=str<128>("Synthetic receiver");
    function.observation.identification_reason=w::IdentificationReason::none;
    function.grouping=w::DiscoveryGroupingState::bound;
    function.grouping_reason.present=false;
    function.enclosure_id.present=true;function.enclosure_id.value=0U;
    auto& candidate=function.selector_candidates.items[0];
    candidate.state=w::DiscoveryCandidateState::usable;
    candidate.reason.present=false;

    scan.enclosures.count=1U;
    auto& e=scan.enclosures.items[0];
    e.descriptor.id=0U;
    e.descriptor.model=function.observation.model;
    e.descriptor.identity.serial_state=w::IdentitySerialState::absent;
    e.descriptor.identity.uniqueness=w::Uniqueness::not_applicable;
    e.descriptor.limits.max_concurrent_streams.present=true;
    e.descriptor.limits.max_concurrent_streams.value=1U;
    e.topology.state=w::TopologyState::complete;
    e.topology.expected_function_count.present=true;
    e.topology.expected_function_count.value=1U;
    e.usb_function_ids.count=1U;
    e.usb_function_ids.items[0]=0U;
    e.selector_candidates.count=1U;
    e.selector_candidates.items[0]=candidate;
    e.receivers.present=true;
    e.receivers.value.count=1U;
    auto& r=e.receivers.value.items[0];
    r.id=0U;r.systems.mask=1U;
    r.binding.present=true;
    r.binding.value.function_id=0U;r.binding.value.local_id=0U;
    e.readers.present=true;
    e.readers.value.count=0U;
    return scan;
}
void expect_invalid(const FixtureDocument& value)
{
    const auto document=value.view();
    auto size=w::discovery_json_size(document);
    CHECK(!size&&size.error()==Error::INVALID_ARGUMENT);
    std::vector<std::uint8_t> output(4096U,0xa5U);
    auto result=w::encode_discovery_json(document,{output.data(),output.size()});
    CHECK(!result&&result.error()==Error::INVALID_ARGUMENT);
    for(const auto c:output)CHECK(c==0xa5U);
}

void test_empty()
{
    const std::string out=json(empty_scan());
    CHECK(has(out,"\"name\":\"userland.discovery\",\"major\":1,\"minor\":0"));
    CHECK(!has(out,"\"id\":\"10000000000000000000000000000000\""));
    CHECK(has(out,"\"complete\":true,\"issues\":[]"));
    CHECK(has(out,"\"enclosures\":[],\"ungrouped_usb_devices\":[]"));
    CHECK(out.back()=='\n');
}
void test_cold_and_partial()
{
    auto cold=cold_scan();
    auto steps=empty_scan();
    steps.usb_functions.count=1U;
    CHECK(w::discovery_json_size(steps.view()));
    steps.usb_functions.items[0].observation=cold.usb_functions.items[0].observation;
    CHECK(w::discovery_json_size(steps.view()));
    steps.usb_functions.items[0].grouping_reason=cold.usb_functions.items[0].grouping_reason;
    CHECK(w::discovery_json_size(steps.view()));
    steps.usb_functions.items[0].selector_candidates=
        cold.usb_functions.items[0].selector_candidates;
    CHECK(w::discovery_json_size(steps.view()));
    const std::string out=json(cold);
    CHECK(has(out,"\"usb\":\"1738:5211\""));
    CHECK(!has(out,"\"firmware_loaded\":true"));
    CHECK(has(out,"\"port\":\"1-2\""));
    CHECK(has(out,"\"ungrouped_usb_devices\":[{"));
    CHECK(!has(out,"\"requires_resolution\""));
    CHECK(has(out,"\"enclosures\":[]"));
    CHECK(!has(out,"\"present\":"));
    CHECK(!has(out,"\"last_error\":"));

    cold.complete=false;
    cold.issues.count=1U;
    cold.issues.items[0].code=static_cast<std::uint32_t>(Error::USB_IO);
    cold.issues.items[0].detail=str<256>("synthetic enumeration failure");
    const std::string partial=json(cold);
    CHECK(has(partial,"\"complete\":false"));
    CHECK(has(partial,"\"detail\":\"synthetic enumeration failure\""));
    CHECK(has(partial,"\"ungrouped_usb_devices\":["));
}
void test_bound()
{
    auto snapshot=bound_scan();
    const auto out=json(snapshot);
    CHECK(has(out,"\"model\":\"Synthetic receiver\""));
    CHECK(has(out,"\"in_loader_mode\":false"));
    CHECK(has(out,"\"serial\":null,\"serial_unique\":null"));
    CHECK(has(out,"\"devices\":[{\"device\":1,\"usb\":\"06b8:102b\""));
    CHECK(has(out,"\"receivers\":[{\"receiver\":0,\"device\":1,\"local\":0,\"systems\":[\"ISDB-T\"]"));
    CHECK(has(out,"\"lnb_15v_supported\":false"));
    CHECK(has(out,"\"readers\":[]"));
    CHECK(!has(out,"max_concurrent_streams"));
}
void test_model_absent_but_topology_known()
{
    // Model metadata may be unavailable even when the actual topology and
    // public receiver/reader collections have been established.
    auto known=bound_scan();
    known.enclosures.items[0].descriptor.model.present=false;
    const auto output=json(known);
    CHECK(!output.empty());
    CHECK(has(output,"\"model\":null,\"serial\":null"));
    CHECK(!has(output,"\"topology\""));
    CHECK(has(output,"\"receivers\":[{\"receiver\":0"));
    CHECK(has(output,"\"readers\":[]"));

    // Once topology is known, omission is not a substitute for known-empty.
    known.enclosures.items[0].readers.present=false;
    expect_invalid(known);
}

void test_atomicity_and_refs()
{
    const auto good=bound_scan();
    const auto size=w::discovery_json_size(good.view());
    CHECK(size);
    if(size&&size.value()>1U){
        std::vector<std::uint8_t> short_output(size.value()-1U,0xa5U);
        auto result=w::encode_discovery_json(good.view(),
                                           {short_output.data(),short_output.size()});
        CHECK(!result&&result.error()==Error::BUFFER_TOO_SMALL);
        for(const auto c:short_output)CHECK(c==0xa5U);
    }
    auto bad=good;
    bad.enclosures.items[0].usb_function_ids.items[0]=9U;expect_invalid(bad);
    bad=good;bad.usb_functions.items[0].enclosure_id.value=3U;expect_invalid(bad);
    bad=good;bad.enclosures.items[0].receivers.value.items[0].binding.value.function_id=3U;
    expect_invalid(bad);
    bad=good;bad.enclosures.items[0].topology.expected_function_count.value=2U;
    expect_invalid(bad);
    bad=good;bad.enclosures.items[0].readers.present=false;expect_invalid(bad);
    bad=good;bad.usb_functions.items[0].selector_candidates.items[0].value=
        str<w::kMaxDiscoverySelectorBytes>("usb:not-a-port");expect_invalid(bad);
    bad=good;bad.usb_functions.items[0].observation.serial_state=w::SerialState::absent;
    bad.usb_functions.items[0].observation.serial.present=true;
    bad.usb_functions.items[0].observation.serial.value=
        str<dtv::userland::detail::kMaxSerialBytes>("invented");expect_invalid(bad);
    bad=good;bad.complete=false;expect_invalid(bad);
    bad=good;bad.issues.count=1U;
    bad.issues.items[0].code=7U;
    bad.issues.items[0].detail=str<256>("unknown");
    expect_invalid(bad); // complete=true cannot silently contain partial scan issues
    bad=good;bad.usb_functions.items[0].observation.model.value.id.length=1U;
    bad.usb_functions.items[0].observation.model.value.id.bytes[0]=static_cast<char>(0xc0);
    expect_invalid(bad);
}
void test_sorted_functions()
{
    auto d=empty_scan();
    d.usb_functions.count=2U;
    d.usb_functions.items[0].observation.id=8U;
    d.usb_functions.items[1].observation.id=2U;
    const auto out=json(d);
    CHECK(has(out,"\"ungrouped_usb_devices\":[{"));
    CHECK(!has(out,"\"usb_functions\""));
    d.usb_functions.items[0].observation.id=2U;
    expect_invalid(d); // duplicate IDs
}
void test_scan_serial_uniqueness()
{
    auto d = bound_scan();
    auto& first = d.enclosures.items[0].descriptor.identity;
    first.serial_state = w::IdentitySerialState::present;
    first.serial.present = true;
    first.serial.value = str<dtv::userland::detail::kMaxSerialBytes>("same-serial");
    first.uniqueness = w::Uniqueness::unknown;
    const auto solo = json(d);
    CHECK(has(solo, "\"serial_unique\":true"));

    // A second enclosure with a shared serial makes BOTH values false.
    d.usb_functions.count = 2U;
    d.usb_functions.items[1] = d.usb_functions.items[0];
    d.usb_functions.items[1].observation.id = 1U;
    d.usb_functions.items[1].enclosure_id.value = 1U;
    d.usb_functions.items[1].observation.location.usb_path.value =
        str<dtv::userland::detail::kMaxUsbPathBytes>("1-3");
    d.usb_functions.items[1].selector_candidates.items[0].value =
        str<w::kMaxDiscoverySelectorBytes>("usb:1-3");

    d.enclosures.count = 2U;
    d.enclosures.items[1] = d.enclosures.items[0];
    d.enclosures.items[1].descriptor.id = 1U;
    d.enclosures.items[1].usb_function_ids.items[0] = 1U;
    d.enclosures.items[1].receivers.value.items[0].binding.value.function_id = 1U;
    d.enclosures.items[1].selector_candidates.items[0].value =
        str<w::kMaxDiscoverySelectorBytes>("usb:1-3");
    const auto duplicated = json(d);
    CHECK(!duplicated.empty());
    std::size_t count = 0;
    for (std::size_t i = 0; (i = duplicated.find("\"serial_unique\":false", i))
         != std::string::npos; i += 17) ++count;
    CHECK(count == 2U);

    d.enclosures.items[1].descriptor.identity.serial.value =
        str<dtv::userland::detail::kMaxSerialBytes>("other-serial");
    const auto distinct = json(d);
    CHECK(has(distinct,"\"serial_unique\":true"));
    d.complete = false;
    d.issues.count = 1U;
    d.issues.items[0].code = static_cast<std::uint32_t>(Error::USB_IO);
    d.issues.items[0].detail = str<256>("incomplete enumeration");
    CHECK(has(json(d), "\"serial_unique\":null"));
}

void test_two_usb_eight_receiver_enclosure()
{
    auto d = bound_scan();
    auto& e = d.enclosures.items[0];
    e.descriptor.model.value.id = str<64>("PX-Q3U4");
    e.descriptor.model.value.name = str<128>("PX-Q3U4");
    e.descriptor.identity.serial_state = w::IdentitySerialState::present;
    e.descriptor.identity.serial.present = true;
    e.descriptor.identity.serial.value =
        str<dtv::userland::detail::kMaxSerialBytes>("00001205000960");
    e.descriptor.identity.uniqueness = w::Uniqueness::unknown;
    d.usb_functions.items[0].observation.vid = 0x0511U;
    d.usb_functions.items[0].observation.pid = 0x084aU;
    d.usb_functions.items[0].observation.model.value.id = str<64>("PX-Q3U4");
    d.usb_functions.items[0].observation.model.value.name = str<128>("PX-Q3U4");

    d.usb_functions.count = 2U;
    d.usb_functions.items[1] = d.usb_functions.items[0];
    d.usb_functions.items[1].observation.id = 1U;
    d.usb_functions.items[1].observation.location.usb_path.value =
        str<dtv::userland::detail::kMaxUsbPathBytes>("1-2.2.2");
    d.usb_functions.items[1].selector_candidates.items[0].value =
        str<w::kMaxDiscoverySelectorBytes>("usb:1-2.2.2");

    e.usb_function_ids.count = 2U;
    e.usb_function_ids.items[1] = 1U;
    e.topology.expected_function_count.value = 2U;
    e.receivers.value.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        auto& receiver = e.receivers.value.items[i];
        receiver.id = static_cast<std::uint16_t>(i);
        receiver.binding.present = true;
        receiver.binding.value.function_id = i < 4U ? 0U : 1U;
        receiver.binding.value.local_id = static_cast<std::uint16_t>(i % 4U);
        receiver.systems.mask = i % 4U < 2U ? 2U : 1U;
        if (i % 4U == 0U) {
            receiver.capabilities.controls = w::kControlBitLnb;
            receiver.capabilities.lnb.values.count = 2U;
            receiver.capabilities.lnb.values.items[0] = 0U;
            receiver.capabilities.lnb.values.items[1] = 15U;
        }
    }
    d.usb_functions.items[1].observation.id = 1U;
    e.readers.value.count = 1U;
    e.readers.value.items[0].id = 0U;
    e.readers.value.items[0].binding.present = true;
    e.readers.value.items[0].binding.value.function_id = 0U;
    e.readers.value.items[0].binding.value.local_id = 0U;
    d.usb_functions.items[0].observation.location.usb_path.value =
        str<dtv::userland::detail::kMaxUsbPathBytes>("1-2.2.1");
    d.usb_functions.items[0].selector_candidates.items[0].value =
        str<w::kMaxDiscoverySelectorBytes>("usb:1-2.2.1");
    const auto out = json(d);
    CHECK(has(out,"\"serial_unique\":true"));
    CHECK(has(out,"\"lnb_15v_supported\":true"));
    CHECK(has(out,"\"reader\":0,\"device\":1"));
    CHECK(has(out,"\"device\":2,\"usb\":\"0511:084a\""));
    CHECK(has(out,"\"receiver\":7,\"device\":2,\"local\":3,\"systems\":[\"ISDB-T\"]"));
    std::size_t count = 0;
    for (std::size_t i = 0; (i = out.find("\"receiver\":", i))
         != std::string::npos; i += 11) ++count;
    CHECK(count == 8U);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--emit") == 0) {
        const auto output = json(bound_scan());
        std::fwrite(output.data(), 1U, output.size(), stdout);
        return output.empty() ? 1 : 0;
    }
    test_empty();
    test_cold_and_partial();
    test_bound();
    test_model_absent_but_topology_known();
    test_atomicity_and_refs();
    test_sorted_functions();
    test_scan_serial_uniqueness();
    test_two_usb_eight_receiver_enclosure();
    return failures==0?0:1;
}
