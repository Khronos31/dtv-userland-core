// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>

int main()
{
    const auto terrestrial = px4::userland::cli::parse_px4_ts_channel("T13");
    const auto satellite = px4::userland::cli::parse_px4_ts_channel("BS01_0");
    const auto invalid = px4::userland::cli::parse_px4_ts_channel("T7");
    if (!terrestrial.valid || terrestrial.frequency_khz == 0U ||
        !satellite.valid || satellite.slot != 0U || invalid.valid) return 1;
    return 0;
}
