// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/tests/card_tests.cpp at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d, which is derived from
// tsukumijima/px4_drv commit 9eedea8c502875a788697984b93b50032339b9aa.
// ASICEN modification, 2026-10-09: adds checks for initialize_with_atr().
// The fixture case names and expected values are preserved; only the Core
// namespace/type seams differ.
// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_card_t1/card_protocol_session.h"
#include "dtv/userland/host_card_t1/card_t1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

using namespace dtv::userland::host_card_t1;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;

constexpr std::array<std::uint8_t, 13U> kBcasAtr1{
    0x3bU, 0xf0U, 0x12U, 0x00U, 0xffU, 0x91U, 0x81U,
    0xb1U, 0x7cU, 0x45U, 0x1fU, 0x01U, 0x9bU};
constexpr std::array<std::uint8_t, 13U> kBcasAtr2{
    0x3bU, 0xf0U, 0x12U, 0x00U, 0xffU, 0x91U, 0x81U,
    0xb1U, 0x7cU, 0x45U, 0x1fU, 0x03U, 0x99U};

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "card check failed at line %d: %s\n", __LINE__, \
                         #condition);                                                 \
            return false;                                                            \
        }                                                                            \
    } while (false)

template <std::size_t Size>
ByteView view(const std::array<std::uint8_t, Size>& bytes) noexcept
{
    return ByteView{bytes.data(), bytes.size()};
}

class FakeTime final : public CardTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now_ms; }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        sleeps.push_back(milliseconds);
        now_ms += milliseconds;
    }

    std::uint64_t now_ms = 0U;
    std::vector<std::uint32_t> sleeps;
};

class MockCardHardware final : public CardHardware {
public:
    Result<bool> detect_card() noexcept override
    {
        ++detect_count;
        if (detect_error != Error::OK) {
            const Error error = detect_error;
            detect_error = Error::OK;
            return Result<bool>::failure(error);
        }
        return Result<bool>::success(present);
    }

    Result<void> reset_card(CardDelay& delay) noexcept override
    {
        ++reset_count;
        delay_was_supplied = true;
        if (reset_error != Error::OK) {
            return Result<void>::failure(reset_error);
        }
        delay.sleep_ms(5U);
        receive.clear();
        response_sequence = 0U;
        last_response_pcb = 0U;
        last_response.clear();
        final_response_lost = false;
        chained_response_pending = false;
        const std::size_t index = static_cast<std::size_t>(reset_count - 1U);
        const std::vector<std::uint8_t> default_atr(kBcasAtr1.begin(), kBcasAtr1.end());
        const auto& selected = atr_attempts.empty()
                                   ? default_atr
                                   : atr_attempts[std::min(index, atr_attempts.size() - 1U)];
        receive.insert(receive.end(), selected.begin(), selected.end());
        read_permitted = false;
        return Result<void>::success();
    }

    Result<bool> data_ready() noexcept override
    {
        ++ready_checks;
        if (time_to_advance != nullptr) {
            time_to_advance->now_ms += ready_advance_ms;
        }
        if (ready_error != Error::OK) {
            const Error error = ready_error;
            ready_error = Error::OK;
            return Result<bool>::failure(error);
        }
        if (never_ready) {
            return Result<bool>::success(false);
        }
        if (ready_delay_checks != 0U) {
            --ready_delay_checks;
            return Result<bool>::success(false);
        }
        read_permitted = !receive.empty();
        return Result<bool>::success(read_permitted);
    }

    Result<std::size_t> read_data(MutableByteView output) noexcept override
    {
        ++read_count;
        if (!read_permitted) {
            read_before_ready = true;
        }
        read_permitted = false;
        if (read_error != Error::OK) {
            return Result<std::size_t>::failure(read_error);
        }
        if (zero_read_checks != 0U) {
            --zero_read_checks;
            return Result<std::size_t>::success(0U);
        }
        std::size_t count = std::min(output.size, receive.size());
        if (read_chunk_limit != 0U) {
            count = std::min(count, read_chunk_limit);
        }
        for (std::size_t index = 0U; index < count; ++index) {
            output.data[index] = receive.front();
            receive.pop_front();
        }
        return Result<std::size_t>::success(count);
    }

