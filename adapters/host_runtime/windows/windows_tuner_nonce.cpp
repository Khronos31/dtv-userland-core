// SPDX-License-Identifier: GPL-2.0-only
// Windows cryptographic nonce source using BCryptGenRandom.
#include "px4/windows_tuner_nonce.h"

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>

namespace px4::userland::ipc::windows {

Result<std::array<std::uint8_t, ipc::kNonceLength>> WindowsTunerNonceSource::generate() noexcept
{
    std::array<std::uint8_t, ipc::kNonceLength> nonce{};
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return Result<decltype(nonce)>::failure(Error::INTERNAL);
    }
    return Result<decltype(nonce)>::success(nonce);
}

}  // namespace px4::userland::ipc::windows

#endif
