// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace w = dtv::userland::wire;
using dtv::userland::Error;
int failures = 0;

#define CHECK(value)                                                                              \
    do {                                                                                          \
        if (!(value)) {                                                                           \
            ++failures;                                                                           \
            std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #value);     \
        }                                                                                         \
    } while (false)

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> out;
    out.length = static_cast<std::uint16_t>(std::strlen(value));
    std::memcpy(out.bytes.data(), value, out.length);
    return out;
}

w::StatusResponse make_status(w::ListResponse& list)
{
    w::StatusResponse status;
    status.producer.product = text<32>("px4");
    status.producer.version = text<64>("1.0.0");
    status.context.epoch[0] = 1U;
    status.context.state = w::ContextState::ready;
    status.snapshot.sequence = 9007199254740993ULL;
    status.snapshot.observed_at_ns = 7U;
    status.scope.kind = w::ScopeKind::enclosure;
    status.enclosure.present = true;
    status.enclosure.value.id = 0U;
    status.enclosure.value.model.present = true;
    status.enclosure.value.model.value.name = text<128>("PX-Q3U4");
    status.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    status.enclosure.value.identity.serial.present = true;
    status.enclosure.value.identity.serial.value = text<512>("serial-\"\\\t");
    status.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    status.lnb_15v_enabled.present = true;
    status.lnb_15v_enabled.value = true;
    status.card_inserted.present = true;
    status.card_inserted.value = false;
    status.topology.state = w::TopologyState::complete;
    status.topology.expected_function_count.present = true;
    status.topology.expected_function_count.value = 1U;
    status.functions.present = true;
    status.functions.value.count = 1U;
    w::UsbFunction& function = status.functions.value.items[0];
    function.id = 2U;
    function.vid = 0x0511U;
    function.pid = 0x084aU;
    function.mode = w::Mode::runtime;
    function.location.usb_path.present = true;
    function.location.usb_path.value = text<64>("1-2.3");
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    w::ReceiverStatus& receiver = status.receivers.value.items[0];
    receiver.descriptor.id = 0U;
    receiver.descriptor.systems.mask = 3U;
    receiver.descriptor.binding.present = true;
    receiver.descriptor.binding.value.function_id = 2U;
    receiver.descriptor.binding.value.local_id = 0U;
    receiver.present = true;
    receiver.tune_revision = 9007199254740993ULL;
    receiver.receiver_statistics.entries.count = 1U;
    w::CounterEntry& counter = receiver.receiver_statistics.entries.items[0];
    counter.id = w::kCounterIdCapturedPackets;
    counter.counter.measurement.state = w::MeasureState::valid;
    counter.counter.measurement.value = 18446744073709551615ULL;
    counter.counter.measurement.sampled_at_ns = 7U;
    status.readers.present = true;
    status.readers.value.count = 0U;

    list = w::ListResponse{};
    list.epoch = status.context.epoch;
    list.producer = status.producer;
    list.topology = status.topology;
    list.functions.count = 1U;
    list.functions.items[0] = function;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0] = receiver.descriptor;
    list.readers.present = true;
    return status;
}

std::string encode(const w::StatusResponse& status, const w::ListResponse& list)
{
    const w::StatusContext context{&list};
    const auto size = w::status_json_size(status, context);
    if (!size) {
        std::fprintf(stderr, "STATUS validation error=%u shape=%u list=%u\n",
                     static_cast<unsigned>(size.error()),
                     static_cast<unsigned>(w::validate_status_response_scope(status)),
                     static_cast<unsigned>(w::validate_list_response(list)));
    }
    CHECK(size.has_value());
    if (!size) return {};
    std::vector<std::uint8_t> output(size.value(), 0xa5U);
    const auto written = w::encode_status_json(status, context,
                                               {output.data(), output.size()});
    CHECK(written.has_value() && written.value() == size.value());
    if (!written) return {};
    CHECK(output.back() == '\n');
    return std::string(reinterpret_cast<const char*>(output.data()), written.value());
}

