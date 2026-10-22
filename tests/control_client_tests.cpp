// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/control_connection.h>

#include <array>
#include <cstdio>
#include <cstring>
#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
#include <pthread.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DTV_USERLAND_CONTROL_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DTV_USERLAND_CONTROL_ADDRESS_SANITIZER 1
#endif
#endif

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
        }                                                                                          \
    } while (0)

constexpr std::size_t kFrameCapacity = w::kHeaderSize + w::kMaxControlPayload;

template <std::size_t N>
void set_text(w::WString<N>& out, const char* text)
{
    out.length = 0U;
    while (text[out.length] != '\0') {
        out.bytes[out.length] = text[out.length];
        ++out.length;
    }
}

w::ServerHello make_server()
{
    w::ServerHello server;
    for (std::size_t i = 0U; i < server.epoch.size(); ++i) {
        server.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    set_text(server.producer.product, "test-product");
    set_text(server.producer.version, "1.2.3");
    return server;
}

void sparse_list(w::ListResponse& list, const w::Uuid& epoch)
{
    list = w::ListResponse{};
    list.epoch = epoch;
    set_text(list.producer.product, "test-product");
    set_text(list.producer.version, "1.2.3");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.enclosure.present = true;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.enclosure.value.identity.uniqueness = w::Uniqueness::not_applicable;
    list.functions.count = 1U;
    list.functions.items[0].id = 0U;
    list.functions.items[0].vid = 0x1234U;
    list.functions.items[0].pid = 0x5678U;
    list.functions.items[0].mode = w::Mode::runtime;
    list.functions.items[0].serial_state = w::SerialState::absent;
    list.functions.items[0].present = true;
    list.functions.items[0].identification_reason = w::IdentificationReason::model_unconfirmed;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0].id = 513U;
    list.receivers.value.items[0].systems.mask = 0x01U;
    list.receivers.value.items[0].binding.present = true;
    list.receivers.value.items[0].binding.value = w::Binding{0U, 0U};
    list.readers.present = true;
    list.readers.value.count = 0U;
}

enum class Fault : std::uint8_t {
    none,
    bad_hello_version,
    unrequested_hello_feature,
    wrong_list_request_id,
    wrong_status_opcode,
    bad_magic,
    truncated_status,
    wrong_list_epoch,
    wrong_status_epoch,
    noncanonical_status,
    remote_list_error,
};

class FakeTransport final : public w::ClientTransport {
public:
    explicit FakeTransport(Fault fault = Fault::none)
        : server_(make_server()), connection_(server_), fault_(fault)
    {
        sparse_list(list_, server_.epoch);
        context_.epoch = server_.epoch;
        context_.state = w::ContextState::ready;
        snapshot_ = w::Snapshot{3U, 5000U};
    }

    Result<void> write_all(ByteView bytes) noexcept override
    {
        if (closed_ || reply_offset_ != reply_size_ || request_count_ >= request_ids_.size()) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const auto frame = w::decode_frame(bytes);
        if (!frame || frame.value().header.kind != w::Kind::request) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const std::uint32_t request_id = frame.value().header.request_id;
        const std::uint16_t opcode = frame.value().header.opcode;
        request_ids_[request_count_] = request_id;
        request_opcodes_[request_count_] = opcode;
        ++request_count_;

        w::ControlRequest request{};
        const w::Inbound inbound = connection_.accept(
            frame.value(), request, MutableByteView{reply_.data(), reply_.size()});
        if (inbound.action == w::InboundAction::close) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        if (inbound.action == w::InboundAction::respond) {
            reply_size_ = inbound.reply_size;
        } else if (!queue_dispatch(request)) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        connection_.complete();
        reply_offset_ = 0U;
        apply_fault(opcode, request_id);
        if (reply_size_ != 0U && request_count_ <= recorded_replies_.size()) {
            const std::size_t index = request_count_ - 1U;
            std::memcpy(recorded_replies_[index].data(), reply_.data(), reply_size_);
            recorded_sizes_[index] = reply_size_;
        }
        return Result<void>::success();
    }

