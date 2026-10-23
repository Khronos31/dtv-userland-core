// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINDOWS_TUNER_NONCE_H
#define PX4_USERLAND_WINDOWS_TUNER_NONCE_H

#include "px4/tuner_service.h"

namespace px4::userland::ipc::windows {

class WindowsTunerNonceSource final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override;
};

}  // namespace px4::userland::ipc::windows

#endif  // PX4_USERLAND_WINDOWS_TUNER_NONCE_H
