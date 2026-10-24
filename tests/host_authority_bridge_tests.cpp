// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/host_authority_bridge.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace px4::userland;
using namespace px4::userland::ipc::posix;

class Backend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        ++opens[receiver];
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (static_cast<int>(receiver) == gate_receiver) {
            gate_entered = true;
            gate_condition.notify_all();
            gate_condition.wait(lock, [this] { return gate_released; });
        }
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t, std::uint64_t, std::uint32_t) noexcept override
    {
        return Result<TunerHzProgramming>::success({});
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        TunerStrictProfile profile;
        if ((receiver == 2U || receiver == 3U) && system == ipc::System::ISDB_T) {
            profile.native_hz = true;
            profile.min_hz = 400000000U;
            profile.max_hz = 800000000U;
        }
        return profile;
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
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
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        ++closes[receiver];
        std::unique_lock<std::mutex> lock(close_mutex);
        if (static_cast<int>(receiver) == close_gate_receiver) {
            close_entered = true;
            close_condition.notify_all();
            close_condition.wait(lock, [this] { return close_released; });
        }
        return Result<void>::success();
    }
    Result<void> start_capture(std::uint8_t receiver, ipc::System) noexcept override
    {
        ++capture_starts;
        std::unique_lock<std::mutex> lock(capture_mutex);
        if (static_cast<int>(receiver) == capture_gate_receiver) {
            capture_entered = true;
            capture_condition.notify_all();
            capture_condition.wait(lock, [this] { return capture_released; });
        }
        return fail_capture_start && receiver == 2U
            ? Result<void>::failure(Error::UNSUPPORTED)
            : Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, ipc::System) noexcept override
    {
        ++capture_stops;
        return Result<void>::success();
    }
    std::array<unsigned, 8U> opens{};
    std::array<unsigned, 8U> closes{};
    unsigned capture_starts = 0U;
    unsigned capture_stops = 0U;
    bool fail_capture_start = false;
    void block_open(std::uint8_t receiver) noexcept
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        gate_receiver = receiver;
        gate_entered = false;
        gate_released = false;
    }
    bool wait_open() noexcept
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        return gate_condition.wait_for(lock, std::chrono::seconds(5),
                                       [this] { return gate_entered; });
    }
    void release_open() noexcept
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        gate_released = true;
        gate_condition.notify_all();
    }
    void block_close(std::uint8_t receiver) noexcept
    {
        std::lock_guard<std::mutex> lock(close_mutex);
        close_gate_receiver = receiver;
        close_entered = false;
        close_released = false;
    }
    bool wait_close() noexcept
    {
        std::unique_lock<std::mutex> lock(close_mutex);
        return close_condition.wait_for(lock, std::chrono::seconds(5),
                                        [this] { return close_entered; });
    }
    bool close_started() noexcept
    {
        std::lock_guard<std::mutex> lock(close_mutex);
        return close_entered;
    }
    void release_close() noexcept
    {
        std::lock_guard<std::mutex> lock(close_mutex);
        close_released = true;
        close_condition.notify_all();
    }
    void block_capture(std::uint8_t receiver) noexcept
    {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_gate_receiver = receiver;
        capture_entered = false;
        capture_released = false;
    }
    bool wait_capture() noexcept
    {
        std::unique_lock<std::mutex> lock(capture_mutex);
        return capture_condition.wait_for(lock, std::chrono::seconds(5),
                                          [this] { return capture_entered; });
    }
    void release_capture() noexcept
    {
        std::lock_guard<std::mutex> lock(capture_mutex);
        capture_released = true;
        capture_condition.notify_all();
    }
private:
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    int gate_receiver = -1;
    bool gate_entered = false;
    bool gate_released = false;
    std::mutex close_mutex;
    std::condition_variable close_condition;
    int close_gate_receiver = -1;
    bool close_entered = false;
    bool close_released = false;
    std::mutex capture_mutex;
    std::condition_variable capture_condition;
    int capture_gate_receiver = -1;
    bool capture_entered = false;
    bool capture_released = false;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success({{1U}});
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class Stream final : public TunerStreamControl {
public:
    explicit Stream(bool missing_final_when_unattached = false) noexcept
        : missing_final_when_unattached_(missing_final_when_unattached)
    {
    }
    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        identity = attachment;
        attached = true;
        packets_remaining = 2U;
        counters = TunerStreamCounters{};
        counters.packets = 2U;
        counters.bytes = 2U * 188U;
        final = TunerStreamFinalSnapshot{};
        final_released = false;
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!attached) {
            final.counters = counters;
            final.terminal = static_cast<std::uint8_t>(TunerStreamTerminal::stopped);
            return Result<void>::success();
        }
        if (!same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        attached = false;
        final.counters = counters;
        final.terminal = static_cast<std::uint8_t>(TunerStreamTerminal::stopped);
        return Result<void>::success();
    }
    Result<TunerStreamCounters> stats(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        return attached && same(attachment)
            ? Result<TunerStreamCounters>::success(counters)
            : Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!attached && missing_final_when_unattached_)
            return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
        return !attached || same(attachment)
            ? Result<TunerStreamFinalSnapshot>::success(final)
            : Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (attached && !same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        final_released = true;
        return Result<void>::success();
    }
    Result<TunerStreamReadResult> read(const TunerAttachment& attachment,
                                        MutableByteView output,
                                        Timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!same(attachment)) return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
        if (packets_remaining != 0U && output.data != nullptr && output.size >= 188U) {
            for (std::size_t i = 0U; i < 188U; ++i) output.data[i] = static_cast<std::uint8_t>(i);
            --packets_remaining;
            return Result<TunerStreamReadResult>::success({188U, false, false,
                TunerStreamTerminal::none});
        }
        return Result<TunerStreamReadResult>::success({0U, !attached, attached,
            attached ? TunerStreamTerminal::none : TunerStreamTerminal::stopped});
    }
    bool final_released = false;
private:
    mutable std::mutex mutex;
    bool same(const TunerAttachment& other) const noexcept
    {
        return identity.owner_client_id == other.owner_client_id &&
            identity.lease_id == other.lease_id &&
            identity.attachment_id == other.attachment_id &&
            identity.nonce == other.nonce;
    }
    TunerAttachment identity{};
    TunerStreamCounters counters{};
    TunerStreamFinalSnapshot final{};
    bool attached = false;
    unsigned packets_remaining = 0U;
    bool missing_final_when_unattached_ = false;
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

void mark_ingress_stopped(void* context) noexcept
{
    ++*static_cast<unsigned*>(context);
}

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
    for (std::size_t i = 0U; i < list.epoch.size(); ++i)
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    list.producer.product = text<32U>("bridge-test");
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
    list.receivers.value.count = 3U;
    for (std::size_t i = 0U; i < 3U; ++i) {
        auto& receiver = list.receivers.value.items[i];
        receiver.id = static_cast<std::uint16_t>(i + 2U);
        receiver.systems.mask = 1U;
        receiver.binding.present = true;
        receiver.binding.value.function_id = 1U;
        receiver.binding.value.local_id = static_cast<std::uint16_t>(i);
    }
    list.readers.present = true;
    return list;
}

w::Frame frame(std::uint16_t opcode, std::uint32_t id, ByteView payload)
{
    w::Frame value;
    value.header.kind = w::Kind::request;
    value.header.opcode = opcode;
    value.header.request_id = id;
    value.header.payload_length = static_cast<std::uint32_t>(payload.size);
    value.payload = payload;
    return value;
}

bool hello_unwritten(HostAuthorityBridge& bridge, std::uint8_t slot)
{
    std::array<std::uint8_t, w::kHelloRequestSize> payload{};
    const auto size = w::encode_hello_request({{1U, 0U}, {1U, 0U}, 0U, 0U},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeHello, 1U,
        {payload.data(), size.value()}), context, snapshot, 0U);
    return reply.action == w::InboundAction::respond;
}

bool hello(HostAuthorityBridge& bridge, std::uint8_t slot)
{
    return hello_unwritten(bridge, slot) &&
           bridge.response_sent(slot, true) == Error::OK;
}

enum class FakeSendResult : std::uint8_t { full, partial, would_block, error };

Error finish_fake_send(HostAuthorityBridge& bridge, std::uint8_t slot,
                       FakeSendResult result)
{
    return bridge.response_sent(slot, result == FakeSendResult::full);
}

