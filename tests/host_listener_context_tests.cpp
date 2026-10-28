// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/enclosure_context.h"
#include "host_listener_context_fake_io.h"

#include "dtv/userland/host_daemon/card_service.h"
#include "px4/tuner_service.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::host_listener_context;
using namespace px4::userland;
namespace w = dtv::userland::wire;
namespace detail = dtv::userland::host_listener_context::detail;
const char* protocol_failure_stage = "start";

class NonceGate final {
public:
    void arm()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entered_ = false;
        blocked_ = true;
    }
    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(5),
            [this] { return entered_; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked_ = false;
        changed_.notify_all();
    }
    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!blocked_) return;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return !blocked_; });
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool blocked_ = false;
};

class Backend final : public TunerServiceBackend {
public:
    explicit Backend(std::shared_ptr<std::array<std::atomic<std::size_t>, 8U>> tune_calls)
        : tune_calls_(std::move(tune_calls))
    {
    }
    std::uint8_t receiver_count() const noexcept override { return 8U; }
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t, std::uint32_t) noexcept override
    {
        (*tune_calls_)[receiver].fetch_add(1U, std::memory_order_relaxed);
        return Result<TunerHzProgramming>::success({});
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        TunerStrictProfile profile;
        if ((receiver == 2U || receiver == 3U || receiver == 4U || receiver == 6U) &&
            system == ipc::System::ISDB_T) {
            profile.native_hz = true;
            profile.min_hz = 400000000U;
            profile.max_hz = 800000000U;
        }
        return profile;
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> start_capture(std::uint8_t, ipc::System) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, ipc::System) noexcept override
    {
        return Result<void>::success();
    }
private:
    std::shared_ptr<std::array<std::atomic<std::size_t>, 8U>> tune_calls_;
};

class Nonce final : public TunerNonceSource {
public:
    explicit Nonce(std::shared_ptr<NonceGate> gate = {}) : gate_(std::move(gate)) {}
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        if (gate_) gate_->wait();
        std::array<std::uint8_t, ipc::kNonceLength> value{};
        value[0U] = next_.fetch_add(1U, std::memory_order_relaxed);
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(value);
    }
private:
    std::shared_ptr<NonceGate> gate_;
    std::atomic<std::uint8_t> next_{1U};
};

class Clock final : public TunerServiceTime {
public:
    explicit Clock(std::shared_ptr<std::atomic<std::uint64_t>> now)
        : now_(std::move(now))
    {
    }
    std::uint64_t monotonic_ms() noexcept override
    {
        return now_->load(std::memory_order_acquire);
    }
    void sleep_ms(std::uint32_t) noexcept override {}
private:
    std::shared_ptr<std::atomic<std::uint64_t>> now_;
};

class Stream final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attachment_ = attachment;
        attached_ = true;
        packets_remaining_ = 2U;
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        attached_ = false;
        final_.counters = counters_;
        final_.terminal = static_cast<std::uint8_t>(TunerStreamTerminal::stopped);
        return Result<void>::success();
    }
    Result<TunerStreamCounters> stats(const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return attached_ && same(attachment)
            ? Result<TunerStreamCounters>::success(counters_)
            : Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return same(attachment) ? Result<TunerStreamFinalSnapshot>::success(final_)
                                : Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return same(attachment) ? Result<void>::success()
                                : Result<void>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamReadResult> read(const TunerAttachment& attachment,
                                        MutableByteView output,
                                        Timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same(attachment)) return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
        if (packets_remaining_ != 0U && output.size >= 188U) {
            for (std::size_t i = 0U; i < 188U; ++i)
                output.data[i] = static_cast<std::uint8_t>(i);
            --packets_remaining_;
            counters_.packets++;
            counters_.bytes += 188U;
            return Result<TunerStreamReadResult>::success({188U, false, false,
                TunerStreamTerminal::none});
        }
        return Result<TunerStreamReadResult>::success({0U, !attached_, attached_,
            attached_ ? TunerStreamTerminal::none : TunerStreamTerminal::stopped});
    }
private:
    mutable std::mutex mutex_;
    bool same(const TunerAttachment& other) const noexcept
    {
        return attachment_.owner_client_id == other.owner_client_id &&
            attachment_.lease_id == other.lease_id &&
            attachment_.attachment_id == other.attachment_id &&
            attachment_.receiver == other.receiver && attachment_.system == other.system &&
            attachment_.nonce == other.nonce;
    }
    TunerAttachment attachment_{};
    TunerStreamCounters counters_{};
    TunerStreamFinalSnapshot final_{};
    std::size_t packets_remaining_ = 0U;
    bool attached_ = false;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    bool initialized() const noexcept override { return false; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtrBytes atr_{};
};