    Result<std::size_t> read_some(MutableByteView bytes) noexcept override
    {
        if (bytes.data == nullptr && bytes.size != 0U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        if (reply_offset_ == reply_size_) {
            return Result<std::size_t>::success(0U);
        }
        const std::size_t remaining = reply_size_ - reply_offset_;
        const std::size_t count = remaining < 13U ? remaining : 13U;
        if (bytes.size < count) {
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        }
        std::memcpy(bytes.data, reply_.data() + reply_offset_, count);
        reply_offset_ += count;
        return Result<std::size_t>::success(count);
    }

    void close() noexcept override { closed_ = true; }

    bool closed() const noexcept { return closed_; }
    std::size_t request_count() const noexcept { return request_count_; }
    std::uint32_t request_id(std::size_t index) const noexcept { return request_ids_[index]; }
    std::uint16_t request_opcode(std::size_t index) const noexcept { return request_opcodes_[index]; }
    const std::uint8_t* recorded_reply(std::size_t index) const noexcept
    {
        return recorded_replies_[index].data();
    }
    std::size_t recorded_size(std::size_t index) const noexcept { return recorded_sizes_[index]; }

private:
    bool queue_dispatch(const w::ControlRequest& request) noexcept
    {
        if (request.kind == w::ControlRequestKind::list) {
            if (fault_ == Fault::remote_list_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::UNSUPPORTED), "list",
                    w::Scope{w::ScopeKind::enclosure, 0U},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            response_list_ = list_;
            w::ListResponse& response = response_list_;
            if (fault_ == Fault::wrong_list_epoch) response.epoch[0] ^= 0x01U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_list_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::status) {
            response_status_ = w::StatusResponse{};
            w::StatusResponse& response = response_status_;
            if (w::build_status_shell(list_, context_, snapshot_,
                                      w::Scope{w::ScopeKind::enclosure, 0U}, response) != Error::OK) {
                return false;
            }
            if (fault_ == Fault::wrong_status_epoch) response.context.epoch[0] ^= 0x01U;
            if (fault_ == Fault::noncanonical_status && response.receivers.present) {
                response.receivers.value.items[0].descriptor.systems.mask = 0x03U;
            }
            Result<std::size_t> encoded =
                fault_ == Fault::wrong_status_epoch || fault_ == Fault::noncanonical_status
                    ? w::encode_record_frame(w::Kind::response, request.opcode,
                                             request.request_id, response,
                                             w::encode_status_response,
                                             MutableByteView{reply_.data(), reply_.size()})
                    : w::encode_status_frame(request.request_id, response, list_,
                                             MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        return false;
    }

    void apply_fault(std::uint16_t opcode, std::uint32_t request_id) noexcept
    {
        if (fault_ == Fault::bad_hello_version && opcode == w::kOpcodeHello) {
            reply_[w::kHeaderSize] = 2U;
        } else if (fault_ == Fault::unrequested_hello_feature && opcode == w::kOpcodeHello) {
            reply_[w::kHeaderSize + 4U] = 1U;
        } else if (fault_ == Fault::wrong_list_request_id && opcode == w::kOpcodeList) {
            rewrite_header(static_cast<std::uint32_t>(request_id + 1U), opcode);
        } else if (fault_ == Fault::wrong_status_opcode && opcode == w::kOpcodeStatus) {
            rewrite_header(request_id, w::kOpcodeList);
        } else if (fault_ == Fault::bad_magic && opcode == w::kOpcodeList) {
            reply_[0] ^= 0xffU;
        } else if (fault_ == Fault::truncated_status && opcode == w::kOpcodeStatus &&
                   reply_size_ != 0U) {
            --reply_size_;
        }
    }

    void rewrite_header(std::uint32_t request_id, std::uint16_t opcode) noexcept
    {
        const auto decoded = w::decode_frame(ByteView{reply_.data(), reply_size_});
        if (!decoded) return;
        w::Header header = decoded.value().header;
        header.request_id = request_id;
        header.opcode = opcode;
        const auto encoded = w::encode_frame(
            header, decoded.value().payload,
            MutableByteView{scratch_.data(), scratch_.size()});
        if (encoded) {
            std::memcpy(reply_.data(), scratch_.data(), encoded.value());
            reply_size_ = encoded.value();
        }
    }

    w::ServerHello server_;
    w::ControlConnection connection_;
    w::ListResponse list_{};
    w::ListResponse response_list_{};
    w::StatusResponse response_status_{};
    w::Context context_{};
    w::Snapshot snapshot_{};
    Fault fault_;
    std::array<std::uint8_t, kFrameCapacity> reply_{};
    std::array<std::uint8_t, kFrameCapacity> scratch_{};
    std::size_t reply_size_ = 0U;
    std::size_t reply_offset_ = 0U;
    std::array<std::uint32_t, 3U> request_ids_{};
    std::array<std::uint16_t, 3U> request_opcodes_{};
    std::array<std::array<std::uint8_t, kFrameCapacity>, 3U> recorded_replies_{};
    std::array<std::size_t, 3U> recorded_sizes_{};
    std::size_t request_count_ = 0U;
    bool closed_ = false;
};

void test_successful_read_only_exchange()
{
    FakeTransport transport;
    const w::StatusFetchResult result = w::fetch_status(transport);
    CHECK(result);
    CHECK(!transport.closed());
    CHECK(transport.request_count() == 3U);
    CHECK(transport.request_id(0U) == 1U && transport.request_id(1U) == 2U &&
          transport.request_id(2U) == 3U);
    CHECK(transport.request_opcode(0U) == w::kOpcodeHello &&
          transport.request_opcode(1U) == w::kOpcodeList &&
          transport.request_opcode(2U) == w::kOpcodeStatus);
    CHECK(result.snapshot.list.epoch == result.snapshot.status.context.epoch);
    CHECK(result.snapshot.status.scope.kind == w::ScopeKind::enclosure &&
          result.snapshot.status.scope.id == 0U);
    CHECK(result.snapshot.list.receivers.value.count == 1U &&
          result.snapshot.status.receivers.value.count == 1U);
}

void test_invalid_responses_close_without_publishing_snapshot()
{
    const Fault faults[] = {Fault::bad_hello_version, Fault::unrequested_hello_feature,
                            Fault::wrong_list_request_id, Fault::wrong_status_opcode, Fault::bad_magic,
                            Fault::truncated_status, Fault::wrong_list_epoch,
                            Fault::wrong_status_epoch, Fault::noncanonical_status};
    for (const Fault fault : faults) {
        FakeTransport transport(fault);
        const w::StatusFetchResult result = w::fetch_status(transport);
        CHECK(!result);
        CHECK(transport.closed());
        CHECK(result.snapshot.list.epoch == w::Uuid{});
    }
}

void test_remote_error_code_is_preserved()
{
    FakeTransport transport(Fault::remote_list_error);
    const w::StatusFetchResult result = w::fetch_status(transport);
    CHECK(!result && result.error == Error::UNSUPPORTED);
    CHECK(result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(transport.closed());
    CHECK(result.snapshot.list.epoch == w::Uuid{});
}

#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
class ReplayTransport final : public w::ClientTransport {
public:
    explicit ReplayTransport(const FakeTransport& source) noexcept : source_(source) {}

    Result<void> write_all(ByteView bytes) noexcept override
    {
        if (closed_ || reply_index_ != request_count_ || request_count_ >= 3U) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const auto frame = w::decode_frame(bytes);
        if (!frame || frame.value().header.kind != w::Kind::request ||
            frame.value().header.request_id != request_count_ + 1U ||
            frame.value().header.opcode != expected_opcodes_[request_count_]) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        reply_offset_ = 0U;
        ++request_count_;
        return Result<void>::success();
    }

    Result<std::size_t> read_some(MutableByteView bytes) noexcept override
    {
        const std::size_t index = request_count_ - 1U;
        const std::size_t size = source_.recorded_size(index);
        if (reply_offset_ == size) {
            return Result<std::size_t>::success(0U);
        }
        const std::size_t remaining = size - reply_offset_;
        const std::size_t count = remaining < bytes.size ? remaining : bytes.size;
        std::memcpy(bytes.data, source_.recorded_reply(index) + reply_offset_, count);
        reply_offset_ += count;
        if (reply_offset_ == size) ++reply_index_;
        return Result<std::size_t>::success(count);
    }

    void close() noexcept override { closed_ = true; }
    bool closed() const noexcept { return closed_; }
    std::size_t request_count() const noexcept { return request_count_; }

private:
    const FakeTransport& source_;
    const std::uint16_t expected_opcodes_[3U] = {w::kOpcodeHello, w::kOpcodeList,
                                                  w::kOpcodeStatus};
    std::size_t request_count_ = 0U;
    std::size_t reply_index_ = 0U;
    std::size_t reply_offset_ = 0U;
    bool closed_ = false;
};

class EofTransport final : public w::ClientTransport {
public:
    Result<void> write_all(ByteView) noexcept override { return Result<void>::success(); }
    Result<std::size_t> read_some(MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    void close() noexcept override { closed_ = true; }
    bool closed() const noexcept { return closed_; }

private:
    bool closed_ = false;
};

struct StackProbe final {
    w::ClientTransport* transport;
    Error result = Error::INTERNAL;
    bool snapshot_present = false;
};

void* fetch_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<StackProbe*>(value);
    const w::StatusFetchResult result = w::fetch_status(*probe.transport);
    probe.result = result.error;
    probe.snapshot_present = result.snapshot.list.epoch != w::Uuid{};
    return nullptr;
}

bool run_stack_probe(w::ClientTransport& transport, std::size_t stack_size,
                     Error expected_error, bool expect_snapshot)
{
    StackProbe probe{&transport};
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, fetch_on_thread, &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    if (pthread_join(thread, nullptr) != 0) return false;
    return probe.result == expected_error && probe.snapshot_present == expect_snapshot;
}

void test_small_stack_probes()
{
#if defined(DTV_USERLAND_CONTROL_ADDRESS_SANITIZER)
    constexpr std::size_t kStackSizes[] = {512U * 1024U, 768U * 1024U};
#else
    constexpr std::size_t kStackSizes[] = {256U * 1024U, 512U * 1024U, 768U * 1024U};
#endif
    FakeTransport source;
    const w::StatusFetchResult seeded = w::fetch_status(source);
    CHECK(seeded);
    for (const std::size_t stack_size : kStackSizes) {
        EofTransport eof;
        CHECK(run_stack_probe(eof, stack_size, Error::DISCONNECTED, false));
        CHECK(eof.closed());

        ReplayTransport normal(source);
        CHECK(run_stack_probe(normal, stack_size, Error::OK, true));
        CHECK(!normal.closed());
        CHECK(normal.request_count() == 3U);
    }
}
#endif

}  // namespace

int main()
{
    test_successful_read_only_exchange();
    test_invalid_responses_close_without_publishing_snapshot();
    test_remote_error_code_is_preserved();
#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
    test_small_stack_probes();
#endif
    std::printf("control_client_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