bool acquire(HostAuthorityBridge& bridge, std::uint8_t slot, std::uint16_t receiver,
             std::uint64_t& lease, std::uint32_t request_id = 2U)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({make_list().epoch, receiver},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeAcquire, request_id,
        {payload.data(), size.value()}), context, snapshot, 0U);
    if (!reply.queued) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        const auto completed = bridge.take_response(slot);
        if (completed.action == w::InboundAction::respond) {
            if (completed.frame.size < w::kHeaderSize + w::kAcquireResponseSize)
                return false;
            const auto decoded = w::decode_acquire_response(
                {completed.frame.data + w::kHeaderSize, w::kAcquireResponseSize});
            if (!decoded || bridge.response_sent(slot, true) != Error::OK) return false;
            lease = decoded.value().lease_id;
            return lease != 0U;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool acquire_rejected(HostAuthorityBridge& bridge, std::uint8_t slot,
                      std::uint16_t receiver, Error expected,
                      std::uint32_t request_id = 2U)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({make_list().epoch, receiver},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    auto reply = bridge.accept(slot, frame(w::kOpcodeAcquire, request_id,
        {payload.data(), size.value()}), context, snapshot, 0U);
    const auto matches_error = [expected](const dtv::userland::detail::AuthorityReply& value) {
        const auto header = w::decode_header({value.frame.data, w::kHeaderSize});
        if (value.action != w::InboundAction::respond || !header ||
            header.value().kind != w::Kind::error_response) return false;
        const auto error = w::decode_error_response(
            {value.frame.data + w::kHeaderSize, header.value().payload_length});
        return error && error.value().error.code == static_cast<std::uint32_t>(expected);
    };
    if (reply.queued) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until) {
            (void)bridge.poll();
            reply = bridge.take_response(slot);
            if (reply.action == w::InboundAction::respond) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return matches_error(reply) && bridge.response_sent(slot, true) == Error::OK;
}

bool release(HostAuthorityBridge& bridge, std::uint8_t slot,
             const w::Uuid& epoch, std::uint64_t lease,
             std::uint32_t request_id = 4U)
{
    std::array<std::uint8_t, 64U> payload{};
    const auto size = w::encode_release_request({epoch, lease},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeRelease, request_id,
        {payload.data(), size.value()}), context, snapshot, 0U);
    if (!reply.queued) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        const auto completed = bridge.take_response(slot);
        if (completed.action == w::InboundAction::respond) {
            const auto header = w::decode_header({completed.frame.data, w::kHeaderSize});
            if (!header || header.value().kind != w::Kind::response ||
                bridge.response_sent(slot, true) != Error::OK) return false;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool tune(HostAuthorityBridge& bridge, std::uint8_t slot,
          const w::Uuid& epoch, std::uint64_t lease,
          std::uint32_t request_id = 3U)
{
    std::array<std::uint8_t, 128U> payload{};
    w::TuneRequest request;
    request.epoch = epoch;
    request.lease_id = lease;
    request.system = w::System::isdb_t;
    request.frequency_hz = 473142857U;
    request.timeout_ms = 100U;
    const auto size = w::encode_tune_request(request,
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    context.epoch = epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeTune, request_id,
        {payload.data(), size.value()}), context, snapshot, 0U);
    if (!reply.queued) {
        const auto header = w::decode_header({reply.frame.data, w::kHeaderSize});
        if (header) {
            const auto error = w::decode_error_response(
                {reply.frame.data + w::kHeaderSize, header.value().payload_length});
            if (error) std::cerr << "bridge tune dispatch error=" << error.value().error.code << '\n';
        }
        return false;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        const auto completed = bridge.take_response(slot);
        if (completed.action == w::InboundAction::respond) {
            const auto header = w::decode_header({completed.frame.data, w::kHeaderSize});
            if (!header || header.value().kind != w::Kind::response ||
                bridge.response_sent(slot, true) != Error::OK) return false;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool wait_free(HostAuthorityBridge& bridge, std::uint8_t slot)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        if (bridge.client_id(slot) == 0U)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto counts = bridge.counts();
    std::cerr << "wait-free counts " << counts.free << "/" << counts.active << "/"
              << counts.tombstone << "/" << counts.fatal_unresolved
              << " id=" << bridge.client_id(slot) << "\n";
    return false;
}

struct ActiveStreamFixture final {
    explicit ActiveStreamFixture(bool missing_final_when_unattached = false) noexcept
        : stream(missing_final_when_unattached)
    {
    }

    Backend backend;
    Nonce nonce;
    Time time;
    Stream stream;
    TunerService tuner{backend, nonce, time, nullptr, nullptr, &stream};
    CardBackend card_backend;
    CardSession card_session;
    CardService card{card_backend, card_session};
    std::unique_ptr<ControlWorkerLanes> workers{};
    std::unique_ptr<HostAuthorityBridge> bridge{};
    w::ListResponse list{make_list()};
    std::uint8_t control = 0U;
    std::uint64_t lease = 0U;
    std::uint64_t stream_id = 0U;
    dtv::userland::detail::ReceiverToken attach_token{};

    bool start(const ControlWorkerStartupOptions* options = nullptr)
    {
        if (!tuner.configure_strict_capabilities() ||
            !tuner.configure_stream_contract(
                dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL))
            return false;
        auto created = options == nullptr
            ? ControlWorkerLanes::create(card, tuner)
            : ControlWorkerLanes::create_for_test(card, tuner, *options);
        if (!created) return false;
        workers = std::move(created.value());
        const w::ServerHello server{list.epoch, list.producer, 0U};
        bridge = std::make_unique<HostAuthorityBridge>(tuner, *workers, server, list);
        if (bridge->configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK)
            return false;
        const auto opened = bridge->open_session();
        if (!opened || !hello(*bridge, opened.value())) return false;
        control = opened.value();
        if (!acquire(*bridge, control, 2U, lease) ||
            !tune(*bridge, control, list.epoch, lease)) return false;
        const auto status = tuner.engine_status();
        if (!status) return false;
        const auto started = bridge->start_stream(control, lease,
            status.value().tune_metadata[2U].current_revision, 100U, 5000U);
        if (!started) return false;
        stream_id = started.value().stream_id;
        attach_token = started.value().token;
        return stream_id != 0U;
    }

    bool attach(HostAuthorityDataConnection data)
    {
        w::AttachRequest request;
        request.epoch = list.epoch;
        request.lease_id = lease;
        request.stream_id = stream_id;
        request.attach_token = attach_token;
        if (bridge->prepare_data_attach(data, control, request, 101U) != Error::OK)
            return false;
        HostAuthorityAttachResult result{};
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result.ready && std::chrono::steady_clock::now() < until) {
            (void)bridge->poll();
            result = bridge->take_data_attach_result(data);
            if (!result.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return result.ready && result.error == Error::OK &&
               bridge->data_attach_response_sent(data, true) == Error::OK;
    }
};

bool test_attached_missing_final_stays_fatal()
{
    ActiveStreamFixture fixture(true);
    if (!fixture.start()) return false;
    const auto data = fixture.bridge->open_data_session();
    if (!data || !fixture.attach(data.value())) return false;
    const Error closed = fixture.bridge->close_data_session(data.value());
    if (closed != Error::OK && closed != Error::RESOURCE_LIMIT) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.bridge->counts().fatal_unresolved == 0U &&
           std::chrono::steady_clock::now() < until) {
        (void)fixture.bridge->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool rejected = fixture.bridge->counts().fatal_unresolved != 0U &&
        fixture.bridge->data_counts().fatal_unresolved == 1U;
    fixture.bridge->close_session_for_test(fixture.control);
    const auto stopped = fixture.workers->stop_and_join();
    return rejected && static_cast<bool>(stopped);
}

bool test_cleanup_requires_quiesced_terminal()
{
    ActiveStreamFixture fixture;
    if (!fixture.start()) return false;
    const auto data = fixture.bridge->open_data_session();
    if (!data || !fixture.attach(data.value())) return false;
    fixture.bridge->force_terminal_unquiesced_for_test(true);
    const Error closed = fixture.bridge->close_data_session(data.value());
    if (closed != Error::OK && closed != Error::RESOURCE_LIMIT) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.bridge->counts().fatal_unresolved == 0U &&
           std::chrono::steady_clock::now() < until) {
        (void)fixture.bridge->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto counts = fixture.bridge->data_counts();
    const bool passed = fixture.bridge->terminal_observed_for_test() &&
        fixture.bridge->forced_terminal_unquiesced_for_test() &&
        fixture.bridge->quiesced_guard_rejected_for_test() &&
        fixture.bridge->counts().fatal_unresolved == 1U &&
        counts.fatal_unresolved == 1U;
    if (!passed) {
        std::cerr << "M1 unexpected state fatal=" << fixture.bridge->counts().fatal_unresolved
                  << " data=" << counts.fatal_unresolved << "/" << counts.free
                  << " final-released=" << fixture.stream.final_released
                  << " terminal=" << fixture.bridge->terminal_observed_for_test()
                  << "/" << fixture.bridge->terminal_quiesced_for_test()
                  << " rejected=" << fixture.bridge->quiesced_guard_rejected_for_test()
                  << "\n";
    }
    fixture.bridge->close_session_for_test(fixture.control);
    return passed;
}

bool test_prepare_attach_rejects_old_generation()
{
    ControlWorkerStartupOptions options;
    options.fail_tuner_attach_submit = true;
    ActiveStreamFixture fixture;
    if (!fixture.start(&options)) return false;
    const auto old = fixture.bridge->open_data_session();
    if (!old || fixture.bridge->close_data_session(old.value()) != Error::OK) return false;
    const auto current = fixture.bridge->open_data_session();
    if (!current || current.value().slot != old.value().slot ||
        current.value().generation == old.value().generation) return false;
    w::AttachRequest request;
    request.epoch = fixture.list.epoch;
    request.lease_id = fixture.lease;
    request.stream_id = fixture.stream_id;
    request.attach_token = fixture.attach_token;
    const Error result = fixture.bridge->prepare_data_attach(
        old.value(), fixture.control, request, 101U);
    const bool rejected = result == Error::NOT_FOUND &&
        fixture.bridge->data_counts().active == 1U &&
        fixture.bridge->data_counts().tombstone == 0U;
    if (!rejected)
        std::cerr << "M8 stale generation was not rejected before attach reservation\n";
    const Error data_closed = fixture.bridge->close_data_session(current.value());
    const Error control_closed = fixture.bridge->close_session(fixture.control);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fixture.bridge->data_counts().free != kHostAuthoritySessionCapacity &&
           std::chrono::steady_clock::now() < until) {
        (void)fixture.bridge->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool freed = wait_free(*fixture.bridge, fixture.control);
    return rejected &&
        (data_closed == Error::OK || data_closed == Error::RESOURCE_LIMIT) &&
        control_closed == Error::OK && freed;
}

bool test_finish_control_close_waits_for_owned_stream()
{
    ActiveStreamFixture fixture;
    if (!fixture.start()) { std::cerr << "B1 setup\n"; return false; }
    const auto data = fixture.bridge->open_data_session();
    if (!data || !fixture.attach(data.value())) { std::cerr << "B1 attach\n"; return false; }
    if (fixture.bridge->close_session(fixture.control) != Error::OK) { std::cerr << "B1 close call\n"; return false; }
    if (!fixture.bridge->session_connected_for_test(fixture.control) ||
        !fixture.bridge->close_pending_for_test(fixture.control) ||
        fixture.bridge->data_counts().tombstone != 1U) {
        std::cerr << "B1 close disconnected before owned stream cleanup advanced\n";
        return false;
    }
    const bool freed = wait_free(*fixture.bridge, fixture.control);
    if (!freed || fixture.bridge->begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fixture.bridge->stopped() && std::chrono::steady_clock::now() < until) {
        (void)fixture.bridge->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return fixture.bridge->stopped() &&
           fixture.bridge->shutdown_result() == Error::OK;
}

bool test_accept_preserves_close_pending_before_poll()
{
    ActiveStreamFixture fixture;
    if (!fixture.start()) return false;
    const auto data = fixture.bridge->open_data_session();
    if (!data || !fixture.attach(data.value())) return false;
    w::Context context;
    w::Snapshot snapshot;
    auto bad_frame = frame(w::kOpcodeList, 11U, {});
    bad_frame.header.kind = w::Kind::response;
    const auto reply = fixture.bridge->accept(fixture.control, bad_frame,
                                               context, snapshot, 0U);
    if (reply.action != w::InboundAction::close ||
        !fixture.bridge->close_pending_before_accept_for_test() ||
        !fixture.bridge->accept_preserved_close_pending_for_test() ||
        !fixture.bridge->close_pending_for_test(fixture.control) ||
        fixture.bridge->data_counts().tombstone != 1U) {
        std::cerr << "N4 accept state action=" << static_cast<unsigned>(reply.action)
                  << " pending=" << fixture.bridge->close_pending_for_test(fixture.control)
                  << " connected="
                  << fixture.bridge->session_connected_for_test(fixture.control) << "\n";
        return false;
    }
    const Error data_closed = fixture.bridge->close_data_session(data.value());
    if (data_closed != Error::OK && data_closed != Error::RESOURCE_LIMIT &&
        data_closed != Error::NOT_FOUND) return false;
    const bool freed = wait_free(*fixture.bridge, fixture.control);
    fixture.bridge->close_session_for_test(fixture.control);
    return freed;
}

bool test_response_sent_retains_rejected_hello_disconnect()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.reject_next_authority_cleanup_submit = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    const w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot) return false;
    std::array<std::uint8_t, w::kHelloRequestSize> payload{};
    const auto size = w::encode_hello_request(
        {{1U, 0U}, {1U, 0U}, w::kFeatureCardService, 0U},
        {payload.data(), payload.size()});
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot.value(), frame(w::kOpcodeHello, 1U,
        {payload.data(), size.value()}), context, snapshot, 0U);
    const auto client = bridge.client_id(slot.value());
    if (!size || reply.action != w::InboundAction::respond ||
        !reply.close_after_reply ||
        bridge.response_sent(slot.value(), true) != Error::RESOURCE_LIMIT ||
        !bridge.retained_disconnect_for_test(slot.value(), client) ||
        bridge.session_connected_for_test(slot.value())) {
        std::cerr << "N5 rejected HELLO DISCONNECT value was not retained\n";
        return false;
    }
    if (!wait_free(bridge, slot.value())) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_disconnect_direct_observation()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) { std::cerr << "worker create\n"; return false; }
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);

    const auto legacy = tuner.acquire(31U, 2U);
    if (!legacy) { std::cerr << "legacy acquire\n"; return false; }
    const auto opened = bridge.open_session();
    if (!opened || !hello(bridge, opened.value())) { std::cerr << "first open/hello\n"; return false; }
    if ((bridge.client_id(opened.value()) & (std::uint64_t{1U} << 63U)) == 0U ||
        bridge.client_id(opened.value()) == 31U) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, opened.value(), 3U, lease)) { std::cerr << "first authority acquire\n"; return false; }
    auto engine = tuner.engine_status();
    if (!engine || !engine.value().leased[2U] || !engine.value().leased[3U]) { std::cerr << "both leases pre-close\n"; return false; }

    backend.block_close(3U);
    const Error close_error = bridge.close_session(opened.value());
    bool entered_close = false;
    const auto close_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered_close && std::chrono::steady_clock::now() < close_until) {
        (void)bridge.poll();
        entered_close = backend.close_started();
        if (!entered_close) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (close_error != Error::OK || bridge.counts().tombstone != 1U || !entered_close) {
        backend.release_close();
        std::cerr << "first close/gate\n";
        return false;
    }
    if (bridge.client_id(opened.value()) == 0U) {
        backend.release_close();
        std::cerr << "close action consumed session before cleanup completion\n";
        return false;
    }
    backend.release_close();
    if (!wait_free(bridge, opened.value())) { std::cerr << "first cleanup completion wait\n"; return false; }
    engine = tuner.engine_status();
    if (!engine || engine.value().leased[3U] || !engine.value().leased[2U] ||
        bridge.client_id(opened.value()) != 0U) { std::cerr << "first post-completion observation\n"; return false; }
    if (!tuner.disconnect_client(31U)) { std::cerr << "first legacy disconnect\n"; return false; }

    const auto reverse = bridge.open_session();
    if (!reverse || !hello(bridge, reverse.value())) { std::cerr << "reverse open/hello\n"; return false; }
    if (!acquire(bridge, reverse.value(), 2U, lease)) { std::cerr << "reverse authority acquire\n"; return false; }
    const auto other_legacy = tuner.acquire(32U, 3U);
    if (!other_legacy) { std::cerr << "second legacy acquire\n"; return false; }
    if (!tuner.disconnect_client(32U)) { std::cerr << "second legacy disconnect\n"; return false; }
    engine = tuner.engine_status();
    if (!engine || !engine.value().leased[2U] || engine.value().leased[3U]) { std::cerr << "reverse cross-domain isolation\n"; return false; }
    backend.block_close(2U);
    if (bridge.begin_shutdown() != Error::OK) { std::cerr << "reverse shutdown begin\n"; return false; }
    bool shutdown_close_entered = false;
    const auto shutdown_gate_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shutdown_close_entered && std::chrono::steady_clock::now() < shutdown_gate_until) {
        (void)bridge.poll();
        shutdown_close_entered = backend.close_started();
        if (!shutdown_close_entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!shutdown_close_entered || bridge.stopped() ||
        bridge.shutdown_result() != Error::NOT_READY) {
        backend.release_close();
        std::cerr << "shutdown finalized before cleanup gate\n";
        return false;
    }
    backend.release_close();
    const auto stop_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < stop_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine = tuner.engine_status();
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK || !engine ||
        engine.value().leased[2U] || engine.value().leased[3U] ||
        bridge.client_id(reverse.value()) != 0U) { std::cerr << "reverse shutdown observation\n"; return false; }
    return true;
}