class Family final : public host_runtime::EnclosureFamily {
public:
    explicit Family(std::shared_ptr<std::atomic<bool>> shutdown,
                    std::shared_ptr<std::atomic<bool>> destroyed,
                    std::shared_ptr<std::atomic<std::uint64_t>> now,
                    std::shared_ptr<std::array<std::atomic<std::size_t>, 8U>> tune_calls)
        : shutdown_(std::move(shutdown)), destroyed_(std::move(destroyed)),
          backend_(std::move(tune_calls)), clock_(std::move(now))
    {
    }
    ~Family() noexcept override
    {
        destroyed_->store(true, std::memory_order_release);
    }
    Result<void> shutdown() noexcept override
    {
        shutdown_->store(true, std::memory_order_release);
        return Result<void>::success();
    }
    TunerServiceBackend& tuner_backend() noexcept override { return backend_; }
    CardServiceBackend& card_backend() noexcept override { return card_backend_; }
    CardProtocolSession& card_protocol() noexcept override { return card_session_; }
    TunerServiceTime& time() noexcept override { return clock_; }
    TunerStreamControl* stream_control() noexcept override { return &stream_; }
private:
    std::shared_ptr<std::atomic<bool>> shutdown_;
    std::shared_ptr<std::atomic<bool>> destroyed_;
    Backend backend_;
    CardBackend card_backend_;
    CardSession card_session_;
    Clock clock_;
    Stream stream_;
};

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> result;
    while (value[result.length] != '\0') {
        result.bytes[result.length] = value[result.length];
        ++result.length;
    }
    return result;
}

w::ListResponse make_list()
{
    w::ListResponse list;
    list.epoch[0] = 1U;
    list.producer.product = text<32U>("context-test");
    list.producer.version = text<64U>("1");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    auto& receiver = list.receivers.value.items[0U];
    receiver.id = 2U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    receiver.binding.value.local_id = 0U;
    list.readers.present = true;
    return list;
}

ContextConfig config()
{
    ContextConfig value;
#if defined(_WIN32)
    const char* runtime_directory = std::getenv("LOCALAPPDATA");
    value.runtime_directory = runtime_directory == nullptr ? "" : runtime_directory;
#else
    value.runtime_directory = "/tmp";
#endif
    value.product = "context-test";
    value.instance = "one";
    value.stream_engine_epoch = 42U;
    value.stream_arm_budget_ms = 5000U;
    value.canonical_list = make_list();
    value.server.epoch = value.canonical_list.epoch;
    value.server.producer = value.canonical_list.producer;
    value.context.epoch = value.server.epoch;
    return value;
}

std::array<std::uint8_t, w::kHeaderSize + w::kHelloRequestSize> hello_frame()
{
    std::array<std::uint8_t, w::kHeaderSize + w::kHelloRequestSize> frame{};
    w::HelloRequest request{};
    request.min = w::Version{w::kVersionMajor, w::kVersionMinor};
    request.max = w::Version{w::kVersionMajor, w::kVersionMinor};
    const auto payload = w::encode_hello_request(request,
        MutableByteView{frame.data() + w::kHeaderSize, w::kHelloRequestSize});
    if (!payload) return {};
    const w::Header header{w::Kind::request, w::kOpcodeHello, 1U,
                           static_cast<std::uint32_t>(payload.value())};
    (void)w::encode_header(header, MutableByteView{frame.data(), w::kHeaderSize});
    return frame;
}

std::vector<std::uint8_t> request_frame(std::uint16_t opcode, std::uint32_t request_id,
                                        ByteView payload)
{
    std::vector<std::uint8_t> frame(w::kHeaderSize + payload.size);
    const auto header = w::encode_header({w::Kind::request, opcode, request_id,
        static_cast<std::uint32_t>(payload.size)},
        {frame.data(), w::kHeaderSize});
    if (!header) return {};
    if (payload.size != 0U)
        std::memcpy(frame.data() + w::kHeaderSize, payload.data, payload.size);
    return frame;
}

bool wait_frame(FakeListenerIoControl& io, EnclosureContext& context,
                detail::ConnectionToken connection,
                std::size_t& output_offset, std::vector<std::uint8_t>& frame,
                std::uint16_t expected_opcode)
{
    for (std::size_t i = 0U; i < 1000U; ++i) {
        const auto polled = context.poll_once(Timeout{10U});
        if (!polled) {
            std::fprintf(stderr, "fake poll error=%u\n",
                static_cast<unsigned>(polled.error()));
            return false;
        }
        const auto output = io.output(connection);
        if (output.size() < output_offset + w::kHeaderSize) continue;
        const auto header = w::decode_header(
            {output.data() + output_offset, w::kHeaderSize});
        if (!header || output.size() < output_offset + w::kHeaderSize +
                header.value().payload_length) continue;
        const std::size_t size = w::kHeaderSize + header.value().payload_length;
        frame.assign(output.begin() + static_cast<std::ptrdiff_t>(output_offset),
                     output.begin() + static_cast<std::ptrdiff_t>(output_offset + size));
        output_offset += size;
        const auto decoded = w::decode_frame({frame.data(), frame.size()});
        return decoded && (expected_opcode == 0U ||
                           header.value().opcode == expected_opcode);
    }
    std::fprintf(stderr, "wait frame timeout output=%zu offset=%zu\n",
        io.output(connection).size(), output_offset);
    return false;
}

