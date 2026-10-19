// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/wire.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Timeout;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

constexpr std::uint16_t kReaderId = 2U;
constexpr std::uint64_t kGeneration = 5U;
constexpr std::uint64_t kHandle = 9U;
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

void set_producer(w::Producer& producer) noexcept
{
    (void)set_text(producer.product, "px4-userland");
    producer.version.length = 5U;
    std::memcpy(producer.version.bytes.data(), "1.2.3", 5U);
}

w::ListResponse fixture_list() noexcept
{
    w::ListResponse list{};
    list.epoch[0] = kEpochByte;
    set_producer(list.producer);
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

w::StatusResponse fixture_status(const w::ListResponse& list) noexcept
{
    w::StatusResponse status{};
    status.producer = list.producer;
    status.context.epoch = list.epoch;
    status.context.state = w::ContextState::ready;
    status.context.instance.present = true;
    (void)set_text(status.context.instance.value, "instance");
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

enum class Mode {
    card_service,
    no_card_service,
    transmit_card_removed,
};

class FakeTransport final : public w::ClientTransport {
public:
    explicit FakeTransport(Mode mode) noexcept : mode_(mode) {}

    dtv::userland::Result<void> write_all(ByteView bytes, Timeout) noexcept override
    {
        const auto frame = w::decode_frame(bytes);
        if (!frame) return dtv::userland::Result<void>::failure(frame.error());
        request_ids_.push_back(frame.value().header.request_id);
        const std::uint16_t opcode = frame.value().header.opcode;
        const std::uint32_t request_id = frame.value().header.request_id;
        if (opcode == w::kOpcodeHello) {
            const auto hello = w::decode_hello_request(frame.value().payload);
            if (!hello) return dtv::userland::Result<void>::failure(hello.error());
            hello_required_features_ = hello.value().required_features;
            queue_hello(request_id);
        } else if (opcode == w::kOpcodeList) {
            const w::ListResponse list = fixture_list();
            queue_response(opcode, request_id, list, &w::encode_list_response);
        } else if (opcode == w::kOpcodeStatus) {
            const w::ListResponse list = fixture_list();
            const w::StatusResponse status = fixture_status(list);
            queue_response(opcode, request_id, status, &w::encode_status_response);
        } else if (opcode == w::kOpcodeCardStatus) {
            queue_card_status(request_id);
        } else if (opcode == w::kOpcodeCardConnect) {
            queue_card_session(opcode, request_id);
        } else if (opcode == w::kOpcodeCardTransmit) {
            if (mode_ == Mode::transmit_card_removed) {
                queue_remote_error(opcode, request_id, 13U);
            } else {
                w::CardTransmitResponse response{};
                response.key.epoch[0] = kEpochByte;
                response.key.reader_id = kReaderId;
                response.key.bound_generation = kGeneration;
                response.key.card_handle = kHandle;
                response.response.length = 2U;
                response.response.bytes[0] = 0x90U;
                response.response.bytes[1] = 0x00U;
                queue_response(opcode, request_id, response, &w::encode_card_transmit_response);
            }
        } else if (opcode == w::kOpcodeCardReset) {
            queue_card_session(opcode, request_id);
        } else if (opcode == w::kOpcodeCardDisconnect) {
            w::CardDisconnectResponse response{};
            response.key.epoch[0] = kEpochByte;
            response.key.reader_id = kReaderId;
            response.card_handle = kHandle;
            response.consumed = true;
            response.quiesced = true;
            queue_response(opcode, request_id, response, &w::encode_card_disconnect_response);
        } else {
            return dtv::userland::Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        return dtv::userland::Result<void>::success();
    }

    dtv::userland::Result<std::size_t> read_some(MutableByteView output,
                                                 Timeout) noexcept override
    {
        if (queued_offset_ < queued_size_) {
            const std::size_t count = std::min(output.size, queued_size_ - queued_offset_);
            std::memcpy(output.data, queued_.data() + queued_offset_, count);
            queued_offset_ += count;
            return dtv::userland::Result<std::size_t>::success(count);
        }
        return dtv::userland::Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    }

    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void close() noexcept override { ++close_count_; }
    std::size_t request_count() const noexcept { return request_ids_.size(); }
    std::size_t close_count() const noexcept { return close_count_; }
    std::uint64_t hello_required_features() const noexcept { return hello_required_features_; }
    const std::vector<std::uint32_t>& request_ids() const noexcept { return request_ids_; }

private:
    void queue_frame(std::uint16_t opcode, std::uint32_t request_id,
                     ByteView payload) noexcept
    {
        w::Header header{};
        header.kind = w::Kind::response;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size);
        const auto size = w::encode_frame(
            header, payload, MutableByteView{queued_.data(), queued_.size()});
        if (size) {
            queued_size_ = size.value();
            queued_offset_ = 0U;
        }
    }

    template <typename Record>
    void queue_response(std::uint16_t opcode, std::uint32_t request_id, const Record& value,
                        dtv::userland::Result<std::size_t> (*encode)(
                            const Record&, MutableByteView) noexcept) noexcept
    {
        std::array<std::uint8_t, w::kMaxControlPayload> payload{};
        const auto size = encode(value, MutableByteView{payload.data(), payload.size()});
        if (!size) return;
        queue_frame(opcode, request_id, ByteView{payload.data(), size.value()});
    }

    void queue_hello(std::uint32_t request_id) noexcept
    {
        w::HelloResponse hello{};
        hello.major = w::kVersionMajor;
        hello.minor = w::kVersionMinor;
        hello.features = mode_ == Mode::no_card_service ? 0U : w::kFeatureCardService;
        hello.epoch[0] = kEpochByte;
        set_producer(hello.producer);
        hello.max_control_payload = static_cast<std::uint32_t>(w::kMaxControlPayload);
        hello.max_ts_bytes = static_cast<std::uint32_t>(w::kMaxTsPacketBytes);
        queue_response(w::kOpcodeHello, request_id, hello, &w::encode_hello_response);
    }

    void queue_card_status(std::uint32_t request_id) noexcept
    {
        w::CardStatusResponse response{};
        response.key.epoch[0] = kEpochByte;
        response.key.reader_id = kReaderId;
        response.current_generation = kGeneration;
        response.present = true;
        response.initialized = true;
        response.protocol.present = true;
        response.protocol.value = w::CardProtocol::t1;
        response.atr.present = true;
        response.atr.value.length = 2U;
        response.atr.value.bytes[0] = 0x3bU;
        response.atr.value.bytes[1] = 0x00U;
        queue_response(w::kOpcodeCardStatus, request_id, response,
                       &w::encode_card_status_response);
    }

    void queue_card_session(std::uint16_t opcode, std::uint32_t request_id) noexcept
    {
        w::CardSessionResult response{};
        response.epoch[0] = kEpochByte;
        response.reader_id = kReaderId;
        response.card_handle = kHandle;
        response.bound_generation = kGeneration;
        response.current_generation = kGeneration;
        response.present = true;
        response.initialized = true;
        response.usable = true;
        response.protocol.present = true;
        response.protocol.value = w::CardProtocol::t1;
        response.atr.present = true;
        response.atr.value.length = 2U;
        response.atr.value.bytes[0] = 0x3bU;
        response.atr.value.bytes[1] = 0x00U;
        queue_response(opcode, request_id, response, &w::encode_card_session_result);
    }

    void queue_remote_error(std::uint16_t opcode, std::uint32_t request_id,
                            std::uint32_t code) noexcept
    {
        w::ErrorResponse error{};
        error.epoch[0] = kEpochByte;
        error.error.code = code;
        error.error.operation.length = 11U;
        std::memcpy(error.error.operation.bytes.data(), "card_status", 11U);
        std::array<std::uint8_t, 256U> payload{};
        const auto size = w::encode_error_response(
            error, MutableByteView{payload.data(), payload.size()});
        if (!size) return;
        w::Header header{};
        header.kind = w::Kind::error_response;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(size.value());
        const auto framed = w::encode_frame(
            header, ByteView{payload.data(), size.value()},
            MutableByteView{queued_.data(), queued_.size()});
        if (framed) {
            queued_size_ = framed.value();
            queued_offset_ = 0U;
        }
    }

    Mode mode_;
    std::vector<std::uint32_t> request_ids_;
    std::uint64_t hello_required_features_ = 0U;
    std::size_t close_count_ = 0U;
    std::array<std::uint8_t, w::kHeaderSize + w::kMaxControlPayload> queued_{};
    std::size_t queued_size_ = 0U;
    std::size_t queued_offset_ = 0U;
};

bool test_card_requires_service()
{
    FakeTransport transport(Mode::no_card_service);
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    const auto result = w::begin_card(transport, options, Timeout{100U});
    CHECK(!result && result.error == Error::UNSUPPORTED);
    // fetch_status closes on its own failure and begin_card closes again; the
    // transport must not be reused for a second request.
    CHECK(transport.request_count() == 1U && transport.close_count() >= 1U);
    return true;
}

bool test_card_missing_reader_is_rejected()
{
    FakeTransport transport(Mode::card_service);
    w::CardClientOptions options{};
    options.reader_id = 7U;
    const auto result = w::begin_card(transport, options, Timeout{100U});
    CHECK(!result && result.error == Error::NOT_FOUND);
    CHECK(transport.request_count() == 3U && transport.close_count() == 1U);
    return true;
}

bool test_card_operations_on_one_connection()
{
    FakeTransport transport(Mode::card_service);
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    const auto opened = w::begin_card(transport, options, Timeout{100U});
    if (!opened)
        std::fprintf(stderr, "begin_card failed error=%u requests=%zu\n",
                     static_cast<unsigned int>(opened.error), transport.request_count());
    CHECK(opened);
    CHECK(opened.session.reader_id == kReaderId &&
          opened.session.card_handle == kHandle &&
          opened.session.bound_generation == kGeneration);
    CHECK(opened.reader_status.present && opened.reader_status.initialized &&
          opened.connection.usable);
    CHECK(transport.hello_required_features() == w::kFeatureCardService);

    w::CardClientSession session = opened.session;
    const auto status = w::card_status(transport, session, Timeout{100U});
    CHECK(status && status.response.current_generation == kGeneration && status.response.present);

    const std::array<std::uint8_t, 4U> apdu{0x00U, 0xA4U, 0x04U, 0x00U};
    const auto transmitted = w::card_transmit(transport, session, w::CardProtocol::t1,
                                              ByteView{apdu.data(), apdu.size()},
                                              Timeout{100U});
    CHECK(transmitted);
    CHECK(transmitted.response.response.length == 2U &&
          transmitted.response.response.bytes[0] == 0x90U &&
          transmitted.response.response.bytes[1] == 0x00U);

    const auto reset = w::card_reset(transport, session, Timeout{100U});
    CHECK(reset && reset.response.usable && session.card_handle == kHandle);

    const auto disconnected = w::card_disconnect(transport, session, w::Disposition::leave,
                                                 Timeout{100U});
    CHECK(disconnected && disconnected.response.consumed);
    CHECK(session.card_handle == 0U && session.bound_generation == 0U);
    CHECK(transport.close_count() == 0U);
    const std::vector<std::uint32_t> expected{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    CHECK(transport.request_ids() == expected);
    return true;
}

bool test_card_epoch_mismatch_is_rejected()
{
    FakeTransport transport(Mode::card_service);
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    const auto opened = w::begin_card(transport, options, Timeout{100U});
    CHECK(opened);
    w::CardClientSession session = opened.session;
    session.epoch[0] ^= 0x01U;
    const std::size_t before = transport.request_count();
    const auto status = w::card_status(transport, session, Timeout{100U});
    CHECK(!status && status.error == Error::PROTOCOL_ERROR);
    CHECK(transport.request_count() == before + 1U && transport.close_count() == 1U);
    return true;
}

bool test_card_removal_is_not_retried()
{
    FakeTransport transport(Mode::transmit_card_removed);
    w::CardClientOptions options{};
    options.reader_id = kReaderId;
    const auto opened = w::begin_card(transport, options, Timeout{100U});
    CHECK(opened);
    w::CardClientSession session = opened.session;
    const std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
    const std::size_t before = transport.request_count();
    const auto transmitted = w::card_transmit(transport, session, w::CardProtocol::t1,
                                              ByteView{apdu.data(), apdu.size()},
                                              Timeout{100U});
    CHECK(!transmitted && transmitted.error == Error::CARD_REMOVED &&
          transmitted.remote_error_code == 13U);
    CHECK(transport.request_count() == before + 1U && transport.close_count() == 1U);
    return true;
}

}  // namespace

int main()
{
    return test_card_requires_service() && test_card_missing_reader_is_rejected() &&
                   test_card_operations_on_one_connection() &&
                   test_card_epoch_mismatch_is_rejected() &&
                   test_card_removal_is_not_retried()
               ? 0
               : 1;
}
