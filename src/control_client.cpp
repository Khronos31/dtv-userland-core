// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/control_connection.h>

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace dtv::userland::wire {
namespace {

struct RawReply final {
    Header header{};
    std::size_t length = 0U;
    bool seen = false;
};

constexpr std::size_t kRequestPayloadCapacity = 64U;

struct ExchangeWorkspace final {
    std::array<std::uint8_t, kHeaderSize + kMaxControlPayload> outgoing{};
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    std::array<std::uint8_t, 4096U> input{};
};

class ReplyHandler final : public FrameHandler {
public:
    ReplyHandler(std::uint16_t opcode, std::uint32_t request_id,
                 RawReply& reply, MutableByteView payload) noexcept
        : opcode_(opcode), request_id_(request_id), reply_(reply), payload_(payload) {}

    Result<void> on_frame(const Frame& frame) noexcept override
    {
        if (reply_.seen ||
            (frame.header.kind != Kind::response &&
             frame.header.kind != Kind::error_response) ||
            frame.header.opcode != opcode_ || frame.header.request_id != request_id_ ||
            frame.payload.size > payload_.size) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        reply_.header = frame.header;
        reply_.length = frame.payload.size;
        reply_.seen = true;
        return Result<void>::success();
    }

private:
    std::uint16_t opcode_;
    std::uint32_t request_id_;
    RawReply& reply_;
    MutableByteView payload_;
};

Result<void> exchange(ClientTransport& transport, std::uint16_t opcode,
                      std::uint32_t request_id, ByteView payload,
                      ExchangeWorkspace& workspace, RawReply& reply) noexcept
{
    Header header;
    header.kind = Kind::request;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.size);
    const auto encoded = encode_frame(
        header, payload, MutableByteView{workspace.outgoing.data(), workspace.outgoing.size()});
    if (!encoded) {
        return Result<void>::failure(encoded.error());
    }
    const auto sent = transport.write_all(ByteView{workspace.outgoing.data(), encoded.value()});
    if (!sent) {
        return Result<void>::failure(sent.error());
    }

    reply = RawReply{};
    ReplyHandler handler(opcode, request_id, reply,
                         MutableByteView{workspace.payload.data(), workspace.payload.size()});
    FrameReceiver receiver(MutableByteView{workspace.payload.data(), workspace.payload.size()});
    while (!reply.seen) {
        const auto read = transport.read_some(
            MutableByteView{workspace.input.data(), workspace.input.size()});
        if (!read) {
            if (read.error() == Error::DISCONNECTED) {
                const auto ended = receiver.finish();
                return Result<void>::failure(ended ? Error::DISCONNECTED : ended.error());
            }
            return Result<void>::failure(read.error());
        }
        if (read.value() == 0U) {
            const auto ended = receiver.finish();
            return Result<void>::failure(ended ? Error::DISCONNECTED : ended.error());
        }
        if (read.value() > workspace.input.size()) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const auto fed = receiver.feed(ByteView{workspace.input.data(), read.value()}, handler);
        if (!fed) {
            return Result<void>::failure(fed.error());
        }
    }
    const auto finished = receiver.finish();
    if (!finished) {
        return Result<void>::failure(finished.error());
    }
    return Result<void>::success();
}

bool status_identity_matches(const StatusSnapshot& snapshot, ByteView expected_product,
                             ByteView expected_instance) noexcept;

Error remote_error(std::uint32_t code) noexcept
{
    switch (code) {
    case 1U: return Error::INVALID_ARGUMENT;
    case 2U: return Error::VERSION_MISMATCH;
    case 3U: return Error::NOT_FOUND;
    case 4U: return Error::BUSY;
    case 5U: return Error::NOT_READY;
    case 6U: return Error::TIMEOUT;
    case 7U: return Error::USB_IO;
    case 8U: return Error::DISCONNECTED;
    case 9U: return Error::PROTOCOL_ERROR;
    case 10U: return Error::FIRMWARE_REJECTED;
    case 11U: return Error::UNSUPPORTED;
    case 12U: return Error::NO_CARD;
    case 13U: return Error::CARD_REMOVED;
    case 14U: return Error::BUFFER_TOO_SMALL;
    case 15U: return Error::SLOW_CONSUMER;
    case 17U: return Error::RESOURCE_LIMIT;
    case 255U: return Error::INTERNAL;
    default: return Error::PROTOCOL_ERROR;
    }
}

bool status_identity_matches(const StatusSnapshot& snapshot,
                             const StatusAssertion& assertion) noexcept
{
    if (!status_identity_matches(snapshot, assertion.expected_product,
                                 assertion.expected_instance)) return false;
    if (assertion.expected_device == nullptr) return true;
    const auto& expected = *assertion.expected_device;
    if (expected.kind == identity::IdentityKind::serial) {
        if (expected.serial.length == 0U ||
            expected.serial.length > expected.serial.bytes.size()) return false;
        if (!snapshot.list.enclosure.present) return false;
        const auto& actual = snapshot.list.enclosure.value.identity;
        return actual.serial_state == IdentitySerialState::present &&
               actual.serial.present &&
               actual.serial.value.length == expected.serial.length &&
               std::memcmp(actual.serial.value.bytes.data(), expected.serial.bytes.data(),
                           expected.serial.length) == 0;
    }
    if (!snapshot.list.enclosure.present ||
        expected.kind != identity::IdentityKind::usb_topology ||
        expected.topology.count == 0U ||
        expected.topology.count > expected.topology.paths.size() ||
        snapshot.list.functions.count == 0U ||
        snapshot.list.topology.state != TopologyState::complete ||
        !snapshot.list.topology.expected_function_count.present ||
        snapshot.list.topology.expected_function_count.value !=
            snapshot.list.functions.count) return false;
    std::size_t matched = 0U;
    for (std::size_t candidate = 0U; candidate < snapshot.list.functions.count;
         ++candidate) {
        const auto& actual = snapshot.list.functions.items[candidate].location.usb_path;
        if (!actual.present || actual.value.length == 0U ||
            actual.value.length > actual.value.bytes.size()) return false;
        for (std::size_t prior = 0U; prior < candidate; ++prior) {
            const auto& prior_path =
                snapshot.list.functions.items[prior].location.usb_path;
            if (prior_path.present && prior_path.value.length == actual.value.length &&
                std::memcmp(prior_path.value.bytes.data(), actual.value.bytes.data(),
                            actual.value.length) == 0)
                return false;
        }
        for (std::size_t index = 0U; index < expected.topology.count; ++index) {
            const auto& path = expected.topology.paths[index];
            if (path.length == 0U || path.length > path.bytes.size()) return false;
            if (actual.value.length == path.length &&
                std::memcmp(actual.value.bytes.data(), path.bytes.data(), path.length) == 0)
                ++matched;
        }
    }
    if (expected.topology.count == 1U) return matched == 1U;
    return expected.topology.count == snapshot.list.functions.count &&
           matched == snapshot.list.functions.count;
}

Result<void> roundtrip(ClientTransport& transport, std::uint16_t opcode,
                       std::uint32_t request_id, ByteView payload,
                       std::uint32_t& remote_code, ExchangeWorkspace& workspace,
                       RawReply& reply, const Uuid* expected_epoch = nullptr) noexcept
{
    const auto exchanged = exchange(transport, opcode, request_id, payload, workspace, reply);
    if (!exchanged) {
        return exchanged;
    }
    if (reply.header.kind == Kind::error_response) {
        const auto error = decode_error_response(
            ByteView{workspace.payload.data(), reply.length});
        if (!error) {
            return Result<void>::failure(error.error());
        }
        if (expected_epoch != nullptr && error.value().epoch != *expected_epoch) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        remote_code = error.value().error.code;
        return Result<void>::failure(remote_error(remote_code));
    }
    return Result<void>::success();
}

template <std::size_t N>
bool same_text(const WString<N>& left, const WString<N>& right) noexcept
{
    return left.length == right.length &&
           std::memcmp(left.bytes.data(), right.bytes.data(), left.length) == 0;
}

template <std::size_t N>
bool same_optional_text(const Optional<WString<N>>& left,
                        const Optional<WString<N>>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || same_text(left.value, right.value));
}