bool exchange_frame(FakeListenerIoControl& io, EnclosureContext& context,
                    detail::ConnectionToken connection,
                    std::size_t& output_offset, const std::vector<std::uint8_t>& request,
                    std::vector<std::uint8_t>& response, std::uint16_t opcode)
{
    io.send(connection, {request.data(), request.size()});
    return wait_frame(io, context, connection, output_offset, response, opcode);
}

ContextConfig multi_receiver_config()
{
    ContextConfig value = config();
    auto& receivers = value.canonical_list.receivers.value;
    receivers.count = 4U;
    const std::array<std::uint8_t, 4U> ids{{2U, 3U, 4U, 6U}};
    for (std::size_t i = 0U; i < ids.size(); ++i) {
        receivers.items[i] = receivers.items[0U];
        receivers.items[i].id = ids[i];
        receivers.items[i].binding.value.local_id = static_cast<std::uint8_t>(i);
    }
    return value;
}

std::vector<std::uint8_t> acquire_request(std::uint32_t request_id,
                                         std::uint8_t receiver)
{
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    const auto size = w::encode_acquire_request({config().server.epoch, receiver},
        {payload.data(), payload.size()});
    return size ? request_frame(w::kOpcodeAcquire, request_id,
        {payload.data(), size.value()}) : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> tune_request(std::uint32_t request_id,
                                      std::uint64_t lease_id,
                                      std::uint32_t timeout_ms)
{
    w::TuneRequest tune;
    tune.epoch = config().server.epoch;
    tune.lease_id = lease_id;
    tune.system = w::System::isdb_t;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = timeout_ms;
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    const auto size = w::encode_tune_request(tune, {payload.data(), payload.size()});
    return size ? request_frame(w::kOpcodeTune, request_id,
        {payload.data(), size.value()}) : std::vector<std::uint8_t>{};
}

bool response_frame(const std::vector<std::uint8_t>& bytes, std::uint16_t opcode,
                    w::Frame& decoded)
{
    const auto result = w::decode_frame({bytes.data(), bytes.size()});
    if (!result || result.value().header.kind != w::Kind::response ||
        result.value().header.opcode != opcode) return false;
    decoded = result.value();
    return true;
}

std::unique_ptr<EnclosureContext> make_context(
    const std::shared_ptr<std::atomic<bool>>& shutdown,
    const std::shared_ptr<std::atomic<bool>>& destroyed,
    const ContextConfig& context_config,
    const std::shared_ptr<std::atomic<std::uint64_t>>& now,
    const std::shared_ptr<std::array<std::atomic<std::size_t>, 8U>>& tune_calls,
    const std::shared_ptr<NonceGate>& gate);

std::unique_ptr<EnclosureContext> make_context(
    const std::shared_ptr<std::atomic<bool>>& shutdown,
    const std::shared_ptr<std::atomic<bool>>& destroyed =
        std::make_shared<std::atomic<bool>>(false))
{
    auto now = std::make_shared<std::atomic<std::uint64_t>>(1U);
    auto tune_calls = std::make_shared<std::array<std::atomic<std::size_t>, 8U>>();
    for (auto& count : *tune_calls) count.store(0U, std::memory_order_relaxed);
    auto gate = std::make_shared<NonceGate>();
    return make_context(shutdown, destroyed, config(), now, tune_calls, gate);
}

std::unique_ptr<EnclosureContext> make_context(
    const std::shared_ptr<std::atomic<bool>>& shutdown,
    const std::shared_ptr<std::atomic<bool>>& destroyed,
    const ContextConfig& context_config,
    const std::shared_ptr<std::atomic<std::uint64_t>>& now,
    const std::shared_ptr<std::array<std::atomic<std::size_t>, 8U>>& tune_calls,
    const std::shared_ptr<NonceGate>& gate)
{
    std::unique_ptr<host_runtime::EnclosureFamily> family(
        new Family(shutdown, destroyed, now, tune_calls));
    auto assembled = EnclosureContext::assemble(
        std::move(family), std::unique_ptr<TunerNonceSource>(new Nonce(gate)),
        context_config);
    if (!assembled) return {};
    if (!assembled.value()->reserve_namespace() || !assembled.value()->publish()) return {};
    return std::move(assembled.value());
}

bool unpublish_error_still_quiesces_owner()
{
    FakeListenerIoControl io;
    io.fail_unpublish_after_cleanup();
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown, destroyed);
    if (!context) return false;
    const auto first = context->close(Timeout{5000U});
    const bool first_error = !first && first.error() == Error::INTERNAL;
    const auto repeated = context->close(Timeout{0U});
    const bool repeated_error = !repeated && repeated.error() == Error::INTERNAL;
    const bool teardown = io.unpublish_count() == 1U &&
        io.namespace_release_count() == 1U && !io.published() && !io.reserved() &&
        shutdown->load(std::memory_order_acquire);
    context.reset();
    set_fake_listener_io_control(nullptr);
    const bool owner_destroyed = destroyed->load(std::memory_order_acquire);
    return first_error && repeated_error && teardown && owner_destroyed;
}

