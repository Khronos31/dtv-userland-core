// SPDX-License-Identifier: MIT
// Representative proof for the optional identity constraint in begin_card.
// One process, one fake transport: a mismatched product or instance must stop
// after HELLO/LIST/STATUS without sending CARD_STATUS/CARD_CONNECT, the
// matching pair must reach CARD_CONNECT, and the empty-view input semantics
// must be exact without a separate test matrix.
#include <dtv/userland/control_client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Timeout;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #condition);                                \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

constexpr std::uint16_t kReaderId = 0U;
constexpr std::uint64_t kHandle = 9U;
constexpr std::uint64_t kGeneration = 3U;
constexpr std::uint8_t kEpochByte = 0x5aU;

template <std::size_t N>
bool set_text(w::WString<N>& out, const char* value) noexcept
{
    const std::size_t length = std::strlen(value);
    if (length > out.bytes.size()) return false;
    out.length = static_cast<std::uint16_t>(length);
    std::memcpy(out.bytes.data(), value, length);
    return true;
}

ByteView view(const char* bytes) noexcept
{
    return ByteView{reinterpret_cast<const std::uint8_t*>(bytes), std::strlen(bytes)};
}

void set_producer(w::Producer& producer, const char* product) noexcept
{
    (void)set_text(producer.product, product);
    producer.version.length = 5U;
    std::memcpy(producer.version.bytes.data(), "1.2.3", 5U);
}

w::ListResponse fixture_list(const char* product) noexcept
{
    w::ListResponse list{};
    list.epoch[0] = kEpochByte;
    set_producer(list.producer, product);
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0];
    function.id = 0U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.location.usb_path.present = true;
    function.location.usb_path.value.length = 3U;
    std::memcpy(function.location.usb_path.value.bytes.data(), "1-2", 3U);
    function.serial_state = w::SerialState::present;
    function.serial.present = true;
    (void)set_text(function.serial.value, "function-serial");
    function.model.present = true;
    (void)set_text(function.model.value.id, "model-id");
    (void)set_text(function.model.value.name, "Model name");
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    (void)set_text(list.enclosure.value.model.value.id, "model-id");
    (void)set_text(list.enclosure.value.model.value.name, "Model name");
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    list.enclosure.value.identity.serial.present = true;
    (void)set_text(list.enclosure.value.identity.serial.value, "enclosure-serial");
    list.enclosure.value.identity.uniqueness = w::Uniqueness::unique_in_scan;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0].id = 0U;
    list.receivers.value.items[0].systems.mask = 1U;
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0].id = kReaderId;
    return list;
}

w::StatusResponse fixture_status(const w::ListResponse& list,
                                 const char* instance) noexcept
{
    w::StatusResponse status{};
    status.producer = list.producer;
    status.context.epoch = list.epoch;
    status.context.state = w::ContextState::ready;
    status.context.instance.present = true;
    (void)set_text(status.context.instance.value, instance);
    status.snapshot.sequence = 1U;
    status.snapshot.observed_at_ns = 5U;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology = list.topology;
    status.functions.present = true;
    status.functions.value = list.functions;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    status.receivers.value.items[0].descriptor = list.receivers.value.items[0];
    status.readers.present = true;
    status.readers.value.count = 1U;
    status.readers.value.items[0].descriptor = list.readers.value.items[0];
    status.readers.value.items[0].present = true;
    status.readers.value.items[0].card.state = w::CardState::present;
    status.readers.value.items[0].card.sampled_at_ns.present = true;
    status.readers.value.items[0].card.sampled_at_ns.value = 5U;
    status.enclosure.present = true;
    status.enclosure.value = list.enclosure.value;
    status.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    return status;
}

class FakeTransport final : public w::ClientTransport {
public:
    FakeTransport(const char* product, const char* instance) noexcept
        : product_(product), instance_(instance) {}

