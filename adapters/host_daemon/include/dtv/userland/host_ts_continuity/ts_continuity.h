// Modified/ported for dtv-userland-core on 2026-10-30.
//
// Derived from px4-userland userland/include/px4/ts_continuity.h at
// 5c91e8c57b288c0f490061789829e42dbbf61b3d. The ASICEN e2a86ca vendor copy is
// byte-identical (sha256 dec396cf3a759c71c4f336ea8e9f384f18cff4941cab23178855c2ad92b6963b).
// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_TS_CONTINUITY_TS_CONTINUITY_H
#define DTV_USERLAND_HOST_TS_CONTINUITY_TS_CONTINUITY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dtv::userland::host_ts_continuity {

enum class TsContinuityKind : std::uint8_t {
    initial,
    continuous,
    duplicate,
    discontinuity,
    error,
    null_packet,
    transport_error,
    invalid_packet,
};

struct TsContinuityResult final {
    TsContinuityKind kind = TsContinuityKind::invalid_packet;
    std::uint16_t pid = 0U;
    std::uint8_t counter = 0U;
    bool has_payload = false;
    bool discontinuity = false;
};

// CC-only observation of complete 188-byte TS packets (H.222.0, 2.4.3.3–5).
// No transport, filtering, startup grace, counters, allocation, or clock API.
// This is NOT a full MPEG conformance validator: PMT/PCR_PID, clock accuracy,
// DI-run constraints and adaptation syntax other than its length/PCR are not
// checked. Each valid current DI packet rebases this bounded CC observer.
// TEI/invalid packets preserve trusted CC baselines, but invalidate duplicate
// adjacency: even their PID cannot be assumed reliable.
// Own one checker per stream, outside the realtime callback's stack; reset at
// each stream epoch. Storage is about 1.6 MiB, with constant work per packet.
class TsContinuityChecker final {
public:
    static constexpr std::size_t kPacketSize = 188U;
    static constexpr std::size_t kPidCount = 8192U;

    void reset() noexcept
    {
        // Snapshot bytes are inaccessible until seen is true; no large
        // temporary or packet-buffer clearing is needed at an epoch boundary.
        for (State& state : states_) state.seen = false;
        untrusted_epoch_ = 0U;
    }

    // Use when an upstream framer/transport reports an untrusted packet but
    // cannot supply its bytes (for example, a hardware-tagged TEI discard).
    // Preserve CC baselines while refusing to infer duplicate adjacency.
    void observe_untrusted_packet() noexcept
    {
        invalidate_duplicate_adjacency();
    }

    TsContinuityResult observe(const std::uint8_t* packet,
                               std::size_t size) noexcept
    {
        TsContinuityResult result;
        if (packet == nullptr || size != kPacketSize || packet[0U] != 0x47U) {
            invalidate_duplicate_adjacency();
            return result;
        }
        result.pid = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(packet[1U] & 0x1fU) << 8U) | packet[2U]);
        result.counter = static_cast<std::uint8_t>(packet[3U] & 0x0fU);
        if ((packet[1U] & 0x80U) != 0U) {
            result.kind = TsContinuityKind::transport_error;
            invalidate_duplicate_adjacency();
            return result;
        }
        if (result.pid == 0x1fffU) {
            result.kind = TsContinuityKind::null_packet;
            return result;
        }
        const std::uint8_t adaptation =
            static_cast<std::uint8_t>((packet[3U] >> 4U) & 3U);
        if (adaptation == 0U) return invalid(result);
        result.has_payload = (adaptation & 1U) != 0U;
        bool has_pcr = false;
        if ((adaptation & 2U) != 0U) {
            const std::size_t length = packet[4U];
            if ((adaptation == 2U && length != 183U) ||
                (adaptation == 3U && length > 182U))
                return invalid(result);
            if (length != 0U) {
                result.discontinuity = (packet[5U] & 0x80U) != 0U;
                has_pcr = (packet[5U] & 0x10U) != 0U;
                if (has_pcr) {
                    if (length < 7U) return invalid(result);
                    const unsigned extension =
                        (static_cast<unsigned>(packet[10U] & 1U) << 8U) | packet[11U];
                    if ((packet[10U] & 0x7eU) != 0x7eU || extension >= 300U)
                        return invalid(result);
                }
            }
        }

        State& state = states_[result.pid];
        bool repeated = false;
        if (result.discontinuity) {
            result.kind = TsContinuityKind::discontinuity;
        } else if (!state.seen) {
            result.kind = TsContinuityKind::initial;
        } else {
            const std::uint8_t expected = static_cast<std::uint8_t>(
                (state.counter + (result.has_payload ? 1U : 0U)) & 0x0fU);
            repeated = result.has_payload && result.counter == state.counter;
            if (repeated && state.has_payload && !state.repeated &&
                state.trusted_epoch == untrusted_epoch_ &&
                equivalent(state.packet, packet, has_pcr)) {
                result.kind = TsContinuityKind::duplicate;
            } else {
                result.kind = result.counter == expected
                    ? TsContinuityKind::continuous : TsContinuityKind::error;
            }
        }
        state.seen = true;
        state.counter = result.counter;
        state.has_payload = result.has_payload;
        // Remain consumed for every further same-CC payload, including a
        // conflicting packet. Never accept every other member of a long run.
        state.repeated = repeated;
        state.trusted_epoch = untrusted_epoch_;
        std::memcpy(state.packet.data(), packet, kPacketSize);
        return result;
    }

private:
    struct State final {
        std::uint64_t trusted_epoch = 0U;
        std::array<std::uint8_t, kPacketSize> packet{};
        std::uint8_t counter = 0U;
        bool seen = false;
        bool has_payload = false;
        bool repeated = false;
    };

    TsContinuityResult invalid(TsContinuityResult result) noexcept
    {
        invalidate_duplicate_adjacency();
        return result;
    }

    void invalidate_duplicate_adjacency() noexcept
    {
        if (untrusted_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            // Avoid epoch aliasing even after 2^64 invalid observations.
            for (State& state : states_) state.has_payload = false;
            untrusted_epoch_ = 0U;
        } else {
            ++untrusted_epoch_;
        }
    }

    static bool equivalent(const std::array<std::uint8_t, kPacketSize>& previous,
                           const std::uint8_t* packet, bool has_pcr) noexcept
    {
        if (!has_pcr) return std::memcmp(previous.data(), packet, kPacketSize) == 0;
        // A matching prefix includes AFC, adaptation length and PCR_flag.
        // Only the 42 PCR timestamp bits can differ, never reserved bits,
        // OPCR, flags, stuffing, or payload. Both PCRs were range-checked.
        return std::memcmp(previous.data(), packet, 6U) == 0 &&
            (previous[10U] & 0x7eU) == (packet[10U] & 0x7eU) &&
            std::memcmp(previous.data() + 12U, packet + 12U, kPacketSize - 12U) == 0;
    }

    std::array<State, kPidCount> states_{};
    std::uint64_t untrusted_epoch_ = 0U;
};

}  // namespace dtv::userland::host_ts_continuity

#endif  // DTV_USERLAND_HOST_TS_CONTINUITY_TS_CONTINUITY_H