    Result<void> write_data(ByteView input) noexcept override
    {
        if (input.data == nullptr || input.size < 4U || input.size > 255U) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        if (fail_next_write) {
            fail_next_write = false;
            return Result<void>::failure(Error::USB_IO);
        }
        const std::uint8_t pcb = input.data[1];
        const std::uint8_t data_length = input.data[2];
        const std::size_t expected_length = static_cast<std::size_t>(data_length) +
                                            (expected_crc ? 5U : 4U);
        if (expected_length != input.size || !validate_edc(input.data, input.size)) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        written_pcbs.push_back(pcb);
        if (pcb == 0xc1U && data_length == 1U) {
            ifs_values.push_back(input.data[3]);
        }
        if (drop_first_ifs && pcb == 0xc1U) {
            drop_first_ifs = false;
            return Result<void>::success();
        }

        if ((pcb & 0xc0U) == 0xc0U) {
            if (endless_wtx && pcb == 0xe3U) {
                constexpr std::uint8_t multiplier = 1U;
                queue_block(0xc3U, &multiplier, 1U);
            } else {
                queue_block(static_cast<std::uint8_t>(pcb | 0x20U),
                            input.data + 3U, data_length);
            }
            return Result<void>::success();
        }
        if (endless_wtx) {
            constexpr std::uint8_t multiplier = 1U;
            queue_block(0xc3U, &multiplier, 1U);
            return Result<void>::success();
        }

        if ((pcb & 0x80U) == 0U) {
            apdu_block_lengths.push_back(data_length);
            if ((pcb & 0x20U) != 0U) {
                const std::uint8_t next_sequence = (pcb & 0x40U) != 0U ? 0U : 0x10U;
                queue_block(static_cast<std::uint8_t>(0x80U | next_sequence), nullptr, 0U);
                return Result<void>::success();
            }
        }

        if ((pcb & 0xc0U) == 0x80U && chained_response_pending) {
            chained_response_pending = false;
            constexpr std::array<std::uint8_t, 2U> tail{0x90U, 0x00U};
            const std::uint8_t response_pcb = response_sequence != 0U ? 0x40U : 0x00U;
            response_sequence ^= 1U;
            queue_block(response_pcb, tail.data(), tail.size());
            return Result<void>::success();
        }
        if ((pcb & 0xc0U) == 0x80U && !last_response.empty()) {
            const std::uint8_t expected_sequence =
                (last_response_pcb & 0x40U) != 0U ? 0x10U : 0x00U;
            if ((pcb & 0x10U) == expected_sequence) {
                ++cached_resend_count;
                queue_block(last_response_pcb, last_response.data(), last_response.size());
                return Result<void>::success();
            }
        }
        if (final_response_lost) {
            final_response_lost = false;
            const std::uint8_t next_sequence = (pcb & 0x40U) != 0U ? 0U : 0x10U;
            queue_block(static_cast<std::uint8_t>(0x80U | next_sequence), nullptr, 0U);
            return Result<void>::success();
        }

        const std::uint8_t response_pcb = response_sequence != 0U ? 0x40U : 0x00U;
        response_sequence ^= 1U;
        constexpr std::array<std::uint8_t, 2U> response{0x90U, 0x00U};
        last_response_pcb = response_pcb;
        last_response.assign(response.begin(), response.end());
        if (lose_final_response) {
            lose_final_response = false;
            final_response_lost = true;
            return Result<void>::success();
        }
        if (response_chain_once) {
            response_chain_once = false;
            chained_response_pending = true;
            constexpr std::array<std::uint8_t, 2U> head{0x61U, 0x02U};
            queue_block(static_cast<std::uint8_t>(response_pcb | 0x20U),
                        head.data(), head.size());
            return Result<void>::success();
        }
        if (large_duplicate_response) {
            large_duplicate_response = false;
            const std::vector<std::uint8_t> large(200U, 0xa5U);
            queue_block(response_pcb, large.data(), large.size());
            queue_block(response_pcb, large.data(), large.size());
            return Result<void>::success();
        }
        if (max_duplicate_response) {
            max_duplicate_response = false;
            const std::vector<std::uint8_t> maximum(251U, 0x5aU);
            queue_block(response_pcb, maximum.data(), maximum.size());
            queue_block(response_pcb, maximum.data(), maximum.size());
            return Result<void>::success();
        }
        queue_block(response_pcb, response.data(), response.size());
        if (duplicate_next_response) {
            duplicate_next_response = false;
            queue_block(response_pcb, response.data(), response.size());
        }
        return Result<void>::success();
    }