void test_projection_contract()
{
    w::ListResponse list;
    w::StatusResponse status = make_status(list);
    w::ReceiverStatus& receiver = status.receivers.value.items[0U];
    receiver.stream_statistics.present = true;
    receiver.stream_statistics.value.entries.count = 5U;
    receiver.stream_statistics.value.entries.items[0U].id = w::kCounterIdTransmittedPackets;
    receiver.stream_statistics.value.entries.items[0U].counter.measurement.state =
        w::MeasureState::valid;
    receiver.stream_statistics.value.entries.items[0U].counter.measurement.value = 0U;
    receiver.stream_statistics.value.entries.items[0U].counter.measurement.sampled_at_ns = 7U;
    receiver.stream_statistics.value.entries.items[1U].id = w::kCounterIdQueueDroppedPackets;
    receiver.stream_statistics.value.entries.items[1U].counter.measurement.state =
        w::MeasureState::unknown;
    receiver.stream_statistics.value.entries.items[1U].counter.measurement.reason =
        w::UnknownReason::stale;
    receiver.stream_statistics.value.entries.items[2U].id = w::kCounterIdStreamBytes;
    receiver.stream_statistics.value.entries.items[2U].counter.measurement.state =
        w::MeasureState::unsupported;
    receiver.stream_statistics.value.entries.items[3U].id = w::kCounterIdSyncErrors;
    receiver.stream_statistics.value.entries.items[3U].counter.measurement.state =
        w::MeasureState::valid;
    receiver.stream_statistics.value.entries.items[3U].counter.measurement.value = 3U;
    receiver.stream_statistics.value.entries.items[3U].counter.measurement.sampled_at_ns = 7U;
    receiver.stream_statistics.value.entries.items[4U].id = 13U;
    receiver.stream_statistics.value.entries.items[4U].counter.measurement.state =
        w::MeasureState::unknown;
    receiver.stream_statistics.value.entries.items[4U].counter.measurement.reason =
        w::UnknownReason::not_sampled;
    const std::string json = encode(status, list);
    CHECK(json.find("\"model\":\"PX-Q3U4\"") != std::string::npos);
    CHECK(json.find("model_id") == std::string::npos);
    CHECK(json.find("\"serial\":\"serial-\\\"\\\\\\t\"") != std::string::npos);
    CHECK(json.find("\"lnb_15v_enabled\":true") != std::string::npos);
    CHECK(json.find("\"card_inserted\":false") != std::string::npos);
    CHECK(json.find("\"systems\":[\"ISDB-T\",\"ISDB-S\"]") != std::string::npos);
    CHECK(json.find("\"receiver\":0") != std::string::npos);
    CHECK(json.find("\"function_id\":2") != std::string::npos);
    CHECK(json.find("\"usb\":\"0511:084a\"") != std::string::npos);
    CHECK(json.find("\"sequence\":\"9007199254740993\"") != std::string::npos);
    CHECK(json.find("\"captured_packets\":{\"value\":\"18446744073709551615\"") !=
          std::string::npos);
    CHECK(json.find("\"transmitted_packets\":{\"value\":\"0\"") !=
          std::string::npos);
    CHECK(json.find("\"queue_dropped_packets\":{\"value\":null") !=
          std::string::npos);
    CHECK(json.find("\"sync_errors\":{\"value\":\"3\"") !=
          std::string::npos);
    CHECK(json.find("\"stream_bytes\"") == std::string::npos);
    CHECK(json.find("\"queue_dropped_packets\":{\"value\":\"0\"") ==
          std::string::npos);
    CHECK(json.find("\"13\"") == std::string::npos);
    CHECK(json.find("serial_state") == std::string::npos);
    CHECK(json.find("uniqueness") == std::string::npos);
    CHECK(json.find("serial_unique") == std::string::npos);

    status.functions.value.items[0].vid = 0x1234U;
    status.functions.value.items[0].pid = 0x1234U;
    list.functions.items[0] = status.functions.value.items[0];
    const std::string equal_ids = encode(status, list);
    CHECK(equal_ids.find("\"usb\":\"1234:1234\"") != std::string::npos);

    const bool states[] = {false, true};
    for (bool value : states) {
        status.lnb_15v_enabled.present = true;
        status.lnb_15v_enabled.value = value;
        status.card_inserted.present = true;
        status.card_inserted.value = value;
        const std::string current = encode(status, list);
        CHECK(current.find(value ? "\"lnb_15v_enabled\":true"
                                 : "\"lnb_15v_enabled\":false") != std::string::npos);
        CHECK(current.find(value ? "\"card_inserted\":true"
                                 : "\"card_inserted\":false") != std::string::npos);
    }
    status.lnb_15v_enabled.present = false;
    status.card_inserted.present = false;
    const std::string unknown = encode(status, list);
    CHECK(unknown.find("\"lnb_15v_enabled\":null") != std::string::npos);
    CHECK(unknown.find("\"card_inserted\":null") != std::string::npos);
}