bool same_model(const Optional<Model>& left, const Optional<Model>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || same_text(left.value.name, right.value.name));
}

bool same_identity(const Identity& left, const Identity& right) noexcept
{
    return left.serial_state == right.serial_state &&
           same_optional_text(left.serial, right.serial);
}

bool same_location(const Location& left, const Location& right) noexcept
{
    return same_optional_text(left.usb_path, right.usb_path) &&
           left.bus.present == right.bus.present &&
           (!left.bus.present || left.bus.value == right.bus.value) &&
           left.address.present == right.address.present &&
           (!left.address.present || left.address.value == right.address.value);
}

bool same_function_identity(const UsbFunction& left, const UsbFunction& right) noexcept
{
    return left.id == right.id && left.vid == right.vid && left.pid == right.pid &&
           same_location(left.location, right.location) &&
           left.serial_state == right.serial_state &&
           same_optional_text(left.serial, right.serial) && same_model(left.model, right.model);
}

bool same_snapshot_identity(const ListResponse& list,
                            const StatusResponse& status) noexcept
{
    if (list.enclosure.present != status.enclosure.present) {
        return false;
    }
    if (list.enclosure.present &&
        (list.enclosure.value.id != status.enclosure.value.id ||
         !same_model(list.enclosure.value.model, status.enclosure.value.model) ||
         !same_identity(list.enclosure.value.identity, status.enclosure.value.identity))) {
        return false;
    }
    if (!status.functions.present || list.functions.count != status.functions.value.count) {
        return false;
    }
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        const UsbFunction& listed = list.functions.items[index];
        std::size_t observed_index = status.functions.value.count;
        for (std::size_t candidate = 0U; candidate < status.functions.value.count; ++candidate) {
            if (status.functions.value.items[candidate].id == listed.id) {
                if (observed_index != status.functions.value.count) {
                    return false;
                }
                observed_index = candidate;
            }
        }
        if (observed_index == status.functions.value.count ||
            !same_function_identity(listed, status.functions.value.items[observed_index])) {
            return false;
        }
    }
    return true;
}