bool test_capacity_and_send_failure()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) { std::cerr << "capacity worker create\n"; return false; }
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    std::array<std::uint8_t, kHostAuthoritySessionCapacity> sessions{};
    for (std::size_t i = 0U; i < sessions.size(); ++i) {
        const auto opened = bridge.open_session();
        if (!opened || opened.value() != i || !hello(bridge, opened.value())) {
            std::cerr << "capacity opening/hello at " << i << '\n';
            return false;
        }
        sessions[i] = opened.value();
    }
    const std::uint8_t a = sessions[0U];
    const std::uint8_t b = sessions[1U];
    if (bridge.open_session().error() != Error::RESOURCE_LIMIT ||
        bridge.client_id(a) < (std::uint64_t{1U} << 63U)) {
        std::cerr << "control capacity/Authority ID namespace\n";
        return false;
    }
    for (std::size_t i = 2U; i < sessions.size(); ++i) {
        if (bridge.close_session(sessions[i]) != Error::OK ||
            !wait_free(bridge, sessions[i])) {
            std::cerr << "capacity extra close/wait at " << i << '\n';
            return false;
        }
    }
    ControlWorkerTask legacy_task{};
    legacy_task.operation = ControlWorkerOperation::tuner_status;
    legacy_task.kind = ControlWorkerTaskKind::background;
    if (!created.value()->submit(ControlWorkerLane::tuner_dev1, legacy_task)) return false;
    ControlWorkerCompletion legacy_completion{};
    const auto legacy_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.take_legacy_completion(legacy_completion) &&
           std::chrono::steady_clock::now() < legacy_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (legacy_completion.operation != ControlWorkerOperation::tuner_status) {
        std::cerr << "shared worker completion was not preserved\n";
        return false;
    }
    const auto old_id = bridge.client_id(a);
    if (finish_fake_send(bridge, a, FakeSendResult::partial) != Error::OK) { std::cerr << "capacity partial send\n"; return false; }
    if (!wait_free(bridge, a)) { std::cerr << "capacity first wait free\n"; return false; }
    const auto reused = bridge.open_session();
    if (!reused || reused.value() != a || bridge.client_id(reused.value()) <= old_id ||
        !hello_unwritten(bridge, reused.value()) ||
        finish_fake_send(bridge, reused.value(), FakeSendResult::would_block) != Error::OK ||
        !wait_free(bridge, reused.value())) { std::cerr << "slot reuse/would-block send\n"; return false; }
    const auto error_slot = bridge.open_session();
    if (!error_slot || error_slot.value() != a ||
        !hello_unwritten(bridge, error_slot.value()) ||
        finish_fake_send(bridge, error_slot.value(), FakeSendResult::error) != Error::OK ||
        !wait_free(bridge, error_slot.value())) { std::cerr << "transport error send\n"; return false; }
    if (bridge.close_session(b) != Error::OK || !wait_free(bridge, b)) { std::cerr << "capacity second close/wait\n"; return false; }
    if (bridge.begin_shutdown() != Error::OK) { std::cerr << "capacity shutdown begin\n"; return false; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) { std::cerr << "capacity shutdown result\n"; return false; }
    return true;
}

bool test_paired_ingress_capacity_and_stop()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    unsigned control_stops = 0U;
    unsigned data_stops = 0U;
    if (bridge.set_control_ingress_stop(&mark_ingress_stopped, &control_stops) != Error::OK ||
        bridge.set_data_ingress_stop(&mark_ingress_stopped, &data_stops) != Error::OK)
        return false;

    std::array<HostAuthorityDataConnection, kHostAuthoritySessionCapacity> data{};
    for (std::size_t i = 0U; i < data.size(); ++i) {
        const auto opened = bridge.open_data_session();
        if (!opened || opened.value().slot != i || opened.value().generation == 0U)
            return false;
        data[i] = opened.value();
    }
    if (bridge.open_data_session().error() != Error::RESOURCE_LIMIT) return false;

    std::array<std::uint8_t, kHostAuthoritySessionCapacity> control{};
    for (std::size_t i = 0U; i < control.size(); ++i) {
        const auto opened = bridge.open_session();
        if (!opened || opened.value() != i) return false;
        control[i] = opened.value();
    }
    if (bridge.open_session().error() != Error::RESOURCE_LIMIT ||
        bridge.counts().active != kHostAuthoritySessionCapacity ||
        bridge.data_counts().active != kHostAuthoritySessionCapacity) return false;

    if (bridge.close_data_session(data[0U]) != Error::OK ||
        bridge.close_data_session(data[0U]) != Error::NOT_FOUND) return false;
    const auto reused = bridge.open_data_session();
    if (!reused || reused.value().slot != data[0U].slot ||
        reused.value().generation <= data[0U].generation ||
        bridge.close_data_session(data[0U]) != Error::NOT_FOUND) return false;
    data[0U] = reused.value();

    if (bridge.begin_shutdown() != Error::OK ||
        bridge.begin_shutdown() != Error::OK || control_stops != 1U || data_stops != 1U)
        return false;
    if (bridge.open_data_session().error() != Error::NOT_READY) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto counts = bridge.counts();
    const auto data_counts = bridge.data_counts();
    return bridge.stopped() && bridge.shutdown_result() == Error::OK &&
           counts.free == kHostAuthoritySessionCapacity && counts.active == 0U &&
           data_counts.free == kHostAuthoritySessionCapacity && data_counts.active == 0U &&
           control_stops == 1U && data_stops == 1U;
}

