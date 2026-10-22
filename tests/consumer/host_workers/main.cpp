// SPDX-License-Identifier: MIT
#include <dtv/userland/host_daemon/control_workers.h>

namespace {

using namespace px4::userland;

class TunerBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
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
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
};

class NonceSource final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> value{};
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(value);
    }
};

class TimeSource final : public TunerServiceTime {
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

}  // namespace

int main()
{
    TunerBackend tuner_backend;
    NonceSource nonce;
    TimeSource time;
    TunerService tuner(tuner_backend, nonce, time);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto workers = px4::userland::ipc::posix::ControlWorkerLanes::create(card, tuner);
    if (!workers) return 1;
    if (!workers.value()->request_stop()) return 1;
    return workers.value()->join() ? 0 : 1;
}