bool same_producer(const Producer& left, const Producer& right) noexcept
{
    return same_text(left.product, right.product) && same_text(left.version, right.version);
}

template <typename Request>
Result<std::size_t> encode_request(const Request& request, MutableByteView output,
                                   Result<std::size_t> (*encode)(const Request&,
                                                                  MutableByteView) noexcept) noexcept
{
    return encode(request, output);
}

}  // namespace

namespace {

StatusFetchResult fetch_status_with_required_features(ClientTransport& transport,
                                                     std::uint64_t required_features) noexcept
{
    StatusFetchResult result{};
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
    };
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        fail(Error::RESOURCE_LIMIT);
        return result;
    }
    RawReply reply{};

    HelloRequest hello_request{};
    hello_request.min = Version{kVersionMajor, kVersionMinor};
    hello_request.max = Version{kVersionMajor, kVersionMinor};
    hello_request.required_features = required_features;
    std::array<std::uint8_t, kHelloRequestSize> hello_payload{};
    const auto hello_size = encode_hello_request(
        hello_request, MutableByteView{hello_payload.data(), hello_payload.size()});
    if (!hello_size) {
        fail(hello_size.error());
        return result;
    }
    std::uint32_t remote_code = 0U;
    const auto hello_reply = roundtrip(
        transport, kOpcodeHello, 1U, ByteView{hello_payload.data(), hello_size.value()},
        remote_code, *workspace, reply);
    if (!hello_reply) {
        result.remote_error_code = remote_code;
        fail(hello_reply.error());
        return result;
    }
    const auto hello = decode_hello_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!hello) {
        fail(hello.error());
        return result;
    }
    if (hello.value().major != kVersionMajor || hello.value().minor != kVersionMinor) {
        fail(Error::VERSION_MISMATCH);
        return result;
    }
    if (hello.value().features != required_features) {
        const Error error =
            (hello.value().features & required_features) == required_features
                ? Error::PROTOCOL_ERROR : Error::UNSUPPORTED;
        fail(error);
        return result;
    }

    std::array<std::uint8_t, kRequestPayloadCapacity> request_payload{};
    ListRequest list_request{};
    list_request.epoch = hello.value().epoch;
    const auto list_size = encode_request(
        list_request, MutableByteView{request_payload.data(), request_payload.size()},
        encode_list_request);
    if (!list_size) {
        fail(list_size.error());
        return result;
    }
    remote_code = 0U;
    const auto list_reply = roundtrip(
        transport, kOpcodeList, 2U,
        ByteView{request_payload.data(), list_size.value()}, remote_code, *workspace, reply,
        &hello.value().epoch);
    if (!list_reply) {
        result.remote_error_code = remote_code;
        fail(list_reply.error());
        return result;
    }
    const auto list = decode_list_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!list) {
        fail(list.error());
        return result;
    }
    if (list.value().epoch != hello.value().epoch ||
        !same_producer(list.value().producer, hello.value().producer)) {
        fail(Error::PROTOCOL_ERROR);
        return result;
    }
    const Error list_valid = validate_list_response(list.value());
    if (list_valid != Error::OK) {
        fail(list_valid);
        return result;
    }

    StatusRequest status_request{};
    status_request.epoch = hello.value().epoch;
    status_request.scope = Scope{ScopeKind::enclosure, 0U};
    const auto status_size = encode_request(
        status_request, MutableByteView{request_payload.data(), request_payload.size()},
        encode_status_request);
    if (!status_size) {
        fail(status_size.error());
        return result;
    }
    remote_code = 0U;
    const auto status_reply = roundtrip(
        transport, kOpcodeStatus, 3U,
        ByteView{request_payload.data(), status_size.value()}, remote_code, *workspace, reply,
        &hello.value().epoch);
    if (!status_reply) {
        result.remote_error_code = remote_code;
        fail(status_reply.error());
        return result;
    }
    const auto status = decode_status_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!status) {
        fail(status.error());
        return result;
    }
    if (status.value().scope.kind != status_request.scope.kind ||
        status.value().scope.id != status_request.scope.id ||
        status.value().context.epoch != hello.value().epoch ||
        !same_producer(status.value().producer, hello.value().producer)) {
        fail(Error::PROTOCOL_ERROR);
        return result;
    }
    const StatusContext context{&list.value()};
    const Error status_valid = validate_status_response(status.value(), context);
    if (status_valid != Error::OK) {
        fail(status_valid);
        return result;
    }
    if (!same_snapshot_identity(list.value(), status.value())) {
        fail(Error::PROTOCOL_ERROR);
        return result;
    }

    result.snapshot.list = list.value();
    result.snapshot.status = status.value();
    result.error = Error::OK;
    return result;
}

}  // namespace

