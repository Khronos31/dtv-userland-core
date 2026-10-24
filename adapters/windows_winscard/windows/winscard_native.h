// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINSCARD_NATIVE_H
#define PX4_USERLAND_WINSCARD_NATIVE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winscard.h>

namespace px4::userland::windows_card {

// Native contexts and cards never leave this boundary as public proxy handles.
struct NativeApi final {
    decltype(&::SCardEstablishContext) establish = nullptr;
    decltype(&::SCardReleaseContext) release = nullptr;
    decltype(&::SCardListReadersA) list_a = nullptr;
    decltype(&::SCardListReadersW) list_w = nullptr;
    decltype(&::SCardConnectA) connect_a = nullptr;
    decltype(&::SCardConnectW) connect_w = nullptr;
    decltype(&::SCardDisconnect) disconnect = nullptr;
    decltype(&::SCardTransmit) transmit = nullptr;
    decltype(&::SCardStatusW) status_w = nullptr;
    decltype(&::SCardGetStatusChangeW) change_w = nullptr;

    bool complete() const noexcept
    {
        return establish && release && list_a && list_w && connect_a && connect_w &&
               disconnect && transmit && status_w && change_w;
    }
};

#if defined(PX4_WINSCARD_TESTING)
// Test executable only. It is not compiled into or exported by the DLL.
void set_native_api_for_tests(const NativeApi& api) noexcept;
#endif

}  // namespace px4::userland::windows_card

#endif  // PX4_USERLAND_WINSCARD_NATIVE_H
