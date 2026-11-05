// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>

#include <cstdio>
#include <string>
#include <vector>

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

bool rejects_removed_lnb_option(const char* value)
{
    const char* legacy[] = {
        "siano-ts", "--device", "12345678901234", "--receiver", "0",
        "--system", "isdb-s", "--frequency-khz", "1200000", "--slot", "0",
        "--lnb-voltage", value};
    const char* product[] = {
        "siano-ts", "--instance", "siano0", "--receiver", "0",
        "--channel", "BS01_0", "--lnb-voltage", value};
    px4::userland::cli::TsClientProfile profile{};
    profile.tool_name = "siano-ts";
    profile.product_slug = "siano-userland";
    profile.preserve_instance_case = false;
    return !px4::userland::cli::parse_px4_ts_arguments(13, legacy).valid &&
           !px4::userland::cli::parse_px4_ts_product_arguments(9, product).valid &&
           !px4::userland::cli::parse_px4_ts_product_arguments(9, product, profile).valid;
}

bool usage_omits_lnb_option()
{
    px4::userland::cli::TsClientProfile profile{};
    profile.tool_name = "siano-ts";
    FILE* output = std::tmpfile();
    if (output == nullptr) return false;
    px4::userland::cli::print_px4_ts_usage(output);
    px4::userland::cli::print_px4_ts_usage(profile, output);
    if (std::fflush(output) != 0 || std::fseek(output, 0L, SEEK_SET) != 0) {
        std::fclose(output);
        return false;
    }
    std::string text;
    char buffer[256];
    for (;;) {
        const std::size_t read = std::fread(buffer, 1U, sizeof(buffer), output);
        text.append(buffer, read);
        if (read < sizeof(buffer)) break;
    }
    std::fclose(output);
    return text.find("--lnb-voltage") == std::string::npos &&
           text.find("[--receiver ID]") == std::string::npos &&
           text.find("--receiver ID (-c|--channel") != std::string::npos;
}