StatusFetchResult fetch_status(ClientTransport& transport) noexcept
{
    return fetch_status_with_required_features(transport, 0U);
}

namespace {

class TimedTransportAdapter final : public ClientTransport {
public:
    explicit TimedTransportAdapter(TimedClientTransport& transport) noexcept
        : transport_(transport) {}

    bool set_deadline(Timeout timeout) noexcept
    {
        if (timeout.milliseconds == 0U) return false;
        const std::uint64_t now = transport_.monotonic_ms();
        if (now > std::numeric_limits<std::uint64_t>::max() - timeout.milliseconds)
            return false;
        deadline_ = now + timeout.milliseconds;
        valid_ = true;
        return true;
    }

    Result<void> write_all(ByteView bytes) noexcept override
    {
        const std::uint32_t left = remaining_ms();
        if (left == 0U) return Result<void>::failure(Error::TIMEOUT);
        const auto sent = transport_.write_all(bytes, Timeout{left});
        if (sent && remaining_ms() == 0U) return Result<void>::failure(Error::TIMEOUT);
        return sent;
    }

    Result<std::size_t> read_some(MutableByteView bytes) noexcept override
    {
        const std::uint32_t left = remaining_ms();
        if (left == 0U) return Result<std::size_t>::failure(Error::TIMEOUT);
        const auto read = transport_.read_some(bytes, Timeout{left});
        if (read && remaining_ms() == 0U) return Result<std::size_t>::failure(Error::TIMEOUT);
        return read;
    }

    void close() noexcept override { transport_.close(); }

private:
    std::uint32_t remaining_ms() noexcept
    {
        if (!valid_) return 0U;
        const std::uint64_t now = transport_.monotonic_ms();
        if (now >= deadline_) return 0U;
        const std::uint64_t remaining = deadline_ - now;
        return remaining > std::numeric_limits<std::uint32_t>::max()
                   ? std::numeric_limits<std::uint32_t>::max()
                   : static_cast<std::uint32_t>(remaining);
    }

