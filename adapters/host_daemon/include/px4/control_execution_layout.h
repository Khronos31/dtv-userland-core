// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CONTROL_EXECUTION_LAYOUT_H
#define PX4_USERLAND_CONTROL_EXECUTION_LAYOUT_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace px4::userland::ipc::posix {

enum class ControlWorkerLane : std::uint8_t {
    tuner_dev1,
    tuner_dev2,
    card,
};

struct ControlExecutionLayout final {
#if defined(ASICEN_PROFILE_ASICEN)
    std::uint8_t receiver_count = 4U;
    std::array<ControlWorkerLane, 8U> receiver_lane{{
        ControlWorkerLane::tuner_dev1, ControlWorkerLane::tuner_dev1,
        ControlWorkerLane::tuner_dev2, ControlWorkerLane::tuner_dev2,
        ControlWorkerLane::tuner_dev2, ControlWorkerLane::tuner_dev2,
        ControlWorkerLane::tuner_dev2, ControlWorkerLane::tuner_dev2}};
#else
    std::uint8_t receiver_count = 8U;
    std::array<ControlWorkerLane, 8U> receiver_lane{{
        ControlWorkerLane::tuner_dev1, ControlWorkerLane::tuner_dev1,
        ControlWorkerLane::tuner_dev1, ControlWorkerLane::tuner_dev1,
        ControlWorkerLane::tuner_dev2, ControlWorkerLane::tuner_dev2,
        ControlWorkerLane::tuner_dev2, ControlWorkerLane::tuner_dev2}};
#endif
    ControlWorkerLane reader_lane = ControlWorkerLane::card;
    ControlWorkerLane status_lane = ControlWorkerLane::tuner_dev1;
    ControlWorkerLane lnb_lane = ControlWorkerLane::tuner_dev1;
    ControlWorkerLane tuner_shutdown_lane = ControlWorkerLane::tuner_dev1;
    ControlWorkerLane card_shutdown_lane = ControlWorkerLane::card;
};

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_CONTROL_EXECUTION_LAYOUT_H