bool lifecycle()
{
    FakeListenerIoControl control;
    control.set_chunks(5U, 3U);
    set_fake_listener_io_control(&control);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown);
    if (!context) return false;
    const auto request = hello_frame();
    const auto connection = control.connect(
        detail::ListenerKind::control, ByteView{request.data(), request.size()});
    std::vector<std::uint8_t> output;
    Result<w::Frame> frame = Result<w::Frame>::failure(Error::NOT_READY);
    for (std::size_t i = 0U; i < 100U; ++i) {
        if (!context->poll_once(Timeout{0U})) return false;
        output = control.output(connection);
        if (output.size() < w::kHeaderSize) continue;
        const auto header = w::decode_header(ByteView{output.data(), w::kHeaderSize});
        if (!header || output.size() < w::kHeaderSize + header.value().payload_length)
            continue;
        frame = w::decode_frame(ByteView{output.data(), output.size()});
        break;
    }
    if (!frame || frame.value().header.kind != w::Kind::response ||
        frame.value().header.opcode != w::kOpcodeHello ||
        frame.value().header.request_id != 1U ||
        !w::decode_hello_response(frame.value().payload)) return false;
    const auto closed = context->close(Timeout{5000U});
    if (!closed || closed.value() != CloseState::quiesced) return false;
    if (!shutdown->load(std::memory_order_acquire)) return false;
    context.reset();
    return true;
}

bool control_and_stream_protocol()
{
    protocol_failure_stage = "context";
    FakeListenerIoControl io;
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown);
    if (!context) return false;
    const auto control = io.connect(detail::ListenerKind::control);
    if (!context->poll_once(Timeout{0U})) return false;
    std::size_t control_offset = 0U;
    std::vector<std::uint8_t> response;
    protocol_failure_stage = "HELLO";
    const auto hello = hello_frame();
    const std::vector<std::uint8_t> hello_request(hello.begin(), hello.end());
    if (!exchange_frame(io, *context, control, control_offset,
            hello_request, response, w::kOpcodeHello)) return false;

    protocol_failure_stage = "ACQUIRE";
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    auto size = w::encode_acquire_request({config().server.epoch, 2U},
        {payload.data(), payload.size()});
    auto request = request_frame(w::kOpcodeAcquire, 3U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeAcquire)) return false;
    auto decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto acquired = w::decode_acquire_response(decoded.value().payload);
    if (!acquired || acquired.value().lease_id == 0U) return false;

    protocol_failure_stage = "TUNE";
    w::TuneRequest tune;
    tune.epoch = config().server.epoch;
    tune.lease_id = acquired.value().lease_id;
    tune.system = w::System::isdb_t;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = 100U;
    payload.fill(0U);
    size = w::encode_tune_request(tune, {payload.data(), payload.size()});
    request = request_frame(w::kOpcodeTune, 4U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeTune)) {
        std::fprintf(stderr, "TUNE exchange failed\n");
        return false;
    }
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) {
        std::fprintf(stderr, "TUNE kind=%u\n", decoded ?
            static_cast<unsigned>(decoded.value().header.kind) : 99U);
        return false;
    }
    const auto tuned = w::decode_tune_result(decoded.value().payload);
    if (!tuned || tuned.value().tune_revision == 0U) return false;

    protocol_failure_stage = "START";
    w::StartRequest start;
    start.epoch = config().server.epoch;
    start.lease_id = acquired.value().lease_id;
    start.tune_revision = tuned.value().tune_revision;
    payload.fill(0U);
    size = w::encode_start_request(start, {payload.data(), payload.size()});
    request = request_frame(w::kOpcodeStartStream, 7U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeStartStream)) return false;
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto started = w::decode_start_response(decoded.value().payload);
    if (!started || started.value().stream_id == 0U) return false;

    protocol_failure_stage = "data accept";
    const auto data = io.connect(detail::ListenerKind::data);
    if (!context->poll_once(Timeout{0U})) return false;
    w::AttachRequest attach;
    attach.epoch = config().server.epoch;
    attach.lease_id = acquired.value().lease_id;
    attach.stream_id = started.value().stream_id;
    attach.attach_token = started.value().attach_token;
    std::array<std::uint8_t, 64U> attach_payload{};
    size = w::encode_attach_request(attach,
        {attach_payload.data(), attach_payload.size()});
    request = request_frame(w::kOpcodeAttachStream, 1U,
        {attach_payload.data(), size ? size.value() : 0U});
    protocol_failure_stage = "ATTACH";
    std::size_t data_offset = 0U;
    if (!size || !exchange_frame(io, *context, data, data_offset, request,
            response, w::kOpcodeAttachStream)) return false;
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto attached = w::decode_attach_response(decoded.value().payload);
    if (!attached || attached.value().stream_id != started.value().stream_id) return false;
    protocol_failure_stage = "TS_DATA";
    if (!wait_frame(io, *context, data, data_offset, response, w::kOpcodeTsDataEvent))
        return false;
    decoded = w::decode_frame({response.data(), response.size()});
    const auto packet = decoded ? w::decode_ts_data(decoded.value().payload)
                                : Result<w::TsData>::failure(Error::PROTOCOL_ERROR);
    if (!packet || packet.value().stream_id != started.value().stream_id ||
        packet.value().packets.size != 188U || packet.value().packets.data[187U] != 187U)
        return false;

    protocol_failure_stage = "STOP";
    w::StopRequest stop;
    stop.epoch = config().server.epoch;
    stop.lease_id = acquired.value().lease_id;
    stop.stream_id = started.value().stream_id;
    payload.fill(0U);
    size = w::encode_stop_request(stop, {payload.data(), payload.size()});
    request = request_frame(w::kOpcodeStopStream, 12U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeStopStream)) return false;
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto stopped = w::decode_stop_response(decoded.value().payload);
    if (!stopped || stopped.value().final.id != started.value().stream_id) return false;

    protocol_failure_stage = "STREAM_END";
    bool stream_end = false;
    for (std::size_t i = 0U; i < 8U && !stream_end; ++i) {
        if (!wait_frame(io, *context, data, data_offset, response, 0U)) return false;
        decoded = w::decode_frame({response.data(), response.size()});
        if (!decoded || decoded.value().header.kind != w::Kind::event) return false;
        if (decoded.value().header.opcode == w::kOpcodeStreamEndEvent) {
            const auto ended = w::decode_stream_end_event(decoded.value().payload);
            stream_end = ended && ended.value().final.id == started.value().stream_id;
        } else if (decoded.value().header.opcode == w::kOpcodeTsDataEvent) {
            const auto drained = w::decode_ts_data(decoded.value().payload);
            if (!drained || drained.value().stream_id != started.value().stream_id) return false;
        } else {
            return false;
        }
    }
    if (!stream_end) return false;

    protocol_failure_stage = "RELEASE";
    w::ReleaseRequest release;
    release.epoch = config().server.epoch;
    release.lease_id = acquired.value().lease_id;
    payload.fill(0U);
    size = w::encode_release_request(release, {payload.data(), payload.size()});
    request = request_frame(w::kOpcodeRelease, 13U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeRelease)) return false;
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto released = w::decode_release_response(decoded.value().payload);
    if (!released || !released.value().released) return false;

    protocol_failure_stage = "LIST";
    w::ListRequest list;
    list.epoch = config().server.epoch;
    payload.fill(0U);
    size = w::encode_list_request(list, {payload.data(), payload.size()});
    request = request_frame(w::kOpcodeList, 15U,
        {payload.data(), size ? size.value() : 0U});
    if (!size || !exchange_frame(io, *context, control, control_offset, request,
            response, w::kOpcodeList)) return false;
    decoded = w::decode_frame({response.data(), response.size()});
    if (!decoded || decoded.value().header.kind != w::Kind::response) return false;
    const auto closed = context->close(Timeout{5000U});
    return closed && closed.value() == CloseState::quiesced;
}