    TimedClientTransport& transport_;
    std::uint64_t deadline_ = 0U;
    bool valid_ = false;
};

bool nonzero_uuid(const Uuid& value) noexcept
{
    for (const std::uint8_t byte : value) {
        if (byte != 0U) return true;
    }
    return false;
}

bool status_identity_matches(const StatusSnapshot& snapshot,
                             ByteView expected_product,
                             ByteView expected_instance) noexcept
{
    if (expected_product.size != 0U) {
        if (expected_product.data == nullptr) return false;
        const auto& actual = snapshot.list.producer.product;
        if (expected_product.size != actual.length ||
            std::memcmp(expected_product.data, actual.bytes.data(), expected_product.size) != 0)
            return false;
    }
    if (expected_instance.size != 0U) {
        if (expected_instance.data == nullptr || !snapshot.status.context.instance.present)
            return false;
        const auto& actual = snapshot.status.context.instance.value;
        if (expected_instance.size != actual.length ||
            std::memcmp(expected_instance.data, actual.bytes.data(), expected_instance.size) != 0)
            return false;
    }
    return true;
}

bool lists_reader(const ListResponse& list, std::uint16_t reader_id) noexcept
{
    if (!list.readers.present) return false;
    for (std::size_t index = 0U; index < list.readers.value.count; ++index) {
        if (list.readers.value.items[index].id == reader_id) return true;
    }
    return false;
}

template <typename Request>
Result<std::size_t> encode_body(const Request& value, MutableByteView output,
                                Result<std::size_t> (*encode)(const Request&,
                                                               MutableByteView) noexcept) noexcept
{
    return encode(value, output);
}

}  // namespace

StatusFetchResult fetch_status(TimedClientTransport& transport, Timeout timeout) noexcept
{
    return fetch_status(transport, timeout, 0U);
}

StatusFetchResult fetch_status(TimedClientTransport& transport, Timeout timeout,
                               std::uint64_t required_features) noexcept
{
    TimedTransportAdapter adapter(transport);
    if (timeout.milliseconds == 0U) {
        transport.close();
        return StatusFetchResult{StatusSnapshot{}, Error::INVALID_ARGUMENT, 0U};
    }
    if (!adapter.set_deadline(timeout)) {
        transport.close();
        return StatusFetchResult{StatusSnapshot{}, Error::TIMEOUT, 0U};
    }
    return fetch_status_with_required_features(adapter, required_features);
}

LnbPowerResult set_lnb_power(TimedClientTransport& transport, bool enabled,
                             Timeout timeout) noexcept
{
    return set_lnb_power(transport, enabled, timeout, StatusAssertion{});
}

LnbPowerResult set_lnb_power(TimedClientTransport& transport, bool enabled,
                             Timeout timeout, const StatusAssertion& assertion) noexcept
{
    LnbPowerResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }

    const StatusFetchResult status = fetch_status_with_required_features(adapter, 0U);
    if (!status) {
        result.error = status.error;
        result.remote_error_code = status.remote_error_code;
        return result;
    }
    if (!status_identity_matches(status.snapshot, assertion)) {
        result.error = Error::NOT_FOUND;
        transport.close();
        return result;
    }

    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    LnbPowerRequest request{};
    request.epoch = status.snapshot.status.context.epoch;
    request.enabled = enabled;
    const auto size = encode_body(request,
                                 MutableByteView{workspace->payload.data(),
                                                 workspace->payload.size()},
                                 encode_lnb_power_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }

    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeLnbPower, 4U,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &request.epoch);
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto response = decode_lnb_power_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!response) {
        result.error = response.error();
        transport.close();
        return result;
    }
    if (response.value().epoch != request.epoch || response.value().enabled != enabled) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.response = response.value();
    result.error = Error::OK;
    return result;
}