    void queue_block(std::uint8_t pcb, const std::uint8_t* data,
                     std::size_t data_length)
    {
        std::vector<std::uint8_t> frame{0x00U, pcb,
                                        static_cast<std::uint8_t>(data_length)};
        if (data_length != 0U) {
            frame.insert(frame.end(), data, data + data_length);
        }
        if (expected_crc) {
            const std::uint16_t crc = calculate_crc(frame.data(), frame.size());
            frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
            frame.push_back(static_cast<std::uint8_t>(crc));
        } else {
            std::uint8_t lrc = 0U;
            for (const std::uint8_t value : frame) {
                lrc = static_cast<std::uint8_t>(lrc ^ value);
            }
            frame.push_back(lrc);
        }
        if (truncate_queue_bytes != 0U) {
            const std::size_t remove = std::min(truncate_queue_bytes, frame.size());
            frame.resize(frame.size() - remove);
        }
        if (corruption == Corruption::bad_edc) {
            frame.back() ^= 1U;
        } else if (corruption == Corruption::bad_nad) {
            frame[0] = 1U;
            if (expected_crc) {
                const std::uint16_t crc = calculate_crc(frame.data(), frame.size() - 2U);
                frame[frame.size() - 2U] = static_cast<std::uint8_t>(crc >> 8U);
                frame.back() = static_cast<std::uint8_t>(crc);
            } else {
                std::uint8_t lrc = 0U;
                for (std::size_t index = 0U; index + 1U < frame.size(); ++index) {
                    lrc = static_cast<std::uint8_t>(lrc ^ frame[index]);
                }
                frame.back() = lrc;
            }
        } else if (corruption == Corruption::bad_length) {
            frame[2] = static_cast<std::uint8_t>(frame[2] + 1U);
        } else if (corruption == Corruption::oversized_length) {
            frame[2] = expected_crc ? 251U : 252U;
            last_declared_length = frame[2];
        }
        receive.insert(receive.end(), frame.begin(), frame.end());
    }

    void queue_stale_response(std::uint8_t pcb)
    {
        constexpr std::array<std::uint8_t, 2U> response{0x90U, 0x00U};
        queue_block(pcb, response.data(), response.size());
    }

    static std::uint16_t calculate_crc(const std::uint8_t* data, std::size_t length)
    {
        std::uint16_t crc = 0xffffU;
        for (std::size_t index = 0U; index < length; ++index) {
            crc ^= static_cast<std::uint16_t>(data[index]) << 8U;
            for (unsigned int bit = 0U; bit < 8U; ++bit) {
                crc = (crc & 0x8000U) != 0U
                          ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                          : static_cast<std::uint16_t>(crc << 1U);
            }
        }
        return crc;
    }

    bool validate_edc(const std::uint8_t* frame, std::size_t length) const
    {
        if (expected_crc) {
            const std::uint16_t crc = calculate_crc(frame, length - 2U);
            return frame[length - 2U] == static_cast<std::uint8_t>(crc >> 8U) &&
                   frame[length - 1U] == static_cast<std::uint8_t>(crc);
        }
        std::uint8_t lrc = 0U;
        for (std::size_t index = 0U; index < length; ++index) {
            lrc = static_cast<std::uint8_t>(lrc ^ frame[index]);
        }
        return lrc == 0U;
    }

    enum class Corruption : std::uint8_t {
        none,
        bad_edc,
        bad_nad,
        bad_length,
        oversized_length,
    };

    Result<void> set_baud_rate(CardBaudRate baud_rate) noexcept override
    {
        ++baud_set_count;
        selected_baud = baud_rate;
        return baud_error == Error::OK ? Result<void>::success()
                                       : Result<void>::failure(baud_error);
    }

    bool present = true;
    bool never_ready = false;
    bool read_before_ready = false;
    bool read_permitted = false;
    bool delay_was_supplied = false;
    bool expected_crc = false;
    bool drop_first_ifs = false;
    bool endless_wtx = false;
    bool fail_next_write = false;
    bool duplicate_next_response = false;
    bool large_duplicate_response = false;
    bool max_duplicate_response = false;
    bool lose_final_response = false;
    bool final_response_lost = false;
    bool response_chain_once = false;
    bool chained_response_pending = false;
    unsigned int detect_count = 0U;
    unsigned int reset_count = 0U;
    unsigned int ready_checks = 0U;
    unsigned int ready_delay_checks = 0U;
    unsigned int read_count = 0U;
    unsigned int zero_read_checks = 0U;
    std::size_t read_chunk_limit = 0U;
    std::size_t truncate_queue_bytes = 0U;
    unsigned int baud_set_count = 0U;
    unsigned int cached_resend_count = 0U;
    std::uint8_t response_sequence = 0U;
    std::uint8_t last_response_pcb = 0U;
    std::uint8_t last_declared_length = 0U;
    Corruption corruption = Corruption::none;
    FakeTime* time_to_advance = nullptr;
    std::uint32_t ready_advance_ms = 0U;
    Error detect_error = Error::OK;
    Error reset_error = Error::OK;
    Error ready_error = Error::OK;
    Error read_error = Error::OK;
    Error baud_error = Error::OK;
    CardBaudRate selected_baud = CardBaudRate::baud_9600;
    std::vector<std::vector<std::uint8_t>> atr_attempts;
    std::vector<std::uint8_t> written_pcbs;
    std::vector<std::uint8_t> ifs_values;
    std::vector<std::uint8_t> apdu_block_lengths;
    std::vector<std::uint8_t> last_response;
    std::deque<std::uint8_t> receive;
};