bool test_authority_stream_prepare_commit_stop()
{
    Backend backend;
    Nonce nonce;
    Time time;
    Stream stream;
    TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
    if (!tuner.configure_strict_capabilities()) return false;
    if (!tuner.configure_stream_contract(
            dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL))
        return false;
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.reject_next_authority_stream_cleanup_submit = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    if (bridge.configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK) return false;
    const auto control = bridge.open_session();
    const auto other_control = bridge.open_session();
    if (!control || !other_control || !hello(bridge, control.value()) ||
        !hello(bridge, other_control.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, control.value(), 2U, lease, 3U) ||
        !tune(bridge, control.value(), list.epoch, lease, 4U)) return false;
    std::uint64_t other_lease = 0U;
    if (!acquire(bridge, other_control.value(), 3U, other_lease) ||
        !tune(bridge, other_control.value(), list.epoch, other_lease)) return false;
    const auto status = tuner.engine_status();
    if (!status) return false;
    const std::uint64_t revision = status.value().tune_metadata[2U].current_revision;
    if (revision == 0U) return false;
    w::StartRequest start_request;
    start_request.epoch = list.epoch;
    start_request.lease_id = lease;
    start_request.tune_revision = revision;
    std::array<std::uint8_t, w::kMaxControlPayload> start_payload{};
    const auto start_size = w::encode_start_request(start_request,
        {start_payload.data(), start_payload.size()});
    if (!start_size) return false;
    w::Context start_context;
    start_context.epoch = list.epoch;
    start_context.state = w::ContextState::ready;
    w::Snapshot start_snapshot;
    const auto start_reply = bridge.accept(control.value(), frame(w::kOpcodeStartStream,
        7U, {start_payload.data(), start_size.value()}), start_context,
        start_snapshot, 100U);
    if (start_reply.action != w::InboundAction::respond ||
        bridge.response_sent(control.value(), true) != Error::OK) return false;
    const auto started = w::decode_start_response(
        {start_reply.frame.data + w::kHeaderSize,
         start_reply.frame.size - w::kHeaderSize});
    if (!started || started.value().stream_id == 0U) return false;
    const auto data = bridge.open_data_session();
    if (!data) return false;
    w::AttachRequest request;
    request.epoch = list.epoch;
    request.lease_id = lease;
    request.stream_id = started.value().stream_id;
    request.attach_token = started.value().attach_token;
    if (bridge.prepare_data_attach(data.value(), control.value(), request, 101U) !=
        Error::OK) return false;
    HostAuthorityAttachResult attached{};
    const auto attach_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!attached.ready && std::chrono::steady_clock::now() < attach_until) {
        (void)bridge.poll();
        attached = bridge.take_data_attach_result(data.value());
        if (!attached.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!attached.ready || attached.error != Error::OK ||
        attached.response.stream_id != started.value().stream_id) return false;
    const auto before_ack = tuner.engine_status();
    if (!before_ack || before_ack.value().states[2U] !=
        dtv::userland::detail::ReceiverState::tuned) return false;
    if (bridge.data_attach_response_sent(data.value(), true) != Error::OK) return false;
    const auto committed = tuner.engine_status();
    if (!committed || committed.value().states[2U] !=
        dtv::userland::detail::ReceiverState::streaming || backend.capture_starts != 1U)
        return false;
    std::array<std::uint8_t, 188U> packet{};
    const auto data_read = bridge.read_data(data.value(),
        {packet.data(), packet.size()}, Timeout{0U});
    if (data_read.error != Error::OK || data_read.eof || data_read.timed_out ||
        data_read.data.stream_id != started.value().stream_id ||
        data_read.data.sequence != 0U || data_read.data.packets.size != packet.size() ||
        packet[0U] != 0U || packet[187U] != 187U) return false;
    const auto send_stop = [&bridge](std::uint8_t slot, std::uint32_t request_id,
                                     const w::Uuid& epoch, std::uint64_t stop_lease,
                                     std::uint64_t stream_id) {
        w::StopRequest stop_request;
        stop_request.epoch = epoch;
        stop_request.lease_id = stop_lease;
        stop_request.stream_id = stream_id;
        std::array<std::uint8_t, w::kMaxControlPayload> payload{};
        const auto size = w::encode_stop_request(stop_request,
            {payload.data(), payload.size()});
        if (!size) return dtv::userland::detail::AuthorityReply{};
        w::Context context;
        context.epoch = epoch;
        context.state = w::ContextState::ready;
        w::Snapshot snapshot;
        return bridge.accept(slot, frame(w::kOpcodeStopStream, request_id,
            {payload.data(), size.value()}), context, snapshot, 150U);
    };
    const auto unhello = bridge.open_session();
    if (!unhello) return false;
    const auto unhello_stop = send_stop(unhello.value(), 1U, list.epoch, lease,
                                        started.value().stream_id);
    if (unhello_stop.action != w::InboundAction::close ||
        bridge.counts().fatal_unresolved != 0U || backend.capture_stops != 0U ||
        !wait_free(bridge, unhello.value())) return false;
    w::Uuid wrong_epoch = list.epoch;
    wrong_epoch[0U] ^= 0xffU;
    auto rejected = send_stop(control.value(), 8U, wrong_epoch, lease,
                              started.value().stream_id);
    auto rejected_header = w::decode_header({rejected.frame.data, w::kHeaderSize});
    if (rejected.action != w::InboundAction::respond || !rejected_header ||
        rejected_header.value().kind != w::Kind::error_response ||
        bridge.response_sent(control.value(), true) != Error::OK) return false;
    auto decoded_error = w::decode_error_response(
        {rejected.frame.data + w::kHeaderSize, rejected_header.value().payload_length});
    if (!decoded_error || decoded_error.value().error.code != w::kWireErrorStaleHandle ||
        bridge.data_counts().active != 1U || bridge.counts().fatal_unresolved != 0U) {
        std::cerr << "wrong epoch reply action=" << static_cast<unsigned>(rejected.action)
                  << " error=" << (decoded_error ? decoded_error.value().error.code : 0U)
                  << " active=" << bridge.data_counts().active
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }
    rejected = send_stop(control.value(), 9U, list.epoch, lease,
                         started.value().stream_id + 1U);
    rejected_header = w::decode_header({rejected.frame.data, w::kHeaderSize});
    if (rejected.action != w::InboundAction::respond || !rejected_header ||
        rejected_header.value().kind != w::Kind::error_response ||
        bridge.response_sent(control.value(), true) != Error::OK) return false;
    decoded_error = w::decode_error_response(
        {rejected.frame.data + w::kHeaderSize, rejected_header.value().payload_length});
    if (!decoded_error || decoded_error.value().error.code !=
            static_cast<std::uint32_t>(Error::NOT_FOUND) ||
        bridge.data_counts().active != 1U || bridge.counts().fatal_unresolved != 0U)
        return false;
    rejected = send_stop(control.value(), 10U, list.epoch, lease + 1U,
                         started.value().stream_id);
    rejected_header = w::decode_header({rejected.frame.data, w::kHeaderSize});
    if (rejected.action != w::InboundAction::respond || !rejected_header ||
        rejected_header.value().kind != w::Kind::error_response ||
        bridge.response_sent(control.value(), true) != Error::OK) return false;
    decoded_error = w::decode_error_response(
        {rejected.frame.data + w::kHeaderSize, rejected_header.value().payload_length});
    if (!decoded_error || decoded_error.value().error.code !=
            static_cast<std::uint32_t>(Error::NOT_FOUND) ||
        bridge.data_counts().active != 1U || bridge.counts().fatal_unresolved != 0U)
        return false;
    const auto stop_reply = send_stop(control.value(), 12U, list.epoch, lease,
                                      started.value().stream_id);
    const auto stop_header = w::decode_header({stop_reply.frame.data, w::kHeaderSize});
    const auto stop_response = w::decode_stop_response(
        {stop_reply.frame.data + w::kHeaderSize,
         stop_reply.frame.size - w::kHeaderSize});
    const auto pending_stop = tuner.authority_stop_stream_v1(
        bridge.client_id(control.value()), lease, started.value().stream_id);
    const auto queue_rejected = bridge.data_counts();
    if (stop_reply.action != w::InboundAction::respond || !stop_header ||
        stop_header.value().kind != w::Kind::response ||
        stop_header.value().request_id != 12U || !stop_response ||
        stop_response.value().epoch != list.epoch ||
        stop_response.value().final.id != started.value().stream_id ||
        stop_response.value().final.tune_revision != revision ||
        stop_response.value().quiesced || !stop_response.value().final.error.present ||
        stop_response.value().final.error.value.code !=
            static_cast<std::uint32_t>(Error::BUSY) ||
        !pending_stop || pending_stop.value().stream_id != started.value().stream_id ||
        pending_stop.value().quiesced || pending_stop.value().cleanup_owner ||
        queue_rejected.tombstone != 1U ||
        queue_rejected.fatal_unresolved != 0U || bridge.counts().fatal_unresolved != 0U ||
        backend.capture_stops != 0U ||
        bridge.response_sent(control.value(), true) != Error::OK) {
        std::cerr << "wire STOP queue rejection mismatch action="
                  << static_cast<unsigned>(stop_reply.action)
                  << " response=" << static_cast<bool>(stop_response)
                  << " quiesced=" << (stop_response && stop_response.value().quiesced)
                  << " id=" << (stop_response ? stop_response.value().final.id : 0U)
                  << " expected=" << started.value().stream_id
                  << " revision=" << (stop_response ? stop_response.value().final.tune_revision : 0U)
                  << " expected_revision=" << revision
                  << " err_present=" << (stop_response && stop_response.value().final.error.present)
                  << " err=" << (stop_response && stop_response.value().final.error.present
                        ? stop_response.value().final.error.value.code : 0U)
                  << " capstops=" << backend.capture_stops
                  << " tombstone=" << queue_rejected.tombstone
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }
    w::ListRequest list_request;
    list_request.epoch = list.epoch;
    std::array<std::uint8_t, w::kMaxControlPayload> list_payload{};
    const auto list_size = w::encode_list_request(list_request,
        {list_payload.data(), list_payload.size()});
    if (!list_size) return false;
    const auto list_reply = bridge.accept(control.value(), frame(w::kOpcodeList, 15U,
        {list_payload.data(), list_size.value()}), start_context, start_snapshot, 151U);
    const auto list_header = w::decode_header({list_reply.frame.data, w::kHeaderSize});
    const auto list_sent = bridge.response_sent(control.value(), true);
    if (list_reply.action != w::InboundAction::respond || !list_header ||
        list_header.value().kind != w::Kind::response ||
        list_header.value().request_id != 15U ||
        list_sent != Error::OK) {
        std::cerr << "LIST after STOP mismatch action="
                  << static_cast<unsigned>(list_reply.action)
                  << " header=" << static_cast<bool>(list_header)
                  << " kind=" << (list_header ? static_cast<unsigned>(list_header.value().kind) : 0U)
                  << " id=" << (list_header ? list_header.value().request_id : 0U)
                  << " sent=" << static_cast<unsigned>(list_sent) << "\n";
        return false;
    }
    const auto cleanup_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    HostAuthorityStreamEndResult stream_end{};
    bool drained_packet_seen = false;
    while (!stream_end.ready && std::chrono::steady_clock::now() < cleanup_until) {
        (void)bridge.poll();
        const auto draining = bridge.read_data(data.value(),
            {packet.data(), packet.size()}, Timeout{0U});
        if (draining.error == Error::OK && draining.data.packets.size != 0U) {
            if (draining.data.sequence != 1U) return false;
            drained_packet_seen = true;
        }
        stream_end = bridge.take_data_stream_end(data.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto final = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(control.value()), lease);
    const auto first_end_counts = bridge.data_counts();
    if (!stream_end.ready || !drained_packet_seen ||
        stream_end.event.final.id != started.value().stream_id ||
        stream_end.event.final.statistics.entries.items[0U].counter.measurement.value != 2U ||
        !final || !final.value().quiesced || backend.capture_stops != 1U ||
        first_end_counts.active != 0U || first_end_counts.tombstone != 1U) {
        std::cerr << "first STREAM_END mismatch ready=" << stream_end.ready
                  << " drained=" << drained_packet_seen
                  << " id=" << stream_end.event.final.id
                  << " expected_id=" << started.value().stream_id
                  << " packets=" << stream_end.event.final.statistics.entries.items[0U]
                         .counter.measurement.value
                  << " quiesced=" << (final && final.value().quiesced)
                  << " capture_stops=" << backend.capture_stops
                  << " counts=" << first_end_counts.active << "/"
                  << first_end_counts.tombstone << "\n";
        return false;
    }
    if (bridge.data_stream_end_sent(data.value(), true) != Error::OK ||
        bridge.data_counts().tombstone != 0U || !stream.final_released) return false;
    const auto other_start = bridge.start_stream(other_control.value(), other_lease,
                                                  revision, 160U, 5000U);
    if (!other_start) { std::cerr << "other typed START failed\n"; return false; }
    const auto other_stop = send_stop(other_control.value(), 12U, list.epoch,
                                      other_lease, other_start.value().stream_id);
    const auto other_stop_header = w::decode_header(
        {other_stop.frame.data, w::kHeaderSize});
    const auto other_stop_response = w::decode_stop_response(
        {other_stop.frame.data + w::kHeaderSize,
         other_stop.frame.size - w::kHeaderSize});
    const auto other_stopped = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(other_control.value()), other_lease);
    if (other_stop.action != w::InboundAction::respond || !other_stop_header ||
        other_stop_header.value().kind != w::Kind::response ||
        other_stop_header.value().request_id != 12U || !other_stop_response ||
        !other_stop_response.value().quiesced || !other_stopped ||
        other_stopped.value().stream_id != other_start.value().stream_id ||
        !other_stopped.value().quiesced ||
        bridge.response_sent(other_control.value(), true) != Error::OK) {
        std::cerr << "other wire STOP response mismatch action="
                  << static_cast<unsigned>(other_stop.action)
                  << " header=" << static_cast<bool>(other_stop_header)
                  << " decoded=" << static_cast<bool>(other_stop_response)
                  << " quiesced=" << (other_stop_response && other_stop_response.value().quiesced)
                  << " terminal=" << static_cast<bool>(other_stopped) << "\n";
        return false;
    }
    if (other_stopped.value().stream_id != other_start.value().stream_id ||
        !other_stopped.value().quiesced || backend.capture_stops != 1U) {
        std::cerr << "duplicate STOP precondition stream=" << other_stopped.value().stream_id
                  << " expected=" << other_start.value().stream_id
                  << " quiesced=" << other_stopped.value().quiesced
                  << " stops=" << backend.capture_stops << "\n";
        return false;
    }
    const auto duplicate_other_stop = send_stop(other_control.value(), 12U, list.epoch,
                                      other_lease, other_start.value().stream_id);
    const bool other_free = wait_free(bridge, other_control.value());
    if (duplicate_other_stop.action != w::InboundAction::close || !other_free ||
        bridge.counts().fatal_unresolved != 0U) {
        std::cerr << "duplicate STOP was not rejected or released action="
                  << static_cast<unsigned>(duplicate_other_stop.action)
                  << " free=" << other_free
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }
    const auto pending_control = bridge.open_session();
    if (!pending_control || !hello(bridge, pending_control.value())) return false;
    backend.block_open(3U);
    std::array<std::uint8_t, w::kAcquireRequestSize> pending_payload{};
    const auto pending_size = w::encode_acquire_request({list.epoch, 3U},
        {pending_payload.data(), pending_payload.size()});
    if (!pending_size) return false;
    const auto pending_acquire = bridge.accept(pending_control.value(),
        frame(w::kOpcodeAcquire, 2U, {pending_payload.data(), pending_size.value()}),
        start_context, start_snapshot, 170U);
    if (!pending_acquire.queued || !backend.wait_open()) {
        backend.release_open();
        return false;
    }
    const auto stop_while_pending = send_stop(pending_control.value(), 3U,
        list.epoch, 1U, 1U);
    if (stop_while_pending.action != w::InboundAction::close ||
        bridge.counts().fatal_unresolved != 0U) {
        backend.release_open();
        return false;
    }
    w::ListRequest progress_request;
    progress_request.epoch = list.epoch;
    std::array<std::uint8_t, w::kMaxControlPayload> progress_payload{};
    const auto progress_size = w::encode_list_request(progress_request,
        {progress_payload.data(), progress_payload.size()});
    if (!progress_size) {
        backend.release_open();
        return false;
    }
    const auto progress = bridge.accept(control.value(), frame(w::kOpcodeList, 16U,
        {progress_payload.data(), progress_size.value()}), start_context,
        start_snapshot, 171U);
    const auto progress_header = w::decode_header({progress.frame.data, w::kHeaderSize});
    const auto progress_sent = bridge.response_sent(control.value(), true);
    if (progress.action != w::InboundAction::respond || !progress_header ||
        progress_header.value().kind != w::Kind::response ||
        progress_header.value().request_id != 16U || progress_sent != Error::OK) {
        backend.release_open();
        return false;
    }
    backend.release_open();
    if (!wait_free(bridge, pending_control.value())) return false;
    const auto older_control = bridge.open_session();
    if (!older_control || !hello(bridge, older_control.value())) return false;
    std::uint64_t older_lease = 0U;
    if (!acquire(bridge, older_control.value(), 3U, older_lease) ||
        !tune(bridge, older_control.value(), list.epoch, older_lease)) return false;
    const auto older_status = tuner.engine_status();
    if (!older_status) return false;
    const auto older_start = bridge.start_stream(older_control.value(), older_lease,
        older_status.value().tune_metadata[3U].current_revision, 172U, 5000U);
    if (!older_start) return false;
    const auto accepted_older_stream = send_stop(older_control.value(), 12U,
        list.epoch, older_lease, older_start.value().stream_id);
    if (accepted_older_stream.action != w::InboundAction::respond ||
        bridge.response_sent(older_control.value(), true) != Error::OK) return false;
    const auto before_older_stop = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(older_control.value()), older_lease);
    if (!before_older_stop || before_older_stop.value().stream_id != older_start.value().stream_id ||
        !before_older_stop.value().quiesced || backend.capture_stops != 1U) return false;
    const auto older_stop = send_stop(older_control.value(), 11U, list.epoch,
        older_lease, older_start.value().stream_id);
    if (older_stop.action != w::InboundAction::close ||
        !wait_free(bridge, older_control.value()) ||
        bridge.counts().fatal_unresolved != 0U) return false;
    const auto partial_control = bridge.open_session();
    if (!partial_control || !hello(bridge, partial_control.value())) return false;
    std::uint64_t partial_lease = 0U;
    if (!acquire(bridge, partial_control.value(), 3U, partial_lease) ||
        !tune(bridge, partial_control.value(), list.epoch, partial_lease)) return false;
    const auto partial_status = tuner.engine_status();
    if (!partial_status) return false;
    const auto partial_start = bridge.start_stream(partial_control.value(), partial_lease,
        partial_status.value().tune_metadata[3U].current_revision, 173U, 5000U);
    if (!partial_start) return false;
    const auto partial_stop = send_stop(partial_control.value(), 12U, list.epoch,
        partial_lease, partial_start.value().stream_id);
    const auto partial_terminal = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(partial_control.value()), partial_lease);
    if (partial_stop.action != w::InboundAction::respond ||
        !partial_terminal || partial_terminal.value().stream_id != partial_start.value().stream_id ||
        !partial_terminal.value().quiesced ||
        bridge.response_sent(partial_control.value(), false) != Error::OK ||
        !wait_free(bridge, partial_control.value())) return false;
    if (backend.capture_stops != 1U) return false;
    const auto second_start = bridge.start_stream(control.value(), lease, revision,
                                                   200U, 5000U);
    const auto second_data = bridge.open_data_session();
    if (!second_start || !second_data) return false;
    request.stream_id = second_start.value().stream_id;
    request.attach_token = second_start.value().token;
    if (bridge.prepare_data_attach(second_data.value(), control.value(), request, 201U) !=
        Error::OK) return false;
    attached = HostAuthorityAttachResult{};
    const auto second_attach_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!attached.ready && std::chrono::steady_clock::now() < second_attach_until) {
        (void)bridge.poll();
        attached = bridge.take_data_attach_result(second_data.value());
        if (!attached.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!attached.ready || !attached.response.stream_id) return false;
    const auto uncommitted = tuner.engine_status();
    if (!uncommitted || uncommitted.value().states[2U] !=
        dtv::userland::detail::ReceiverState::tuned ||
        bridge.data_attach_response_sent(second_data.value(), false) != Error::OK)
        return false;
    const auto second_cleanup_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    stream_end = HostAuthorityStreamEndResult{};
    while (!stream_end.ready && std::chrono::steady_clock::now() < second_cleanup_until) {
        (void)bridge.poll();
        (void)bridge.read_data(second_data.value(), {packet.data(), packet.size()}, Timeout{0U});
        stream_end = bridge.take_data_stream_end(second_data.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto aborted = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(control.value()), lease);
    if (!stream_end.ready || !aborted || !aborted.value().quiesced ||
        backend.capture_starts != 2U || backend.capture_stops != 2U ||
        bridge.data_stream_end_sent(second_data.value(), true) != Error::OK) return false;
    const auto third_start = bridge.start_stream(control.value(), lease, revision,
                                                  300U, 5000U);
    const auto third_data = bridge.open_data_session();
    if (!third_start || !third_data) {
        std::cerr << "third stream start/data failed\n";
        return false;
    }
    request.stream_id = third_start.value().stream_id;
    request.attach_token = third_start.value().token;
    backend.block_capture(2U);
    if (bridge.prepare_data_attach(third_data.value(), control.value(), request, 301U) !=
        Error::OK || !backend.wait_capture()) {
        std::cerr << "third stream prepare/wait failed\n";
        backend.release_capture();
        return false;
    }
    const auto racing_stop = bridge.stop_stream(control.value(), lease,
                                                 third_start.value().stream_id);
    if (!racing_stop || bridge.data_counts().tombstone != 1U) {
        std::cerr << "third stream racing STOP failed\n";
        backend.release_capture();
        return false;
    }
    backend.release_capture();
    HostAuthorityAttachResult rejected_attach{};
    const auto race_cleanup_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!rejected_attach.ready && std::chrono::steady_clock::now() < race_cleanup_until) {
        (void)bridge.poll();
        rejected_attach = bridge.take_data_attach_result(third_data.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!rejected_attach.ready || rejected_attach.error != Error::NOT_FOUND ||
        rejected_attach.response.stream_id != third_start.value().stream_id) {
        std::cerr << "third attach completion ready=" << rejected_attach.ready
                  << " error=" << static_cast<unsigned>(rejected_attach.error)
                  << " stream=" << rejected_attach.response.stream_id << " expected="
                  << third_start.value().stream_id << "\n";
        return false;
    }
    const Error third_closed = bridge.close_data_session(third_data.value());
    if (third_closed != Error::OK && third_closed != Error::RESOURCE_LIMIT) return false;
    const auto recycled_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bridge.data_counts().free != kHostAuthoritySessionCapacity &&
           std::chrono::steady_clock::now() < recycled_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto raced_final = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(control.value()), lease);
    if (!raced_final || !raced_final.value().quiesced ||
        bridge.data_counts().free != kHostAuthoritySessionCapacity ||
        bridge.data_counts().tombstone != 0U ||
        backend.capture_starts != 3U || backend.capture_stops != 3U ||
        bridge.counts().fatal_unresolved != 0U) return false;
    const auto before_duplicate = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(control.value()), lease);
    if (!before_duplicate ||
        before_duplicate.value().stream_id != third_start.value().stream_id ||
        !before_duplicate.value().quiesced || backend.capture_stops != 3U ||
        bridge.counts().fatal_unresolved != 0U) {
        std::cerr << "duplicate STOP precondition final=" << static_cast<bool>(before_duplicate)
                  << " id=" << (before_duplicate ? before_duplicate.value().stream_id : 0U)
                  << " expected=" << third_start.value().stream_id
                  << " quiesced=" << (before_duplicate && before_duplicate.value().quiesced)
                  << " stops=" << backend.capture_stops
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }
    const auto duplicate_stop = send_stop(control.value(), 12U, list.epoch, lease,
                                          started.value().stream_id);
    if (duplicate_stop.action != w::InboundAction::close ||
        !wait_free(bridge, control.value()) || bridge.counts().fatal_unresolved != 0U) {
        std::cerr << "duplicate STOP was not rejected or released action="
                  << static_cast<unsigned>(duplicate_stop.action)
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }
    if (bridge.begin_shutdown() != Error::OK) {
        std::cerr << "third stream begin shutdown failed\n";
        return false;
    }
    const auto stop_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < stop_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stopped = bridge.stopped() && bridge.shutdown_result() == Error::OK;
    if (!stopped) std::cerr << "third stream shutdown failed stopped="
                            << bridge.stopped() << " result="
                            << static_cast<unsigned>(bridge.shutdown_result()) << " fatal="
                            << bridge.counts().fatal_unresolved << "\n";
    return stopped;
}