CardOpenResult begin_card(TimedClientTransport& transport,
                          const CardClientOptions& options,
                          Timeout timeout) noexcept
{
    CardOpenResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    const StatusFetchResult status =
        fetch_status_with_required_features(adapter, kFeatureCardService);
    if (!status) {
        result.error = status.error;
        result.remote_error_code = status.remote_error_code;
        return result;
    }
    const StatusAssertion assertion{options.expected_product, options.expected_instance,
                                    options.expected_device};
    if (!status_identity_matches(status.snapshot, assertion) ||
        !lists_reader(status.snapshot.list, options.reader_id)) {
        result.error = Error::NOT_FOUND;
        transport.close();
        return result;
    }

    CardClientSession session{};
    session.epoch = status.snapshot.status.context.epoch;
    session.reader_id = options.reader_id;
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    CardStatusRequest status_request{};
    status_request.key.epoch = session.epoch;
    status_request.key.reader_id = session.reader_id;
    auto size = encode_body(status_request,
                            MutableByteView{workspace->payload.data(), workspace->payload.size()},
                            encode_card_status_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    auto exchanged = roundtrip(adapter, kOpcodeCardStatus, 4U,
                               ByteView{workspace->payload.data(), size.value()}, remote_code,
                               *workspace, reply, &session.epoch);
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto reader_status = decode_card_status_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!reader_status) {
        result.error = reader_status.error();
        transport.close();
        return result;
    }
    if (reader_status.value().key.epoch != session.epoch ||
        reader_status.value().key.reader_id != session.reader_id ||
        validate_card_status_response(reader_status.value()) != Error::OK) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }

    CardConnectRequest connect{};
    connect.key.epoch = session.epoch;
    connect.key.reader_id = session.reader_id;
    connect.share = options.share;
    connect.preferred_protocols = options.preferred_protocols;
    size = encode_body(connect,
                       MutableByteView{workspace->payload.data(), workspace->payload.size()},
                       encode_card_connect_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    remote_code = 0U;
    exchanged = roundtrip(adapter, kOpcodeCardConnect, 5U,
                          ByteView{workspace->payload.data(), size.value()}, remote_code,
                          *workspace, reply, &session.epoch);
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto connection = decode_card_session_result(
        ByteView{workspace->payload.data(), reply.length});
    if (!connection) {
        result.error = connection.error();
        transport.close();
        return result;
    }
    if (connection.value().epoch != session.epoch ||
        connection.value().reader_id != session.reader_id ||
        validate_card_session_result(connection.value()) != Error::OK) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    session.card_handle = connection.value().card_handle;
    session.bound_generation = connection.value().bound_generation;
    session.protocol = connection.value().protocol;
    session.next_request_id = 6U;
    result.session = session;
    result.reader_status = reader_status.value();
    result.connection = connection.value();
    result.error = Error::OK;
    return result;
}

CardStatusResult card_status(TimedClientTransport& transport,
                             CardClientSession& session,
                             Timeout timeout) noexcept
{
    CardStatusResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    CardStatusRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    const auto size = encode_body(request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_card_status_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeCardStatus, session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto decoded = decode_card_status_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!decoded) {
        result.error = decoded.error();
        transport.close();
        return result;
    }
    if (decoded.value().key.epoch != session.epoch ||
        decoded.value().key.reader_id != session.reader_id ||
        validate_card_status_response(decoded.value()) != Error::OK) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.response = decoded.value();
    result.error = Error::OK;
    return result;
}