bool pending_admission_retries_through_poll_for_each_session()
{
    FakeListenerIoControl io;
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto now = std::make_shared<std::atomic<std::uint64_t>>(100U);
    auto tune_calls = std::make_shared<std::array<std::atomic<std::size_t>, 8U>>();
    for (auto& count : *tune_calls) count.store(0U, std::memory_order_relaxed);
    auto gate = std::make_shared<NonceGate>();
    auto context = make_context(shutdown, destroyed, multi_receiver_config(), now,
                                tune_calls, gate);
    if (!context) {
        set_fake_listener_io_control(nullptr);
        return false;
    }
    const auto first = io.connect(detail::ListenerKind::control);
    const auto second = io.connect(detail::ListenerKind::control);
    const auto blocker = io.connect(detail::ListenerKind::control);
    bool passed = static_cast<bool>(context->poll_once(Timeout{0U}));
    std::array<std::size_t, 3U> offsets{};
    std::vector<std::uint8_t> response;
    const auto hello = hello_frame();
    const std::vector<std::uint8_t> hello_request(hello.begin(), hello.end());
    passed = passed && exchange_frame(io, *context, first, offsets[0U], hello_request,
                                     response, w::kOpcodeHello) &&
        exchange_frame(io, *context, second, offsets[1U], hello_request,
                       response, w::kOpcodeHello) &&
        exchange_frame(io, *context, blocker, offsets[2U], hello_request,
                       response, w::kOpcodeHello);

    std::array<std::uint64_t, 2U> leases{};
    const std::array<detail::ConnectionToken, 2U> owners{{first, second}};
    const std::array<std::uint8_t, 2U> receivers{{2U, 3U}};
    for (std::size_t i = 0U; passed && i < owners.size(); ++i) {
        const auto request = acquire_request(2U, receivers[i]);
        passed = exchange_frame(io, *context, owners[i], offsets[i], request,
                                response, w::kOpcodeAcquire);
        w::Frame decoded{};
        if (passed) passed = response_frame(response, w::kOpcodeAcquire, decoded);
        if (passed) {
            const auto acquired = w::decode_acquire_response(decoded.payload);
            passed = static_cast<bool>(acquired);
            if (passed) leases[i] = acquired.value().lease_id;
        }
    }

    if (passed) {
        gate->arm();
        const auto request = acquire_request(2U, 4U);
        io.send(blocker, {request.data(), request.size()});
        passed = static_cast<bool>(context->poll_once(Timeout{0U})) && gate->wait_entered();
    }
    if (passed) {
        const auto first_tune = tune_request(3U, leases[0U], 1000U);
        const auto second_tune = tune_request(3U, leases[1U], 1000U);
        io.send(first, {first_tune.data(), first_tune.size()});
        io.send(second, {second_tune.data(), second_tune.size()});
        passed = static_cast<bool>(context->poll_once(Timeout{0U}));
    }
    gate->open();
    w::Frame first_result{};
    w::Frame second_result{};
    w::Frame blocker_result{};
    if (passed) passed = wait_frame(io, *context, first, offsets[0U], response,
                                    w::kOpcodeTune) &&
        response_frame(response, w::kOpcodeTune, first_result);
    if (passed) passed = wait_frame(io, *context, second, offsets[1U], response,
                                    w::kOpcodeTune) &&
        response_frame(response, w::kOpcodeTune, second_result);
    if (passed) passed = wait_frame(io, *context, blocker, offsets[2U], response,
                                    w::kOpcodeAcquire) &&
        response_frame(response, w::kOpcodeAcquire, blocker_result);
    if (passed) {
        const auto first_tuned = w::decode_tune_result(first_result.payload);
        const auto second_tuned = w::decode_tune_result(second_result.payload);
        const auto acquired = w::decode_acquire_response(blocker_result.payload);
        passed = first_tuned && second_tuned && acquired &&
            (*tune_calls)[2U].load(std::memory_order_relaxed) == 1U &&
            (*tune_calls)[3U].load(std::memory_order_relaxed) == 1U;
    }
    const auto closed = context->close(Timeout{5000U});
    passed = passed && closed && closed.value() == CloseState::quiesced &&
        shutdown->load(std::memory_order_acquire);
    context.reset();
    set_fake_listener_io_control(nullptr);
    return passed;
}

