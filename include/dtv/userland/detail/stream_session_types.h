// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H
#define DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>

// Neutral session identity and observation types shared by the receiver and
// capture components.  These declarations are internal and may change without
// notice; they are not a stable public API.
namespace dtv::userland::detail {

using ReceiverId = std::uint16_t;

inline constexpr std::size_t kReceiverTokenLength = 16U;
using ReceiverToken = std::array<std::uint8_t, kReceiverTokenLength>;

enum class ReceiverSystem : std::uint8_t {
    terrestrial = 1U,
    satellite = 2U,
};

struct Attachment final {
    std::uint64_t owner_client_id = 0U;
    std::uint64_t lease_id = 0U;
    std::uint64_t attachment_id = 0U;
    ReceiverId receiver = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    ReceiverToken token{};
    // The v1 stream contract requests evict-oldest queue handling; legacy
    // attachment behavior keeps the full-ring slow-consumer terminal.
    bool evict_oldest = false;
};

struct StreamCounters final {
    std::uint64_t packets = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t sync_errors = 0U;
    std::uint64_t tei_packets = 0U;
    std::uint64_t continuity_errors = 0U;
    std::uint64_t queue_drops = 0U;
    std::uint64_t usb_errors = 0U;
    std::uint64_t empty_intervals = 0U;
};

struct StreamFinalSnapshot final {
    StreamCounters counters{};
    std::uint8_t terminal = 0U;
};

enum class StreamTerminal : std::uint8_t {
    none = 0,
    slow_consumer = 1,
    usb_error = 2,
    sync_error = 3,
    bridge_fatal = 4,
    stopped = 5,
    disconnected = 6,
};

inline constexpr std::uint8_t kStreamTerminalDisconnected =
    static_cast<std::uint8_t>(StreamTerminal::disconnected);

struct StreamReadResult final {
    std::size_t bytes = 0U;
    bool eof = false;
    bool timed_out = false;
    StreamTerminal terminal = StreamTerminal::none;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H
