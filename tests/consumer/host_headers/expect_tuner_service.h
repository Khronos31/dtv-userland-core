// SPDX-License-Identifier: MIT
#ifndef DTV_CONSUMER_HOST_HEADERS_EXPECT_TUNER_SERVICE_H
#define DTV_CONSUMER_HOST_HEADERS_EXPECT_TUNER_SERVICE_H

// Compile-time contract for the installed host tuner entrance.  The including
// probe TU must include the one header under test before this file.  Each
// pointer below names the exact representative signature that header has to
// provide; a different or missing definition fails to compile.

#include <array>
#include <cstdint>
#include <type_traits>

namespace host_headers_probe {

using px4::userland::ipc::System;
using px4::userland::TunerAcquireResult;
using px4::userland::TunerService;
using px4::userland::TunerServiceBackend;
using px4::userland::TunerServiceTime;
using px4::userland::TunerNonceSource;
using px4::userland::TunerStrictProfile;

[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kOpenReceiver)(std::uint8_t) noexcept =
    &TunerServiceBackend::open_receiver;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kTuneTerrestrial)(
    std::uint8_t, std::uint32_t, std::uint32_t) noexcept =
    &TunerServiceBackend::tune_terrestrial;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kTuneSatellite)(
    std::uint8_t, std::uint32_t, std::uint32_t) noexcept =
    &TunerServiceBackend::tune_satellite;
[[maybe_unused]] constexpr px4::userland::Result<bool> (
    TunerServiceBackend::*kIsLocked)(std::uint8_t, System) noexcept =
    &TunerServiceBackend::is_locked;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kSelectSatelliteSlot)(
    std::uint8_t, std::uint8_t, std::uint32_t) noexcept =
    &TunerServiceBackend::select_satellite_slot;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kSelectSatelliteTsid)(
    std::uint8_t, std::uint16_t, std::uint32_t) noexcept =
    &TunerServiceBackend::select_satellite_tsid;
[[maybe_unused]] constexpr px4::userland::Result<void> (
    TunerServiceBackend::*kCloseReceiver)(std::uint8_t) noexcept =
    &TunerServiceBackend::close_receiver;
[[maybe_unused]] constexpr TunerStrictProfile (TunerServiceBackend::*kStrictProfile)(
    std::uint8_t, System) const noexcept = &TunerServiceBackend::strict_profile;

[[maybe_unused]] constexpr px4::userland::Result<
    std::array<std::uint8_t, px4::userland::ipc::kNonceLength>> (
    TunerNonceSource::*kGenerateNonce)() noexcept = &TunerNonceSource::generate;
[[maybe_unused]] constexpr std::uint64_t (
    TunerServiceTime::*kMonotonicMs)() noexcept = &TunerServiceTime::monotonic_ms;
[[maybe_unused]] constexpr void (TunerServiceTime::*kSleepMs)(
    std::uint32_t) noexcept = &TunerServiceTime::sleep_ms;

[[maybe_unused]] constexpr px4::userland::Result<TunerAcquireResult> (
    TunerService::*kAcquire)(std::uint64_t, std::uint8_t) noexcept =
    &TunerService::acquire;

static_assert(std::is_constructible_v<TunerService, TunerServiceBackend&,
                                      TunerNonceSource&, TunerServiceTime&>,
              "TunerService is not constructible from the public backend seams");

}  // namespace host_headers_probe

#endif  // DTV_CONSUMER_HOST_HEADERS_EXPECT_TUNER_SERVICE_H