bool pending_admission_deadline_and_observed_close()
{
    FakeListenerIoControl io;
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto now = std::make_shared<std::atomic<std::uint64_t>>(100U);
    auto tune_calls = std::make_shared<std::array<std::atomic<std::size_t>, 8U>>();
    for (auto& count : *tune_calls) count.store(0U, std::memory_order_relaxed);
    auto gate = std::make_shared<NonceGate>();
    auto context = make_context(shutdown, destroyed, multi_receiver_config(), now,
                                tune_calls, gate);
    if (!context) {
        set_fake_listener_io_control(nullptr);
        return false;
    }
    const auto timed = io.connect(detail::ListenerKind::control);
    const auto closed_owner = io.connect(detail::ListenerKind::control);
    const auto blocker = io.connect(detail::ListenerKind::control);
    bool passed = static_cast<bool>(context->poll_once(Timeout{0U}));
    const char* failure_stage = "accept";
    std::array<std::size_t, 3U> offsets{};
    std::vector<std::uint8_t> response;
    const auto hello = hello_frame();
    const std::vector<std::uint8_t> hello_request(hello.begin(), hello.end());
    passed = passed && exchange_frame(io, *context, timed, offsets[0U], hello_request,
                                     response, w::kOpcodeHello) &&
        exchange_frame(io, *context, closed_owner, offsets[1U], hello_request,
                       response, w::kOpcodeHello) &&
        exchange_frame(io, *context, blocker, offsets[2U], hello_request,
                       response, w::kOpcodeHello);
    std::array<std::uint64_t, 2U> leases{};
    const std::array<detail::ConnectionToken, 2U> owners{{timed, closed_owner}};
    const std::array<std::uint8_t, 2U> receivers{{2U, 3U}};
    for (std::size_t i = 0U; passed && i < owners.size(); ++i) {
        const auto request = acquire_request(2U, receivers[i]);
        passed = exchange_frame(io, *context, owners[i], offsets[i], request,
                                response, w::kOpcodeAcquire);
        w::Frame decoded{};
        if (passed) passed = response_frame(response, w::kOpcodeAcquire, decoded);
        if (passed) {
            const auto acquired = w::decode_acquire_response(decoded.payload);
            passed = static_cast<bool>(acquired);
            if (passed) leases[i] = acquired.value().lease_id;
        }
    }
    if (passed) {
        failure_stage = "blocked acquire";
        gate->arm();
        const auto request = acquire_request(2U, 4U);
        io.send(blocker, {request.data(), request.size()});
        passed = static_cast<bool>(context->poll_once(Timeout{0U})) && gate->wait_entered();
    }
    if (passed) {
        failure_stage = "pending tunes";
        const auto timed_tune = tune_request(3U, leases[0U], 20U);
        const auto closing_tune = tune_request(3U, leases[1U], 1000U);
        io.send(timed, {timed_tune.data(), timed_tune.size()});
        io.send(closed_owner, {closing_tune.data(), closing_tune.size()});
        passed = static_cast<bool>(context->poll_once(Timeout{0U}));
    }
    if (passed) {
        failure_stage = "deadline wait cap";
        passed = static_cast<bool>(context->poll_once(Timeout{100U})) &&
            io.last_poll_timeout_ms() == 20U;
    }
    if (passed) {
        failure_stage = "deadline poll";
        now->store(120U, std::memory_order_release);
        const auto deadline_poll = context->poll_once(Timeout{100U});
        passed = static_cast<bool>(deadline_poll);
    }
    w::Frame timed_result{};
    if (passed) {
        failure_stage = "deadline result";
        passed = wait_frame(io, *context, timed, offsets[0U], response,
                            w::kOpcodeTune);
        const auto decoded = w::decode_frame({response.data(), response.size()});
        passed = passed && decoded && decoded.value().header.kind == w::Kind::error_response &&
            decoded.value().header.opcode == w::kOpcodeTune;
        if (passed) timed_result = decoded.value();
    }
    if (passed) {
        const auto error = w::decode_error_response(timed_result.payload);
        passed = error && error.value().error.code ==
            static_cast<std::uint32_t>(Error::TIMEOUT);
    }
    if (passed) {
        failure_stage = "observed close";
        gate->open();
        io.disconnect(closed_owner);
        const auto close_poll = context->poll_once(Timeout{0U});
        passed = static_cast<bool>(close_poll) &&
            io.closed(closed_owner);
    }
    gate->open();
    if (passed) {
        failure_stage = "close cleanup";
        passed = wait_frame(io, *context, blocker, offsets[2U], response,
                            w::kOpcodeAcquire) &&
            (*tune_calls)[2U].load(std::memory_order_relaxed) == 0U &&
            (*tune_calls)[3U].load(std::memory_order_relaxed) == 0U;
    }
    const auto closed = context->close(Timeout{5000U});
    passed = passed && closed && closed.value() == CloseState::quiesced &&
        shutdown->load(std::memory_order_acquire);
    context.reset();
    set_fake_listener_io_control(nullptr);
    if (!passed) std::fprintf(stderr, "pending admission failure at %s; poll timeout=%u\n",
        failure_stage, io.last_poll_timeout_ms());
    return passed;
}

