// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DISCOVERY_JSON_H
#define DTV_USERLAND_DISCOVERY_JSON_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

enum class DiscoveryEnclosureStatus : std::uint8_t {
    ready = 0U,
    incomplete = 1U,
    duplicate = 2U,
    invalid_observation = 3U,
    unknown = 4U,
};

enum class DiscoveryObservationStatus : std::uint8_t {
    usable = 0U,
    unsupported = 1U,
    invalid_serial = 2U,
    insufficient_speed = 3U,
    invalid_topology = 4U,
    open_failed = 5U,
    unknown = 6U,
};

struct DiscoveryUsbLocation final {
    Optional<std::uint8_t> bus;
    Optional<std::uint8_t> address;
    Optional<WString<64>> port;
};

struct DiscoveryReceiver final {
    std::uint16_t receiver = 0U;
    std::uint16_t device = 0U;
    std::uint16_t local = 0U;
    Systems systems;
};

struct DiscoveryCandidate final {
    std::uint16_t device = 0U;
    Optional<WString<detail::kMaxSerialBytes>> serial;
    DiscoveryUsbLocation location;
};

struct DiscoveryDevice final {
    std::uint16_t device = 0U;
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    Optional<WString<detail::kMaxSerialBytes>> serial;
    DiscoveryUsbLocation location;
    bool receivers_known = false;
    std::size_t receiver_count = 0U;
    const DiscoveryReceiver* receivers = nullptr;
};

struct DiscoveryReader final {
    std::uint16_t reader = 0U;
    std::uint16_t device = 0U;
};

struct DiscoveryEnclosure final {
    std::uint16_t enclosure = 0U;
    Optional<WString<128>> model;
    Optional<WString<detail::kMaxSerialBytes>> serial;
    Optional<bool> lnb_15v_supported;
    bool readers_known = false;
    std::size_t reader_count = 0U;
    const DiscoveryReader* readers = nullptr;
    DiscoveryEnclosureStatus status = DiscoveryEnclosureStatus::unknown;
    std::size_t device_count = 0U;
    const DiscoveryDevice* devices = nullptr;
    std::size_t candidate_count = 0U;
    const DiscoveryCandidate* candidates = nullptr;
};

struct DiscoveryUsbObservation final {
    Optional<WString<detail::kMaxSerialBytes>> serial;
    Optional<WString<128>> model;
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    DiscoveryObservationStatus status = DiscoveryObservationStatus::unknown;
    DiscoveryUsbLocation location;
};

struct DiscoveryScanIssue final {
    WString<64> code;
    WString<256> detail;
};

// A complete scan is required before a non-duplicate serial can be declared
// unique. Observed duplicates remain false even if another part of the scan failed.
struct DiscoveryScan final {
    bool complete = false;
    WString<32> product;
    WString<64> version;
    std::size_t issue_count = 0U;
    const DiscoveryScanIssue* issues = nullptr;
    std::size_t enclosure_count = 0U;
    const DiscoveryEnclosure* enclosures = nullptr;
    std::size_t ungrouped_usb_device_count = 0U;
    const DiscoveryUsbObservation* ungrouped_usb_devices = nullptr;
};

// Both functions validate the complete scan. Encoding is atomic: invalid input
// and insufficient capacity leave output unchanged.
Result<std::size_t> discovery_json_size(const DiscoveryScan& value) noexcept;
Result<std::size_t> encode_discovery_json(const DiscoveryScan& value,
                                          MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_DISCOVERY_JSON_H