CardResetResult card_reset(TimedClientTransport& transport,
                           CardClientSession& session,
                           Timeout timeout) noexcept
{
    CardResetResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    CardResetRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    request.key.bound_generation = session.bound_generation;
    request.key.card_handle = session.card_handle;
    const auto size = encode_body(request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_card_reset_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeCardReset, session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto decoded = decode_card_session_result(
        ByteView{workspace->payload.data(), reply.length});
    if (!decoded) {
        result.error = decoded.error();
        transport.close();
        return result;
    }
    if (decoded.value().epoch != session.epoch ||
        decoded.value().reader_id != session.reader_id ||
        decoded.value().card_handle != session.card_handle ||
        validate_card_session_result(decoded.value()) != Error::OK) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    session.bound_generation = decoded.value().bound_generation;
    session.protocol = decoded.value().protocol;
    result.response = decoded.value();
    result.error = Error::OK;
    return result;
}

CardTransmitResult card_transmit(TimedClientTransport& transport,
                                 CardClientSession& session,
                                 CardProtocol protocol, ByteView command,
                                 Timeout timeout) noexcept
{
    CardTransmitResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    if (!session.protocol.present) {
        result.error = Error::NOT_READY;
        transport.close();
        return result;
    }
    if (protocol != CardProtocol::t1 || protocol != session.protocol.value ||
        command.data == nullptr || command.size == 0U || command.size > kMaxApduBytes) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    std::unique_ptr<CardTransmitRequest> request(new (std::nothrow) CardTransmitRequest{});
    if (!request) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    request->key.epoch = session.epoch;
    request->key.reader_id = session.reader_id;
    request->key.bound_generation = session.bound_generation;
    request->key.card_handle = session.card_handle;
    request->protocol = protocol;
    request->command.length = static_cast<std::uint32_t>(command.size);
    std::memcpy(request->command.bytes.data(), command.data, command.size);
    const auto size = encode_body(*request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_card_transmit_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeCardTransmit, session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto decoded = decode_card_transmit_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!decoded) {
        result.error = decoded.error();
        transport.close();
        return result;
    }
    if (decoded.value().key.epoch != session.epoch ||
        decoded.value().key.reader_id != session.reader_id ||
        decoded.value().key.card_handle != session.card_handle ||
        decoded.value().key.bound_generation != session.bound_generation) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.response = decoded.value();
    result.error = Error::OK;
    return result;
}

CardDisconnectResult card_disconnect(TimedClientTransport& transport,
                                     CardClientSession& session,
                                     Disposition disposition,
                                     Timeout timeout) noexcept
{
    CardDisconnectResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    CardDisconnectRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    request.key.bound_generation = session.bound_generation;
    request.key.card_handle = session.card_handle;
    request.disposition = disposition;
    const auto size = encode_body(request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_card_disconnect_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeCardDisconnect,
                                     session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.error = exchanged.error();
        result.remote_error_code = remote_code;
        transport.close();
        return result;
    }
    const auto decoded = decode_card_disconnect_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!decoded) {
        result.error = decoded.error();
        transport.close();
        return result;
    }
    if (decoded.value().key.epoch != session.epoch ||
        decoded.value().key.reader_id != session.reader_id ||
        decoded.value().card_handle != session.card_handle) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    if (!decoded.value().consumed) {
        if (decoded.value().error.present) {
            result.remote_error_code = decoded.value().error.value.code;
            result.error = remote_error(result.remote_error_code);
        } else {
            result.error = Error::PROTOCOL_ERROR;
        }
        transport.close();
        return result;
    }
    result.response = decoded.value();
    session.card_handle = 0U;
    session.bound_generation = 0U;
    session.protocol = Optional<CardProtocol>{};
    if (decoded.value().error.present) {
        result.remote_error_code = decoded.value().error.value.code;
        result.error = remote_error(result.remote_error_code);
        transport.close();
        return result;
    }
    if (!decoded.value().quiesced) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.error = Error::OK;
    return result;
}