bool test_known_atrs_and_parameters()
{
    for (const auto* known : {&kBcasAtr1, &kBcasAtr2}) {
        const auto parsed = parse_card_atr(view(*known));
        CHECK(parsed);
        CHECK(parsed.value().length == known->size());
        CHECK(parsed.value().baud_rate == CardBaudRate::baud_19200);
        CHECK(parsed.value().ifsc == 0x7cU);
        CHECK(parsed.value().edc == CardEdc::lrc);
        CHECK(parsed.value().block_timeout_ms == 1600U);
        CHECK(std::equal(known->begin(), known->end(), parsed.value().bytes.begin()));
    }

    constexpr std::array<std::uint8_t, 8U> crc_atr{
        0x3bU, 0x80U, 0x91U, 0x01U, 0x51U, 0x40U, 0x01U, 0x00U};
    const auto crc = parse_card_atr(view(crc_atr));
    CHECK(crc && crc.value().ifsc == 0x40U && crc.value().edc == CardEdc::crc);
    return true;
}

bool test_parse_failures()
{
    constexpr std::array<std::uint8_t, 3U> partial{0x3bU, 0xf0U, 0x12U};
    CHECK(!parse_card_atr(view(partial)) &&
          parse_card_atr(view(partial)).error() == Error::NOT_READY);

    constexpr std::array<std::uint8_t, 2U> bad_ts{0x00U, 0x00U};
    CHECK(!parse_card_atr(view(bad_ts)) &&
          parse_card_atr(view(bad_ts)).error() == Error::PROTOCOL_ERROR);

    auto bad_tck = kBcasAtr1;
    bad_tck.back() ^= 1U;
    CHECK(!parse_card_atr(view(bad_tck)) &&
          parse_card_atr(view(bad_tck)).error() == Error::PROTOCOL_ERROR);

    std::array<std::uint8_t, 34U> oversized{};
    oversized[0] = 0x3bU;
    CHECK(!parse_card_atr(view(oversized)) &&
          parse_card_atr(view(oversized)).error() == Error::PROTOCOL_ERROR);

    auto unsupported = kBcasAtr1;
    unsupported[2] = 0x14U;
    unsupported.back() = 0x9dU;
    CHECK(!parse_card_atr(view(unsupported)) &&
          parse_card_atr(view(unsupported)).error() == Error::UNSUPPORTED);
    return true;
}

bool test_success_waits_for_ready_and_sets_baud()
{
    MockCardHardware hardware;
    hardware.ready_delay_checks = 2U;
    hardware.atr_attempts.push_back({kBcasAtr1.begin(), kBcasAtr1.end()});
    FakeTime time;
    const auto atr = reset_and_read_card_atr(hardware, time);
    CHECK(atr);
    CHECK(hardware.detect_count == 1U);
    CHECK(hardware.reset_count == 1U);
    CHECK(hardware.delay_was_supplied);
    CHECK(hardware.ready_checks == 3U);
    CHECK(hardware.read_count == 1U);
    CHECK(!hardware.read_before_ready);
    CHECK(hardware.baud_set_count == 1U);
    CHECK(hardware.selected_baud == CardBaudRate::baud_19200);
    CHECK((time.sleeps == std::vector<std::uint32_t>{5U, 5U, 5U}));
    return true;
}

bool test_malformed_only_retry_policy()
{
    auto malformed = kBcasAtr1;
    malformed.back() ^= 1U;
    MockCardHardware hardware;
    hardware.atr_attempts.push_back({malformed.begin(), malformed.end()});
    hardware.atr_attempts.push_back({kBcasAtr2.begin(), kBcasAtr2.end()});
    FakeTime time;
    const auto recovered = reset_and_read_card_atr(hardware, time);
    CHECK(recovered);
    CHECK(hardware.reset_count == 2U);
    CHECK(hardware.baud_set_count == 1U);

    MockCardHardware timeout_hardware;
    timeout_hardware.never_ready = true;
    timeout_hardware.atr_attempts.push_back({kBcasAtr1.begin(), kBcasAtr1.end()});
    FakeTime timeout_time;
    const auto timeout = reset_and_read_card_atr(timeout_hardware, timeout_time);
    CHECK(!timeout && timeout.error() == Error::TIMEOUT);
    CHECK(timeout_hardware.reset_count == 1U);
    CHECK(timeout_hardware.read_count == 0U);
    CHECK(timeout_hardware.baud_set_count == 0U);

    MockCardHardware usb_hardware;
    usb_hardware.ready_error = Error::USB_IO;
    usb_hardware.atr_attempts.push_back({kBcasAtr1.begin(), kBcasAtr1.end()});
    FakeTime usb_time;
    const auto usb = reset_and_read_card_atr(usb_hardware, usb_time);
    CHECK(!usb && usb.error() == Error::USB_IO);
    CHECK(usb_hardware.reset_count == 1U);
    CHECK(usb_hardware.read_count == 0U);
    CHECK(usb_hardware.baud_set_count == 0U);
    return true;
}