    dtv::userland::Result<void> write_all(ByteView bytes, Timeout) noexcept override
    {
        const auto frame = w::decode_frame(bytes);
        if (!frame) return dtv::userland::Result<void>::failure(frame.error());
        opcodes_.push_back(frame.value().header.opcode);
        const std::uint16_t opcode = frame.value().header.opcode;
        const std::uint32_t request_id = frame.value().header.request_id;
        if (opcode == w::kOpcodeHello) {
            w::HelloResponse hello{};
            hello.major = w::kVersionMajor;
            hello.minor = w::kVersionMinor;
            hello.features = w::kFeatureCardService;
            hello.epoch[0] = kEpochByte;
            set_producer(hello.producer, product_);
            hello.max_control_payload = static_cast<std::uint32_t>(w::kMaxControlPayload);
            hello.max_ts_bytes = static_cast<std::uint32_t>(w::kMaxTsPacketBytes);
            queue(w::kOpcodeHello, request_id, hello, &w::encode_hello_response);
        } else if (opcode == w::kOpcodeList) {
            const w::ListResponse list = fixture_list(product_);
            queue(w::kOpcodeList, request_id, list, &w::encode_list_response);
        } else if (opcode == w::kOpcodeStatus) {
            const w::StatusResponse status =
                fixture_status(fixture_list(product_), instance_);
            queue(w::kOpcodeStatus, request_id, status, &w::encode_status_response);
        } else if (opcode == w::kOpcodeCardStatus) {
            w::CardStatusResponse status{};
            status.key.epoch[0] = kEpochByte;
            status.key.reader_id = kReaderId;
            status.current_generation = kGeneration;
            status.present = true;
            status.initialized = true;
            status.protocol.present = true;
            status.protocol.value = w::CardProtocol::t1;
            status.atr.present = true;
            status.atr.value.length = 2U;
            status.atr.value.bytes[0] = 0x3bU;
            status.atr.value.bytes[1] = 0x00U;
            queue(w::kOpcodeCardStatus, request_id, status,
                  &w::encode_card_status_response);
        } else if (opcode == w::kOpcodeCardConnect) {
            w::CardSessionResult session{};
            session.epoch[0] = kEpochByte;
            session.reader_id = kReaderId;
            session.card_handle = kHandle;
            session.bound_generation = kGeneration;
            session.current_generation = kGeneration;
            session.present = true;
            session.initialized = true;
            session.usable = true;
            session.protocol.present = true;
            session.protocol.value = w::CardProtocol::t1;
            session.atr.present = true;
            session.atr.value.length = 2U;
            session.atr.value.bytes[0] = 0x3bU;
            session.atr.value.bytes[1] = 0x00U;
            queue(w::kOpcodeCardConnect, request_id, session,
                  &w::encode_card_session_result);
        } else {
            return dtv::userland::Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        return dtv::userland::Result<void>::success();
    }

    dtv::userland::Result<std::size_t> read_some(MutableByteView output,
                                                 Timeout) noexcept override
    {
        if (offset_ < size_) {
            const std::size_t count = std::min(output.size, size_ - offset_);
            std::memcpy(output.data, queued_.data() + offset_, count);
            offset_ += count;
            return dtv::userland::Result<std::size_t>::success(count);
        }
        return dtv::userland::Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    }

    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void close() noexcept override { ++closes_; }
    const std::vector<std::uint16_t>& opcodes() const noexcept { return opcodes_; }
    std::size_t closes() const noexcept { return closes_; }

private:
    template <typename Record>
    void queue(std::uint16_t opcode, std::uint32_t request_id, const Record& value,
               dtv::userland::Result<std::size_t> (*encode)(
                   const Record&, MutableByteView) noexcept) noexcept
    {
        std::array<std::uint8_t, w::kMaxControlPayload> payload{};
        const auto payload_size = encode(value, MutableByteView{payload.data(), payload.size()});
        if (!payload_size) return;
        w::Header header{};
        header.kind = w::Kind::response;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload_size.value());
        const auto framed = w::encode_frame(
            header, ByteView{payload.data(), payload_size.value()},
            MutableByteView{queued_.data(), queued_.size()});
        if (framed) {
            size_ = framed.value();
            offset_ = 0U;
        }
    }

    const char* product_;
    const char* instance_;
    std::vector<std::uint16_t> opcodes_;
    std::array<std::uint8_t, w::kHeaderSize + w::kMaxControlPayload> queued_{};
    std::size_t size_ = 0U;
    std::size_t offset_ = 0U;
    std::size_t closes_ = 0U;
};

bool mismatch_stops_before_card(const char* expected_product,
                                const char* expected_instance)
{
    FakeTransport transport("px4-userland", "rooma");
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    options.expected_product = view(expected_product);
    options.expected_instance = view(expected_instance);
    const auto result = w::begin_card(transport, options, Timeout{100U});
    CHECK(!result && result.error == Error::NOT_FOUND);
    const std::vector<std::uint16_t> expected{
        w::kOpcodeHello, w::kOpcodeList, w::kOpcodeStatus};
    CHECK(transport.opcodes() == expected);
    CHECK(transport.closes() == 1U);
    return true;
}

bool test_begin_card_identity()
{
    mismatch_stops_before_card("asicen-userland", "rooma");
    mismatch_stops_before_card("px4-userland", "roomb");
    mismatch_stops_before_card("px4-userland", "ROOMA");

    FakeTransport transport("px4-userland", "rooma");
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    options.expected_product = view("px4-userland");
    options.expected_instance = view("rooma");
    const auto opened = w::begin_card(transport, options, Timeout{100U});
    CHECK(opened);
    CHECK(opened.session.card_handle == kHandle &&
          opened.session.bound_generation == kGeneration);
    const std::vector<std::uint16_t> expected{
        w::kOpcodeHello, w::kOpcodeList, w::kOpcodeStatus,
        w::kOpcodeCardStatus, w::kOpcodeCardConnect};
    CHECK(transport.opcodes() == expected);
    return true;
}

bool test_empty_view_semantics()
{
    FakeTransport transport("px4-userland", "rooma");
    const auto status = w::fetch_status(transport, Timeout{100U});
    CHECK(status);
    if (!status) return false;

    const std::uint8_t empty[] = {0U};
    // Null data with size zero and non-null data with size zero both mean
    // "no constraint"; a positive size with null data cannot match.
    CHECK(w::status_identity_matches(status.snapshot, ByteView{}, ByteView{}));
    CHECK(w::status_identity_matches(
        status.snapshot, ByteView{empty, 0U}, ByteView{empty, 0U}));
    CHECK(!w::status_identity_matches(
        status.snapshot, ByteView{nullptr, 1U}, ByteView{}));
    CHECK(!w::status_identity_matches(
        status.snapshot, ByteView{}, ByteView{nullptr, 1U}));
    CHECK(w::status_identity_matches(
        status.snapshot, view("px4-userland"), view("rooma")));
    CHECK(!w::status_identity_matches(
        status.snapshot, view("px4-userland"), view("ROOMA")));
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_empty_view_semantics() && test_begin_card_identity();
    return ok && failures == 0 ? 0 : 1;
}