bool failed_attach_cleanup_case(bool missing_final_when_unattached,
                                bool force_unquiesced = false)
{
    Backend backend;
    backend.fail_capture_start = true;
    Nonce nonce;
    Time time;
    Stream stream(missing_final_when_unattached);
    TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
    if (!tuner.configure_strict_capabilities() ||
        !tuner.configure_stream_contract(
            dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL)) {
        std::cerr << "failed ATTACH fixture stream configuration\n";
        return false;
    }
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    const char* worker_runtime_directory = "/tmp";
#if defined(_WIN32)
    worker_runtime_directory = std::getenv("LOCALAPPDATA");
    if (worker_runtime_directory == nullptr || worker_runtime_directory[0] == '\0') {
        std::cerr << "failed ATTACH fixture LOCALAPPDATA is not set\n";
        return false;
    }
#endif
    auto created = ControlWorkerLanes::create(card, tuner, worker_runtime_directory);
    if (!created) {
        std::cerr << "failed ATTACH fixture worker create error="
                  << static_cast<unsigned>(created.error()) << "\n";
        return false;
    }
    const auto list = make_list();
    const w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    if (bridge.configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK) {
        std::cerr << "failed ATTACH fixture bridge stream configuration\n";
        return false;
    }
    const auto control = bridge.open_session();
    if (!control || !hello(bridge, control.value())) {
        std::cerr << "failed ATTACH fixture control HELLO\n";
        return false;
    }
    std::uint8_t other_control_slot =
        static_cast<std::uint8_t>(kHostAuthoritySessionCapacity);
    const char* failed_stage = "other control open";
    struct Cleanup final {
        HostAuthorityBridge& bridge;
        ControlWorkerLanes& workers;
        std::uint8_t control;
        std::uint8_t& other;
        const char*& failed_stage;
        bool complete = false;
        ~Cleanup()
        {
            if (complete) return;
            std::cerr << "failed ATTACH case stage=" << failed_stage << "\n";
            bridge.close_session_for_test(control);
            if (other < kHostAuthoritySessionCapacity)
                bridge.close_session_for_test(other);
            (void)workers.stop_and_join();
        }
    } cleanup{bridge, *created.value(), control.value(), other_control_slot,
              failed_stage};
    const auto other_control = bridge.open_session();
    if (!other_control || !hello(bridge, other_control.value())) return false;
    other_control_slot = other_control.value();
    std::uint64_t lease = 0U;
    failed_stage = "acquire/tune/status";
    if (!acquire(bridge, control.value(), 2U, lease, 3U) ||
        !tune(bridge, control.value(), list.epoch, lease, 4U)) {
        std::cerr << "failed ATTACH fixture acquire/tune\n";
        return false;
    }
    const auto status = tuner.engine_status();
    if (!status || status.value().tune_metadata[2U].current_revision == 0U) {
        std::cerr << "failed ATTACH fixture tune status\n";
        return false;
    }
    w::StartRequest start;
    start.epoch = list.epoch;
    start.lease_id = lease;
    start.tune_revision = status.value().tune_metadata[2U].current_revision;
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    const auto size = w::encode_start_request(start, {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    context.epoch = list.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    failed_stage = "START";
    const auto started = bridge.accept(control.value(), frame(w::kOpcodeStartStream, 7U,
        {payload.data(), size.value()}), context, snapshot, 100U);
    if (started.action != w::InboundAction::respond ||
        bridge.response_sent(control.value(), true) != Error::OK) {
        std::cerr << "failed ATTACH fixture start action="
                  << static_cast<unsigned>(started.action) << "\n";
        return false;
    }
    const auto start_response = w::decode_start_response(
        {started.frame.data + w::kHeaderSize, started.frame.size - w::kHeaderSize});
    if (!start_response) {
        std::cerr << "failed ATTACH fixture START response decode\n";
        return false;
    }
    failed_stage = "data open/prepare";
    const auto data = bridge.open_data_session();
    if (!data) {
        std::cerr << "failed ATTACH fixture data open error="
                  << static_cast<unsigned>(data.error()) << "\n";
        return false;
    }
    const auto other_data = bridge.open_data_session();
    if (!other_data) return false;
    w::AttachRequest attach;
    attach.epoch = list.epoch;
    attach.lease_id = lease;
    attach.stream_id = start_response.value().stream_id;
    attach.attach_token = start_response.value().attach_token;
    const Error prepared = bridge.prepare_data_attach(data.value(), control.value(), attach, 101U);
    if (prepared != Error::OK) {
        std::cerr << "failed ATTACH fixture prepare error="
                  << static_cast<unsigned>(prepared) << "\n";
        return false;
    }
    if (force_unquiesced) bridge.force_terminal_unquiesced_for_test(true);
    HostAuthorityAttachResult result{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result.ready && std::chrono::steady_clock::now() < deadline) {
        (void)bridge.poll();
        result = bridge.take_data_attach_result(data.value());
        if (!result.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!result.ready || result.error != Error::UNSUPPORTED ||
        result.response.stream_id != start_response.value().stream_id) {
        std::cerr << "failed ATTACH result ready=" << result.ready
                  << " error=" << static_cast<unsigned>(result.error)
                  << " stream=" << result.response.stream_id
                  << " expected=" << start_response.value().stream_id
                  << " capture_starts=" << backend.capture_starts << "\n";
        return false;
    }
    const Error data_closed = bridge.close_data_session(data.value());
    if (data_closed != Error::OK && data_closed != Error::RESOURCE_LIMIT) return false;
    if (force_unquiesced) {
        failed_stage = "expected nonquiesced rejection";
        const auto fatal_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (bridge.counts().fatal_unresolved == 0U &&
               std::chrono::steady_clock::now() < fatal_until) {
            (void)bridge.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool rejected = bridge.counts().fatal_unresolved != 0U &&
            bridge.data_counts().fatal_unresolved == 1U &&
            bridge.forced_terminal_unquiesced_for_test() &&
            bridge.quiesced_guard_rejected_for_test();
        if (!rejected)
            std::cerr << "failed ATTACH nonquiesced guard state fatal="
                      << bridge.counts().fatal_unresolved << " data="
                      << bridge.data_counts().free << "/"
                      << bridge.data_counts().active << "/"
                      << bridge.data_counts().tombstone << "/"
                      << bridge.data_counts().fatal_unresolved << " forced="
                      << bridge.forced_terminal_unquiesced_for_test() << " rejected="
                      << bridge.quiesced_guard_rejected_for_test() << "\n";
        bridge.close_session_for_test(control.value());
        bridge.close_session_for_test(other_control.value());
        cleanup.complete = static_cast<bool>(created.value()->stop_and_join());
        return rejected && cleanup.complete;
    }
    failed_stage = "data slot free";
    const auto data_free_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bridge.data_counts().free != kHostAuthoritySessionCapacity - 1U &&
           std::chrono::steady_clock::now() < data_free_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto counts = bridge.data_counts();
    if (counts.free != kHostAuthoritySessionCapacity - 1U ||
        counts.tombstone != 0U || counts.fatal_unresolved != 0U ||
        bridge.counts().fatal_unresolved != 0U ||
        !bridge.stream_owned_for_test(control.value(), start_response.value().stream_id)) {
        std::cerr << "failed ATTACH cleanup did not release data slot final_missing="
                  << missing_final_when_unattached << " counts=" << counts.free << "/"
                  << counts.active << "/" << counts.tombstone << "/"
                  << counts.fatal_unresolved << " authority_fatal="
                  << bridge.counts().fatal_unresolved << "\n";
        return false;
    }

    failed_stage = "slot reuse and other control list";
    const auto reused = bridge.open_data_session();
    if (!reused || reused.value().slot != data.value().slot ||
        reused.value().generation == data.value().generation) return false;
    w::ListRequest list_request;
    list_request.epoch = list.epoch;
    std::array<std::uint8_t, w::kEpochSize> list_payload{};
    const auto list_size = w::encode_list_request(list_request,
        {list_payload.data(), list_payload.size()});
    if (!list_size) return false;
    context.state = w::ContextState::ready;
    context.epoch = list.epoch;
    const auto listed = bridge.accept(other_control.value(),
        frame(w::kOpcodeList, 2U, {list_payload.data(), list_size.value()}),
        context, snapshot, 102U);
    const auto list_header = w::decode_header({listed.frame.data, w::kHeaderSize});
    if (listed.action != w::InboundAction::respond || !list_header ||
        list_header.value().kind != w::Kind::response ||
        list_header.value().opcode != w::kOpcodeList ||
        bridge.response_sent(other_control.value(), true) != Error::OK ||
        bridge.counts().fatal_unresolved != 0U) return false;
    if (bridge.close_data_session(reused.value()) != Error::OK ||
        bridge.close_data_session(other_data.value()) != Error::OK) return false;
    const auto all_data_free_until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bridge.data_counts().free != kHostAuthoritySessionCapacity &&
           std::chrono::steady_clock::now() < all_data_free_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    counts = bridge.data_counts();
    if (counts.free != kHostAuthoritySessionCapacity || counts.tombstone != 0U ||
        counts.fatal_unresolved != 0U) return false;
    failed_stage = "normal owner disconnect after quiesced failed ATTACH";
    if (bridge.close_session(control.value()) != Error::OK) return false;
    const auto owner_closed_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (bridge.client_id(control.value()) != 0U &&
           bridge.counts().fatal_unresolved == 0U &&
           std::chrono::steady_clock::now() < owner_closed_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto owner_status = tuner.engine_status();
    if (!owner_status || bridge.client_id(control.value()) != 0U ||
        bridge.counts().fatal_unresolved != 0U ||
        !owner_status.value().leased[2U]) {
        std::cerr << "failed ATTACH disconnect fatal="
                  << bridge.counts().fatal_unresolved << " owner="
                  << bridge.client_id(control.value()) << " leased="
                  << (owner_status && owner_status.value().leased[2U]) << "\n";
        return false;
    }
    failed_stage = "surviving control LIST";
    const auto listed_after_close = bridge.accept(other_control.value(),
        frame(w::kOpcodeList, 3U, {list_payload.data(), list_size.value()}),
        context, snapshot, 103U);
    const auto listed_after_close_header = w::decode_header(
        {listed_after_close.frame.data, w::kHeaderSize});
    if (listed_after_close.action != w::InboundAction::respond ||
        !listed_after_close_header ||
        listed_after_close_header.value().kind != w::Kind::response ||
        listed_after_close_header.value().opcode != w::kOpcodeList ||
        bridge.response_sent(other_control.value(), true) != Error::OK ||
        bridge.counts().fatal_unresolved != 0U) return false;

    failed_stage = "new client receiver quarantine";
    const auto new_control = bridge.open_session();
    if (!new_control || !hello(bridge, new_control.value()) ||
        !acquire_rejected(bridge, new_control.value(), 2U, Error::BUSY)) return false;
    std::uint64_t new_lease = 0U;
    if (!acquire(bridge, new_control.value(), 3U, new_lease, 3U) ||
        !tune(bridge, new_control.value(), list.epoch, new_lease, 4U)) return false;
    const auto new_status = tuner.engine_status();
    if (!new_status || !new_status.value().leased[2U] ||
        !new_status.value().leased[3U] ||
        bridge.counts().fatal_unresolved != 0U) return false;

    failed_stage = "other receiver release";
    if (!release(bridge, new_control.value(), list.epoch, new_lease, 5U) ||
        bridge.counts().fatal_unresolved != 0U) return false;
    const auto released_status = tuner.engine_status();
    if (!released_status || !released_status.value().leased[2U] ||
        released_status.value().leased[3U]) {
        return false;
    }

    failed_stage = "remaining sessions close and shutdown";
    if (bridge.close_session(new_control.value()) != Error::OK ||
        bridge.close_session(other_control.value()) != Error::OK) return false;
    const auto sessions_closed_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while ((bridge.client_id(new_control.value()) != 0U ||
            bridge.client_id(other_control.value()) != 0U) &&
           bridge.counts().fatal_unresolved == 0U &&
           std::chrono::steady_clock::now() < sessions_closed_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (bridge.counts().fatal_unresolved != 0U ||
        bridge.begin_shutdown() != Error::OK) return false;
    const auto stopped_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < stopped_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK ||
        bridge.counts().fatal_unresolved != 0U) return false;
    cleanup.complete = static_cast<bool>(created.value()->stop_and_join());
    return cleanup.complete;
}

bool test_failed_attach_prepare_is_reported_to_data_owner()
{
    const bool final_available = failed_attach_cleanup_case(false);
    const bool final_missing = failed_attach_cleanup_case(true);
    const bool nonquiesced = failed_attach_cleanup_case(true, true);
    const bool attached_final_missing = test_attached_missing_final_stays_fatal();
    return final_available && final_missing && nonquiesced && attached_final_missing;
}

bool test_rejected_start_does_not_record_stream()
{
    Backend backend;
    Nonce nonce;
    Time time;
    Stream stream;
    TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
    if (!tuner.configure_strict_capabilities() ||
        !tuner.configure_stream_contract(
            dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL))
        return false;
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    if (bridge.configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK)
        return false;
    const auto first = bridge.open_session();
    const auto other = bridge.open_session();
    if (!first || !other || !hello(bridge, first.value()) ||
        !hello(bridge, other.value())) return false;
    std::uint64_t lease = 0U;
    std::uint64_t other_lease = 0U;
    if (!acquire(bridge, first.value(), 2U, lease) ||
        !tune(bridge, first.value(), list.epoch, lease) ||
        !acquire(bridge, other.value(), 3U, other_lease)) return false;

    auto status = tuner.engine_status();
    if (!status) return false;
    w::StartRequest start_request;
    start_request.epoch = list.epoch;
    start_request.lease_id = lease;
    start_request.tune_revision = status.value().tune_metadata[2U].current_revision + 1U;
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    const auto size = w::encode_start_request(start_request,
        {payload.data(), payload.size()});
    w::Context context;
    context.epoch = list.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto rejected = size ? bridge.accept(first.value(),
        frame(w::kOpcodeStartStream, 5U, {payload.data(), size.value()}),
        context, snapshot, 100U) : dtv::userland::detail::AuthorityReply{};
    const auto rejected_header = rejected.frame.size >= w::kHeaderSize
        ? w::decode_header({rejected.frame.data, w::kHeaderSize})
        : Result<w::Header>::failure(Error::INVALID_ARGUMENT);
    if (rejected.action != w::InboundAction::respond || !rejected_header ||
        rejected_header.value().kind != w::Kind::error_response ||
        bridge.response_sent(first.value(), true) != Error::OK) {
        std::cerr << "rejected START response was not handled as an error response action="
                  << static_cast<unsigned>(rejected.action) << " size="
                  << rejected.frame.size << " header=" << static_cast<bool>(rejected_header)
                  << " kind=" << (rejected_header
                      ? static_cast<unsigned>(rejected_header.value().kind) : 0U)
                  << " connected=" << bridge.session_connected_for_test(first.value())
                  << " fatal=" << bridge.counts().fatal_unresolved << "\n";
        return false;
    }

    status = tuner.engine_status();
    if (!status) return false;
    start_request.tune_revision = status.value().tune_metadata[2U].current_revision;
    const auto valid_size = w::encode_start_request(start_request,
        {payload.data(), payload.size()});
    const auto accepted = valid_size ? bridge.accept(first.value(),
        frame(w::kOpcodeStartStream, 6U, {payload.data(), valid_size.value()}),
        context, snapshot, 101U) : dtv::userland::detail::AuthorityReply{};
    if (accepted.action != w::InboundAction::respond ||
        bridge.response_sent(first.value(), true) != Error::OK) {
        std::cerr << "valid START after rejected START failed\n";
        return false;
    }
    const auto accepted_header = w::decode_header({accepted.frame.data, w::kHeaderSize});
    const auto accepted_response = w::decode_start_response(
        {accepted.frame.data + w::kHeaderSize,
         accepted.frame.size - w::kHeaderSize});
    if (!accepted_header || accepted_header.value().kind != w::Kind::response ||
        !accepted_response || accepted_response.value().stream_id == 0U ||
        !tune(bridge, other.value(), list.epoch, other_lease)) {
        std::cerr << "START rejection disturbed later client progress\n";
        return false;
    }
    if (bridge.close_session(first.value()) != Error::OK ||
        !wait_free(bridge, first.value()) ||
        bridge.close_session(other.value()) != Error::OK ||
        !wait_free(bridge, other.value())) {
        std::cerr << "START rejection fixture close wait failed\n";
        return false;
    }
    status = tuner.engine_status();
    if (!status || status.value().leased[2U] || status.value().leased[3U] ||
        backend.capture_starts != 0U || backend.capture_stops != 0U ||
        bridge.begin_shutdown() != Error::OK) {
        std::cerr << "START rejection fixture lease cleanup failed\n";
        return false;
    }
    const auto shutdown_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < shutdown_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) {
        std::cerr << "START rejection fixture shutdown failed result="
                  << static_cast<unsigned>(bridge.shutdown_result()) << "\n";
        return false;
    }
    return true;
}

bool test_stream_owned_disconnect_submit_entry_points()
{
    for (unsigned scenario = 0U; scenario < 2U; ++scenario) {
        Backend backend;
        Nonce nonce;
        Time time;
        Stream stream;
        TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
        if (!tuner.configure_strict_capabilities() ||
            !tuner.configure_stream_contract(
                dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL))
            return false;
        CardBackend card_backend;
        CardSession card_session;
        CardService card(card_backend, card_session);
        auto created = ControlWorkerLanes::create(card, tuner);
        if (!created) return false;
        const auto list = make_list();
        w::ServerHello server{list.epoch, list.producer, 0U};
        HostAuthorityBridge bridge(tuner, *created.value(), server, list);
        if (bridge.configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK)
            return false;
        const auto control = bridge.open_session();
        if (!control || !hello(bridge, control.value())) return false;
        std::uint64_t lease = 0U;
        if (!acquire(bridge, control.value(), 2U, lease) ||
            !tune(bridge, control.value(), list.epoch, lease)) return false;
        const auto engine = tuner.engine_status();
        if (!engine) return false;
        w::StartRequest start_request;
        start_request.epoch = list.epoch;
        start_request.lease_id = lease;
        start_request.tune_revision = engine.value().tune_metadata[2U].current_revision;
        std::array<std::uint8_t, w::kMaxControlPayload> start_payload{};
        const auto start_size = w::encode_start_request(start_request,
            {start_payload.data(), start_payload.size()});
        w::Context context;
        context.epoch = list.epoch;
        context.state = w::ContextState::ready;
        w::Snapshot snapshot;
        const auto started = start_size ? bridge.accept(control.value(),
            frame(w::kOpcodeStartStream, 5U,
                  {start_payload.data(), start_size.value()}), context,
            snapshot, 100U) : dtv::userland::detail::AuthorityReply{};
        const auto started_response = started.frame.size >= w::kHeaderSize
            ? w::decode_start_response(
                {started.frame.data + w::kHeaderSize,
                 started.frame.size - w::kHeaderSize})
            : Result<w::StartResponse>::failure(Error::INVALID_ARGUMENT);
        if (started.action != w::InboundAction::respond || !started_response ||
            bridge.response_sent(control.value(), true) != Error::OK) {
            std::cerr << "N2 setup START failed scenario=" << scenario << "\n";
            return false;
        }
        const auto data = bridge.open_data_session();
        if (!data) return false;
        w::AttachRequest attach;
        attach.epoch = list.epoch;
        attach.lease_id = lease;
        attach.stream_id = started_response.value().stream_id;
        attach.attach_token = started_response.value().attach_token;
        if (bridge.prepare_data_attach(data.value(), control.value(), attach, 101U) != Error::OK)
            return false;
        HostAuthorityAttachResult prepared{};
        const auto prepare_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (!prepared.ready && std::chrono::steady_clock::now() < prepare_until) {
            (void)bridge.poll();
            prepared = bridge.take_data_attach_result(data.value());
            if (!prepared.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!prepared.ready || prepared.error != Error::OK ||
            bridge.data_attach_response_sent(data.value(), true) != Error::OK) {
            std::cerr << "N2 setup ATTACH failed scenario=" << scenario << "\n";
            return false;
        }

        if (scenario == 0U) {
            auto invalid_peer_frame = frame(w::kOpcodeList, 6U, {});
            invalid_peer_frame.header.kind = w::Kind::response;
            const auto closed = bridge.accept(control.value(), invalid_peer_frame,
                context, snapshot, 102U);
            if (closed.action != w::InboundAction::close || closed.cleanup_required) {
                std::cerr << "N2 framing close did not accept deferred cleanup\n";
                return false;
            }
        } else if (bridge.response_sent(control.value(), false) != Error::OK) {
            std::cerr << "N2 partial response close failed\n";
            return false;
        }
        const std::uint64_t client = bridge.client_id(control.value());
        if (client == 0U || bridge.counts().fatal_unresolved != 0U) {
            std::cerr << "N2 disconnect slot was not retained for stream cleanup\n";
            return false;
        }
        const auto cleanup_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < cleanup_until) {
            (void)bridge.poll();
            const auto current = tuner.engine_status();
            if (bridge.client_id(control.value()) == 0U &&
                bridge.data_counts().free == kHostAuthoritySessionCapacity && current &&
                !current.value().leased[2U]) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto final_status = tuner.engine_status();
        if (bridge.client_id(control.value()) != 0U ||
            bridge.data_counts().free != kHostAuthoritySessionCapacity || !final_status ||
            final_status.value().leased[2U] || backend.capture_starts != 1U ||
            backend.capture_stops != 1U || bridge.counts().fatal_unresolved != 0U) {
            std::cerr << "N2 stream cleanup did not finish scenario=" << scenario
                      << " client=" << bridge.client_id(control.value())
                      << " data-free=" << bridge.data_counts().free
                      << " lease=" << (final_status && final_status.value().leased[2U])
                      << " capture=" << backend.capture_starts << "/"
                      << backend.capture_stops << " fatal="
                      << bridge.counts().fatal_unresolved << "\n";
            return false;
        }
        if (bridge.begin_shutdown() != Error::OK) return false;
        const auto shutdown_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (!bridge.stopped() && std::chrono::steady_clock::now() < shutdown_until) {
            (void)bridge.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) {
            std::cerr << "N2 shutdown failed scenario=" << scenario << "\n";
            return false;
        }
    }

    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto control = bridge.open_session();
    if (!control) return false;
    std::array<std::uint8_t, w::kHelloRequestSize> payload{};
    const auto size = w::encode_hello_request(
        {{1U, 0U}, {1U, 0U}, w::kFeatureCardService, 0U},
        {payload.data(), payload.size()});
    w::Context context;
    w::Snapshot snapshot;
    const auto close_after = size ? bridge.accept(control.value(),
        frame(w::kOpcodeHello, 1U, {payload.data(), size.value()}),
        context, snapshot, 0U) : dtv::userland::detail::AuthorityReply{};
    if (close_after.action != w::InboundAction::respond ||
        !close_after.close_after_reply || close_after.cleanup_required) {
        std::cerr << "N2 close-after-reply response shape failed action="
                  << static_cast<unsigned>(close_after.action) << " close="
                  << close_after.close_after_reply << " cleanup="
                  << close_after.cleanup_required << "\n";
        return false;
    }
    if (bridge.response_sent(control.value(), true) != Error::OK ||
        !wait_free(bridge, control.value())) {
        std::cerr << "N2 close-after-reply disconnect failed\n";
        return false;
    }
    if (bridge.begin_shutdown() != Error::OK) {
        std::cerr << "N2 close-after-reply begin shutdown failed\n";
        return false;
    }
    const auto stop_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < stop_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) {
        std::cerr << "N2 close-after-reply shutdown failed\n";
        return false;
    }
    return true;
}

bool test_control_close_cleans_owned_stream_before_disconnect()
{
    for (unsigned scenario = 0U; scenario < 4U; ++scenario) {
        const auto fail = [scenario](const char* step) {
            std::cerr << "stream close setup scenario=" << scenario << " step="
                      << step << "\n";
            return false;
        };
        Backend backend;
        Nonce nonce;
        Time time;
        Stream stream;
        TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
        if (!tuner.configure_strict_capabilities()) return false;
        if (!tuner.configure_stream_contract(
                dtv::userland::detail::StreamContract::v1, 0x5349414e4fULL)) return fail("contract");
        CardBackend card_backend;
        CardSession card_session;
        CardService card(card_backend, card_session);
        auto created = ControlWorkerLanes::create(card, tuner);
        if (!created) return fail("workers");
        const auto list = make_list();
        w::ServerHello server{list.epoch, list.producer, 0U};
        HostAuthorityBridge bridge(tuner, *created.value(), server, list);
        if (bridge.configure_stream_contract(0x5349414e4fULL, 5000U) != Error::OK)
            return fail("configure");
        const auto control = bridge.open_session();
        if (!control || !hello(bridge, control.value())) return fail("hello");
        const auto other_control = bridge.open_session();
        if (!other_control || !hello(bridge, other_control.value())) return fail("other-hello");
        std::uint64_t lease = 0U;
        if (!acquire(bridge, control.value(), 2U, lease) ||
            !tune(bridge, control.value(), list.epoch, lease)) return fail("acquire-tune");
        std::uint64_t other_lease = 0U;
        if (!acquire(bridge, other_control.value(), 3U, other_lease))
            return fail("other-acquire");
        const auto status = tuner.engine_status();
        if (!status) return fail("status");
        const std::uint64_t revision = status.value().tune_metadata[2U].current_revision;
        w::StartRequest start_request;
        start_request.epoch = list.epoch;
        start_request.lease_id = lease;
        start_request.tune_revision = revision;
        std::array<std::uint8_t, w::kMaxControlPayload> start_payload{};
        const auto start_size = w::encode_start_request(start_request,
            {start_payload.data(), start_payload.size()});
        w::Context start_context;
        start_context.epoch = list.epoch;
        start_context.state = w::ContextState::ready;
        w::Snapshot start_snapshot;
        const auto start_reply = start_size ? bridge.accept(control.value(),
            frame(w::kOpcodeStartStream, 5U,
                  {start_payload.data(), start_size.value()}), start_context,
            start_snapshot, 100U) : dtv::userland::detail::AuthorityReply{};
        if (start_reply.action != w::InboundAction::respond ||
            bridge.response_sent(control.value(), true) != Error::OK) return fail("start");
        const auto started = w::decode_start_response(
            {start_reply.frame.data + w::kHeaderSize,
             start_reply.frame.size - w::kHeaderSize});
        if (!started) return fail("start-decode");
        if (scenario == 3U) {
            const auto stopped = bridge.stop_stream(control.value(), lease,
                                                     started.value().stream_id);
            const auto restarted = bridge.start_stream(control.value(), lease, revision,
                                                        102U, 5000U);
            if (!stopped || !stopped.value().quiesced || !restarted)
                return fail("stop-restart-armed");
        }
        const auto data = scenario == 3U
            ? Result<HostAuthorityDataConnection>::failure(Error::NOT_FOUND)
            : bridge.open_data_session();
        if (scenario != 3U && !data) return fail("start-data");
        w::AttachRequest request;
        request.epoch = list.epoch;
        request.lease_id = lease;
        request.stream_id = started.value().stream_id;
        request.attach_token = started.value().attach_token;
        HostAuthorityAttachResult attached{};
        if (scenario != 3U) {
            if (scenario == 2U) backend.block_capture(2U);
            if (bridge.prepare_data_attach(data.value(), control.value(), request, 101U) !=
                Error::OK) return fail("prepare");
            if (scenario == 2U && !backend.wait_capture()) {
                backend.release_capture();
                return fail("wait-capture");
            }
            const auto attach_until = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            while (!attached.ready && std::chrono::steady_clock::now() < attach_until) {
                (void)bridge.poll();
                attached = bridge.take_data_attach_result(data.value());
                if (!attached.ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (scenario == 2U) {
            if (attached.ready || bridge.close_data_session(data.value()) != Error::OK ||
                bridge.data_counts().tombstone != 1U) {
                backend.release_capture();
                return fail("P7-close");
            }
            backend.release_capture();
            const auto late_until = std::chrono::steady_clock::now() +
                std::chrono::seconds(5);
            while (bridge.data_counts().free != kHostAuthoritySessionCapacity &&
                   std::chrono::steady_clock::now() < late_until) {
                (void)bridge.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto late_final = tuner.authority_stream_last_terminal_v1(
                bridge.client_id(control.value()), lease);
            if (bridge.data_counts().free != kHostAuthoritySessionCapacity ||
                !late_final || !late_final.value().quiesced ||
                backend.capture_starts != 1U || backend.capture_stops != 1U)
                return fail("P7-late-cleanup");
            if (bridge.close_session(control.value()) != Error::OK)
                return fail("P7-control-close");
        } else if (scenario != 3U && (!attached.ready || attached.error != Error::OK ||
                   bridge.data_attach_response_sent(data.value(), true) != Error::OK)) {
            return fail("attach-commit");
        }
        if (scenario == 0U) {
            std::array<std::uint8_t, 188U> packet{};
            const auto read = bridge.read_data(data.value(),
                {packet.data(), packet.size()}, Timeout{0U});
            if (read.error != Error::OK || read.data.packets.size != packet.size()) return fail("read");
            if (bridge.close_session(control.value()) != Error::OK) return fail("close-control");
        } else if (scenario == 1U && bridge.begin_shutdown() != Error::OK) {
            return fail("shutdown");
        } else if (scenario == 3U &&
                   bridge.close_session(control.value()) != Error::OK) {
            return fail("close-armed-stream");
        }

        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until) {
            (void)bridge.poll();
            if (scenario != 2U && scenario != 3U) {
                std::array<std::uint8_t, 188U> packet{};
                (void)bridge.read_data(data.value(),
                    {packet.data(), packet.size()}, Timeout{0U});
            }
            if (bridge.client_id(control.value()) == 0U &&
                bridge.data_counts().free == kHostAuthoritySessionCapacity) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto final_status = tuner.engine_status();
        const unsigned expected_capture = scenario == 3U ? 0U : 1U;
        if (!final_status || final_status.value().leased[2U] ||
            backend.capture_starts != expected_capture ||
            backend.capture_stops != expected_capture ||
            bridge.client_id(control.value()) != 0U ||
            bridge.counts().fatal_unresolved != 0U ||
            (scenario != 1U && !final_status.value().leased[3U]) ||
            bridge.data_counts().free != kHostAuthoritySessionCapacity) {
            std::cerr << "stream close ordering scenario=" << scenario
                      << " lease=" << (final_status && final_status.value().leased[2U])
                      << " capture=" << backend.capture_starts << "/"
                      << backend.capture_stops << " bridge="
                      << bridge.counts().free << "/" << bridge.counts().active << "/"
                      << bridge.counts().tombstone << "/"
                      << bridge.counts().fatal_unresolved << " data-free="
                      << bridge.data_counts().free << " phase="
                      << static_cast<unsigned>(final_status.value().stream_phases[2U])
                      << " state="
                      << static_cast<unsigned>(final_status.value().states[2U])
                      << "\n";
            return false;
        }
        if (scenario != 1U) {
            if (!tune(bridge, other_control.value(), list.epoch, other_lease) ||
                bridge.close_session(other_control.value()) != Error::OK ||
                !wait_free(bridge, other_control.value())) return fail("other-progress");
            if (bridge.begin_shutdown() != Error::OK) return fail("postclose-shutdown");
        } else if (final_status.value().leased[3U]) {
            return fail("shutdown-other-lease");
        }
        const auto stop_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!bridge.stopped() && std::chrono::steady_clock::now() < stop_until) {
            (void)bridge.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) return fail("final-shutdown");
    }
    return true;
}

bool test_duplicate_completion_is_ignored()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.duplicate_next_authority_completion = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot || !hello(bridge, slot.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, slot.value(), 2U, lease)) return false;
    const auto reply = bridge.take_response(slot.value());
    if (reply.action != w::InboundAction::close || reply.frame.size != 0U) {
        std::cerr << "duplicate completion generated an extra response\n";
        return false;
    }
    const auto status = tuner.engine_status();
    if (!status || !status.value().leased[2U] || bridge.counts().active != 1U ||
        bridge.counts().fatal_unresolved != 0U) {
        std::cerr << "duplicate completion changed lease/owner state\n";
        return false;
    }
    if (bridge.close_session(slot.value()) != Error::OK ||
        !wait_free(bridge, slot.value())) return false;
    const auto released = tuner.engine_status();
    if (!released || released.value().leased[2U]) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_gate_close_waits_for_other_lane_release()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto request_lane = created.value()->authority_lane(4U, false);
    const auto cleanup_lane = created.value()->authority_lane(4U, true);
    if (!request_lane || !cleanup_lane || request_lane.value() == cleanup_lane.value()) {
        std::cerr << "receiver 4 did not route away from cleanup lane\n";
        return false;
    }
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot || !hello(bridge, slot.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, slot.value(), 4U, lease)) return false;

    backend.block_close(4U);
    std::array<std::uint8_t, 64U> release_payload{};
    const auto release_size = w::encode_release_request({list.epoch, lease},
        {release_payload.data(), release_payload.size()});
    if (!release_size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto release_reply = bridge.accept(slot.value(),
        frame(w::kOpcodeRelease, 4U,
              {release_payload.data(), release_size.value()}),
        context, snapshot, 0U);
    if (!release_reply.queued || !backend.wait_close()) {
        backend.release_close();
        return false;
    }

    std::array<std::uint8_t, w::kEpochSize> list_payload{};
    const auto list_size = w::encode_list_request({list.epoch},
        {list_payload.data(), list_payload.size()});
    if (!list_size) {
        backend.release_close();
        return false;
    }
    const std::uint64_t id = bridge.client_id(slot.value());
    const auto second = bridge.accept(slot.value(),
        frame(w::kOpcodeList, 5U, {list_payload.data(), list_payload.size()}),
        context, snapshot, 0U);
    if (second.action != w::InboundAction::close || second.cleanup_required) {
        backend.release_close();
        std::cerr << "second frame did not enter accepted gate-close cleanup\n";
        return false;
    }
    for (unsigned i = 0U; i < 20U; ++i) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (bridge.counts().tombstone != 1U || bridge.client_id(slot.value()) != id ||
        bridge.counts().fatal_unresolved != 0U) {
        backend.release_close();
        std::cerr << "gate close bypassed same-client inflight barrier\n";
        return false;
    }

    backend.release_close();
    if (!wait_free(bridge, slot.value())) {
        std::cerr << "gate close did not drain after RELEASE completion\n";
        return false;
    }
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) {
        std::cerr << "gate-close late RELEASE made bridge fatal\n";
        return false;
    }
    const auto engine = tuner.engine_status();
    return engine && !engine.value().leased[4U];
}

bool test_release_uses_worker_and_clears_engine_lease()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    if (!tuner.configure_strict_capabilities()) return false;
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot || !hello(bridge, slot.value())) { std::cerr << "release setup/hello\n"; return false; }
    std::uint64_t lease = 0U;
    if (!acquire(bridge, slot.value(), 2U, lease)) { std::cerr << "release setup acquire\n"; return false; }
    auto status = tuner.engine_status();
    if (!status || !status.value().leased[2U]) return false;
    if (!tune(bridge, slot.value(), list.epoch, lease)) {
        std::cerr << "bridge tune\n";
        (void)bridge.close_session(slot.value());
        (void)wait_free(bridge, slot.value());
        return false;
    }
    if (!release(bridge, slot.value(), list.epoch, lease)) { std::cerr << "bridge release\n"; return false; }
    status = tuner.engine_status();
    if (!status || status.value().leased[2U]) return false;
    if (bridge.close_session(slot.value()) != Error::OK || !wait_free(bridge, slot.value()))
        return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_rejected_disconnect_submit_retains_cleanup()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.reject_next_authority_cleanup_submit = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot || !hello(bridge, slot.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, slot.value(), 2U, lease)) return false;
    const std::uint64_t client = bridge.client_id(slot.value());
    if (bridge.close_session(slot.value()) != Error::OK ||
        bridge.counts().tombstone != 1U || bridge.client_id(slot.value()) != client)
        return false;
    auto status = tuner.engine_status();
    if (!status || !status.value().leased[2U]) return false;
    if (!wait_free(bridge, slot.value())) return false;
    status = tuner.engine_status();
    if (!status || status.value().leased[2U]) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_accept_close_rejected_cleanup_is_retained()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.reject_next_authority_cleanup_submit = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto slot = bridge.open_session();
    if (!slot || !hello(bridge, slot.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, slot.value(), 2U, lease)) return false;
    const std::uint64_t client = bridge.client_id(slot.value());
    auto status = tuner.engine_status();
    if (!status || !status.value().leased[2U]) return false;

    w::Context context;
    w::Snapshot snapshot;
    auto invalid_peer_frame = frame(w::kOpcodeList, 3U, {});
    invalid_peer_frame.header.kind = w::Kind::response;
    const auto reply = bridge.accept(slot.value(), invalid_peer_frame,
                                     context, snapshot, 0U);
    if (reply.action != w::InboundAction::close || !reply.cleanup_required ||
        reply.cleanup.kind != dtv::userland::detail::AuthorityJobKind::disconnect ||
        bridge.counts().active != 0U || bridge.counts().tombstone != 1U ||
        bridge.client_id(slot.value()) != client ||
        bridge.session_connected_for_test(slot.value())) {
        std::cerr << "accept-close did not retain the rejected cleanup in a tombstone\n";
        return false;
    }
    if (bridge.close_session(slot.value()) != Error::NOT_FOUND ||
        bridge.counts().tombstone != 1U || bridge.client_id(slot.value()) != client) {
        std::cerr << "close_session changed the accept-close cleanup owner\n";
        return false;
    }
    status = tuner.engine_status();
    if (!status || !status.value().leased[2U]) {
        std::cerr << "lease cleared before typed cleanup completion\n";
        return false;
    }
    if (!wait_free(bridge, slot.value())) {
        std::cerr << "poll owner did not retry and complete accept-close cleanup\n";
        return false;
    }
    status = tuner.engine_status();
    if (!status || status.value().leased[2U] || bridge.client_id(slot.value()) != 0U) {
        std::cerr << "typed cleanup completion did not clear lease and slot\n";
        return false;
    }
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_rollback_cleanup_does_not_block_other_session()
{
    Backend backend;
    backend.block_close(2U);
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    if (!tuner.configure_strict_capabilities()) return false;
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    ControlWorkerStartupOptions options;
    options.mismatch_next_authority_acquire_receiver = true;
    auto created = ControlWorkerLanes::create_for_test(card, tuner, options);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto first = bridge.open_session();
    const auto second = bridge.open_session();
    if (!first || !second || !hello(bridge, first.value()) ||
        !hello(bridge, second.value())) return false;

    std::array<std::uint8_t, w::kAcquireRequestSize> acquire_payload{};
    const auto acquire_size = w::encode_acquire_request({list.epoch, 2U},
        {acquire_payload.data(), acquire_payload.size()});
    if (!acquire_size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto queued = bridge.accept(first.value(),
        frame(w::kOpcodeAcquire, 2U,
              {acquire_payload.data(), acquire_size.value()}),
        context, snapshot, 0U);
    bool entered_close = false;
    const auto close_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered_close && std::chrono::steady_clock::now() < close_until) {
        (void)bridge.poll();
        entered_close = backend.close_started();
        if (!entered_close) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!queued.queued || !entered_close) {
        (void)bridge.poll();
        const auto pending_response = bridge.take_response(first.value());
        std::cerr << "rollback setup queued=" << queued.queued << " close=" << entered_close
                  << " response=" << static_cast<unsigned>(pending_response.action)
                  << " worker=" << static_cast<unsigned>(created.value()->error()) << "\n";
        backend.release_close();
        (void)bridge.close_session(first.value());
        (void)bridge.close_session(second.value());
        for (unsigned i = 0U; i < 1000U && bridge.counts().free != kHostAuthoritySessionCapacity; ++i) {
            (void)bridge.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (bridge.counts().free == kHostAuthoritySessionCapacity) {
            (void)bridge.begin_shutdown();
            for (unsigned i = 0U; i < 1000U && !bridge.stopped(); ++i) {
                (void)bridge.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return false;
    }

    std::array<std::uint8_t, w::kEpochSize> list_payload{};
    const auto list_size = w::encode_list_request({list.epoch},
        {list_payload.data(), list_payload.size()});
    const auto listed = list_size ? bridge.accept(second.value(),
        frame(w::kOpcodeList, 3U, {list_payload.data(), list_size.value()}),
        context, snapshot, 0U) : dtv::userland::detail::AuthorityReply{};
    if (listed.action != w::InboundAction::respond ||
        bridge.response_sent(second.value(), true) != Error::OK) {
        backend.release_close();
        return false;
    }
    backend.release_close();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool rollback_response = false;
    while (std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        const auto response = bridge.take_response(first.value());
        if (response.action == w::InboundAction::respond) {
            const auto header = w::decode_header({response.frame.data, w::kHeaderSize});
            rollback_response = header && header.value().kind == w::Kind::error_response &&
                bridge.response_sent(first.value(), true) == Error::OK;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto status = tuner.engine_status();
    if (!rollback_response || !status || status.value().leased[2U] ||
        bridge.close_session(first.value()) != Error::OK ||
        !wait_free(bridge, first.value()) ||
        bridge.close_session(second.value()) != Error::OK ||
        !wait_free(bridge, second.value())) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_deferred_acquire_tombstone_and_progress()
{
    Backend backend;
    backend.block_open(3U);
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return false;
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    HostAuthorityBridge bridge(tuner, *created.value(), server, list);
    const auto first = bridge.open_session();
    const auto second = bridge.open_session();
    if (!first || !second || !hello(bridge, first.value()) ||
        !hello(bridge, second.value())) return false;

    std::array<std::uint8_t, w::kAcquireRequestSize> acquire_payload{};
    const auto acquire_size = w::encode_acquire_request({list.epoch, 3U},
        {acquire_payload.data(), acquire_payload.size()});
    if (!acquire_size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto pending = bridge.accept(first.value(),
        frame(w::kOpcodeAcquire, 2U,
              {acquire_payload.data(), acquire_size.value()}),
        context, snapshot, 0U);
    if (!pending.queued || !backend.wait_open()) return false;
    const std::uint64_t id = bridge.client_id(first.value());
    if (bridge.close_session(first.value()) != Error::OK ||
        bridge.counts().tombstone != 1U || bridge.client_id(first.value()) != id)
        return false;
    std::array<std::uint8_t, kHostAuthoritySessionCapacity - 2U> extra{};
    for (std::size_t i = 0U; i < extra.size(); ++i) {
        const auto opened = bridge.open_session();
        if (!opened) return false;
        extra[i] = opened.value();
    }
    if (bridge.open_session().error() != Error::RESOURCE_LIMIT) return false;

    std::array<std::uint8_t, w::kEpochSize> list_payload{};
    const auto list_size = w::encode_list_request({list.epoch},
        {list_payload.data(), list_payload.size()});
    if (!list_size) return false;
    const auto list_reply = bridge.accept(second.value(),
        frame(w::kOpcodeList, 3U, {list_payload.data(), list_size.value()}),
        context, snapshot, 0U);
    if (list_reply.action != w::InboundAction::respond ||
        bridge.response_sent(second.value(), true) != Error::OK) return false;

    backend.release_open();
    if (!wait_free(bridge, first.value())) return false;
    const auto engine = tuner.engine_status();
    if (!engine || engine.value().leased[3U] || bridge.client_id(first.value()) != 0U)
        return false;
    const auto reused = bridge.open_session();
    if (!reused || reused.value() != first.value() ||
        bridge.close_session(reused.value()) != Error::OK ||
        !wait_free(bridge, reused.value())) return false;
    for (const std::uint8_t slot : extra) {
        if (bridge.close_session(slot) != Error::OK || !wait_free(bridge, slot))
            return false;
    }
    if (bridge.close_session(second.value()) != Error::OK ||
        !wait_free(bridge, second.value())) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
}

bool test_fatal_late_acquire_is_not_finalized_early()
{
    // A fatal unresolved session intentionally retains every owner through
    // process exit; destroying these objects would invalidate accepted work.
    auto* backend = new Backend;
    backend->block_open(4U);
    auto* nonce = new Nonce;
    auto* time = new Time;
    auto* tuner = new TunerService(*backend, *nonce, *time);
    auto* card_backend = new CardBackend;
    auto* card_session = new CardSession;
    auto* card = new CardService(*card_backend, *card_session);
    ControlWorkerStartupOptions options;
    options.fatal_after_authority_lane = static_cast<int>(ControlWorkerLane::tuner_dev1);
    auto created = ControlWorkerLanes::create_for_test(*card, *tuner, options);
    if (!created) { std::cerr << "fatal setup worker create\n"; return false; }
    ControlWorkerLanes* workers = created.value().release();
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    auto* bridge = new HostAuthorityBridge(*tuner, *workers, server, list);
    unsigned other_ingress_stops = 0U;
    if (bridge->set_ingress_stop(&mark_ingress_stopped, &other_ingress_stops) != Error::OK) {
        std::cerr << "fatal setup ingress callback\n";
        return false;
    }
    const auto first = bridge->open_session();
    const auto second = bridge->open_session();
    if (!first || !second || !hello(*bridge, first.value()) ||
        !hello(*bridge, second.value())) {
        std::cerr << "fatal setup session hello\n";
        return false;
    }

    std::array<std::uint8_t, w::kAcquireRequestSize> payload_a{};
    std::array<std::uint8_t, w::kAcquireRequestSize> payload_b{};
    const auto size_a = w::encode_acquire_request({list.epoch, 2U},
        {payload_a.data(), payload_a.size()});
    const auto size_b = w::encode_acquire_request({list.epoch, 4U},
        {payload_b.data(), payload_b.size()});
    if (!size_a || !size_b) { std::cerr << "fatal setup acquire encoding\n"; return false; }
    w::Context context;
    w::Snapshot snapshot;
    const auto queued_b = bridge->accept(second.value(), frame(w::kOpcodeAcquire, 2U,
        {payload_b.data(), size_b.value()}), context, snapshot, 0U);
    if (!queued_b.queued) {
        std::cerr << "fatal setup blocked acquire dispatch failed\n";
        backend->release_open();
        return false;
    }
    if (!backend->wait_open()) {
        std::cerr << "fatal setup blocked open was not reached\n";
        backend->release_open();
        return false;
    }
    const auto queued_a = bridge->accept(first.value(), frame(w::kOpcodeAcquire, 2U,
        {payload_a.data(), size_a.value()}), context, snapshot, 0U);
    if (!queued_a.queued) {
        std::cerr << "fatal setup fatal-lane acquire dispatch failed\n";
        backend->release_open();
        return false;
    }

    const auto fatal_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (workers->error() == Error::OK &&
           std::chrono::steady_clock::now() < fatal_until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (workers->error() == Error::OK) {
        backend->release_open();
        std::cerr << "fatal worker error did not become observable\n";
        return false;
    }

    const Error early = bridge->poll();
    if (early != Error::OK || bridge->stopped() || other_ingress_stops != 1U ||
        bridge->shutdown_result() != Error::NOT_READY ||
        bridge->counts().tombstone + bridge->counts().fatal_unresolved != 2U ||
        bridge->counts().active != 0U ||
        !bridge->fatal_unresolved_for_test(first.value())) {
        const auto counts = bridge->counts();
        std::cerr << "fatal early poll=" << static_cast<unsigned>(early)
                  << " stopped=" << bridge->stopped() << " counts=" << counts.free
                  << "/" << counts.active << "/" << counts.tombstone << "/"
                  << counts.fatal_unresolved << " result="
                  << static_cast<unsigned>(bridge->shutdown_result()) << "\n";
        backend->release_open();
        return false;
    }
    const auto late_acquire_status = tuner->engine_status();
    if (!late_acquire_status || !late_acquire_status.value().leased[2U]) {
        backend->release_open();
        std::cerr << "fatal late ACQUIRE was not directly observed as leased\n";
        return false;
    }
    backend->release_open();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge->stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto status = tuner->engine_status();
    const auto final_counts = bridge->counts();
    if (!(bridge->stopped() && bridge->shutdown_result() == Error::INTERNAL &&
           final_counts.fatal_unresolved == 2U && status &&
           status.value().leased[2U] && status.value().leased[4U])) {
        std::cerr << "fatal final stopped=" << bridge->stopped() << " result="
                  << static_cast<unsigned>(bridge->shutdown_result()) << " counts="
                  << final_counts.free << "/" << final_counts.active << "/"
                  << final_counts.tombstone << "/" << final_counts.fatal_unresolved
                  << " status=" << static_cast<bool>(status);
        if (status) std::cerr << " leases=" << status.value().leased[2U] << "/"
                              << status.value().leased[4U];
        std::cerr << '\n';
    }
    return bridge->stopped() && bridge->shutdown_result() == Error::INTERNAL &&
           final_counts.fatal_unresolved == 2U && status &&
           other_ingress_stops == 1U &&
           status.value().leased[2U] && status.value().leased[4U];
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2) {
        const std::string_view selected(argv[1]);
        bool passed = false;
        if (selected == "m1") passed = test_cleanup_requires_quiesced_terminal();
        else if (selected == "failed-attach")
            passed = test_failed_attach_prepare_is_reported_to_data_owner();
        else if (selected == "m8") passed = test_prepare_attach_rejects_old_generation();
        else if (selected == "b1") passed = test_finish_control_close_waits_for_owned_stream();
        else if (selected == "n4") passed = test_accept_preserves_close_pending_before_poll();
        else if (selected == "n5") passed = test_response_sent_retains_rejected_hello_disconnect();
        else return 2;
        if (!passed) return 1;
        return 0;
    }
    if (!test_cleanup_requires_quiesced_terminal() ||
        !test_prepare_attach_rejects_old_generation() ||
        !test_finish_control_close_waits_for_owned_stream() ||
        !test_accept_preserves_close_pending_before_poll() ||
        !test_response_sent_retains_rejected_hello_disconnect()) {
        std::cerr << "stream guard direct-observation fixture failed\n";
        return 1;
    }
    if (!test_disconnect_direct_observation()) {
        std::cerr << "disconnect direct observation fixture failed\n";
        return 1;
    }
    if (!test_capacity_and_send_failure()) {
        std::cerr << "capacity/send failure fixture failed\n";
        return 1;
    }
    if (!test_paired_ingress_capacity_and_stop()) {
        std::cerr << "paired ingress capacity/stop fixture failed\n";
        return 1;
    }
    if (!test_authority_stream_prepare_commit_stop()) {
        std::cerr << "Authority stream prepare/commit/STOP fixture failed\n";
        return 1;
    }
    if (!test_failed_attach_prepare_is_reported_to_data_owner()) {
        std::cerr << "failed ATTACH prepare result fixture failed\n";
        return 1;
    }
    if (!test_control_close_cleans_owned_stream_before_disconnect()) {
        std::cerr << "control close stream cleanup ordering fixture failed\n";
        return 1;
    }
    if (!test_rejected_start_does_not_record_stream()) {
        std::cerr << "rejected START response fixture failed\n";
        return 1;
    }
    if (!test_stream_owned_disconnect_submit_entry_points()) {
        std::cerr << "stream-owned DISCONNECT submit entry-point fixture failed\n";
        return 1;
    }
    if (!test_duplicate_completion_is_ignored()) {
        std::cerr << "duplicate completion fixture failed\n";
        return 1;
    }
    if (!test_gate_close_waits_for_other_lane_release()) {
        std::cerr << "gate-close inflight barrier fixture failed\n";
        return 1;
    }
    if (!test_release_uses_worker_and_clears_engine_lease()) {
        std::cerr << "RELEASE worker fixture failed\n";
        return 1;
    }
    if (!test_rejected_disconnect_submit_retains_cleanup()) {
        std::cerr << "rejected DISCONNECT submit fixture failed\n";
        return 1;
    }
    if (!test_accept_close_rejected_cleanup_is_retained()) {
        std::cerr << "accept-close rejected cleanup fixture failed\n";
        return 1;
    }
    if (!test_rollback_cleanup_does_not_block_other_session()) {
        std::cerr << "rollback cleanup fixture failed\n";
        return 1;
    }
    if (!test_deferred_acquire_tombstone_and_progress()) {
        std::cerr << "deferred ACQUIRE fixture failed\n";
        return 1;
    }
    if (!test_fatal_late_acquire_is_not_finalized_early()) {
        std::cerr << "fatal late ACQUIRE fixture failed\n";
        return 1;
    }
    return 0;
}
