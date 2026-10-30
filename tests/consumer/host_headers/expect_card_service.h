// SPDX-License-Identifier: MIT
#ifndef DTV_CONSUMER_HOST_HEADERS_EXPECT_CARD_SERVICE_H
#define DTV_CONSUMER_HOST_HEADERS_EXPECT_CARD_SERVICE_H

// Compile-time contract for the installed host card entrance.  The including
// probe TU must include the one header under test before this file.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace host_headers_probe {

using px4::userland::CardAtrBytes;
using px4::userland::CardClientId;
using px4::userland::CardProtocolSession;
using px4::userland::CardService;
using px4::userland::CardServiceBackend;
using px4::userland::CardServiceConnectResult;
using px4::userland::CardServiceStatus;
using px4::userland::ipc::ShareMode;

[[maybe_unused]] constexpr px4::userland::Result<void> (
    CardServiceBackend::*kSetPower)(bool) noexcept =
    &CardServiceBackend::set_power;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    CardServiceBackend::*kInitializeUart)() noexcept =
    &CardServiceBackend::initialize_uart;
[[maybe_unused]] constexpr px4::userland::Result<bool> (
    CardServiceBackend::*kDetectCard)() noexcept =
    &CardServiceBackend::detect_card;

[[maybe_unused]] constexpr px4::userland::Result<void> (
    CardProtocolSession::*kInitializeSession)() noexcept =
    &CardProtocolSession::initialize;
[[maybe_unused]] constexpr px4::userland::Result<std::size_t> (
    CardProtocolSession::*kTransmit)(
    px4::userland::ByteView, px4::userland::MutableByteView) noexcept =
    &CardProtocolSession::transmit;
[[maybe_unused]] constexpr bool (
    CardProtocolSession::*kInitialized)() const noexcept =
    &CardProtocolSession::initialized;
[[maybe_unused]] constexpr const CardAtrBytes& (
    CardProtocolSession::*kAtr)() const noexcept = &CardProtocolSession::atr;
[[maybe_unused]] constexpr void (CardProtocolSession::*kInvalidate)() noexcept =
    &CardProtocolSession::invalidate;

[[maybe_unused]] constexpr px4::userland::Result<CardServiceStatus> (
    CardService::*kStatus)() noexcept = &CardService::status;
[[maybe_unused]] constexpr px4::userland::Result<CardServiceConnectResult> (
    CardService::*kConnect)(CardClientId, ShareMode, bool) noexcept =
    &CardService::connect;

static_assert(std::is_same_v<decltype(CardServiceStatus::present), bool>,
              "CardServiceStatus no longer exposes the presence flag");
static_assert(std::is_same_v<decltype(CardServiceStatus::reader_generation),
                             std::uint64_t>,
              "CardServiceStatus no longer exposes the reader generation");
static_assert(std::is_constructible_v<CardService, CardServiceBackend&,
                                      CardProtocolSession&>,
              "CardService is not constructible from the public backend seams");

}  // namespace host_headers_probe

#endif  // DTV_CONSUMER_HOST_HEADERS_EXPECT_CARD_SERVICE_H
