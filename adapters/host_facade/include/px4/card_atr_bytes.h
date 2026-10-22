// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CARD_ATR_BYTES_H
#define PX4_USERLAND_CARD_ATR_BYTES_H

#include "px4/transport.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace px4::userland {

inline constexpr std::size_t kCardAtrMaxLength = 33U;

struct CardAtrBytes {
    std::array<std::uint8_t, kCardAtrMaxLength> bytes{};
    std::size_t length = 0U;

    ByteView view() const noexcept { return ByteView{bytes.data(), length}; }
};

}  // namespace px4::userland

#endif  // PX4_USERLAND_CARD_ATR_BYTES_H