bool test_no_card_and_incomplete_timeout()
{
    MockCardHardware absent;
    absent.present = false;
    FakeTime absent_time;
    const auto no_card = reset_and_read_card_atr(absent, absent_time);
    CHECK(!no_card && no_card.error() == Error::NO_CARD);
    CHECK(absent.reset_count == 0U);

    MockCardHardware partial;
    partial.atr_attempts.push_back({0x3bU, 0xf0U, 0x12U});
    FakeTime partial_time;
    const auto incomplete = reset_and_read_card_atr(partial, partial_time);
    CHECK(!incomplete && incomplete.error() == Error::TIMEOUT);
    CHECK(partial.reset_count == 1U);
    CHECK(partial.read_count == 1U);
    CHECK(!partial.read_before_ready);
    return true;
}

bool test_t1_initialize_and_consecutive_apdus()
{
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());
    CHECK(session.initialized());
    CHECK(session.atr().length == kBcasAtr1.size());
    CHECK((hardware.written_pcbs == std::vector<std::uint8_t>{0xc0U, 0xc1U}));
    CHECK((hardware.ifs_values == std::vector<std::uint8_t>{251U}));

    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    const auto first = session.transmit(view(apdu),
                                        MutableByteView{response.data(), response.size()});
    CHECK(first && first.value() == 2U && response[0] == 0x90U && response[1] == 0x00U);
    const auto second = session.transmit(view(apdu),
                                         MutableByteView{response.data(), response.size()});
    CHECK(second && second.value() == 2U);
    CHECK(hardware.written_pcbs.size() == 4U);
    CHECK(hardware.written_pcbs[2] == 0x00U);
    CHECK(hardware.written_pcbs[3] == 0x40U);
    CHECK(!hardware.read_before_ready);
    return true;
}

bool test_short_read_receive_patterns()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};

    for (const std::size_t chunk : {3U, 1U}) {
        MockCardHardware hardware;
        FakeTime time;
        CardSession session(hardware, time);
        CHECK(session.initialize());
        hardware.read_chunk_limit = chunk;
        const auto transmitted = session.transmit(
            view(apdu), MutableByteView{response.data(), response.size()});
        CHECK(transmitted && transmitted.value() == 2U);
        CHECK(response[0] == 0x90U && response[1] == 0x00U);
        CHECK(!hardware.read_before_ready);
    }

    constexpr std::array<std::uint8_t, 8U> crc_atr{
        0x3bU, 0x80U, 0x91U, 0x01U, 0x51U, 0x40U, 0x01U, 0x00U};
    MockCardHardware crc_hardware;
    crc_hardware.expected_crc = true;
    crc_hardware.atr_attempts.push_back({crc_atr.begin(), crc_atr.end()});
    FakeTime crc_time;
    CardSession crc_session(crc_hardware, crc_time);
    CHECK(crc_session.initialize());
    crc_hardware.read_chunk_limit = 2U;
    const auto crc = crc_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(crc && crc.value() == 2U);

    MockCardHardware zero_read;
    FakeTime zero_time;
    CardSession zero_session(zero_read, zero_time);
    CHECK(zero_session.initialize());
    zero_read.zero_read_checks = 1U;
    const auto after_zero = zero_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(after_zero && after_zero.value() == 2U);
    CHECK(!zero_time.sleeps.empty());

    MockCardHardware incomplete;
    FakeTime incomplete_time;
    CardSession incomplete_session(incomplete, incomplete_time);
    CHECK(incomplete_session.initialize());
    incomplete.truncate_queue_bytes = 1U;
    incomplete.time_to_advance = &incomplete_time;
    incomplete.ready_advance_ms = 100U;
    const auto timed_out = incomplete_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!timed_out && timed_out.error() == Error::TIMEOUT);
    CHECK(!incomplete_session.initialized());
    return true;
}

