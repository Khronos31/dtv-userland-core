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

w::DiscoveryResponse empty_scan()
{
    w::DiscoveryResponse result{};
    result.producer.product=str<32>("example-userland");
    result.producer.version=str<64>("0.0.0-test");
    result.scan_id[0]=0x10U;
    return result;
}
std::string json(const w::DiscoveryResponse& document)
{
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

w::DiscoveryResponse cold_scan()
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
w::DiscoveryResponse bound_scan()
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
void expect_invalid(const w::DiscoveryResponse& value)
{
    auto size=w::discovery_json_size(value);
    CHECK(!size&&size.error()==Error::INVALID_ARGUMENT);
    std::vector<std::uint8_t> output(4096U,0xa5U);
    auto result=w::encode_discovery_json(value,{output.data(),output.size()});
    CHECK(!result&&result.error()==Error::INVALID_ARGUMENT);
    for(const auto c:output)CHECK(c==0xa5U);
}

void test_empty()
{
    const std::string out=json(empty_scan());
    CHECK(has(out,"\"name\":\"userland.discovery\",\"major\":1,\"minor\":0"));
    CHECK(has(out,"\"id\":\"10000000000000000000000000000000\""));
    CHECK(has(out,"\"complete\":true,\"issues\":[]"));
    CHECK(has(out,"\"usb_functions\":[],\"enclosures\":[]"));
    CHECK(out.back()=='\n');
}
void test_cold_and_partial()
{
    auto cold=cold_scan();
    auto steps=empty_scan();
    steps.usb_functions.count=1U;
    CHECK(w::discovery_json_size(steps));
    steps.usb_functions.items[0].observation=cold.usb_functions.items[0].observation;
    CHECK(w::discovery_json_size(steps));
    steps.usb_functions.items[0].grouping_reason=cold.usb_functions.items[0].grouping_reason;
    CHECK(w::discovery_json_size(steps));
    steps.usb_functions.items[0].selector_candidates=
        cold.usb_functions.items[0].selector_candidates;
    CHECK(w::discovery_json_size(steps));
    const std::string out=json(cold);
    CHECK(has(out,"\"mode\":\"cold\""));
    CHECK(has(out,"\"identification\":{\"model\":null,\"reason\":\"cold_model\"}"));
    CHECK(has(out,"\"grouping\":{\"state\":\"unresolved\",\"enclosure_id\":null,\"reason\":\"cold_model\"}"));
    CHECK(has(out,"\"state\":\"requires_resolution\",\"reason\":\"cold_model\""));
    CHECK(has(out,"\"enclosures\":[]"));
    CHECK(!has(out,"\"present\":"));
    CHECK(!has(out,"\"last_error\":"));

    cold.complete=false;
    cold.issues.count=1U;
    cold.issues.items[0].code=static_cast<std::uint32_t>(Error::USB_IO);
    cold.issues.items[0].detail=str<256>("synthetic enumeration failure");
    const std::string partial=json(cold);
    CHECK(has(partial,"\"complete\":false"));
    CHECK(has(partial,"\"code\":\"USB_IO\",\"detail\":\"synthetic enumeration failure\""));
    CHECK(has(partial,"\"usb_functions\":["));
}
void test_bound()
{
    auto snapshot=bound_scan();
    const auto out=json(snapshot);
    CHECK(has(out,"\"grouping\":{\"state\":\"bound\",\"enclosure_id\":\"0\",\"reason\":null}"));
    CHECK(has(out,"\"identity\":{\"serial\":null,\"serial_state\":\"absent\",\"uniqueness\":\"not_applicable\"}"));
    CHECK(has(out,"\"usb_function_ids\":[\"0\"]"));
    CHECK(has(out,"\"receivers\":[{\"id\":\"0\",\"systems\":[\"isdb-t\"]"));
    CHECK(has(out,"\"binding\":{\"function_id\":\"0\",\"local_id\":\"0\"}"));
    CHECK(has(out,"\"readers\":[]"));
    CHECK(has(out,"\"limits\":{\"max_concurrent_streams\":1}"));
}
void test_atomicity_and_refs()
{
    const auto good=bound_scan();
    const auto size=w::discovery_json_size(good);
    CHECK(size);
    if(size&&size.value()>1U){
        std::vector<std::uint8_t> short_output(size.value()-1U,0xa5U);
        auto result=w::encode_discovery_json(good,
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
    CHECK(out.find("\"id\":\"2\",\"vid\"")<out.find("\"id\":\"8\",\"vid\""));
    d.usb_functions.items[0].observation.id=2U;
    expect_invalid(d); // duplicate IDs
}
} // namespace

int main()
{
    test_empty();
    test_cold_and_partial();
    test_bound();
    test_atomicity_and_refs();
    test_sorted_functions();
    return failures==0?0:1;
}
