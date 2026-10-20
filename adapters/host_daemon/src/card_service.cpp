// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"

namespace px4::userland {
namespace {

detail::CardShareMode to_engine(ipc::ShareMode mode) noexcept
{
    switch (mode) {
    case ipc::ShareMode::shared: return detail::CardShareMode::shared;
    case ipc::ShareMode::exclusive: return detail::CardShareMode::exclusive;
    }
    return detail::CardShareMode::invalid;
}

detail::CardDisposition to_engine(ipc::Disposition disposition) noexcept
{
    switch (disposition) {
    case ipc::Disposition::leave: return detail::CardDisposition::leave;
    case ipc::Disposition::reset: return detail::CardDisposition::reset;
    }
    return detail::CardDisposition::invalid;
}

Result<CardServiceStatus> adapt_status(
    Result<detail::CardOwnershipStatus<CardAtrBytes>> result) noexcept
{
    if (!result) return Result<CardServiceStatus>::failure(result.error());
    const auto& value = result.value();
    return Result<CardServiceStatus>::success(
        CardServiceStatus{value.present, value.initialized,
                          value.reader_generation, value.atr});
}

Result<CardPresenceChange> adapt_presence(
    Result<detail::CardOwnershipPresenceChange> result) noexcept
{
    if (!result) return Result<CardPresenceChange>::failure(result.error());
    const auto& value = result.value();
    return Result<CardPresenceChange>::success(
        CardPresenceChange{value.changed, value.present, value.reader_generation});
}

Result<CardServiceConnectResult> adapt_connect(
    Result<detail::CardOwnershipConnectResult<CardAtrBytes>> result) noexcept
{
    if (!result) return Result<CardServiceConnectResult>::failure(result.error());
    const auto& value = result.value();
    return Result<CardServiceConnectResult>::success(
        CardServiceConnectResult{value.handle, value.atr});
}

}  // namespace

CardService::~CardService() noexcept = default;

Result<CardServiceStatus> CardService::status() noexcept
{
    return adapt_status(engine_.status());
}

Result<CardServiceStatus> CardService::handle_status(CardClientId client,
                                                      std::uint64_t handle) noexcept
{
    return adapt_status(engine_.handle_status(client, handle));
}

Result<CardPresenceChange> CardService::poll_presence() noexcept
{
    return adapt_presence(engine_.poll_presence());
}

Result<CardServiceConnectResult> CardService::connect(
    CardClientId client, ipc::ShareMode share_mode, bool generation_bound) noexcept
{
    return adapt_connect(engine_.connect(client, to_engine(share_mode), generation_bound));
}

Result<CardAtrBytes> CardService::reconnect(CardClientId client, std::uint64_t handle,
                                       ipc::ShareMode share_mode,
                                       ipc::Disposition disposition) noexcept
{
    return engine_.reconnect(client, handle, to_engine(share_mode),
                             to_engine(disposition));
}

Result<void> CardService::disconnect(CardClientId client, std::uint64_t handle,
                                     ipc::Disposition disposition) noexcept
{
    return engine_.disconnect(client, handle, to_engine(disposition));
}

Result<CardAtrBytes> CardService::reset(CardClientId client, std::uint64_t handle) noexcept
{
    return engine_.reset(client, handle);
}

Result<std::size_t> CardService::transmit(CardClientId client, std::uint64_t handle,
                                          ByteView apdu,
                                          MutableByteView response) noexcept
{
    return engine_.transmit(client, handle, detail::ByteSpan{apdu.data, apdu.size},
                            detail::MutableByteSpan{response.data, response.size});
}

Result<void> CardService::begin_transaction(CardClientId client,
                                            std::uint64_t handle) noexcept
{
    return engine_.begin_transaction(client, handle);
}

Result<void> CardService::end_transaction(CardClientId client, std::uint64_t handle,
                                          ipc::Disposition disposition) noexcept
{
    return engine_.end_transaction(client, handle, to_engine(disposition));
}

Result<void> CardService::release_connection(CardClientId client) noexcept
{
    return engine_.release_connection(client);
}

Result<void> CardService::shutdown() noexcept
{
    return engine_.shutdown();
}

}  // namespace px4::userland