bool test_initialize_from_prevalidated_atr()
{
    const auto atr = parse_card_atr(view(kBcasAtr1));
    CHECK(atr);
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize_with_atr(atr.value()));
    CHECK(session.initialized());
    CHECK(session.atr().length == kBcasAtr1.size());
    CHECK(hardware.detect_count == 0U);
    CHECK(hardware.reset_count == 0U);
    CHECK((hardware.written_pcbs == std::vector<std::uint8_t>{0xc0U, 0xc1U}));

    MockCardHardware invalid_hardware;
    FakeTime invalid_time;
    CardSession invalid_session(invalid_hardware, invalid_time);
    CardAtr altered = atr.value();
    altered.bytes[0] ^= 0x01U;
    const auto invalid = invalid_session.initialize_with_atr(altered);
    CHECK(!invalid && invalid.error() == Error::PROTOCOL_ERROR);
    CHECK(invalid_hardware.reset_count == 0U);

    CardAtr empty{};
    CHECK(session.initialize_with_atr(empty).error() == Error::INVALID_ARGUMENT);
    CHECK(!session.initialized());
    CardAtr oversized = atr.value();
    oversized.length = oversized.bytes.size() + 1U;
    CHECK(session.initialize_with_atr(oversized).error() == Error::INVALID_ARGUMENT);
    CHECK(!session.initialized());
    CHECK(session.initialize_with_atr(atr.value()));
    CHECK(session.initialized());
    CHECK(hardware.detect_count == 0U && hardware.reset_count == 0U);
    CHECK((hardware.written_pcbs ==
           std::vector<std::uint8_t>{0xc0U, 0xc1U, 0xc0U, 0xc1U}));
    CHECK(session.initialize());
    CHECK(hardware.detect_count == 1U && hardware.reset_count == 1U);
    CHECK((hardware.written_pcbs ==
           std::vector<std::uint8_t>{0xc0U, 0xc1U, 0xc0U, 0xc1U, 0xc0U, 0xc1U}));
    return true;
}

bool test_duplicate_response_discard_and_recovery()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());

    std::array<std::uint8_t, 256U> response{};
    hardware.duplicate_next_response = true;
    const auto duplicate = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(duplicate && duplicate.value() == 2U && hardware.receive.empty());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));

    hardware.large_duplicate_response = true;
    const auto large = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(large && large.value() == 200U && hardware.receive.empty());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));

    hardware.max_duplicate_response = true;
    const auto maximum = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(maximum && maximum.value() == 251U && hardware.receive.empty());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    CHECK(!hardware.read_before_ready);
    return true;
}

bool test_invalidation_and_reinitialization()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};

    MockCardHardware initially_absent;
    initially_absent.present = false;
    FakeTime initially_absent_time;
    CardSession initially_absent_session(initially_absent, initially_absent_time);
    const auto no_card = initially_absent_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!no_card && no_card.error() == Error::NO_CARD &&
          !initially_absent_session.initialized());

    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());

    hardware.present = false;
    const auto removed = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!removed && removed.error() == Error::CARD_REMOVED && !session.initialized());
    hardware.present = true;
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    CHECK(hardware.reset_count == 2U);

    hardware.detect_error = Error::USB_IO;
    const auto detect_error = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!detect_error && detect_error.error() == Error::USB_IO && !session.initialized());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    CHECK(hardware.reset_count == 3U);

    hardware.fail_next_write = true;
    const auto transport_error = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!transport_error && transport_error.error() == Error::USB_IO &&
          !session.initialized());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    CHECK(hardware.reset_count == 4U);
    return true;
}

bool test_wtx_timeout_and_lost_response_recovery()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};

    MockCardHardware wtx_hardware;
    FakeTime wtx_time;
    CardSession wtx_session(wtx_hardware, wtx_time);
    CHECK(wtx_session.initialize());
    wtx_hardware.endless_wtx = true;
    wtx_hardware.time_to_advance = &wtx_time;
    wtx_hardware.ready_advance_ms = 10U;
    const std::uint64_t started = wtx_time.now_ms;
    const auto timeout = wtx_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!timeout && timeout.error() == Error::TIMEOUT && !wtx_session.initialized());
    CHECK(wtx_time.now_ms - started <= 3020U);
    wtx_hardware.endless_wtx = false;
    wtx_hardware.time_to_advance = nullptr;
    CHECK(wtx_session.transmit(view(apdu),
                               MutableByteView{response.data(), response.size()}));

    MockCardHardware lost_hardware;
    FakeTime lost_time;
    CardSession lost_session(lost_hardware, lost_time);
    CHECK(lost_session.initialize());
    lost_hardware.lose_final_response = true;
    const auto recovered = lost_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(recovered && recovered.value() == 2U);
    CHECK(lost_hardware.cached_resend_count == 1U);
    CHECK(lost_hardware.reset_count == 1U);
    return true;
}

