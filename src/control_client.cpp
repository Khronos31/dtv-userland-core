// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/control_connection.h>
#include <dtv/userland/wire.h>

#include <array>
#include <cstring>
#include <limits>

namespace dtv::userland::wire {
namespace {

struct RawReply final {
    Header header{};
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    std::size_t length = 0U;
    bool seen = false;
};

class ReplyHandler final : public FrameHandler {
public:
    ReplyHandler(std::uint16_t opcode, std::uint32_t request_id,
                 RawReply& reply) noexcept
        : opcode_(opcode), request_id_(request_id), reply_(reply) {}

    Result<void> on_frame(const Frame& frame) noexcept override
    {
        if (reply_.seen || frame.header.kind == Kind::event ||
            frame.header.kind == Kind::request || frame.header.opcode != opcode_ ||
            frame.header.request_id != request_id_ ||
            frame.payload.size > reply_.payload.size())
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        reply_.header = frame.header;
        reply_.length = frame.payload.size;
        if (frame.payload.size != 0U)
            std::memcpy(reply_.payload.data(), frame.payload.data, frame.payload.size);
        reply_.seen = true;
        return Result<void>::success();
    }

private:
    std::uint16_t opcode_;
    std::uint32_t request_id_;
    RawReply& reply_;
};

std::uint32_t remaining_ms(ClientTransport& transport, std::uint64_t deadline) noexcept
{
    const std::uint64_t now = transport.monotonic_ms();
    if (now >= deadline) return 0U;
    const std::uint64_t remaining = deadline - now;
    return remaining > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(remaining);
}

Result<RawReply> exchange(ClientTransport& transport, std::uint16_t opcode,
                          std::uint32_t request_id, ByteView payload,
                          std::uint64_t deadline) noexcept
{
    std::array<std::uint8_t, kHeaderSize + kMaxControlPayload> outgoing{};
    Header header;
    header.kind = Kind::request;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.size);
    const auto encoded = encode_frame(header, payload,
                                      MutableByteView{outgoing.data(), outgoing.size()});
    if (!encoded) return Result<RawReply>::failure(encoded.error());
    std::uint32_t left = remaining_ms(transport, deadline);
    if (left == 0U) return Result<RawReply>::failure(Error::TIMEOUT);
    const auto sent = transport.write_all(ByteView{outgoing.data(), encoded.value()}, Timeout{left});
    if (!sent) return Result<RawReply>::failure(sent.error());

    RawReply reply{};
    ReplyHandler handler(opcode, request_id, reply);
    std::array<std::uint8_t, kMaxControlPayload> scratch{};
    FrameReceiver receiver(MutableByteView{scratch.data(), scratch.size()});
    std::array<std::uint8_t, 8192U> input{};
    while (!reply.seen) {
        left = remaining_ms(transport, deadline);
        if (left == 0U) return Result<RawReply>::failure(Error::TIMEOUT);
        const auto read = transport.read_some(MutableByteView{input.data(), input.size()},
                                             Timeout{left});
        if (!read) {
            if (read.error() == Error::DISCONNECTED) {
                const auto ended = receiver.finish();
                return Result<RawReply>::failure(ended ? Error::DISCONNECTED : ended.error());
            }
            return Result<RawReply>::failure(read.error());
        }
        const auto fed = receiver.feed(ByteView{input.data(), read.value()}, handler);
        if (!fed) return Result<RawReply>::failure(fed.error());
        if (reply.seen) {
            const auto finished = receiver.finish();
            if (!finished) return Result<RawReply>::failure(finished.error());
        }
    }
    return Result<RawReply>::success(std::move(reply));
}

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
           (!left.present || (same_text(left.value.id, right.value.id) &&
                              same_text(left.value.name, right.value.name)));
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
           same_optional_text(left.serial, right.serial) &&
           same_model(left.model, right.model);
}

