// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/userland/host_daemon/host_authority_bridge.h>

#include <array>
#include <chrono>
#include <thread>

namespace {

using namespace dtv::userland;
using namespace px4::userland;

class Tuner final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
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

template <std::size_t N>
wire::WString<N> text(const char* value)
{
    wire::WString<N> result;
    while (value[result.length] != '\0') {
        result.bytes[result.length] = value[result.length];
        ++result.length;
    }
    return result;
}

wire::ListResponse list()
{
    wire::ListResponse value;
    for (std::size_t i = 0U; i < value.epoch.size(); ++i)
        value.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    value.producer.product = text<32U>("bridge-consumer");
    value.producer.version = text<64U>("1");
    value.topology.state = wire::TopologyState::complete;
    value.topology.expected_function_count.present = true;
    value.topology.expected_function_count.value = 1U;
    value.functions.count = 1U;
    value.functions.items[0U].id = 1U;
    value.functions.items[0U].vid = 0x1234U;
    value.functions.items[0U].pid = 0x5678U;
    value.functions.items[0U].mode = wire::Mode::runtime;
    value.functions.items[0U].serial_state = wire::SerialState::absent;
    value.functions.items[0U].identification_reason =
        wire::IdentificationReason::descriptor_unreadable;
    value.functions.items[0U].present = true;
    value.enclosure.present = true;
    value.enclosure.value.id = 0U;
    value.enclosure.value.identity.serial_state = wire::IdentitySerialState::absent;
    value.receivers.present = true;
    value.receivers.value.count = 1U;
    auto& receiver = value.receivers.value.items[0U];
    receiver.id = 2U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    receiver.binding.value.local_id = 0U;
    value.readers.present = true;
    return value;
}

}  // namespace

int main()
{
    Tuner tuner_backend;
    Nonce nonce;
    Time time;
    TunerService tuner(tuner_backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto workers = ipc::posix::ControlWorkerLanes::create(card, tuner);
    if (!workers) return 1;
    const auto canonical = list();
    wire::ServerHello server{canonical.epoch, canonical.producer, 0U};
    ipc::posix::HostAuthorityBridge bridge(tuner, *workers.value(), server, canonical);
    const auto slot = bridge.open_session();
    if (!slot || bridge.close_session(slot.value()) != Error::OK) return 1;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bridge.client_id(slot.value()) != 0U &&
           std::chrono::steady_clock::now() < until) {
        if (bridge.poll() != Error::OK) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (bridge.client_id(slot.value()) != 0U || bridge.begin_shutdown() != Error::OK)
        return 1;
    while (!bridge.stopped() && std::chrono::steady_clock::now() < until) {
        (void)bridge.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return bridge.shutdown_result() == Error::OK ? 0 : 1;
}
