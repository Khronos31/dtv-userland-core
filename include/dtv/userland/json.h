// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_JSON_H
#define DTV_USERLAND_JSON_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

// Both functions validate the complete response against its canonical context.
// Keep value, context, canonical list and their referenced storage unchanged for
// the entire call. Encode output must not overlap any of those inputs. The
// returned size includes the final newline; encoded bytes have no NUL terminator.
Result<std::size_t> status_json_size(const StatusResponse& value,
                                     const StatusContext& context) noexcept;

// Writes one compact UTF-8 JSON object. Validation, size-counting and capacity
// failures leave every byte in output unchanged; short capacity returns
// Error::BUFFER_TOO_SMALL.
Result<std::size_t> encode_status_json(const StatusResponse& value,
                                       const StatusContext& context,
                                       MutableByteView output) noexcept;


// Typed, bounded inventory produced by each product's existing read-only USB
// enumeration and grouping. This is not a wire opcode or a new discovery
// service. In particular, cold devices remain cold and ungrouped functions
// remain visible; the serializer must not infer a model or a sibling function.
enum class DiscoveryGroupingState : std::uint8_t {
    bound = 0U,
    unresolved = 1U,
    ambiguous = 2U,
};
enum class DiscoveryCandidateState : std::uint8_t {
    usable = 0U,
    requires_resolution = 1U,
    ambiguous = 2U,
    unavailable = 3U,
};

inline constexpr std::size_t kMaxDiscoverySelectorBytes =
    (detail::kMaxSerialBytes + 8U);  // "serial:" + maximum source serial
struct DiscoverySelectorCandidate final {
    WString<kMaxDiscoverySelectorBytes> value;
    DiscoveryCandidateState state = DiscoveryCandidateState::unavailable;
    Optional<WString<64>> reason;
};
struct DiscoveryScanIssue final {
    std::uint32_t code = 0U;
    WString<256> detail;
};
struct DiscoveryFunction final {
    UsbFunction observation;
    DiscoveryGroupingState grouping = DiscoveryGroupingState::unresolved;
    Optional<std::uint16_t> enclosure_id;
    Optional<WString<64>> grouping_reason;
    BArray<DiscoverySelectorCandidate, kMaxFunctions> selector_candidates;
};
struct DiscoveryEnclosure final {
    Enclosure descriptor;
    Topology topology;
    BArray<std::uint16_t, kMaxFunctions> usb_function_ids;
    BArray<DiscoverySelectorCandidate, kMaxFunctions> selector_candidates;
    // Omit both collections if the configuration is not yet known. A known
    // empty receiver/reader collection is represented by present=true,count=0.
    Optional<BArray<ReceiverDescriptor, kMaxPublicReceivers>> receivers;
    Optional<BArray<ReaderDescriptor, kMaxReaders>> readers;
};
// A scan can contain many enclosures, whereas kMaxFunctions is the limit for
// one enclosure only. Non-owning views avoid imposing that per-enclosure wire
// limit on the whole USB inventory. The producer owns all records during
// discovery_json_size / encode_discovery_json; null is valid only for count 0.
template <typename T>
struct DiscoveryView final {
    const T* items = nullptr;
    std::size_t count = 0U;
};
struct DiscoveryResponse final {
    Producer producer;
    Uuid scan_id{};
    bool complete = true;
    DiscoveryView<DiscoveryScanIssue> issues;
    DiscoveryView<DiscoveryFunction> usb_functions;
    DiscoveryView<DiscoveryEnclosure> enclosures;
};

// A single userland.discovery v1.0 object plus newline. ID references,
// observed/published membership, presence and selector grammar are validated
// before size calculation or writing. Invalid and short-buffer calls leave
// the supplied output completely unchanged. The per-enclosure
// function count remains limited by wire kMaxFunctions; scans have no artificial
// eight-function total limit and must never silently truncate observations.
Result<std::size_t> discovery_json_size(const DiscoveryResponse& value) noexcept;
Result<std::size_t> encode_discovery_json(const DiscoveryResponse& value,
                                          MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_JSON_H