bool close_handoff_and_pending_owner()
{
    FakeListenerIoControl io;
    io.block_unpublish();
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown);
    if (!context || !io.reserved() || !io.published()) return false;
    std::atomic<bool> polling{false};
    Result<void> poll_result = Result<void>::failure(Error::NOT_READY);
    std::thread poller([&] {
        polling.store(true, std::memory_order_release);
        poll_result = context->poll_once(Timeout{100U});
    });
    while (!polling.load(std::memory_order_acquire))
        std::this_thread::yield();
    const bool entered_poll = io.wait_poll();
    const auto first = context->close(Timeout{15000U});
    const bool entered_cleanup = io.wait_unpublish();
    const bool retained = io.reserved() && io.published();
    const auto second = context->close(Timeout{0U});
    io.release_unpublish();
    Result<CloseState> close_a = Result<CloseState>::failure(Error::NOT_READY);
    Result<CloseState> close_b = Result<CloseState>::failure(Error::NOT_READY);
    std::thread closer_a([&] { close_a = context->close(Timeout{5000U}); });
    std::thread closer_b([&] { close_b = context->close(Timeout{5000U}); });
    closer_a.join();
    closer_b.join();
    poller.join();
    const bool passed = entered_poll && entered_cleanup && retained && first &&
        first.value() == CloseState::pending && second &&
        second.value() == CloseState::pending && close_a && close_b &&
        close_a.value() == CloseState::quiesced &&
        close_b.value() == CloseState::quiesced && poll_result;
    if (!passed) std::fprintf(stderr,
        "close handoff values cleanup=%u retained=%u first=%u/%u second=%u/%u a=%u/%u b=%u/%u poll=%u/%u\n",
        entered_cleanup ? 1U : 0U, retained ? 1U : 0U,
        static_cast<bool>(first), static_cast<unsigned>(first.value()),
        static_cast<bool>(second), static_cast<unsigned>(second.value()),
        static_cast<bool>(close_a), static_cast<unsigned>(close_a.value()),
        static_cast<bool>(close_b), static_cast<unsigned>(close_b.value()),
        static_cast<bool>(poll_result), static_cast<unsigned>(poll_result.error()));
    context.reset();
    set_fake_listener_io_control(nullptr);
    return passed && shutdown->load(std::memory_order_acquire) &&
        !io.reserved() && !io.published();
}