bool test_stale_sequence_rejected_then_reinitialized()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    hardware.queue_stale_response(hardware.response_sequence != 0U ? 0x00U : 0x40U);
    const auto stale = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!stale && stale.error() == Error::PROTOCOL_ERROR && !session.initialized());
    CHECK(session.transmit(view(apdu), MutableByteView{response.data(), response.size()}));
    CHECK(hardware.reset_count == 2U);
    return true;
}

bool test_crc_and_malformed_frames()
{
    constexpr std::array<std::uint8_t, 8U> crc_atr{
        0x3bU, 0x80U, 0x91U, 0x01U, 0x51U, 0x40U, 0x01U, 0x00U};
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};

    MockCardHardware crc_hardware;
    crc_hardware.expected_crc = true;
    crc_hardware.atr_attempts.push_back({crc_atr.begin(), crc_atr.end()});
    FakeTime crc_time;
    CardSession crc_session(crc_hardware, crc_time);
    CHECK(crc_session.initialize());
    CHECK((crc_hardware.ifs_values == std::vector<std::uint8_t>{250U}));
    CHECK(crc_session.transmit(view(apdu),
                               MutableByteView{response.data(), response.size()}));

    for (const auto corruption : {MockCardHardware::Corruption::bad_edc,
                                  MockCardHardware::Corruption::bad_nad}) {
        MockCardHardware malformed;
        malformed.corruption = corruption;
        FakeTime malformed_time;
        CardSession malformed_session(malformed, malformed_time);
        const auto initialized = malformed_session.initialize();
        CHECK(!initialized && initialized.error() == Error::PROTOCOL_ERROR);
        CHECK(!malformed_session.initialized());
    }

    // A declared length longer than the available bytes is now accumulated
    // until the existing block deadline instead of being rejected as an
    // immediate short read. The incomplete-frame classification is therefore
    // the existing deadline TIMEOUT, not PROTOCOL_ERROR.
    MockCardHardware short_length;
    short_length.corruption = MockCardHardware::Corruption::bad_length;
    FakeTime short_length_time;
    CardSession short_length_session(short_length, short_length_time);
    const auto short_initialized = short_length_session.initialize();
    CHECK(!short_initialized && short_initialized.error() == Error::TIMEOUT);
    CHECK(!short_length_session.initialized());

    MockCardHardware oversized_lrc;
    FakeTime oversized_lrc_time;
    CardSession oversized_lrc_session(oversized_lrc, oversized_lrc_time);
    CHECK(oversized_lrc_session.initialize());
    oversized_lrc.corruption = MockCardHardware::Corruption::oversized_length;
    const unsigned int lrc_ready_before = oversized_lrc.ready_checks;
    const unsigned int lrc_read_before = oversized_lrc.read_count;
    const std::uint64_t lrc_time_before = oversized_lrc_time.now_ms;
    const auto lrc_limit = oversized_lrc_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!lrc_limit && lrc_limit.error() == Error::PROTOCOL_ERROR &&
          !oversized_lrc_session.initialized());
    CHECK(oversized_lrc.last_declared_length == 252U);
    CHECK(oversized_lrc.ready_checks - lrc_ready_before == 3U);
    CHECK(oversized_lrc.read_count - lrc_read_before == 3U);
    CHECK(oversized_lrc_time.now_ms == lrc_time_before);

    MockCardHardware oversized_crc;
    oversized_crc.expected_crc = true;
    oversized_crc.atr_attempts.push_back({crc_atr.begin(), crc_atr.end()});
    FakeTime oversized_crc_time;
    CardSession oversized_crc_session(oversized_crc, oversized_crc_time);
    CHECK(oversized_crc_session.initialize());
    oversized_crc.corruption = MockCardHardware::Corruption::oversized_length;
    const unsigned int crc_ready_before = oversized_crc.ready_checks;
    const unsigned int crc_read_before = oversized_crc.read_count;
    const std::uint64_t crc_time_before = oversized_crc_time.now_ms;
    const auto crc_limit = oversized_crc_session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(!crc_limit && crc_limit.error() == Error::PROTOCOL_ERROR &&
          !oversized_crc_session.initialized());
    CHECK(oversized_crc.last_declared_length == 251U);
    CHECK(oversized_crc.ready_checks - crc_ready_before == 3U);
    CHECK(oversized_crc.read_count - crc_read_before == 3U);
    CHECK(oversized_crc_time.now_ms == crc_time_before);
    return true;
}