RecordingControlResult begin_recording(TimedClientTransport& transport,
                                       const RecordingOptions& options,
                                       Timeout timeout,
                                       RecordingStatusValidator validator,
                                       void* validator_context) noexcept
{
    RecordingControlResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    const StatusFetchResult status = fetch_status(adapter);
    if (!status) {
        result.error = status.error;
        result.remote_error_code = status.remote_error_code;
        return result;
    }
    if (validator != nullptr) {
        const Error validation = validator(status.snapshot, validator_context);
        if (validation != Error::OK) {
            result.error = validation;
            transport.close();
            return result;
        }
    }
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }

    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    RecordingSession session{};
    session.epoch = status.snapshot.status.context.epoch;
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
        return result;
    };

    AcquireRequest acquire{};
    acquire.epoch = session.epoch;
    acquire.receiver_id = options.receiver_id;
    auto size = encode_body(acquire,
                            MutableByteView{workspace->payload.data(),
                                            workspace->payload.size()},
                            encode_acquire_request);
    if (!size) return fail(size.error());
    auto exchanged = roundtrip(adapter, kOpcodeAcquire, 4U,
                               ByteView{workspace->payload.data(), size.value()},
                               remote_code, *workspace, reply, &session.epoch);
    if (!exchanged) {
        result.remote_error_code = remote_code;
        return fail(exchanged.error());
    }
    const auto acquired = decode_acquire_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!acquired) return fail(acquired.error());
    if (acquired.value().epoch != session.epoch || acquired.value().lease_id == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.lease_id = acquired.value().lease_id;

    TuneRequest tune = options.tune;
    tune.epoch = session.epoch;
    tune.lease_id = session.lease_id;
    size = encode_body(tune,
                       MutableByteView{workspace->payload.data(), workspace->payload.size()},
                       encode_tune_request);
    if (!size) return fail(size.error());
    remote_code = 0U;
    exchanged = roundtrip(adapter, kOpcodeTune, 5U,
                          ByteView{workspace->payload.data(), size.value()},
                          remote_code, *workspace, reply, &session.epoch);
    if (!exchanged) {
        result.remote_error_code = remote_code;
        return fail(exchanged.error());
    }
    const auto tuned = decode_tune_result(
        ByteView{workspace->payload.data(), reply.length});
    if (!tuned) return fail(tuned.error());
    if (tuned.value().epoch != session.epoch || tuned.value().tune_revision == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.tune_result = tuned.value();
    session.tune_revision = tuned.value().tune_revision;

    StartRequest start{};
    start.epoch = session.epoch;
    start.lease_id = session.lease_id;
    start.tune_revision = session.tune_revision;
    size = encode_body(start,
                       MutableByteView{workspace->payload.data(), workspace->payload.size()},
                       encode_start_request);
    if (!size) return fail(size.error());
    remote_code = 0U;
    exchanged = roundtrip(adapter, kOpcodeStartStream, 6U,
                          ByteView{workspace->payload.data(), size.value()},
                          remote_code, *workspace, reply, &session.epoch);
    if (!exchanged) {
        result.remote_error_code = remote_code;
        return fail(exchanged.error());
    }
    const auto started = decode_start_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!started) return fail(started.error());
    if (started.value().epoch != session.epoch || started.value().stream_id == 0U ||
        !nonzero_uuid(started.value().attach_token) || started.value().expires_after_ms == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.stream_id = started.value().stream_id;
    session.attach_token = started.value().attach_token;
    session.expires_after_ms = started.value().expires_after_ms;
    result.session = session;
    result.error = Error::OK;
    return result;
}

StopControlResult stop_recording(TimedClientTransport& transport,
                                 RecordingSession& session,
                                 Timeout timeout) noexcept
{
    StopControlResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    StopRequest request{};
    request.epoch = session.epoch;
    request.lease_id = session.lease_id;
    request.stream_id = session.stream_id;
    const auto size = encode_body(request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_stop_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeStopStream, session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.remote_error_code = remote_code;
        result.error = exchanged.error();
        transport.close();
        return result;
    }
    const auto stopped = decode_stop_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!stopped) {
        result.error = stopped.error();
        transport.close();
        return result;
    }
    if (stopped.value().epoch != session.epoch) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.response = stopped.value();
    result.error = Error::OK;
    return result;
}

ReleaseControlResult release_recording(TimedClientTransport& transport,
                                       RecordingSession& session,
                                       Timeout timeout) noexcept
{
    ReleaseControlResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    TimedTransportAdapter adapter(transport);
    if (!adapter.set_deadline(timeout)) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    std::unique_ptr<ExchangeWorkspace> workspace(new (std::nothrow) ExchangeWorkspace{});
    if (!workspace) {
        result.error = Error::RESOURCE_LIMIT;
        transport.close();
        return result;
    }
    ReleaseRequest request{};
    request.epoch = session.epoch;
    request.lease_id = session.lease_id;
    const auto size = encode_body(request,
                                  MutableByteView{workspace->payload.data(),
                                                  workspace->payload.size()},
                                  encode_release_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    RawReply reply{};
    std::uint32_t remote_code = 0U;
    const auto exchanged = roundtrip(adapter, kOpcodeRelease, session.next_request_id,
                                     ByteView{workspace->payload.data(), size.value()},
                                     remote_code, *workspace, reply, &session.epoch);
    ++session.next_request_id;
    if (!exchanged) {
        result.remote_error_code = remote_code;
        result.error = exchanged.error();
        transport.close();
        return result;
    }
    const auto released = decode_release_response(
        ByteView{workspace->payload.data(), reply.length});
    if (!released) {
        result.error = released.error();
        transport.close();
        return result;
    }
    result.response = released.value();
    if (released.value().epoch != session.epoch) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    if (!released.value().released) {
        if (!released.value().error.present) {
            result.error = Error::PROTOCOL_ERROR;
        } else {
            result.remote_error_code = released.value().error.value.code;
            result.error = remote_error(result.remote_error_code);
        }
        transport.close();
        return result;
    }
    result.error = Error::OK;
    return result;
}

}  // namespace dtv::userland::wire