bool slow_peer_isolation()
{
    FakeListenerIoControl io;
    set_fake_listener_io_control(&io);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown);
    if (!context) return false;
    const auto slow = io.connect(detail::ListenerKind::control);
    const auto fast = io.connect(detail::ListenerKind::control);
    if (!context->poll_once(Timeout{0U})) return false;
    io.block_writes(slow, true);
    const auto hello = hello_frame();
    const std::vector<std::uint8_t> hello_request(hello.begin(), hello.end());
    io.send(slow, {hello_request.data(), hello_request.size()});
    io.send(fast, {hello_request.data(), hello_request.size()});
    std::size_t slow_offset = 0U;
    std::size_t fast_offset = 0U;
    std::vector<std::uint8_t> response;
    const bool fast_progress = wait_frame(io, *context, fast, fast_offset,
                                           response, w::kOpcodeHello);
    const bool slow_held = io.output(slow).empty();
    const auto before = std::chrono::steady_clock::now();
    const auto idle_wait = context->poll_once(Timeout{40U});
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const bool no_write_spin = elapsed >= std::chrono::milliseconds(25);
    io.block_writes(slow, false);
    const bool slow_progress = wait_frame(io, *context, slow, slow_offset,
                                           response, w::kOpcodeHello);
    const auto closed = context->close(Timeout{5000U});
    if (!(fast_progress && slow_held && idle_wait && no_write_spin && slow_progress &&
          closed && closed.value() == CloseState::quiesced))
        std::fprintf(stderr, "slow peer values fast=%u held=%u idle=%u elapsed=%lld slow=%u close=%u/%u\n",
            fast_progress ? 1U : 0U, slow_held ? 1U : 0U,
            static_cast<bool>(idle_wait) ? 1U : 0U,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
            slow_progress ? 1U : 0U, static_cast<bool>(closed),
            static_cast<unsigned>(closed.value()));
    context.reset();
    set_fake_listener_io_control(nullptr);
    return fast_progress && slow_held && idle_wait && no_write_spin &&
        slow_progress && closed && closed.value() == CloseState::quiesced &&
        shutdown->load(std::memory_order_acquire);
}

bool capacity_and_buffers()
{
    FakeListenerIoControl control;
    set_fake_listener_io_control(&control);
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    auto context = make_context(shutdown);
    if (!context) return false;
    std::array<detail::ConnectionToken, 32U> control_connections{};
    std::array<detail::ConnectionToken, 33U> data_connections{};
    for (auto& connection : control_connections)
        connection = control.connect(detail::ListenerKind::control);
    for (std::size_t i = 0U; i < 31U; ++i)
        data_connections[i] = control.connect(detail::ListenerKind::data);
    if (!context->poll_once(Timeout{0U})) return false;
    if (control.accepted(detail::ListenerKind::control) != 32U ||
        control.accepted(detail::ListenerKind::data) != 31U) return false;
    const auto extra_control = control.connect(detail::ListenerKind::control);
    data_connections[31U] = control.connect(detail::ListenerKind::data);
    if (!context->poll_once(Timeout{0U})) return false;
    if (!control.closed(extra_control) || control.closed(data_connections[31U]) ||
        control.accepted(detail::ListenerKind::data) != 32U) return false;
    data_connections[32U] = control.connect(detail::ListenerKind::data);
    if (!context->poll_once(Timeout{0U})) return false;
    if (!control.closed(data_connections[32U]) ||
        host_listener_context_ts_buffer_high_water_for_test() != 67104000U)
        return false;
    const auto closed = context->close(Timeout{10000U});
    if (!closed || closed.value() != CloseState::quiesced ||
        host_listener_context_ts_buffer_current_for_test() != 0U) return false;
    context.reset();
    return true;
}

}  // namespace

int main()
{
    if (!lifecycle()) {
        std::fprintf(stderr, "FAIL: lifecycle\n");
        return 1;
    }
    if (!control_and_stream_protocol()) {
        std::fprintf(stderr, "FAIL: control_and_stream_protocol at %s\n",
                     protocol_failure_stage);
        return 1;
    }
    if (!pending_admission_retries_through_poll_for_each_session()) {
        std::fprintf(stderr, "FAIL: pending_admission_retries_through_poll_for_each_session\n");
        return 1;
    }
    if (!pending_admission_deadline_and_observed_close()) {
        std::fprintf(stderr, "FAIL: pending_admission_deadline_and_observed_close\n");
        return 1;
    }
    if (!close_handoff_and_pending_owner()) {
        std::fprintf(stderr, "FAIL: close_handoff_and_pending_owner\n");
        return 1;
    }
    if (!slow_peer_isolation()) {
        std::fprintf(stderr, "FAIL: slow_peer_isolation\n");
        return 1;
    }
    if (!capacity_and_buffers()) {
        std::fprintf(stderr, "FAIL: capacity_and_buffers\n");
        return 1;
    }
    if (!unpublish_error_still_quiesces_owner()) {
        std::fprintf(stderr, "FAIL: unpublish_error_still_quiesces_owner\n");
        return 1;
    }
    return 0;
}
