// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>

namespace {

using px4::userland::ipc::System;

bool accepts_system_case(const char* value, System expected)
{
    const bool satellite = expected == System::ISDB_S;
    const char* legacy_terrestrial[] = {
        "siano-ts", "--device", "12345678901234", "--receiver", "0",
        "--system", value, "--frequency-khz", "650000"};
    const char* legacy_satellite[] = {
        "siano-ts", "--device", "12345678901234", "--receiver", "0",
        "--system", value, "--frequency-khz", "1200000", "--slot", "0"};
    const char* product_terrestrial[] = {
        "siano-ts", "--instance", "siano0", "--receiver", "0",
        "--system", value, "--freq", "473143000"};
    const char* product_satellite[] = {
        "siano-ts", "--instance", "siano0", "--receiver", "0",
        "--system", value, "--freq", "1049480000", "--slot", "0"};
    const auto legacy = px4::userland::cli::parse_px4_ts_arguments(
        satellite ? 11 : 9,
        satellite ? legacy_satellite : legacy_terrestrial);
    const auto product = px4::userland::cli::parse_px4_ts_product_arguments(
        satellite ? 11 : 9,
        satellite ? product_satellite : product_terrestrial);
    return legacy.valid && product.valid && legacy.system == expected &&
           product.system == expected;
}

}  // namespace

int main()
{
    const auto terrestrial = px4::userland::cli::parse_px4_ts_channel("T13");
    const auto satellite = px4::userland::cli::parse_px4_ts_channel("BS01_0");
    const auto invalid = px4::userland::cli::parse_px4_ts_channel("T7");
    if (!terrestrial.valid || terrestrial.frequency_khz == 0U ||
        !satellite.valid || satellite.slot != 0U || invalid.valid) return 1;
    const char* const terrestrial_values[] = {"isdb-t", "ISDB-T", "IsDb-T"};
    const char* const satellite_values[] = {"isdb-s", "ISDB-S", "IsDb-S"};
    for (const char* value : terrestrial_values) {
        if (!accepts_system_case(value, System::ISDB_T)) return 1;
    }
    for (const char* value : satellite_values) {
        if (!accepts_system_case(value, System::ISDB_S)) return 1;
    }
    const char* const unsupported_values[] = {"ISDB-T2", "ISDB-C"};
    for (const char* value : unsupported_values) {
        if (accepts_system_case(value, System::ISDB_T) ||
            accepts_system_case(value, System::ISDB_S)) return 1;
    }
    return 0;
}