void test_unknown_errors_and_atomic_failures()
{
    w::ListResponse list;
    w::StatusResponse status = make_status(list);
    const std::uint32_t unknown_codes[] = {16U, 18U, 19U, 20U, 0xfedcba98U};
    std::array<std::uint8_t, 8192> output{};
    for (std::uint32_t code : unknown_codes) {
        status.context.last_error.present = true;
        status.context.last_error.value.code = code;
        status.context.last_error.value.operation = text<32>("read\"status");
        status.context.last_error.value.detail = text<256>("read\\failed\n日本語");
        const w::StatusContext context{&list};
        const auto size = w::status_json_size(status, context);
        CHECK(size.has_value());
        if (!size) continue;
        const auto written = w::encode_status_json(status, context,
                                                   {output.data(), output.size()});
        CHECK(written.has_value());
        if (!written) continue;
        const std::string json(reinterpret_cast<const char*>(output.data()), written.value());
        const std::string expected = "\"code\":\"UNKNOWN\",\"numeric_code\":" +
                                     std::to_string(code);
        CHECK(json.find(expected) != std::string::npos);
        CHECK(json.find("STALE_HANDLE") == std::string::npos);
        CHECK(json.find("CANCELLED") == std::string::npos);
        CHECK(json.find("ACCESS_DENIED") == std::string::npos);
        CHECK(json.find("INVALID_STATE") == std::string::npos);
        CHECK(json.find("read\\\"status") != std::string::npos);
        CHECK(json.find("read\\\\failed\\n日本語") != std::string::npos);
    }

    status.context.last_error.value.code = 6U;
    const w::StatusContext known_context{&list};
    const auto known_size = w::status_json_size(status, known_context);
    CHECK(known_size.has_value());
    if (known_size) {
        const auto known_written = w::encode_status_json(
            status, known_context, {output.data(), output.size()});
        CHECK(known_written.has_value());
        if (known_written) {
            const std::string json(reinterpret_cast<const char*>(output.data()),
                                   known_written.value());
            CHECK(json.find("\"code\":\"TIMEOUT\"") != std::string::npos);
            CHECK(json.find("numeric_code") == std::string::npos);
        }
    }

    status.context.last_error.present = false;
    const w::StatusContext context{&list};
    const auto size = w::status_json_size(status, context);
    CHECK(size.has_value());
    if (!size) return;
    output.fill(0xa5U);
    const auto short_result = w::encode_status_json(
        status, context, {output.data(), size.value() - 1U});
    CHECK(!short_result && short_result.error() == Error::BUFFER_TOO_SMALL);
    for (std::uint8_t byte : output) CHECK(byte == 0xa5U);

    status.enclosure.value.identity.uniqueness = w::Uniqueness::duplicate;
    output.fill(0x5aU);
    const auto invalid = w::encode_status_json(status, context,
                                               {output.data(), output.size()});
    CHECK(!invalid && invalid.error() == Error::INVALID_ARGUMENT);
    for (std::uint8_t byte : output) CHECK(byte == 0x5aU);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--json") == 0) {
        w::ListResponse list;
        const w::StatusResponse status = make_status(list);
        const std::string json = encode(status, list);
        std::fwrite(json.data(), 1U, json.size(), stdout);
        return json.empty() ? 1 : 0;
    }
    test_projection_contract();
    test_unknown_errors_and_atomic_failures();
    return failures == 0 ? 0 : 1;
}