bool cli_contract()
{
#define CLI_CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "host_ts cli contract failed at line %d\n", __LINE__); \
    return false; } } while (false)
    using px4::userland::cli::parse_px4_ts_control_arguments;
    using px4::userland::cli::parse_px4_ts_product_arguments;
    px4::userland::cli::TsClientProfile profile{};
    profile.tool_name = "siano-ts";
    profile.product_slug = "siano-userland";

    const char* help_with_invalid[] = {"siano-ts", "--help", "--unknown", "--receiver"};
    const char* short_help_with_version[] = {"siano-ts", "-h", "--version", "--receiver"};
    const char* version_with_invalid[] = {"siano-ts", "--version", "--unknown", "--receiver"};
    const char* version_with_help[] = {"siano-ts", "--version", "-h"};
    const auto help = parse_px4_ts_product_arguments(4, help_with_invalid, profile);
    const auto short_help = parse_px4_ts_product_arguments(4, short_help_with_version, profile);
    const auto version = parse_px4_ts_product_arguments(4, version_with_invalid, profile);
    const auto help_first = parse_px4_ts_product_arguments(3, version_with_help, profile);
    CLI_CHECK(help.valid && help.help && short_help.valid && short_help.help &&
        version.valid && version.version && help_first.valid && help_first.help);

    const char* output_named_help[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--channel", "T13", "--output", "-h"};
    const auto output_value = parse_px4_ts_product_arguments(9, output_named_help, profile);
    CLI_CHECK(output_value.valid && !output_value.help && output_value.output == "-h");
    const char* invalid_receiver_help[] = {"siano-ts", "--instance", "RoomA", "--receiver", "-h"};
    const char* invalid_receiver_help_and_version[] = {"siano-ts", "--instance", "RoomA",
        "--receiver", "-h", "--version"};
    const auto receiver_help = parse_px4_ts_product_arguments(5, invalid_receiver_help, profile);
    const auto receiver_help_first = parse_px4_ts_product_arguments(
        6, invalid_receiver_help_and_version, profile);
    CLI_CHECK(receiver_help.valid && receiver_help.help &&
        receiver_help_first.valid && receiver_help_first.help && !receiver_help_first.version);

    const char* case_upper[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--channel", "T13"};
    const char* case_lower[] = {"siano-ts", "--instance", "rooma", "--receiver", "0",
        "--channel", "T13"};
    const auto upper = parse_px4_ts_product_arguments(7, case_upper, profile);
    const auto lower = parse_px4_ts_product_arguments(7, case_lower, profile);
    CLI_CHECK(upper.valid && lower.valid && upper.instance == "RoomA" &&
        lower.instance == "rooma" && upper.instance != lower.instance);

    const char* canonical_numeric[] = {"siano-ts", "--instance", "12345678901234",
        "--receiver", "0", "--channel", "T13"};
    const auto numeric_identity = parse_px4_ts_product_arguments(7, canonical_numeric, profile);
    CLI_CHECK(numeric_identity.valid);

    const char* no_receiver[] = {"siano-ts", "--instance", "RoomA", "--channel", "T13"};
    const char* explicit_receiver[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--channel", "T13"};
    const auto omitted = parse_px4_ts_product_arguments(5, no_receiver, profile);
    const auto explicit_zero = parse_px4_ts_product_arguments(7, explicit_receiver, profile);
    CLI_CHECK(!omitted.valid && explicit_zero.valid && explicit_zero.receiver_explicit &&
        explicit_zero.receiver == 0U);

    const char* removed_firmware[] = {"siano-ts", "-F", "firmware.bin"};
    const char* removed_version[] = {"siano-ts", "-V"};
    CLI_CHECK(!parse_px4_ts_product_arguments(3, removed_firmware, profile).valid &&
        !parse_px4_ts_product_arguments(2, removed_version, profile).valid);

    const char* duplicate_short_long[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--channel", "T13", "--time", "1", "-t", "2"};
    const char* duplicate_output[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--channel", "T13", "--output", "a.ts", "-o", "b.ts"};
    const char* duplicate_receiver[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--receiver", "0", "--channel", "T13"};
    CLI_CHECK(!parse_px4_ts_product_arguments(11, duplicate_short_long, profile).valid &&
        !parse_px4_ts_product_arguments(11, duplicate_output, profile).valid &&
        !parse_px4_ts_product_arguments(9, duplicate_receiver, profile).valid);

    const char* control_wait[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--time", "-"};
    const auto wait = parse_px4_ts_control_arguments(7, control_wait, profile);
    CLI_CHECK(wait.valid && !wait.tuning_explicit && wait.receiver_explicit &&
        wait.time_explicit && wait.frequency_hz == 0U);
    const char* control_tuned[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--freq", "473143000", "--system", "isdb-t", "--packet-count", "10"};
    const auto tuned = parse_px4_ts_control_arguments(11, control_tuned, profile);
    CLI_CHECK(tuned.valid && tuned.tuning_explicit && tuned.frequency_hz == 473143000U);
    const char* control_without_limit[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0"};
    const char* control_with_invalid[] = {"siano-ts", "--instance", "RoomA", "--receiver", "0",
        "--time", "-", "--unknown"};
    const char* control_without_receiver[] = {"siano-ts", "--instance", "RoomA", "--time", "-"};
    const auto no_limit = parse_px4_ts_control_arguments(5, control_without_limit, profile);
    const auto invalid_control = parse_px4_ts_control_arguments(8, control_with_invalid, profile);
    const auto no_control_receiver = parse_px4_ts_control_arguments(5, control_without_receiver, profile);
    const auto no_normal_tuning = parse_px4_ts_product_arguments(5, control_without_limit, profile);
    CLI_CHECK(no_limit.valid && !no_limit.tuning_explicit && !no_limit.time_explicit &&
        !no_limit.duration_set && !no_limit.packet_count_set && !invalid_control.valid &&
        !no_control_receiver.valid && !no_normal_tuning.valid);
#undef CLI_CHECK
    return true;
}

}  // namespace

int main()
{
    if (px4::userland::cli::px4_ts_exit_status(
            px4::userland::Error::RESOURCE_LIMIT) != 70) return 1;
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
    if (!rejects_removed_lnb_option("0") || !rejects_removed_lnb_option("15") ||
        !usage_omits_lnb_option() || !cli_contract()) return 1;
    const char* const unsupported_values[] = {"ISDB-T2", "ISDB-C"};
    for (const char* value : unsupported_values) {
        if (accepts_system_case(value, System::ISDB_T) ||
            accepts_system_case(value, System::ISDB_S)) return 1;
    }
    return 0;
}