bool test_acas_fallback_bwi_and_uart_limit()
{
    constexpr std::array<std::uint8_t, 13U> acas_atr{
        0x3bU, 0xf0U, 0x13U, 0x00U, 0xffU, 0x91U, 0x81U,
        0xb1U, 0xfeU, 0x46U, 0x1fU, 0x03U, 0x19U};
    MockCardHardware hardware;
    hardware.atr_attempts.push_back({acas_atr.begin(), acas_atr.end()});
    hardware.drop_first_ifs = true;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());
    CHECK((hardware.written_pcbs == std::vector<std::uint8_t>{0xc1U, 0xc0U, 0xc1U}));
    CHECK((hardware.ifs_values == std::vector<std::uint8_t>{254U, 254U}));
    CHECK(session.atr().block_timeout_ms == 1600U);

    constexpr std::array<std::uint8_t, 5U> short_apdu{
        0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    hardware.ready_delay_checks = 120U;
    CHECK(session.transmit(view(short_apdu),
                           MutableByteView{response.data(), response.size()}));

    std::array<std::uint8_t, 254U> long_apdu{};
    const auto long_response = session.transmit(
        view(long_apdu), MutableByteView{response.data(), response.size()});
    CHECK(long_response && long_response.value() == 2U);
    CHECK(hardware.apdu_block_lengths.size() >= 3U);
    const std::size_t count = hardware.apdu_block_lengths.size();
    CHECK(hardware.apdu_block_lengths[count - 2U] == 251U);
    CHECK(hardware.apdu_block_lengths[count - 1U] == 3U);
    return true;
}

bool test_response_chaining_capacity_and_invalid_arguments()
{
    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CHECK(session.initialize());
    hardware.response_chain_once = true;
    std::array<std::uint8_t, 16U> response{};
    const auto chained = session.transmit(
        view(apdu), MutableByteView{response.data(), response.size()});
    CHECK(chained && chained.value() == 4U);
    CHECK(response[0] == 0x61U && response[1] == 0x02U &&
          response[2] == 0x90U && response[3] == 0x00U);

    std::array<std::uint8_t, 1U> too_small{};
    const auto capacity = session.transmit(
        view(apdu), MutableByteView{too_small.data(), too_small.size()});
    CHECK(!capacity && capacity.error() == Error::BUFFER_TOO_SMALL &&
          !session.initialized());

    CHECK(session.initialize());
    const auto null_input = session.transmit(
        ByteView{nullptr, 5U}, MutableByteView{response.data(), response.size()});
    CHECK(!null_input && null_input.error() == Error::INVALID_ARGUMENT &&
          !session.initialized());
    CHECK(session.initialize());
    const auto empty_input = session.transmit(
        ByteView{apdu.data(), 0U}, MutableByteView{response.data(), response.size()});
    CHECK(!empty_input && empty_input.error() == Error::INVALID_ARGUMENT &&
          !session.initialized());
    CHECK(session.initialize());
    const auto null_output = session.transmit(
        view(apdu), MutableByteView{nullptr, response.size()});
    CHECK(!null_output && null_output.error() == Error::INVALID_ARGUMENT &&
          !session.initialized());
    return true;
}

bool test_protocol_session_adapter()
{
    MockCardHardware hardware;
    FakeTime time;
    CardSession session(hardware, time);
    CardProtocolSessionAdapter adapter(session);
    CHECK(adapter.initialize());
    CHECK(adapter.initialized());
    CHECK(adapter.atr().length == kBcasAtr1.size());
    CHECK(std::equal(kBcasAtr1.begin(), kBcasAtr1.end(), adapter.atr().bytes.begin()));

    constexpr std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    const auto transmitted = adapter.transmit(
        ByteView{apdu.data(), apdu.size()},
        MutableByteView{response.data(), response.size()});
    CHECK(transmitted && transmitted.value() == 2U);

    adapter.invalidate();
    CHECK(!adapter.initialized());
    CHECK(adapter.atr().length == 0U);
    return true;
}

}  // namespace

int main()
{
    if (!test_known_atrs_and_parameters() || !test_parse_failures() ||
        !test_success_waits_for_ready_and_sets_baud() ||
        !test_malformed_only_retry_policy() ||
        !test_no_card_and_incomplete_timeout() ||
        !test_t1_initialize_and_consecutive_apdus() ||
        !test_short_read_receive_patterns() ||
        !test_initialize_from_prevalidated_atr() ||
        !test_duplicate_response_discard_and_recovery() ||
        !test_invalidation_and_reinitialization() ||
        !test_wtx_timeout_and_lost_response_recovery() ||
        !test_stale_sequence_rejected_then_reinitialized() ||
        !test_crc_and_malformed_frames() ||
        !test_acas_fallback_bwi_and_uart_limit() ||
        !test_response_chaining_capacity_and_invalid_arguments() ||
        !test_protocol_session_adapter()) {
        return 1;
    }
    std::printf("host card T=1 regression fixtures passed\n");
    return 0;
}
