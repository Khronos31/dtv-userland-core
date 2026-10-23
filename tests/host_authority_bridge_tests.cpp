// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/host_authority_bridge.h"

#include <array>
#include <chrono>
#include <iostream>
#include <condition_variable>
#include <mutex>
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
        return Result<void>::success();
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
    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        identity = attachment;
        attached = true;
        packets_remaining = 2U;
        counters = TunerStreamCounters{};
        final = TunerStreamFinalSnapshot{};
        final_released = false;
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment& attachment) noexcept override
    {
        if (!same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        attached = false;
        final.counters = counters;
        final.terminal = static_cast<std::uint8_t>(TunerStreamTerminal::stopped);
        return Result<void>::success();
    }
    Result<TunerStreamCounters> stats(
        const TunerAttachment& attachment) const noexcept override
    {
        return same(attachment) ? Result<TunerStreamCounters>::success(counters)
                                : Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept override
    {
        return same(attachment) ? Result<TunerStreamFinalSnapshot>::success(final)
                                : Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(const TunerAttachment& attachment) noexcept override
    {
        if (!same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        final_released = true;
        return Result<void>::success();
    }
    Result<TunerStreamReadResult> read(const TunerAttachment& attachment,
                                        MutableByteView output,
                                        Timeout) noexcept override
    {
        if (!same(attachment)) return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
        if (packets_remaining != 0U && output.data != nullptr && output.size >= 188U) {
            for (std::size_t i = 0U; i < 188U; ++i) output.data[i] = static_cast<std::uint8_t>(i);
            --packets_remaining;
            ++counters.packets;
            counters.bytes += 188U;
            return Result<TunerStreamReadResult>::success({188U, false, false,
                TunerStreamTerminal::none});
        }
        return Result<TunerStreamReadResult>::success({0U, !attached, attached,
            attached ? TunerStreamTerminal::none : TunerStreamTerminal::stopped});
    }
    bool final_released = false;
private:
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
             std::uint64_t& lease)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({make_list().epoch, receiver},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeAcquire, 2U,
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

bool release(HostAuthorityBridge& bridge, std::uint8_t slot,
             const w::Uuid& epoch, std::uint64_t lease)
{
    std::array<std::uint8_t, 64U> payload{};
    const auto size = w::encode_release_request({epoch, lease},
        {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = bridge.accept(slot, frame(w::kOpcodeRelease, 4U,
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
          const w::Uuid& epoch, std::uint64_t lease)
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
    const auto reply = bridge.accept(slot, frame(w::kOpcodeTune, 3U,
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
    if (!control || !hello(bridge, control.value())) return false;
    std::uint64_t lease = 0U;
    if (!acquire(bridge, control.value(), 2U, lease) ||
        !tune(bridge, control.value(), list.epoch, lease)) return false;
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
        5U, {start_payload.data(), start_size.value()}), start_context,
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
    const auto stopped = bridge.stop_stream(control.value(), lease,
                                              started.value().stream_id);
    if (stopped || stopped.error() != Error::RESOURCE_LIMIT ||
        bridge.data_counts().tombstone != 1U) return false;
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
    if (!stream_end.ready || !drained_packet_seen ||
        stream_end.event.final.id != started.value().stream_id ||
        stream_end.event.final.statistics.entries.items[0U].counter.measurement.value != 2U ||
        !final || !final.value().quiesced || backend.capture_stops != 1U ||
        bridge.data_counts().active != 0U || bridge.data_counts().tombstone != 1U ||
        bridge.data_stream_end_sent(data.value(), true) != Error::OK ||
        bridge.data_counts().tombstone != 0U || !stream.final_released)
        return false;
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
    if (!third_start || !third_data) return false;
    request.stream_id = third_start.value().stream_id;
    request.attach_token = third_start.value().token;
    backend.block_capture(2U);
    if (bridge.prepare_data_attach(third_data.value(), control.value(), request, 301U) !=
        Error::OK || !backend.wait_capture()) {
        backend.release_capture();
        return false;
    }
    const auto racing_stop = bridge.stop_stream(control.value(), lease,
                                                 third_start.value().stream_id);
    if (!racing_stop || bridge.data_counts().tombstone != 1U) {
        backend.release_capture();
        return false;
    }
    backend.release_capture();
    stream_end = HostAuthorityStreamEndResult{};
    const auto race_cleanup_until = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!stream_end.ready && std::chrono::steady_clock::now() < race_cleanup_until) {
        (void)bridge.poll();
        (void)bridge.read_data(third_data.value(), {packet.data(), packet.size()}, Timeout{0U});
        stream_end = bridge.take_data_stream_end(third_data.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto raced_final = tuner.authority_stream_last_terminal_v1(
        bridge.client_id(control.value()), lease);
    if (!stream_end.ready || !raced_final || !raced_final.value().quiesced ||
        backend.capture_starts != 3U || backend.capture_stops != 3U ||
        bridge.data_stream_end_sent(third_data.value(), true) != Error::OK) return false;
    if (bridge.close_session(control.value()) != Error::OK ||
        !wait_free(bridge, control.value())) return false;
    if (bridge.begin_shutdown() != Error::OK) return false;
    const auto stop_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < stop_until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.stopped() && bridge.shutdown_result() == Error::OK;
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

int main()
{
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