bool same_snapshot_identity(const ListResponse& list,
                            const StatusResponse& status) noexcept
{
    if (list.enclosure.present != status.enclosure.present) return false;
    if (list.enclosure.present &&
        (list.enclosure.value.id != status.enclosure.value.id ||
         !same_model(list.enclosure.value.model, status.enclosure.value.model) ||
         !same_identity(list.enclosure.value.identity, status.enclosure.value.identity)))
        return false;
    if (list.functions.count != status.functions.value.count) return false;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        const UsbFunction& listed = list.functions.items[index];
        std::size_t observed_index = status.functions.value.count;
        for (std::size_t candidate = 0U; candidate < status.functions.value.count;
             ++candidate) {
            if (status.functions.value.items[candidate].id == listed.id) {
                if (observed_index != status.functions.value.count) return false;
                observed_index = candidate;
            }
        }
        if (observed_index == status.functions.value.count ||
            !same_function_identity(listed,
                                    status.functions.value.items[observed_index]))
            return false;
    }
    return true;
}

template <typename Record>
Result<std::size_t> request_payload(const Record& value, MutableByteView output,
                                    Result<std::size_t> (*encode)(const Record&,
                                                                   MutableByteView) noexcept) noexcept
{
    return encode(value, output);
}

Result<RawReply> roundtrip(ClientTransport& transport, std::uint16_t opcode,
                           std::uint32_t request_id, ByteView payload,
                           std::uint64_t deadline, std::uint32_t& remote_code,
                           const Uuid* expected_epoch = nullptr) noexcept
{
    auto reply = exchange(transport, opcode, request_id, payload, deadline);
    if (!reply) return reply;
    if (reply.value().header.kind == Kind::error_response) {
        const auto error = decode_error_response(
            ByteView{reply.value().payload.data(), reply.value().length});
        if (!error) return Result<RawReply>::failure(error.error());
        if (expected_epoch != nullptr && error.value().epoch != *expected_epoch)
            return Result<RawReply>::failure(Error::PROTOCOL_ERROR);
        remote_code = error.value().error.code;
        return Result<RawReply>::failure(remote_error(error.value().error.code));
    }
    if (reply.value().header.kind != Kind::response)
        return Result<RawReply>::failure(Error::PROTOCOL_ERROR);
    return reply;
}

}  // namespace

StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout) noexcept
{
    return fetch_status(transport, timeout, 0U);
}

StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout,
                               std::uint64_t required_features) noexcept
{
    StatusFetchResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t started = transport.monotonic_ms();
    if (started > std::numeric_limits<std::uint64_t>::max() - timeout.milliseconds) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = started + timeout.milliseconds;
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
        return result;
    };

    HelloRequest hello_request{};
    hello_request.min = Version{kVersionMajor, kVersionMinor};
    hello_request.max = Version{kVersionMajor, kVersionMinor};
    hello_request.required_features = required_features;
    std::array<std::uint8_t, kHelloRequestSize> hello_bytes{};
    const auto hello_size = encode_hello_request(
        hello_request, MutableByteView{hello_bytes.data(), hello_bytes.size()});
    if (!hello_size) return fail(hello_size.error());
    std::uint32_t remote_code = 0U;
    auto hello_reply = roundtrip(transport, kOpcodeHello, 1U,
                                 ByteView{hello_bytes.data(), hello_size.value()}, deadline,
                                 remote_code);
    if (!hello_reply) {
        result.remote_error_code = remote_code;
        return fail(hello_reply.error());
    }
    const auto hello = decode_hello_response(
        ByteView{hello_reply.value().payload.data(), hello_reply.value().length});
    if (!hello) return fail(hello.error());
    if (hello.value().major != kVersionMajor || hello.value().minor > kVersionMinor)
        return fail(Error::VERSION_MISMATCH);
    if ((hello.value().features & required_features) != required_features)
        return fail(Error::UNSUPPORTED);

    ListRequest list_request{};
    list_request.epoch = hello.value().epoch;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto list_size = request_payload(
        list_request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_list_request);
    if (!list_size) return fail(list_size.error());
    remote_code = 0U;
    auto list_reply = roundtrip(transport, kOpcodeList, 2U,
                                ByteView{request_bytes.data(), list_size.value()}, deadline,
                                remote_code, &hello.value().epoch);
    if (!list_reply) {
        result.remote_error_code = remote_code;
        return fail(list_reply.error());
    }
    const auto list = decode_list_response(
        ByteView{list_reply.value().payload.data(), list_reply.value().length});
    if (!list) return fail(list.error());
    if (list.value().epoch != hello.value().epoch ||
        validate_list_response(list.value()) != Error::OK ||
        list.value().producer.product.length != hello.value().producer.product.length ||
        std::memcmp(list.value().producer.product.bytes.data(),
                    hello.value().producer.product.bytes.data(),
                    list.value().producer.product.length) != 0 ||
        list.value().producer.version.length != hello.value().producer.version.length ||
        std::memcmp(list.value().producer.version.bytes.data(),
                    hello.value().producer.version.bytes.data(),
                    list.value().producer.version.length) != 0)
    {
        return fail(Error::PROTOCOL_ERROR);
    }

    StatusRequest status_request{};
    status_request.epoch = hello.value().epoch;
    status_request.scope.kind = ScopeKind::enclosure;
    status_request.scope.id = 0U;
    const auto status_size = request_payload(
        status_request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_status_request);
    if (!status_size) return fail(status_size.error());
    remote_code = 0U;
    auto status_reply = roundtrip(transport, kOpcodeStatus, 3U,
                                  ByteView{request_bytes.data(), status_size.value()}, deadline,
                                  remote_code, &hello.value().epoch);
    if (!status_reply) {
        result.remote_error_code = remote_code;
        return fail(status_reply.error());
    }
    const auto status = decode_status_response(
        ByteView{status_reply.value().payload.data(), status_reply.value().length});
    if (!status) return fail(status.error());
    if (status.value().scope.kind != status_request.scope.kind ||
        status.value().scope.id != status_request.scope.id)
        return fail(Error::PROTOCOL_ERROR);
    if (status.value().context.epoch != hello.value().epoch ||
        status.value().producer.product.length != hello.value().producer.product.length ||
        std::memcmp(status.value().producer.product.bytes.data(),
                    hello.value().producer.product.bytes.data(),
                    status.value().producer.product.length) != 0 ||
        status.value().producer.version.length != hello.value().producer.version.length ||
        std::memcmp(status.value().producer.version.bytes.data(),
                    hello.value().producer.version.bytes.data(),
                    status.value().producer.version.length) != 0)
        return fail(Error::PROTOCOL_ERROR);
    StatusContext context{};
    context.list = &list.value();
    const Error valid = validate_status_response(status.value(), context);
    if (valid != Error::OK) return fail(valid);
    if (!same_snapshot_identity(list.value(), status.value()))
        return fail(Error::PROTOCOL_ERROR);
    result.snapshot.list = list.value();
    result.snapshot.status = status.value();
    result.error = Error::OK;
    return result;
}

namespace {

bool nonzero_uuid(const Uuid& value) noexcept
{
    for (const std::uint8_t byte : value) {
        if (byte != 0U) return true;
    }
    return false;
}

std::uint64_t deadline_after(ClientTransport& transport,
                             std::uint32_t milliseconds) noexcept
{
    const std::uint64_t now = transport.monotonic_ms();
    return now > std::numeric_limits<std::uint64_t>::max() - milliseconds
               ? std::numeric_limits<std::uint64_t>::max()
               : now + milliseconds;
}

template <typename Request>
Result<std::size_t> encode_body(const Request& value, MutableByteView output,
                                Result<std::size_t> (*encode)(const Request&,
                                                               MutableByteView) noexcept) noexcept
{
    return encode(value, output);
}

bool lists_reader(const ListResponse& list, std::uint16_t reader_id) noexcept
{
    if (!list.readers.present) return false;
    for (std::size_t index = 0U; index < list.readers.value.count; ++index) {
        if (list.readers.value.items[index].id == reader_id) return true;
    }
    return false;
}

}  // namespace

