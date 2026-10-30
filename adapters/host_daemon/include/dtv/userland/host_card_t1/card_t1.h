// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/include/px4/card.h at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d, which is derived from
// tsukumijima/px4_drv commit 9eedea8c502875a788697984b93b50032339b9aa.
// Origin paths: winusb/src/DriverHost_PX4/card_device.hpp,
// winusb/src/DriverHost_PX4/smart_card.hpp,
// winusb/src/DriverHost_PX4/smart_card.cpp,
// winusb/src/WinSCard_PX4/bcas_atr.hpp.
// ASICEN modification, 2026-10-09: CardSession::initialize_with_atr() validates
// an ATR profile before T=1 negotiation.
// Source snapshot maintained by tsukumijima.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_CARD_T1_CARD_T1_H
#define DTV_USERLAND_HOST_CARD_T1_CARD_T1_H

#include "dtv/userland/error.h"
#include "dtv/userland/transport.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dtv::userland::host_card_t1 {

inline constexpr std::size_t kCardAtrMaxLength = 33U;

enum class CardBaudRate : std::uint8_t {
    baud_9600 = 0,
    baud_19200 = 1,
    baud_38400 = 2,
};

enum class CardEdc : std::uint8_t {
    lrc = 0,
    crc = 1,
};

struct CardAtr final {
    std::array<std::uint8_t, kCardAtrMaxLength> bytes{};
    std::size_t length = 0U;
    CardBaudRate baud_rate = CardBaudRate::baud_9600;
    std::uint8_t ifsc = 32U;
    CardEdc edc = CardEdc::lrc;
    std::uint32_t block_timeout_ms = 500U;

    ByteView view() const noexcept { return ByteView{bytes.data(), length}; }
};

// The reset pulse and ATR polling share one sleep seam. Implementations must
// not represent wall-clock time through monotonic_ms().
class CardDelay {
public:
    virtual ~CardDelay() noexcept = default;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

class CardTime : public CardDelay {
public:
    ~CardTime() noexcept override = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
};

class SystemCardTime final : public CardTime {
public:
    std::uint64_t monotonic_ms() noexcept override;
    void sleep_ms(std::uint32_t milliseconds) noexcept override;
};

// Product transports implement the electrical layer. The T=1 framing, ATR
// parsing, EDC and timing policy stay in this adapter.
class CardHardware {
public:
    virtual ~CardHardware() noexcept = default;
    virtual Result<bool> detect_card() noexcept = 0;
    virtual Result<void> reset_card(CardDelay& delay) noexcept = 0;
    virtual Result<bool> data_ready() noexcept = 0;
    virtual Result<std::size_t> read_data(MutableByteView output) noexcept = 0;
    virtual Result<void> write_data(ByteView input) noexcept = 0;
    virtual Result<void> set_baud_rate(CardBaudRate baud_rate) noexcept = 0;
};

// Returns NOT_READY only when the supplied bytes are a valid but incomplete
// ATR prefix. Malformed ATR data returns PROTOCOL_ERROR.
Result<CardAtr> parse_card_atr(ByteView bytes) noexcept;

// Stateless card reset and ATR acquisition. Only a malformed ATR is retried,
// once, from a fresh hardware reset. No T=1 block is sent by this API.
Result<CardAtr> reset_and_read_card_atr(CardHardware& hardware, CardTime& time) noexcept;

class CardSession final {
public:
    CardSession(CardHardware& hardware, CardTime& time) noexcept
        : hardware_(hardware), time_(time)
    {
    }

    CardSession(const CardSession&) = delete;
    CardSession& operator=(const CardSession&) = delete;
    CardSession(CardSession&&) = delete;
    CardSession& operator=(CardSession&&) = delete;

    Result<void> initialize() noexcept;
    // Initializes T=1 from an ATR already acquired and validated by the caller.
    // The transport integration can reject unsupported ATR profiles before
    // consuming any card response or starting T=1 negotiation.
    Result<void> initialize_with_atr(const CardAtr& atr) noexcept;
    Result<std::size_t> transmit(ByteView apdu, MutableByteView response) noexcept;
    bool initialized() const noexcept { return initialized_; }
    const CardAtr& atr() const noexcept { return atr_; }
    void invalidate() noexcept;

private:
    struct Block final {
        std::uint8_t pcb = 0U;
        std::array<std::uint8_t, 254U> data{};
        std::size_t length = 0U;
    };

    Result<void> initialize_t1(bool resynchronize, std::uint8_t ifsd) noexcept;
    Result<void> send_block(std::uint8_t pcb, ByteView data) noexcept;
    Result<Block> receive_block(std::uint64_t deadline) noexcept;
    Result<Block> exchange_block(std::uint8_t pcb, ByteView data,
                                 std::uint64_t deadline,
                                 unsigned int max_retries = 3U) noexcept;
    Result<void> discard_pending_data() noexcept;
    Result<std::size_t> transmit_initialized(ByteView apdu,
                                             MutableByteView response,
                                             std::uint64_t deadline) noexcept;
    std::uint16_t calculate_crc(const std::uint8_t* data,
                                std::size_t length) const noexcept;

    CardHardware& hardware_;
    CardTime& time_;
    CardAtr atr_{};
    bool initialized_ = false;
    bool use_crc_ = false;
    std::uint8_t card_ifsc_ = 32U;
    std::uint32_t block_timeout_ms_ = 500U;
    std::uint8_t send_sequence_ = 0U;
    std::uint8_t receive_sequence_ = 0U;
};

}  // namespace dtv::userland::host_card_t1

#endif  // DTV_USERLAND_HOST_CARD_T1_CARD_T1_H
