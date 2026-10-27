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
#if defined(DTV_USERLAND_HOST_TS_TEST_ACCESS)
    namespace wire = dtv::userland::wire;
    using px4::userland::ipc::CountersPayload;
    wire::StreamCounters stream_counters{};
    stream_counters.entries.count = 9U;
    const std::uint8_t ids[] = {5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U};
    for (std::size_t i = 0U; i < 8U; ++i) {
        stream_counters.entries.items[i].id = ids[i];
        stream_counters.entries.items[i].counter.measurement.state =
            wire::MeasureState::valid;
        stream_counters.entries.items[i].counter.measurement.value = i + 1U;
    }
    stream_counters.entries.items[0U].counter.measurement.value = 3U;
    stream_counters.entries.items[1U].counter.measurement.value = 0U;
    stream_counters.entries.items[1U].counter.measurement.state = wire::MeasureState::unknown;
    stream_counters.entries.items[2U].counter.measurement.value = 555U;
    stream_counters.entries.items[6U].counter.measurement.state = wire::MeasureState::unsupported;
    stream_counters.entries.items[8U].counter.measurement.state = wire::MeasureState::valid;
    const CountersPayload payload = px4::userland::cli::test_wire_counters(stream_counters);
    if (payload.packets != 3U || payload.bytes != 555U || payload.queue_drops != 0U ||
        payload.availability[0U] != wire::MeasureState::valid ||
        payload.availability[5U] != wire::MeasureState::unknown ||
        payload.availability[6U] != wire::MeasureState::unsupported ||
        payload.availability[1U] != wire::MeasureState::valid) return 1;
#endif

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