RecordingControlResult begin_recording(ClientTransport& transport,
                                       const RecordingOptions& options,
                                       Timeout timeout) noexcept
{
    return begin_recording(transport, options, timeout, nullptr, nullptr);
}

RecordingControlResult begin_recording(ClientTransport& transport,
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
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
        return result;
    };

    // The shared status fetch performs the HELLO, LIST and STATUS sequence on
    // this same connection and leaves it open on success.
    const StatusFetchResult status = fetch_status(transport, timeout);
    if (!status) {
        result.remote_error_code = status.remote_error_code;
        return fail(status.error);
    }
    if (validator != nullptr) {
        const Error validation = validator(status.snapshot, validator_context);
        if (validation != Error::OK) return fail(validation);
    }
    RecordingSession& session = result.session;
    session.epoch = status.snapshot.status.context.epoch;

    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    std::uint32_t remote_code = 0U;

    AcquireRequest acquire{};
    acquire.epoch = session.epoch;
    acquire.receiver_id = options.receiver_id;
    auto acquire_size = encode_body(
        acquire, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_acquire_request);
    if (!acquire_size) return fail(acquire_size.error());
    auto acquire_reply = roundtrip(transport, kOpcodeAcquire, 4U,
                                   ByteView{request_bytes.data(), acquire_size.value()},
                                   deadline, remote_code, &session.epoch);
    if (!acquire_reply) {
        result.remote_error_code = remote_code;
        return fail(acquire_reply.error());
    }
    const auto acquired = decode_acquire_response(
        ByteView{acquire_reply.value().payload.data(), acquire_reply.value().length});
    if (!acquired) return fail(acquired.error());
    if (acquired.value().epoch != session.epoch || acquired.value().lease_id == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.lease_id = acquired.value().lease_id;

    TuneRequest tune = options.tune;
    tune.epoch = session.epoch;
    tune.lease_id = session.lease_id;
    auto tune_size = encode_body(
        tune, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_tune_request);
    if (!tune_size) return fail(tune_size.error());
    remote_code = 0U;
    auto tune_reply = roundtrip(transport, kOpcodeTune, 5U,
                                ByteView{request_bytes.data(), tune_size.value()},
                                deadline, remote_code, &session.epoch);
    if (!tune_reply) {
        result.remote_error_code = remote_code;
        return fail(tune_reply.error());
    }
    const auto tuned = decode_tune_result(
        ByteView{tune_reply.value().payload.data(), tune_reply.value().length});
    if (!tuned) return fail(tuned.error());
    if (tuned.value().epoch != session.epoch || tuned.value().tune_revision == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.tune_result = tuned.value();
    session.tune_revision = tuned.value().tune_revision;

    StartRequest start{};
    start.epoch = session.epoch;
    start.lease_id = session.lease_id;
    start.tune_revision = session.tune_revision;
    auto start_size = encode_body(
        start, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_start_request);
    if (!start_size) return fail(start_size.error());
    remote_code = 0U;
    auto start_reply = roundtrip(transport, kOpcodeStartStream, 6U,
                                 ByteView{request_bytes.data(), start_size.value()},
                                 deadline, remote_code, &session.epoch);
    if (!start_reply) {
        result.remote_error_code = remote_code;
        return fail(start_reply.error());
    }
    const auto started = decode_start_response(
        ByteView{start_reply.value().payload.data(), start_reply.value().length});
    if (!started) return fail(started.error());
    if (started.value().epoch != session.epoch || started.value().stream_id == 0U ||
        !nonzero_uuid(started.value().attach_token) ||
        started.value().expires_after_ms == 0U)
        return fail(Error::PROTOCOL_ERROR);
    session.stream_id = started.value().stream_id;
    session.attach_token = started.value().attach_token;
    session.expires_after_ms = started.value().expires_after_ms;
    session.next_request_id = 7U;
    result.error = Error::OK;
    return result;
}

StopControlResult stop_recording(ClientTransport& transport,
                                 RecordingSession& session,
                                 Timeout timeout) noexcept
{
    StopControlResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    StopRequest request{};
    request.epoch = session.epoch;
    request.lease_id = session.lease_id;
    request.stream_id = session.stream_id;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_stop_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeStopStream, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto stopped = decode_stop_response(
        ByteView{reply.value().payload.data(), reply.value().length});
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

ReleaseControlResult release_recording(ClientTransport& transport,
                                       RecordingSession& session,
                                       Timeout timeout) noexcept
{
    ReleaseControlResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    ReleaseRequest request{};
    request.epoch = session.epoch;
    request.lease_id = session.lease_id;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_release_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeRelease, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto released = decode_release_response(
        ByteView{reply.value().payload.data(), reply.value().length});
    if (!released) {
        result.error = released.error();
        transport.close();
        return result;
    }
    if (released.value().epoch != session.epoch || !released.value().released) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    result.response = released.value();
    result.error = Error::OK;
    return result;
}

CardOpenResult begin_card(ClientTransport& transport, const CardClientOptions& options,
                          Timeout timeout) noexcept
{
    CardOpenResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
        return result;
    };

    // The shared status fetch performs HELLO with a mandatory CARD_SERVICE,
    // then LIST and STATUS on this same connection and leaves it open on
    // success.
    const StatusFetchResult status = fetch_status(transport, timeout, kFeatureCardService);
    if (!status) {
        result.remote_error_code = status.remote_error_code;
        return fail(status.error);
    }
    if (!lists_reader(status.snapshot.list, options.reader_id))
        return fail(Error::NOT_FOUND);
    CardClientSession& session = result.session;
    session.epoch = status.snapshot.status.context.epoch;
    session.reader_id = options.reader_id;

    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    std::uint32_t remote_code = 0U;

    CardStatusRequest status_request{};
    status_request.key.epoch = session.epoch;
    status_request.key.reader_id = session.reader_id;
    const auto status_size = encode_body(
        status_request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_status_request);
    if (!status_size) return fail(status_size.error());
    auto status_reply = roundtrip(transport, kOpcodeCardStatus, 4U,
                                  ByteView{request_bytes.data(), status_size.value()},
                                  deadline, remote_code, &session.epoch);
    if (!status_reply) {
        result.remote_error_code = remote_code;
        return fail(status_reply.error());
    }
    const auto reader_status = decode_card_status_response(
        ByteView{status_reply.value().payload.data(), status_reply.value().length});
    if (!reader_status) return fail(reader_status.error());
    if (reader_status.value().key.epoch != session.epoch ||
        reader_status.value().key.reader_id != session.reader_id ||
        validate_card_status_response(reader_status.value()) != Error::OK)
        return fail(Error::PROTOCOL_ERROR);

    CardConnectRequest connect{};
    connect.key.epoch = session.epoch;
    connect.key.reader_id = session.reader_id;
    connect.share = options.share;
    connect.preferred_protocols = options.preferred_protocols;
    const auto connect_size = encode_body(
        connect, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_connect_request);
    if (!connect_size) return fail(connect_size.error());
    remote_code = 0U;
    auto connect_reply = roundtrip(transport, kOpcodeCardConnect, 5U,
                                   ByteView{request_bytes.data(), connect_size.value()},
                                   deadline, remote_code, &session.epoch);
    if (!connect_reply) {
        result.remote_error_code = remote_code;
        return fail(connect_reply.error());
    }
    const auto connection = decode_card_session_result(
        ByteView{connect_reply.value().payload.data(), connect_reply.value().length});
    if (!connection) return fail(connection.error());
    if (connection.value().epoch != session.epoch ||
        connection.value().reader_id != session.reader_id ||
        validate_card_session_result(connection.value()) != Error::OK)
        return fail(Error::PROTOCOL_ERROR);
    session.card_handle = connection.value().card_handle;
    session.bound_generation = connection.value().bound_generation;
    session.next_request_id = 6U;
    result.reader_status = reader_status.value();
    result.connection = connection.value();
    result.error = Error::OK;
    return result;
}

CardStatusResult card_status(ClientTransport& transport, CardClientSession& session,
                             Timeout timeout) noexcept
{
    CardStatusResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    CardStatusRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_status_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeCardStatus, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto decoded = decode_card_status_response(
        ByteView{reply.value().payload.data(), reply.value().length});
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

CardTransmitResult card_transmit(ClientTransport& transport, CardClientSession& session,
                                 CardProtocol protocol, ByteView command,
                                 Timeout timeout) noexcept
{
    CardTransmitResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U ||
        command.data == nullptr || command.size == 0U || command.size > kMaxApduBytes) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    CardTransmitRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    request.key.bound_generation = session.bound_generation;
    request.key.card_handle = session.card_handle;
    request.protocol = protocol;
    request.command.length = static_cast<std::uint32_t>(command.size);
    std::memcpy(request.command.bytes.data(), command.data, command.size);
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_transmit_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeCardTransmit, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto decoded = decode_card_transmit_response(
        ByteView{reply.value().payload.data(), reply.value().length});
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

CardResetResult card_reset(ClientTransport& transport, CardClientSession& session,
                           Timeout timeout) noexcept
{
    CardResetResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    CardResetRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    request.key.bound_generation = session.bound_generation;
    request.key.card_handle = session.card_handle;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_reset_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeCardReset, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto decoded = decode_card_session_result(
        ByteView{reply.value().payload.data(), reply.value().length});
    if (!decoded) {
        result.error = decoded.error();
        transport.close();
        return result;
    }
    if (decoded.value().epoch != session.epoch ||
        decoded.value().reader_id != session.reader_id ||
        validate_card_session_result(decoded.value()) != Error::OK) {
        result.error = Error::PROTOCOL_ERROR;
        transport.close();
        return result;
    }
    session.card_handle = decoded.value().card_handle;
    session.bound_generation = decoded.value().bound_generation;
    result.response = decoded.value();
    result.error = Error::OK;
    return result;
}

CardDisconnectResult card_disconnect(ClientTransport& transport, CardClientSession& session,
                                     Disposition disposition, Timeout timeout) noexcept
{
    CardDisconnectResult result{};
    if (timeout.milliseconds == 0U || session.next_request_id == 0U ||
        session.card_handle == 0U || session.bound_generation == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = deadline_after(transport, timeout.milliseconds);
    CardDisconnectRequest request{};
    request.key.epoch = session.epoch;
    request.key.reader_id = session.reader_id;
    request.key.bound_generation = session.bound_generation;
    request.key.card_handle = session.card_handle;
    request.disposition = disposition;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto size = encode_body(
        request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_card_disconnect_request);
    if (!size) {
        result.error = size.error();
        transport.close();
        return result;
    }
    std::uint32_t remote_code = 0U;
    auto reply = roundtrip(transport, kOpcodeCardDisconnect, session.next_request_id,
                           ByteView{request_bytes.data(), size.value()}, deadline,
                           remote_code, &session.epoch);
    ++session.next_request_id;
    if (!reply) {
        result.remote_error_code = remote_code;
        result.error = reply.error();
        transport.close();
        return result;
    }
    const auto decoded = decode_card_disconnect_response(
        ByteView{reply.value().payload.data(), reply.value().length});
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
    // The handle is consumed by this connection: a stale reuse must not look
    // like a live owner handle.
    session.card_handle = 0U;
    session.bound_generation = 0U;
    result.response = decoded.value();
    result.error = Error::OK;
    return result;
}

}  // namespace dtv::userland::wire
