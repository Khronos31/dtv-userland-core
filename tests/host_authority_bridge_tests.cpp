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
    std::array<unsigned, 8U> opens{};
    std::array<unsigned, 8U> closes{};
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
    const auto a = bridge.open_session();
    const auto b = bridge.open_session();
    if (!a || !b || bridge.open_session().error() != Error::RESOURCE_LIMIT) { std::cerr << "capacity opening\n"; return false; }
    if (!hello_unwritten(bridge, a.value()) || !hello(bridge, b.value())) { std::cerr << "capacity hello\n"; return false; }
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
    const auto old_id = bridge.client_id(a.value());
    if (finish_fake_send(bridge, a.value(), FakeSendResult::partial) != Error::OK) { std::cerr << "capacity partial send\n"; return false; }
    if (!wait_free(bridge, a.value())) { std::cerr << "capacity first wait free\n"; return false; }
    const auto reused = bridge.open_session();
    if (!reused || reused.value() != a.value() || bridge.client_id(reused.value()) <= old_id ||
        !hello_unwritten(bridge, reused.value()) ||
        finish_fake_send(bridge, reused.value(), FakeSendResult::would_block) != Error::OK ||
        !wait_free(bridge, reused.value())) { std::cerr << "slot reuse/would-block send\n"; return false; }
    const auto error_slot = bridge.open_session();
    if (!error_slot || error_slot.value() != a.value() ||
        !hello_unwritten(bridge, error_slot.value()) ||
        finish_fake_send(bridge, error_slot.value(), FakeSendResult::error) != Error::OK ||
        !wait_free(bridge, error_slot.value())) { std::cerr << "transport error send\n"; return false; }
    if (bridge.close_session(b.value()) != Error::OK || !wait_free(bridge, b.value())) { std::cerr << "capacity second close/wait\n"; return false; }
    if (bridge.begin_shutdown() != Error::OK) { std::cerr << "capacity shutdown begin\n"; return false; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!bridge.stopped() || bridge.shutdown_result() != Error::OK) { std::cerr << "capacity shutdown result\n"; return false; }
    return true;
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
    if (!created) return false;
    ControlWorkerLanes* workers = created.value().release();
    const auto list = make_list();
    w::ServerHello server{list.epoch, list.producer, 0U};
    auto* bridge = new HostAuthorityBridge(*tuner, *workers, server, list);
    unsigned other_ingress_stops = 0U;
    if (bridge->set_ingress_stop(&mark_ingress_stopped, &other_ingress_stops) != Error::OK)
        return false;
    const auto first = bridge->open_session();
    const auto second = bridge->open_session();
    if (!first || !second || !hello(*bridge, first.value()) ||
        !hello(*bridge, second.value())) return false;

    std::array<std::uint8_t, w::kAcquireRequestSize> payload_a{};
    std::array<std::uint8_t, w::kAcquireRequestSize> payload_b{};
    const auto size_a = w::encode_acquire_request({list.epoch, 2U},
        {payload_a.data(), payload_a.size()});
    const auto size_b = w::encode_acquire_request({list.epoch, 4U},
        {payload_b.data(), payload_b.size()});
    if (!size_a || !size_b) return false;
    w::Context context;
    w::Snapshot snapshot;
    if (!bridge->accept(first.value(), frame(w::kOpcodeAcquire, 2U,
            {payload_a.data(), size_a.value()}), context, snapshot, 0U).queued ||
        !bridge->accept(second.value(), frame(w::kOpcodeAcquire, 2U,
            {payload_b.data(), size_b.value()}), context, snapshot, 0U).queued ||
        !backend->wait_open()) return false;

    const Error early = bridge->poll();
    if (early != Error::OK || bridge->stopped() || other_ingress_stops != 1U ||
        bridge->shutdown_result() != Error::NOT_READY ||
        bridge->counts().tombstone + bridge->counts().fatal_unresolved != 2U ||
        bridge->counts().active != 0U) {
        const auto counts = bridge->counts();
        std::cerr << "fatal early poll=" << static_cast<unsigned>(early)
                  << " stopped=" << bridge->stopped() << " counts=" << counts.free
                  << "/" << counts.active << "/" << counts.tombstone << "/"
                  << counts.fatal_unresolved << " result="
                  << static_cast<unsigned>(bridge->shutdown_result()) << "\n";
        backend->release_open();
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
    if (!test_release_uses_worker_and_clears_engine_lease()) {
        std::cerr << "RELEASE worker fixture failed\n";
        return 1;
    }
    if (!test_rejected_disconnect_submit_retains_cleanup()) {
        std::cerr << "rejected DISCONNECT submit fixture failed\n";
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
