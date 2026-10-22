// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_CLIENT_ID_ALLOCATOR_H
#define DTV_USERLAND_DETAIL_CLIENT_ID_ALLOCATOR_H

#include <dtv/userland/error.h>

#include <cstdint>

namespace dtv::userland::detail {

inline constexpr std::uint64_t kLegacyClientIdMax =
    (std::uint64_t{1U} << 63U) - 1U;
inline constexpr std::uint64_t kAuthorityClientIdFirst =
    std::uint64_t{1U} << 63U;

constexpr bool is_legacy_client_id(std::uint64_t client_id) noexcept
{
    return client_id != 0U && client_id <= kLegacyClientIdMax;
}

class ClientIdAllocator final {
public:
    constexpr ClientIdAllocator(std::uint64_t first, std::uint64_t last) noexcept
        : next_(first), last_(last), valid_(first != 0U && first <= last)
    {
    }

    Result<std::uint64_t> next() noexcept
    {
        if (!valid_ || exhausted_) return Result<std::uint64_t>::failure(Error::RESOURCE_LIMIT);
        const std::uint64_t value = next_;
        if (next_ == last_) exhausted_ = true;
        else ++next_;
        return Result<std::uint64_t>::success(value);
    }

private:
    std::uint64_t next_;
    std::uint64_t last_;
    bool valid_;
    bool exhausted_ = false;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_CLIENT_ID_ALLOCATOR_H
