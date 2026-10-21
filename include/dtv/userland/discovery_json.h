// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DISCOVERY_JSON_H
#define DTV_USERLAND_DISCOVERY_JSON_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

struct DiscoveryReceiver final {
    std::uint16_t receiver = 0U;
    std::uint16_t device = 0U;
    std::uint16_t local = 0U;
    Systems systems;
};

struct DiscoveryDevice final {
    std::uint16_t device = 0U;
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
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
    std::size_t device_count = 0U;
    const DiscoveryDevice* devices = nullptr;
};

// A complete scan is required before a non-duplicate serial can be declared
// unique. Observed duplicates remain false even if another part of the scan failed.
struct DiscoveryScan final {
    bool complete = false;
    WString<32> product;
    WString<64> version;
    std::size_t enclosure_count = 0U;
    const DiscoveryEnclosure* enclosures = nullptr;
};

// Both functions validate the complete scan. Encoding is atomic: invalid input
// and insufficient capacity leave output unchanged.
Result<std::size_t> discovery_json_size(const DiscoveryScan& value) noexcept;
Result<std::size_t> encode_discovery_json(const DiscoveryScan& value,
                                          MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_DISCOVERY_JSON_H
