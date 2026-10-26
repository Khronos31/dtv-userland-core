// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>

#include "dtv/userland/control_client.h"
#include "dtv/userland/control_connection.h"
#include "dtv/userland/identity.h"
#include "dtv/userland/records.h"
#include "dtv/userland/tuning.h"
#include "dtv/userland/wire.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#if defined(DTV_USERLAND_HOST_TS_TEST_ACCESS)
#include <atomic>
#endif

namespace px4::userland::cli {
namespace {

using namespace ipc;
using namespace ipc::posix;

constexpr std::uint64_t kTsProgressTimeoutMs = 5000U;

#if defined(DTV_USERLAND_HOST_TS_TEST_ACCESS)
std::atomic<bool> fail_next_frame_storage_allocation{false};
#endif

Px4TsArguments invalid(std::string_view message) noexcept
{
    Px4TsArguments result;
    result.error.assign(message.data(), message.size());
    return result;
}

bool take_value(int& index, int argc, const char* const* argv,
                std::string_view& value) noexcept
{
    if (index + 1 >= argc || argv[index + 1] == nullptr) return false;
    value = std::string_view(argv[++index]);
    return !value.empty() && value.rfind("--", 0U) != 0U;
}

template <typename T>
bool parse_unsigned(std::string_view value, T& output) noexcept
{
    if (value.empty()) return false;
    T parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(),
                                           parsed, 10);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size())
        return false;
    output = parsed;
    return true;
}

// CLI input names are ASCII identifiers: fold A-Z only (not locale-dependent).
bool ascii_system_equals(std::string_view value, std::string_view expected) noexcept
{
    if (value.size() != expected.size()) return false;
    for (std::size_t i = 0U; i < value.size(); ++i) {
        char ch = value[i];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (ch != expected[i]) return false;
    }
    return true;
}

bool parse_system(std::string_view value, System& system) noexcept
{
    if (ascii_system_equals(value, "isdb-t")) {
        system = System::ISDB_T;
        return true;
    }
    if (ascii_system_equals(value, "isdb-s")) {
        system = System::ISDB_S;
        return true;
    }
    return false;
}

// Exactly `digits` ASCII decimal digits, no sign or leading text.  Fixed digit
// counts keep T7/T027/BS1 out of the accepted grammar.
bool parse_fixed_decimal(std::string_view text, std::size_t digits,
                         unsigned int& value) noexcept
{
    if (text.size() != digits) return false;
    unsigned int parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10U + static_cast<unsigned int>(character - '0');
    }
    value = parsed;
    return true;
}

// One or two decimal digits without a leading zero (0 and 10..99).
bool parse_short_decimal(std::string_view text, unsigned int& value) noexcept
{
    if (text.empty() || text.size() > 2U) return false;
    if (text.size() == 2U && text[0] == '0') return false;
    unsigned int parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10U + static_cast<unsigned int>(character - '0');
    }
    value = parsed;
    return true;
}

// A 14-digit PX-Q3U4 base serial or a 15-digit PX-MLT5PE/DTV02A-5TS-P serial.
bool valid_serial(std::string_view value) noexcept
{
    if (value.size() != 14U && value.size() != 15U) return false;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

bool valid_instance(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 80U || value == "." || value == ".." ||
        valid_serial(value))
        return false;
    for (const char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')
            continue;
        return false;
    }
    return true;
}

bool valid_frequency(System system, std::uint64_t frequency) noexcept
{
    if (frequency > std::numeric_limits<std::uint32_t>::max()) return false;
    return system == System::ISDB_T ? frequency >= 40000U && frequency <= 1002000U
                                    : frequency >= 146875U && frequency <= 2350000U;
}

bool valid_arguments(const Px4TsArguments& arguments) noexcept
{
    if (arguments.product_cli) {
        constexpr std::uint64_t kMaxRequestedHz = (std::uint64_t{1U} << 53U) - 1U;
        if ((!arguments.device.empty() &&
             !dtv::userland::identity::parse_device_selector(arguments.device)) ||
            (!arguments.instance.empty() && !valid_instance(arguments.instance)) ||
            (!arguments.common_profile && arguments.device.empty() && arguments.instance.empty()) ||
            arguments.frequency_hz == 0U || arguments.frequency_hz > kMaxRequestedHz ||
            arguments.frequency_khz != 0U || arguments.tune_timeout_ms < 100U ||
            arguments.tune_timeout_ms > 30000U ||
            (arguments.duration_set && arguments.duration_seconds == 0U) ||
            (arguments.duration_set &&
             arguments.duration_seconds > std::numeric_limits<std::uint64_t>::max() / 1000U) ||
            (arguments.packet_count_set && arguments.packet_count == 0U) ||
            (arguments.duration_set && arguments.packet_count_set)) {
            return false;
        }
        if (arguments.system == System::ISDB_T) {
            return arguments.stream_id == 0xffffU && arguments.slot == 0xffffU &&
                   arguments.lnb_voltage == 0U;
        }
        return arguments.system == System::ISDB_S && arguments.bandwidth_hz == 0U &&
               arguments.lnb_voltage <= 15U &&
               (arguments.stream_id != 0xffffU) != (arguments.slot != 0xffffU) &&
               (arguments.slot == 0xffffU || arguments.slot <= 11U);
    }
    const bool have_khz = arguments.frequency_khz != 0U;
    const bool have_hz = arguments.frequency_hz != 0U;
    constexpr std::uint64_t kMaxRequestedHz = (std::uint64_t{1U} << 53U) - 1U;
    const bool valid_frequency_entrance =
        have_khz != have_hz &&
        (have_khz ? valid_frequency(arguments.system, arguments.frequency_khz)
                  : arguments.frequency_hz <= kMaxRequestedHz);
    if ((arguments.instance.empty() ? !valid_serial(arguments.device) :
                                      (!arguments.device.empty() ||
                                       !valid_instance(arguments.instance))) ||
        !valid_frequency_entrance ||
        arguments.tune_timeout_ms < 100U || arguments.tune_timeout_ms > 30000U ||
        (arguments.duration_set && arguments.duration_seconds == 0U) ||
        (arguments.duration_set &&
         arguments.duration_seconds > std::numeric_limits<std::uint64_t>::max() / 1000U) ||
        (arguments.packet_count_set && arguments.packet_count == 0U) ||
        (arguments.duration_set && arguments.packet_count_set)) {
        return false;
    }
    if (arguments.system == System::ISDB_T) {
        return arguments.stream_id == 0xffffU && arguments.slot == 0xffffU &&
               arguments.lnb_voltage == 0U && arguments.bandwidth_hz == 6000000U;
    }
    if (arguments.bandwidth_hz != 0U || arguments.lnb_voltage > 15U ||
        (arguments.stream_id != 0xffffU && arguments.slot != 0xffffU)) {
        return false;
    }
    return (arguments.stream_id == 0xffffU && arguments.slot <= 11U) ||
           (arguments.slot == 0xffffU && arguments.stream_id != 0xffffU);
}

}  // namespace

Px4TsChannel parse_px4_ts_channel(std::string_view text) noexcept
{
    if (text.empty()) return Px4TsChannel{};
    unsigned int number = 0U;
    if (text[0] == 'T') {
        if (!parse_fixed_decimal(text.substr(1U), 2U, number) || number < 13U ||
            number > 62U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::terrestrial;
        result.system = System::ISDB_T;
        result.frequency_khz = 395142U + static_cast<std::uint64_t>(number) * 6000U;
        return result;
    }
    if (text[0] >= '0' && text[0] <= '9') {
        if (!parse_fixed_decimal(text, 2U, number) || number < 13U || number > 62U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::terrestrial;
        result.system = System::ISDB_T;
        result.frequency_khz = 395142U + static_cast<std::uint64_t>(number) * 6000U;
        return result;
    }
    if (text.rfind("BS", 0U) == 0U) {
        const std::string_view rest = text.substr(2U);
        const std::size_t underscore = rest.find('_');
        if (underscore == std::string_view::npos) {
            if (!parse_fixed_decimal(rest, 2U, number) || number < 1U ||
                number > 23U || (number % 2U) == 0U)
                return Px4TsChannel{};
            Px4TsChannel result;
            result.valid = true;
            result.kind = Px4TsChannelKind::satellite_transponder;
            result.system = System::ISDB_S;
            result.frequency_khz =
                1049480U + ((number - 1U) / 2U) * 38360U;
            return result;
        }
        unsigned int slot = 0U;
        if (!parse_fixed_decimal(rest.substr(0U, underscore), 2U, number) ||
            number < 1U || number > 23U || (number % 2U) == 0U ||
            !parse_short_decimal(rest.substr(underscore + 1U), slot) || slot > 11U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::satellite_slot;
        result.system = System::ISDB_S;
        result.frequency_khz = 1049480U + ((number - 1U) / 2U) * 38360U;
        result.slot = static_cast<std::uint16_t>(slot);
        return result;
    }
    if (text.rfind("CS", 0U) == 0U) {
        if (!parse_short_decimal(text.substr(2U), number) || number < 2U ||
            number > 24U || (number % 2U) != 0U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::satellite_cs;
        result.system = System::ISDB_S;
        result.frequency_khz = 1613000U + ((number - 2U) / 2U) * 40000U;
        result.slot = 0U;
        return result;
    }
    return Px4TsChannel{};
}

Px4TsArguments parse_px4_ts_arguments(int argc,
                                      const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
        return invalid("invalid argument vector");
    Px4TsArguments result;
    bool have_device = false;
    bool have_instance = false;
    bool have_runtime = false;
    bool have_receiver = false;
    bool have_system = false;
    bool have_frequency = false;
    bool have_stream_id = false;
    bool have_slot = false;
    bool have_bandwidth = false;
    bool have_lnb = false;
    bool have_timeout = false;
    bool have_channel = false;
    std::string_view channel_text;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--group") {
            if (result.group) return invalid("duplicate --group");
            result.group = true;
            continue;
        }
        const bool takes_value = option == "--device" || option == "--instance" ||
                                 option == "--runtime-dir" ||
                                 option == "--output" || option == "--receiver" ||
                                 option == "--system" || option == "--frequency-khz" ||
                                 option == "--stream-id" || option == "--slot" ||
                                 option == "--bandwidth-hz" || option == "--lnb-voltage" ||
                                 option == "--channel" ||
                                 option == "--tune-timeout-ms" ||
                                 option == "--duration-seconds" || option == "--packet-count";
        if (!takes_value) return invalid("unknown argument");
        std::string_view value;
        if (!take_value(index, argc, argv, value)) return invalid("option requires a value");
        if (option == "--device") {
            if (have_device) return invalid("duplicate --device");
            have_device = true;
            result.device.assign(value.data(), value.size());
        } else if (option == "--instance") {
            if (have_instance) return invalid("duplicate --instance");
            have_instance = true;
            result.instance.assign(value.data(), value.size());
        } else if (option == "--runtime-dir") {
            if (have_runtime) return invalid("duplicate --runtime-dir");
            have_runtime = true;
            result.runtime_directory.assign(value.data(), value.size());
        } else if (option == "--output") {
            if (!result.output.empty()) return invalid("duplicate --output");
            result.output.assign(value.data(), value.size());
        } else if (option == "--receiver") {
            if (have_receiver || !parse_unsigned(value, result.receiver) ||
                result.receiver >= kReceiverCount) return invalid("receiver must be 0..7");
            have_receiver = true;
        } else if (option == "--system") {
            if (have_system || !parse_system(value, result.system))
                return invalid("system must be isdb-t or isdb-s");
            have_system = true;
        } else if (option == "--frequency-khz") {
            if (have_frequency || !parse_unsigned(value, result.frequency_khz))
                return invalid("frequency-khz is invalid");
            have_frequency = true;
        } else if (option == "--channel") {
            if (have_channel) return invalid("duplicate --channel");
            have_channel = true;
            channel_text = value;
        } else if (option == "--stream-id") {
            if (have_stream_id || !parse_unsigned(value, result.stream_id) ||
                result.stream_id == 0xffffU)
                return invalid("stream-id is invalid or reserved");
            have_stream_id = true;
        } else if (option == "--slot") {
            if (have_slot || !parse_unsigned(value, result.slot) || result.slot > 11U)
                return invalid("slot must be 0..11");
            have_slot = true;
        } else if (option == "--bandwidth-hz") {
            if (have_bandwidth || !parse_unsigned(value, result.bandwidth_hz))
                return invalid("bandwidth-hz is invalid");
            have_bandwidth = true;
        } else if (option == "--lnb-voltage") {
            if (have_lnb || !parse_unsigned(value, result.lnb_voltage) ||
                (result.lnb_voltage != 0U && result.lnb_voltage != 15U))
                return invalid("lnb-voltage must be 0 or 15");
            have_lnb = true;
        } else if (option == "--tune-timeout-ms") {
            if (have_timeout || !parse_unsigned(value, result.tune_timeout_ms))
                return invalid("tune-timeout-ms is invalid");
            have_timeout = true;
        } else if (option == "--duration-seconds") {
            if (result.duration_set || !parse_unsigned(value, result.duration_seconds))
                return invalid("duration-seconds is invalid");
            result.duration_set = true;
        } else {
            if (result.packet_count_set || !parse_unsigned(value, result.packet_count))
                return invalid("packet-count is invalid");
            result.packet_count_set = true;
        }
    }
    if (have_device == have_instance)
        return invalid("exactly one of --device or --instance is required");
    if (!have_receiver) return invalid("--receiver is required");
    if (have_channel) {
        if (have_system || have_frequency)
            return invalid("--channel cannot be combined with --system or --frequency-khz");
        const Px4TsChannel channel = parse_px4_ts_channel(channel_text);
        if (!channel.valid) return invalid("channel is invalid");
        result.system = channel.system;
        result.frequency_khz = channel.frequency_khz;
        if (channel.kind == Px4TsChannelKind::satellite_slot) {
            if (have_stream_id || have_slot)
                return invalid("channel already includes the slot");
            result.slot = channel.slot;
            have_slot = true;
        } else if (channel.kind == Px4TsChannelKind::satellite_cs &&
                   !have_stream_id && !have_slot) {
            result.slot = channel.slot;
            have_slot = true;
        }
    } else {
        if (!have_system) return invalid("--system is required");
        if (!have_frequency) return invalid("--frequency-khz is required");
    }
    if (result.output.empty()) result.output = "-";
    if (have_runtime && result.runtime_directory.empty())
        return invalid("--runtime-dir must not be empty");
    if (result.system == System::ISDB_T && (have_stream_id || have_slot || have_lnb ||
                                            (have_bandwidth && result.bandwidth_hz != 6000000U)))
        return invalid("satellite-only fields used with isdb-t");
    if (result.system == System::ISDB_S && !have_bandwidth) result.bandwidth_hz = 0U;
    if (result.system == System::ISDB_S && have_bandwidth && result.bandwidth_hz != 0U)
        return invalid("satellite bandwidth must be 0");
    if (have_stream_id && have_slot) return invalid("stream-id and slot are exclusive");
    if (result.system == System::ISDB_S && !have_stream_id && !have_slot)
        return invalid("isdb-s requires stream-id or slot");
    if (!valid_arguments(result)) return invalid("argument value is out of range");
    result.valid = true;
    return result;
}

static Px4TsArguments parse_product_arguments(int argc,
                                              const char* const* argv,
                                              const TsClientProfile* profile) noexcept
{
    using WireSystem = dtv::userland::wire::System;
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
        return invalid("invalid argument vector");
    Px4TsArguments result;
    result.product_cli = true;
    result.common_profile = profile != nullptr;
    result.bandwidth_hz = 6000000U;
    bool have_device = false;
    bool have_instance = false;
    bool have_receiver = false;
    bool have_channel = false;
    bool have_frequency = false;
    bool have_system = false;
    bool have_slot = false;
    bool have_tsid = false;
    bool have_output = false;
    bool have_lnb = false;
    bool have_timeout = false;
    bool have_time = false;
    std::string_view channel;
    std::optional<std::uint64_t> frequency_hz;
    std::optional<WireSystem> system;
    std::optional<std::uint16_t> slot;
    std::optional<std::uint16_t> tsid;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "-h" || option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--version") {
            if (argc != 2) return invalid("--version cannot be combined");
            result.valid = true;
            result.version = true;
            return result;
        }
        if (option == "--group") {
            if (result.group) return invalid("duplicate --group");
            result.group = true;
            continue;
        }
        const bool takes_value = option == "-d" || option == "--device" ||
                                 option == "--instance" || option == "--runtime-dir" ||
                                 option == "--receiver" || option == "-c" ||
                                 option == "--channel" || option == "-f" ||
                                 option == "--freq" || option == "-s" ||
                                 option == "--system" || option == "--slot" ||
                                 option == "--tsid" || option == "-t" ||
                                 option == "--time" || option == "-o" ||
                                 option == "--output" || option == "--packet-count" ||
                                 option == "--lnb-voltage" ||
                                 option == "--tune-timeout-ms";
        if (!takes_value) return invalid("unknown argument");
        if (index + 1 >= argc || argv[index + 1] == nullptr)
            return invalid("option requires a value");
        const std::string_view value(argv[++index]);
        if (value.empty() || (value.rfind("--", 0U) == 0U && value != "--"))
            return invalid("option requires a value");

        if (option == "-d" || option == "--device") {
            if (have_device) return invalid("duplicate --device");
            have_device = true;
            result.device.assign(value.data(), value.size());
        } else if (option == "--instance") {
            if (have_instance) return invalid("duplicate --instance");
            have_instance = true;
            result.instance.assign(value.data(), value.size());
        } else if (option == "--runtime-dir") {
            if (!result.runtime_directory.empty()) return invalid("duplicate --runtime-dir");
            result.runtime_directory.assign(value.data(), value.size());
        } else if (option == "--receiver") {
            if (have_receiver || !parse_unsigned(value, result.receiver) ||
                (profile == nullptr && result.receiver >= kReceiverCount))
                return invalid("receiver must be an unsigned public receiver ID");
            have_receiver = true;
        } else if (option == "-c" || option == "--channel") {
            if (have_channel) return invalid("duplicate --channel");
            have_channel = true;
            channel = value;
        } else if (option == "-f" || option == "--freq") {
            std::uint64_t parsed = 0U;
            if (have_frequency || !parse_unsigned(value, parsed))
                return invalid("freq must be an unsigned decimal Hz value");
            have_frequency = true;
            frequency_hz = parsed;
        } else if (option == "-s" || option == "--system") {
            if (have_system) return invalid("duplicate --system");
            have_system = true;
            if (ascii_system_equals(value, "isdb-t")) system = WireSystem::isdb_t;
            else if (ascii_system_equals(value, "isdb-s")) system = WireSystem::isdb_s;
            else return invalid("system must be isdb-t or isdb-s");
        } else if (option == "--slot") {
            std::uint16_t parsed = 0U;
            if (have_slot || !parse_unsigned(value, parsed))
                return invalid("slot is invalid");
            have_slot = true;
            slot = parsed;
        } else if (option == "--tsid") {
            std::uint16_t parsed = 0U;
            if (have_tsid || !parse_unsigned(value, parsed))
                return invalid("tsid is invalid");
            have_tsid = true;
            tsid = parsed;
        } else if (option == "-t" || option == "--time") {
            if (have_time) return invalid("duplicate --time");
            have_time = true;
            if (value != "-") {
                if (!parse_unsigned(value, result.duration_seconds))
                    return invalid("time must be seconds or -");
                result.duration_set = true;
            }
        } else if (option == "-o" || option == "--output") {
            if (have_output) return invalid("duplicate --output");
            have_output = true;
            result.output.assign(value.data(), value.size());
        } else if (option == "--packet-count") {
            if (result.packet_count_set || !parse_unsigned(value, result.packet_count))
                return invalid("packet-count is invalid");
            result.packet_count_set = true;
        } else if (option == "--lnb-voltage") {
            if (have_lnb || !parse_unsigned(value, result.lnb_voltage) ||
                (result.lnb_voltage != 0U && result.lnb_voltage != 15U))
                return invalid("lnb-voltage must be 0 or 15");
            have_lnb = true;
        } else {
            if (have_timeout || !parse_unsigned(value, result.tune_timeout_ms))
                return invalid("tune-timeout-ms is invalid");
            have_timeout = true;
        }
    }
    if (!profile && !have_device && !have_instance)
        return invalid("--device or --instance is required");
    if (have_device) {
        const auto selector = dtv::userland::identity::parse_device_selector(result.device);
        if (!selector || (profile != nullptr && !profile->serial_selector_supported &&
                          selector.value().kind ==
                              dtv::userland::identity::SelectorKind::serial))
            return invalid("device must be a supported canonical serial: or usb: selector");
    }
    if (!have_receiver && profile == nullptr) return invalid("--receiver is required");
    result.receiver_explicit = have_receiver;
    if (have_channel == have_frequency)
        return invalid("exactly one of --channel or --freq is required");
    if (have_frequency && !have_system &&
        (profile == nullptr || !profile->frequency_system_optional))
        return invalid("--freq requires --system");
    if (have_frequency && !have_system && profile != nullptr &&
        profile->frequency_system_optional)
        system = WireSystem::isdb_t;
    if (result.output.empty()) result.output = "-";
    if (result.duration_set && result.duration_seconds == 0U)
        return invalid("time must be greater than zero");
    if (result.duration_set && result.packet_count_set)
        return invalid("--time and --packet-count are exclusive");

    const dtv::tuning::Request request{have_channel
                                           ? std::optional<std::string_view>(channel)
                                           : std::nullopt,
                                       frequency_hz, system, slot, tsid};
    const auto resolved = dtv::tuning::resolve(request);
    if (!resolved)
        return invalid("channel, frequency, system, slot, or tsid is invalid");
    result.system = resolved.value().system == WireSystem::isdb_t
                        ? System::ISDB_T : System::ISDB_S;
    result.frequency_hz = resolved.value().frequency_hz;
    if (resolved.value().selection == dtv::userland::wire::Selection::slot) {
        result.slot = resolved.value().selection_value;
    } else if (resolved.value().selection == dtv::userland::wire::Selection::tsid) {
        result.stream_id = resolved.value().selection_value;
    }
    if (result.system == System::ISDB_S) result.bandwidth_hz = 0U;
    if (profile != nullptr && !profile->preserve_instance_case) {
        for (char& character : result.instance) {
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (result.system == System::ISDB_T && have_lnb)
        return invalid("lnb-voltage is satellite-only");
    if (!valid_arguments(result)) return invalid("argument value is out of range");
    result.valid = true;
    return result;
}

Px4TsArguments parse_px4_ts_product_arguments(int argc,
                                              const char* const* argv) noexcept
{
    return parse_product_arguments(argc, argv, nullptr);
}

Px4TsArguments parse_px4_ts_product_arguments(
    int argc, const char* const* argv, const TsClientProfile& profile) noexcept
{
    if (profile.tool_name == nullptr || profile.product_slug == nullptr ||
        profile.tool_name[0] == '\0' || profile.product_slug[0] == '\0')
        return invalid("invalid product profile");
    return parse_product_arguments(argc, argv, &profile);
}

void print_px4_ts_usage(void* output) noexcept
{
    FILE* file = static_cast<FILE*>(output);
    if (file == nullptr) return;
    std::fprintf(file,
                 "usage: px4-ts (-d|--device SELECTOR [--instance TOKEN] | --instance TOKEN) --receiver 0..7 "
                 "(-c|--channel CH | -f|--freq HZ -s|--system isdb-t|isdb-s) [options]\n"
                 "  -c, --channel CH              (T13..T62 | 13..62 | BS<nn>[_<slot>] | CS<n>)\n"
                 "  -f, --freq HZ                 (unsigned decimal Hz)\n"
                 "  -s, --system isdb-t|isdb-s    (required with --freq)\n"
                 "      --slot N | --tsid N       (satellite selection, exactly one)\n"
                 "  -t, --time SECONDS|-          (default unlimited)\n"
                 "  -o, --output PATH|-           (default stdout)\n"
                 "  -d, --device serial:...|usb:... [--instance TOKEN]\n"
                 "      --receiver 0..7 --runtime-dir PATH --group\n"
                 "      --packet-count N --lnb-voltage 0|15 --tune-timeout-ms N\n"
                 "      endpoint socket path limits apply to the complete runtime/product/version/instance/file path and vary by OS; shorten --runtime-dir or --instance if diagnosed\n"
                 "  -h, --help\n"
                 "      --version\n");
}

void print_px4_ts_usage(const TsClientProfile& profile, void* output) noexcept
{
    FILE* file = static_cast<FILE*>(output);
    if (file == nullptr || profile.tool_name == nullptr) return;
    std::fprintf(file,
                 "usage: %s [--device serial:...|usb:...] [--instance TOKEN] "
                 "[--receiver ID] (-c|--channel CH | -f|--freq HZ -s|--system isdb-t|isdb-s) [options]\n"
                 "  -c, --channel CH              (T13..T62 | 13..62 | BS<nn>[_<slot>] | CS<n>)\n"
                 "  -f, --freq HZ                 (unsigned decimal Hz)\n"
                 "  -s, --system isdb-t|isdb-s    (required with --freq)\n"
                 "      --slot N | --tsid N       (satellite selection, exactly one)\n"
                 "  -t, --time SECONDS|-          (default unlimited)\n"
                 "  -o, --output PATH|-           (default stdout)\n"
                 "  -d, --device serial:...|usb:... [--instance TOKEN]\n"
                 "      --receiver ID --runtime-dir PATH --group\n"
                 "      --packet-count N --lnb-voltage 0|15 --tune-timeout-ms N\n"
                 "      endpoint socket path limits apply to the complete runtime/product/version/instance/file path and vary by OS; shorten --runtime-dir or --instance if diagnosed\n"
                 "  -h, --help\n"
                 "      --version\n", profile.tool_name);
}

int px4_ts_exit_status(Error error, Px4TsFailureKind kind) noexcept
{
    if (kind == Px4TsFailureKind::ts_integrity) return 8;
    if (kind == Px4TsFailureKind::output) return 70;
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH: return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::PROTOCOL_ERROR: return 6;
    case Error::SLOW_CONSUMER: return 8;
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::RESOURCE_LIMIT: return 4;
    case Error::INTERNAL: return 70;
    }
    return 70;
}

void print_px4_ts_run_diagnostic(
    std::FILE* output, const char* tool_name, Error error,
    const Px4TsRunDiagnostics& diagnostics) noexcept
{
    if (output == nullptr) return;
    const char* name = tool_name == nullptr ? "px4-ts" : tool_name;
    if (diagnostics.stage == Px4TsDiagnosticStage::endpoint_connect) {
        dtv::userland::host_runtime::print_endpoint_connect_diagnostic(
            output, name, error, diagnostics.endpoint_connect);
        return;
    }
    if (diagnostics.stage == Px4TsDiagnosticStage::receiver_selection) {
        if (!diagnostics.receiver_explicit && diagnostics.receiver_count > 1U) {
            std::fprintf(output,
                "%s: receiver selection is ambiguous (%u receivers are listed); specify --receiver using an ID listed by the service\n",
                name, static_cast<unsigned int>(diagnostics.receiver_count));
        } else if (diagnostics.receiver_explicit && error == Error::NOT_FOUND) {
            std::fprintf(output,
                "%s: --receiver %u is not present in the selected service; check the receiver ID reported by its status\n",
                name, static_cast<unsigned int>(diagnostics.requested_receiver));
        } else if (error == Error::UNSUPPORTED) {
            std::fprintf(output,
                "%s: --receiver %u does not support the requested %s system; select a compatible receiver\n",
                name, static_cast<unsigned int>(diagnostics.requested_receiver),
                diagnostics.requested_system == ipc::System::ISDB_T ? "ISDB-T" : "ISDB-S");
        } else if (diagnostics.receiver_count == 0U) {
            std::fprintf(output,
                "%s: the selected service has no receiver available; check that its tuner is ready\n",
                name);
        } else {
            std::fprintf(output,
                "%s: requested receiver selection is not available; check --receiver and the service receiver list\n",
                name);
        }
        return;
    }
    if (diagnostics.stage == Px4TsDiagnosticStage::recording_setup) {
        std::fprintf(output,
            "%s: daemon rejected a recording setup request (%s); the current protocol does not identify the individual request field; check --receiver and the channel/frequency/system/slot/TSID options against the selected service\n",
            name, error_string(error));
    }
}

namespace {

namespace wire = dtv::userland::wire;

class SocketClientTransport final : public wire::TimedClientTransport {
public:
    explicit SocketClientTransport(SocketStream& stream) noexcept : stream_(stream) {}
    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept override
    {
        return stream_.write_all(bytes, timeout);
    }
    Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept override
    {
        return stream_.read_some(bytes, timeout);
    }
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void close() noexcept override { stream_.close(); }

private:
    SocketStream& stream_;
};

Error wire_error(std::uint32_t code) noexcept
{
    switch (code) {
    case 1U: return Error::INVALID_ARGUMENT;
    case 2U: return Error::VERSION_MISMATCH;
    case 3U: return Error::NOT_FOUND;
    case 4U: return Error::BUSY;
    case 5U: return Error::NOT_READY;
    case 6U: return Error::TIMEOUT;
    case 7U: return Error::USB_IO;
    case 8U: return Error::DISCONNECTED;
    case 9U: return Error::PROTOCOL_ERROR;
    case 10U: return Error::FIRMWARE_REJECTED;
    case 11U: return Error::UNSUPPORTED;
    case 12U: return Error::NO_CARD;
    case 13U: return Error::CARD_REMOVED;
    case 14U: return Error::BUFFER_TOO_SMALL;
    case 15U: return Error::SLOW_CONSUMER;
    case 17U: return Error::RESOURCE_LIMIT;
    case 255U: return Error::INTERNAL;
    default: return Error::PROTOCOL_ERROR;
    }
}

CountersPayload wire_counters(const wire::StreamCounters& counters) noexcept
{
    CountersPayload result{};
    for (std::size_t index = 0U; index < counters.entries.count; ++index) {
        const auto& entry = counters.entries.items[index];
        if (entry.counter.measurement.state != wire::MeasureState::valid) continue;
        if (entry.id == wire::kCounterIdTransmittedPackets)
            result.packets = entry.counter.measurement.value;
        else if (entry.id == wire::kCounterIdQueueDroppedPackets)
            result.queue_drops = entry.counter.measurement.value;
    }
    result.bytes = result.packets * 188U;
    return result;
}

ErrorCode wire_end_error(wire::StreamEndReason reason) noexcept
{
    switch (reason) {
    case wire::StreamEndReason::stopped:
    case wire::StreamEndReason::shutdown: return ErrorCode::OK;
    case wire::StreamEndReason::slow_consumer: return ErrorCode::SLOW_CONSUMER;
    case wire::StreamEndReason::device_lost: return ErrorCode::DISCONNECTED;
    case wire::StreamEndReason::backend_error: return ErrorCode::INTERNAL;
    default: return ErrorCode::PROTOCOL_ERROR;
    }
}

bool wire_system(ipc::System system, wire::System& output) noexcept
{
    if (system == ipc::System::ISDB_T) {
        output = wire::System::isdb_t;
        return true;
    }
    if (system == ipc::System::ISDB_S) {
        output = wire::System::isdb_s;
        return true;
    }
    return false;
}

template <typename Record>
Result<std::size_t> encode_request(std::uint16_t opcode, std::uint32_t request_id,
                                   const Record& value,
                                   Result<std::size_t> (*encode)(const Record&,
                                                                 MutableByteView) noexcept,
                                   MutableByteView output) noexcept
{
    return wire::encode_record_frame(wire::Kind::request, opcode, request_id, value,
                                     encode, output);
}

class WireTsReceiver final : public wire::FrameHandler {
public:
    WireTsReceiver(Px4TsOutput& output, const Px4TsArguments& arguments,
                   Px4TsClock& clock, const Px4TsSignal& signal,
                   std::uint64_t stream_id,
                   Px4TsFailureKind& failure_kind) noexcept
        : output_(output), arguments_(arguments), clock_(clock), signal_(signal),
          stream_id_(stream_id), failure_kind_(failure_kind)
    {
    }

    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        if (frame.header.kind == wire::Kind::error_response) {
            const auto error = wire::decode_error_response(frame.payload);
            if (!error) return fail(error.error(), Px4TsFailureKind::ipc);
            failure_ = wire_error(error.value().error.code);
            return Result<void>::failure(failure_);
        }
        if (frame.header.kind == wire::Kind::response &&
            frame.header.opcode == wire::kOpcodeAttachStream) {
            if (attached_) return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
            const auto response = wire::decode_attach_response(frame.payload);
            if (!response || response.value().stream_id != stream_id_)
                return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
            attached_ = true;
            duration_started_ = true;
            duration_started_ms_ = clock_.monotonic_ms();
            last_progress_ms_ = duration_started_ms_;
            return Result<void>::success();
        }
        if (frame.header.kind == wire::Kind::event &&
            frame.header.opcode == wire::kOpcodeTsDataEvent) {
            if (!attached_) return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
            const auto data = wire::decode_ts_data(frame.payload);
            if (!data || data.value().stream_id != stream_id_ ||
                data.value().packets.size == 0U ||
                (data.value().packets.size % 188U) != 0U ||
                data.value().packets.size > wire::kMaxTsPacketBytes)
                return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ts_integrity);
            if (data.value().sequence != expected_sequence_)
                return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ts_integrity);
            ++expected_sequence_;
            for (std::size_t offset = 0U; offset < data.value().packets.size;
                 offset += 188U) {
                if (data.value().packets.data[offset] != 0x47U)
                    return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ts_integrity);
            }
            std::size_t write_size = data.value().packets.size;
            if (arguments_.packet_count_set) {
                const std::uint64_t remaining = arguments_.packet_count - packets_written_;
                const std::uint64_t available = data.value().packets.size / 188U;
                write_size = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, available) * 188U);
            }
            const std::size_t packets = write_size / 188U;
            for (std::size_t packet = 0U; packet < packets; ++packet) {
                const auto written = output_.write(
                    ByteView{data.value().packets.data + packet * 188U, 188U}, signal_);
                if (!written) {
                    if (written.error() == Error::TIMEOUT && signal_.stop_requested()) {
                        packets_written_ += packet;
                        output_stop_requested_ = true;
                        return Result<void>::success();
                    }
                    return fail(written.error(), Px4TsFailureKind::output);
                }
            }
            packets_written_ += packets;
            last_progress_ms_ = clock_.monotonic_ms();
            if (arguments_.packet_count_set &&
                packets_written_ >= arguments_.packet_count)
                limit_reached_ = true;
            return Result<void>::success();
        }
        if (frame.header.kind == wire::Kind::event &&
            frame.header.opcode == wire::kOpcodeStreamEndEvent) {
            if (!attached_ || final_valid_)
                return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
            const auto end = wire::decode_stream_end_event(frame.payload);
            if (!end || end.value().final.id != stream_id_)
                return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
            final_ = end.value().final;
            final_valid_ = true;
            return Result<void>::success();
        }
        return fail(Error::PROTOCOL_ERROR, Px4TsFailureKind::ipc);
    }

    Result<void> fail(Error error, Px4TsFailureKind kind) noexcept
    {
        failure_ = error;
        failure_kind_ = kind;
        return Result<void>::failure(error);
    }

    bool attached() const noexcept { return attached_; }
    bool limit_reached() const noexcept { return limit_reached_; }
    bool final_valid() const noexcept { return final_valid_; }
    const wire::StreamFinal& final() const noexcept { return final_; }
    std::uint64_t packets_written() const noexcept { return packets_written_; }
    std::uint64_t last_progress_ms() const noexcept { return last_progress_ms_; }
    bool duration_started() const noexcept { return duration_started_; }
    std::uint64_t duration_started_ms() const noexcept { return duration_started_ms_; }
    Px4TsFailureKind failure_kind() const noexcept { return failure_kind_; }
    Error failure() const noexcept { return failure_; }
    bool output_stop_requested() const noexcept { return output_stop_requested_; }

private:
    Px4TsOutput& output_;
    const Px4TsArguments& arguments_;
    Px4TsClock& clock_;
    const Px4TsSignal& signal_;
    std::uint64_t stream_id_ = 0U;
    Px4TsFailureKind& failure_kind_;
    Error failure_ = Error::OK;
    bool attached_ = false;
    bool limit_reached_ = false;
    bool final_valid_ = false;
    bool output_stop_requested_ = false;
    bool duration_started_ = false;
    std::uint64_t duration_started_ms_ = 0U;
    std::uint64_t packets_written_ = 0U;
    std::uint64_t expected_sequence_ = 0U;
    std::uint64_t last_progress_ms_ = 0U;
    wire::StreamFinal final_{};
};

struct ProductStatusAssertion final {
    const dtv::userland::identity::ResolvedIdentity* identity = nullptr;
    const char* product_slug = "px4-userland";
    const TsClientProfile* profile = nullptr;
    bool check_receiver = false;
    std::uint16_t receiver = 0U;
    System system = System::ISDB_T;
    std::string_view instance;
    const wire::StatusSnapshot* discovered = nullptr;
};

template <std::size_t N>
bool equal_text(const dtv::userland::wire::WString<N>& text,
                std::string_view value) noexcept
{
    return text.length == value.size() &&
           std::memcmp(text.bytes.data(), value.data(), value.size()) == 0;
}

bool matches_host_product(const dtv::userland::wire::WString<32U>& product,
                          const TsClientProfile* profile) noexcept
{
    if (profile != nullptr) return equal_text(product, profile->product_slug);
    return equal_text(product, "px4-userland") ||
           equal_text(product, "asicen-userland") ||
           equal_text(product, "pix-userland");
}

bool matches_product_identity(const wire::ListResponse& list,
                              const dtv::userland::identity::ResolvedIdentity& expected,
                              const TsClientProfile* profile = nullptr) noexcept
{
    if (!list.enclosure.present || list.functions.count == 0U ||
        list.topology.state != wire::TopologyState::complete ||
        !list.topology.expected_function_count.present ||
        list.topology.expected_function_count.value != list.functions.count)
        return false;
    if (expected.kind == dtv::userland::identity::IdentityKind::serial) {
        const auto& observed = list.enclosure.value.identity;
        if (observed.serial_state != wire::IdentitySerialState::present ||
            observed.uniqueness != wire::Uniqueness::unique_in_scan ||
            !observed.serial.present ||
            observed.serial.value.length != expected.serial.length ||
            std::memcmp(observed.serial.value.bytes.data(), expected.serial.bytes.data(),
                        expected.serial.length) != 0)
            return false;
        if (profile == nullptr) return false;
        if (profile->product == dtv::userland::identity::Product::px4)
            return profile->matches_serial_identity != nullptr &&
                   profile->matches_serial_identity(list, expected);
        if (profile->product == dtv::userland::identity::Product::pix)
            return profile->matches_serial_identity == nullptr;
        return false;
    }
    if (expected.topology.count == 0U) return false;
    std::size_t matched = 0U;
    for (std::size_t i = 0U; i < list.functions.count; ++i) {
        const auto& path = list.functions.items[i].location.usb_path;
        if (!path.present) return false;
        for (std::size_t j = 0U; j < i; ++j) {
            const auto& prior = list.functions.items[j].location.usb_path;
            if (prior.present && prior.value.length == path.value.length &&
                std::memcmp(prior.value.bytes.data(), path.value.bytes.data(),
                            path.value.length) == 0)
                return false;
        }
        for (std::size_t j = 0U; j < expected.topology.count; ++j) {
            const auto& wanted = expected.topology.paths[j];
            if (path.value.length == wanted.length &&
                std::memcmp(path.value.bytes.data(), wanted.bytes.data(), wanted.length) == 0)
                ++matched;
        }
    }
    return expected.topology.count == 1U ? matched == 1U
                                          : matched == list.functions.count &&
                                                list.functions.count == expected.topology.count;
}

Error validate_product_status(const wire::StatusSnapshot& snapshot,
                              void* context) noexcept
{
    const auto* assertion = static_cast<const ProductStatusAssertion*>(context);
    if (assertion == nullptr ||
        (assertion->product_slug != nullptr &&
         !equal_text(snapshot.status.producer.product, assertion->product_slug)) ||
        (assertion->product_slug == nullptr &&
         !matches_host_product(snapshot.status.producer.product, assertion->profile)) ||
        !snapshot.status.context.instance.present ||
        (!assertion->instance.empty() &&
         !equal_text(snapshot.status.context.instance.value, assertion->instance)) ||
        (assertion->identity != nullptr &&
         !matches_product_identity(snapshot.list, *assertion->identity,
                                   assertion->profile)))
        return Error::NOT_FOUND;
    if (assertion->check_receiver) {
        if (!snapshot.list.receivers.present || snapshot.list.receivers.value.count == 0U)
            return Error::NOT_READY;
        bool receiver_found = false;
        for (std::size_t i = 0U; i < snapshot.list.receivers.value.count; ++i) {
            const auto& receiver = snapshot.list.receivers.value.items[i];
            const std::uint8_t required_system = assertion->system == System::ISDB_T ? 1U : 2U;
            if (receiver.id == assertion->receiver &&
                (receiver.systems.mask & required_system) != 0U) {
                receiver_found = true;
                break;
            }
        }
        if (!receiver_found) return Error::NOT_FOUND;
    }
    if (assertion->discovered != nullptr) {
        const auto& prior = *assertion->discovered;
        if (snapshot.status.context.epoch != prior.status.context.epoch ||
            snapshot.list.functions.count != prior.list.functions.count ||
            snapshot.list.enclosure.present != prior.list.enclosure.present ||
            snapshot.list.enclosure.value.model.present !=
                prior.list.enclosure.value.model.present ||
            (snapshot.list.enclosure.value.model.present &&
            !equal_text(snapshot.list.enclosure.value.model.value.name,
                         std::string_view(prior.list.enclosure.value.model.value.name.bytes.data(),
                                          prior.list.enclosure.value.model.value.name.length))))
            return Error::NOT_FOUND;
        const auto& current_identity = snapshot.list.enclosure.value.identity;
        const auto& prior_identity = prior.list.enclosure.value.identity;
        if (current_identity.serial_state != prior_identity.serial_state ||
            current_identity.serial.present != prior_identity.serial.present ||
            (current_identity.serial.present &&
             (current_identity.serial.value.length != prior_identity.serial.value.length ||
              std::memcmp(current_identity.serial.value.bytes.data(),
                          prior_identity.serial.value.bytes.data(),
                          current_identity.serial.value.length) != 0)))
            return Error::NOT_FOUND;
        for (std::size_t i = 0U; i < snapshot.list.functions.count; ++i) {
            const auto& current = snapshot.list.functions.items[i];
            bool found = false;
            for (std::size_t j = 0U; j < prior.list.functions.count; ++j) {
                const auto& old = prior.list.functions.items[j];
                if (current.vid == old.vid && current.pid == old.pid &&
                    current.location.usb_path.present == old.location.usb_path.present &&
                    (!current.location.usb_path.present ||
                     (current.location.usb_path.value.length == old.location.usb_path.value.length &&
                      std::memcmp(current.location.usb_path.value.bytes.data(),
                                  old.location.usb_path.value.bytes.data(),
                                  current.location.usb_path.value.length) == 0))) {
                    found = true;
                    break;
                }
            }
            if (!found) return Error::NOT_FOUND;
        }
    }
    return Error::OK;
}

Error select_receiver(const wire::ListResponse& list, bool explicit_receiver,
                      std::uint16_t requested, System system,
                      std::uint16_t& selected) noexcept
{
    if (!list.receivers.present || list.receivers.value.count == 0U)
        return Error::NOT_READY;
    const std::uint8_t required_system = system == System::ISDB_T ? 1U : 2U;
    if (explicit_receiver) {
        for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
            const auto& receiver = list.receivers.value.items[i];
            if (receiver.id != requested) continue;
            if ((receiver.systems.mask & required_system) == 0U)
                return Error::UNSUPPORTED;
            selected = requested;
            return Error::OK;
        }
        return Error::NOT_FOUND;
    }
    if (list.receivers.value.count != 1U) return Error::INVALID_ARGUMENT;
    const auto& receiver = list.receivers.value.items[0];
    if ((receiver.systems.mask & required_system) == 0U) return Error::NOT_READY;
    selected = receiver.id;
    return Error::OK;
}

}  // namespace

static Result<Px4TsRunResult> run_wire_profile(
    const Px4TsArguments& arguments, Px4TsOutput& output, Px4TsClock& clock,
    const Px4TsSignal& signal, Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics, const TsClientProfile* profile,
    const ipc::posix::RuntimeRootCandidates* runtime_roots_override = nullptr) noexcept
{
    if (failure_kind != nullptr) *failure_kind = Px4TsFailureKind::ipc;
    if (diagnostics != nullptr) *diagnostics = Px4TsRunDiagnostics{};
    Px4TsFailureKind local_failure_kind = Px4TsFailureKind::ipc;
    Px4TsFailureKind& result_failure_kind =
        failure_kind != nullptr ? *failure_kind : local_failure_kind;
    if (!arguments.valid || !valid_arguments(arguments))
        return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);

    const EndpointAccess access = arguments.group ? EndpointAccess::shared_group
                                                  : EndpointAccess::private_user;
    dtv::userland::identity::ResolvedIdentity resolved_identity{};
    bool has_device_assertion = false;
    if (arguments.product_cli && !arguments.device.empty()) {
        const auto selector = dtv::userland::identity::parse_device_selector(arguments.device);
        if (!selector || (profile != nullptr && !profile->serial_selector_supported &&
                          selector.value().kind ==
                              dtv::userland::identity::SelectorKind::serial))
            return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
        const auto resolved = dtv::userland::identity::resolve_selector(
            profile != nullptr ? profile->product : dtv::userland::identity::Product::px4,
            selector.value());
        if (!resolved) return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
        resolved_identity = resolved.value();
        has_device_assertion = true;
    }
    std::string selected_runtime_directory = arguments.runtime_directory;
    std::string selected_instance = arguments.instance;
    wire::StatusSnapshot discovered_snapshot{};
    bool has_discovered_snapshot = false;
    if (arguments.product_cli && arguments.instance.empty()) {
        ipc::posix::RuntimeRootCandidates roots{};
        if (!arguments.runtime_directory.empty()) {
            roots.count = 1U;
            if (arguments.runtime_directory.size() >= roots.paths[0].size())
                return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
            std::memcpy(roots.paths[0].data(), arguments.runtime_directory.c_str(),
                        arguments.runtime_directory.size() + 1U);
        } else if (runtime_roots_override != nullptr) {
            roots = *runtime_roots_override;
        } else {
            const auto candidates = client_runtime_roots();
            if (!candidates) return Result<Px4TsRunResult>::failure(candidates.error());
            roots = candidates.value();
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(2000);
        std::size_t matches = 0U;
        std::string matched_root;
        std::string matched_instance;
        const auto remaining = [&deadline]() noexcept -> std::uint32_t {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return 0U;
            const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            return static_cast<std::uint32_t>(value > 0 ? value : 1);
        };
        for (std::size_t root_index = 0U; root_index < roots.count; ++root_index) {
            const std::string root(roots.paths[root_index].data());
            const auto discovered = discover_runtime_instances(
                root.c_str(), access,
                profile != nullptr ? profile->product_slug : "px4-userland");
            if (!discovered) {
                if (discovered.error() == Error::NOT_FOUND) continue;
                return Result<Px4TsRunResult>::failure(discovered.error());
            }
            if (discovered.value().canonical_collision)
                return Result<Px4TsRunResult>::failure(Error::NOT_FOUND);
            for (std::size_t candidate = 0U; candidate < discovered.value().count; ++candidate) {
                const std::uint32_t budget = remaining();
                if (budget == 0U) return Result<Px4TsRunResult>::failure(Error::TIMEOUT);
                const std::string candidate_instance(discovered.value().instances[candidate].data());
                const char* product_slug = profile != nullptr ? profile->product_slug :
                                                                "px4-userland";
#if !defined(_WIN32)
                const auto minimum_control_path = dtv::userland::host_runtime::
                    control_endpoint_path({"/", product_slug, candidate_instance});
                const auto minimum_data_path = dtv::userland::host_runtime::
                    endpoint_path({"/", product_slug, candidate_instance,
                                   dtv::userland::host_runtime::EndpointAccess::private_user,
                                   -1, "stream.sock"});
                if (!minimum_control_path || !minimum_data_path)
                    return Result<Px4TsRunResult>::failure(
                        !minimum_control_path ? minimum_control_path.error() :
                                                minimum_data_path.error());
                dtv::userland::host_runtime::EndpointDiagnostic path_diagnostic{};
                const auto candidate_control_path = dtv::userland::host_runtime::
                    control_endpoint_path({root, product_slug, candidate_instance},
                                          &path_diagnostic);
                if (!candidate_control_path) {
                    if (diagnostics != nullptr) {
                        diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
                        diagnostics->endpoint_connect.stage =
                            dtv::userland::host_runtime::EndpointConnectStage::endpoint_path;
                        diagnostics->endpoint_connect.endpoint = path_diagnostic;
                    }
                    continue;
                }
                const auto candidate_data_path = dtv::userland::host_runtime::
                    endpoint_path({root, product_slug, candidate_instance,
                                   dtv::userland::host_runtime::EndpointAccess::private_user,
                                   -1, "stream.sock"}, &path_diagnostic);
                if (!candidate_data_path) {
                    if (diagnostics != nullptr) {
                        diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
                        diagnostics->endpoint_connect.stage =
                            dtv::userland::host_runtime::EndpointConnectStage::endpoint_path;
                        diagnostics->endpoint_connect.endpoint = path_diagnostic;
                    }
                    continue;
                }
#endif
                const EndpointConfig endpoint{root.c_str(), candidate_instance.c_str(),
                                              "control.sock", access,
                                              product_slug};
                if (diagnostics != nullptr)
                    diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
                auto probe = SocketStream::connect(
                    endpoint, Timeout{budget},
                    diagnostics != nullptr ? &diagnostics->endpoint_connect : nullptr);
                if (!probe) {
                    if (probe.error() == Error::NOT_FOUND ||
                        probe.error() == Error::NOT_READY) continue;
                    return Result<Px4TsRunResult>::failure(probe.error());
                }
                if (diagnostics != nullptr)
                    diagnostics->stage = Px4TsDiagnosticStage::none;
                SocketClientTransport probe_transport(probe.value());
                const std::uint32_t fetch_budget = remaining();
                if (fetch_budget == 0U) {
                    probe.value().close();
                    return Result<Px4TsRunResult>::failure(Error::TIMEOUT);
                }
                const auto snapshot = wire::fetch_status(probe_transport, Timeout{fetch_budget});
                probe.value().close();
                if (!snapshot && (snapshot.error == Error::NOT_FOUND ||
                                  snapshot.error == Error::NOT_READY ||
                                  snapshot.error == Error::UNSUPPORTED)) continue;
                if (!snapshot)
                    return Result<Px4TsRunResult>::failure(snapshot.error);
                if (!matches_host_product(snapshot.snapshot.status.producer.product, profile) ||
                    !snapshot.snapshot.status.context.instance.present ||
                    !equal_text(snapshot.snapshot.status.context.instance.value,
                                candidate_instance) ||
                    (has_device_assertion &&
                     !matches_product_identity(snapshot.snapshot.list, resolved_identity,
                                               profile)))
                    continue;
                ++matches;
                if (matches == 1U) {
                    matched_root = root;
                    matched_instance = candidate_instance;
                    discovered_snapshot = snapshot.snapshot;
                }
            }
        }
        if (matches != 1U) return Result<Px4TsRunResult>::failure(Error::NOT_FOUND);
        selected_runtime_directory = matched_root;
        selected_instance = matched_instance;
        has_discovered_snapshot = true;
    }
    const char* runtime_directory = selected_runtime_directory.empty()
                                        ? nullptr : selected_runtime_directory.c_str();
    const char* instance = selected_instance.empty() ? arguments.device.c_str()
                                                       : selected_instance.c_str();
    const EndpointConfig control_endpoint{runtime_directory, instance,
                                          "control.sock", access,
                                          profile != nullptr ? profile->product_slug :
                                                               "px4-userland"};
    const EndpointConfig stream_endpoint{runtime_directory, instance,
                                         "stream.sock", access,
                                         profile != nullptr ? profile->product_slug :
                                                              "px4-userland"};

    std::uint16_t receiver_id = arguments.receiver;
    if (profile != nullptr) {
        if (!has_discovered_snapshot) {
            if (diagnostics != nullptr)
                diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
            auto probe = SocketStream::connect(
                control_endpoint, Timeout{2000U},
                diagnostics != nullptr ? &diagnostics->endpoint_connect : nullptr);
            if (!probe) return Result<Px4TsRunResult>::failure(probe.error());
            if (diagnostics != nullptr)
                diagnostics->stage = Px4TsDiagnosticStage::none;
            SocketClientTransport probe_transport(probe.value());
            const auto snapshot = wire::fetch_status(probe_transport, Timeout{4000U});
            probe.value().close();
            if (!snapshot) return Result<Px4TsRunResult>::failure(snapshot.error);
            discovered_snapshot = snapshot.snapshot;
            has_discovered_snapshot = true;
        }
        if (!matches_host_product(discovered_snapshot.status.producer.product, profile) ||
            (has_device_assertion &&
             !matches_product_identity(discovered_snapshot.list, resolved_identity,
                                       profile)))
            return Result<Px4TsRunResult>::failure(Error::NOT_FOUND);
        if (diagnostics != nullptr) {
            diagnostics->stage = Px4TsDiagnosticStage::receiver_selection;
            diagnostics->receiver_count = static_cast<std::uint16_t>(
                discovered_snapshot.list.receivers.present
                    ? discovered_snapshot.list.receivers.value.count : 0U);
            diagnostics->requested_receiver = arguments.receiver;
            diagnostics->receiver_explicit = arguments.receiver_explicit;
            diagnostics->requested_system = arguments.system;
        }
        const Error selected = select_receiver(discovered_snapshot.list,
                                               arguments.receiver_explicit,
                                               arguments.receiver, arguments.system,
                                               receiver_id);
        if (selected != Error::OK) return Result<Px4TsRunResult>::failure(selected);
        if (diagnostics != nullptr)
            diagnostics->stage = Px4TsDiagnosticStage::none;
    }

    if (diagnostics != nullptr)
        diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
    auto control = SocketStream::connect(
        control_endpoint, Timeout{2000U},
        diagnostics != nullptr ? &diagnostics->endpoint_connect : nullptr);
    if (!control) return Result<Px4TsRunResult>::failure(control.error());
    if (diagnostics != nullptr)
        diagnostics->stage = Px4TsDiagnosticStage::none;
    SocketClientTransport control_transport(control.value());

    wire::RecordingOptions options{};
    options.receiver_id = receiver_id;
    wire::System system{};
    if (!wire_system(arguments.system, system)) {
        control.value().close();
        return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
    }
    options.tune.system = system;
    options.tune.frequency_hz = arguments.frequency_hz != 0U
                                    ? arguments.frequency_hz
                                    : static_cast<std::uint64_t>(arguments.frequency_khz) * 1000U;
    if (arguments.stream_id != 0xffffU) {
        options.tune.selection = wire::Selection::tsid;
        options.tune.selection_value = arguments.stream_id;
    } else if (arguments.slot != 0xffffU) {
        options.tune.selection = wire::Selection::slot;
        options.tune.selection_value = arguments.slot;
    } else {
        options.tune.selection = wire::Selection::automatic;
    }
    if (arguments.lnb_voltage != 0U) {
        options.tune.lnb_voltage.present = true;
        options.tune.lnb_voltage.value = arguments.lnb_voltage;
    }
    options.tune.timeout_ms = arguments.tune_timeout_ms;

    ProductStatusAssertion assertion{};
    assertion.identity = has_device_assertion ? &resolved_identity : nullptr;
    if (profile != nullptr) {
        assertion.product_slug = profile->product_slug;
        assertion.profile = profile;
        assertion.check_receiver = true;
        assertion.receiver = receiver_id;
        assertion.system = arguments.system;
    } else {
        assertion.product_slug = nullptr;
    }
    assertion.instance = selected_instance;
    assertion.discovered = has_discovered_snapshot ? &discovered_snapshot : nullptr;
    auto started = arguments.product_cli
        ? wire::begin_recording(control_transport, options,
                                Timeout{arguments.tune_timeout_ms + 6000U},
                                validate_product_status, &assertion)
        : wire::begin_recording(control_transport, options,
                                Timeout{arguments.tune_timeout_ms + 6000U});
    if (!started) {
        if (diagnostics != nullptr && started.remote_error_code != 0U)
            diagnostics->stage = Px4TsDiagnosticStage::recording_setup;
        control.value().close();
        return Result<Px4TsRunResult>::failure(started.error);
    }
    wire::RecordingSession session = started.session;

    if (diagnostics != nullptr)
        diagnostics->stage = Px4TsDiagnosticStage::endpoint_connect;
    auto data = SocketStream::connect(
        stream_endpoint, Timeout{2000U},
        diagnostics != nullptr ? &diagnostics->endpoint_connect : nullptr);
    if (!data) {
        (void)wire::stop_recording(control_transport, session, Timeout{4000U});
        (void)wire::release_recording(control_transport, session, Timeout{4000U});
        control.value().close();
        return Result<Px4TsRunResult>::failure(data.error());
    }
    if (diagnostics != nullptr)
        diagnostics->stage = Px4TsDiagnosticStage::none;

    wire::AttachRequest attach{};
    attach.epoch = session.epoch;
    attach.lease_id = session.lease_id;
    attach.stream_id = session.stream_id;
    attach.attach_token = session.attach_token;
    std::array<std::uint8_t, wire::kHeaderSize + 128U> attach_frame{};
    const auto attach_size = encode_request(
        wire::kOpcodeAttachStream, 1U, attach, &wire::encode_attach_request,
        MutableByteView{attach_frame.data(), attach_frame.size()});
    if (!attach_size) {
        data.value().close();
        (void)wire::stop_recording(control_transport, session, Timeout{4000U});
        (void)wire::release_recording(control_transport, session, Timeout{4000U});
        control.value().close();
        return Result<Px4TsRunResult>::failure(attach_size.error());
    }
    const auto attach_written =
        data.value().write_all(ByteView{attach_frame.data(), attach_size.value()},
                               Timeout{2000U});
    if (!attach_written) {
        data.value().close();
        (void)wire::stop_recording(control_transport, session, Timeout{4000U});
        (void)wire::release_recording(control_transport, session, Timeout{4000U});
        control.value().close();
        return Result<Px4TsRunResult>::failure(attach_written.error());
    }

    std::unique_ptr<std::uint8_t[]> frame_storage;
#if defined(DTV_USERLAND_HOST_TS_TEST_ACCESS)
    if (!fail_next_frame_storage_allocation.exchange(false, std::memory_order_acq_rel))
#endif
        frame_storage.reset(new (std::nothrow) std::uint8_t[wire::kMaxFrameSize]);
    std::array<std::uint8_t, 8192U> read_buffer{};
    if (!frame_storage) {
        data.value().close();
        (void)wire::stop_recording(control_transport, session, Timeout{4000U});
        (void)wire::release_recording(control_transport, session, Timeout{4000U});
        control.value().close();
        return Result<Px4TsRunResult>::failure(Error::INTERNAL);
    }
    wire::FrameReceiver receiver(
        MutableByteView{frame_storage.get(), wire::kMaxFrameSize});
    bool stop_requested_by_client = false;
    WireTsReceiver reader(output, arguments, clock, signal, session.stream_id,
                          result_failure_kind);

    Error primary = Error::OK;
    std::uint64_t partial_frame_progress_ms = 0U;
    bool partial_frame_pending = false;
    const std::uint64_t first_ts_deadline = reader.last_progress_ms() == 0U
                                                ? clock.monotonic_ms() + kTsProgressTimeoutMs
                                                : reader.last_progress_ms() +
                                                      kTsProgressTimeoutMs;
    while (!reader.final_valid() && primary == Error::OK) {
        const std::uint64_t now = clock.monotonic_ms();
        if (signal.stop_requested()) {
            stop_requested_by_client = true;
            break;
        }
        if (reader.duration_started() && arguments.duration_set &&
            now >= reader.duration_started_ms() &&
            now - reader.duration_started_ms() >= arguments.duration_seconds * 1000U) {
            stop_requested_by_client = true;
            break;
        }
        if (reader.limit_reached()) {
            stop_requested_by_client = true;
            break;
        }
        if (!reader.attached() && now >= first_ts_deadline) {
            primary = Error::TIMEOUT;
            result_failure_kind = Px4TsFailureKind::ts_integrity;
            break;
        }
        if (partial_frame_pending && now >= partial_frame_progress_ms &&
            now - partial_frame_progress_ms >= kTsProgressTimeoutMs) {
            primary = Error::TIMEOUT;
            result_failure_kind = Px4TsFailureKind::ts_integrity;
            break;
        }
        const auto read = data.value().read_some(
            MutableByteView{read_buffer.data(), read_buffer.size()}, Timeout{50U});
        if (!read) {
            if (read.error() == Error::TIMEOUT) {
                clock.sleep_ms(10U);
                continue;
            }
            primary = read.error();
            break;
        }
        if (read.value() == 0U) {
            primary = Error::DISCONNECTED;
            break;
        }
        const auto fed = receiver.feed(
            ByteView{read_buffer.data(), read.value()}, reader);
        if (!fed) {
            primary = fed.error();
            break;
        }
        if (reader.output_stop_requested()) {
            stop_requested_by_client = true;
            break;
        }
        partial_frame_pending = receiver.pending_size() != 0U;
        if (partial_frame_pending) partial_frame_progress_ms = clock.monotonic_ms();
    }

    if (!reader.final_valid() && data.value().valid()) {
        const auto stopped =
            wire::stop_recording(control_transport, session, Timeout{4000U});
        if (!stopped && stopped.error != Error::NOT_FOUND && primary == Error::OK) {
            primary = stopped.error;
        }
        const std::uint64_t deadline = clock.monotonic_ms() + 4000U;
        while (!reader.final_valid() && primary == Error::OK &&
               clock.monotonic_ms() < deadline) {
            const auto read = data.value().read_some(
                MutableByteView{read_buffer.data(), read_buffer.size()}, Timeout{50U});
            if (!read) {
                if (read.error() == Error::TIMEOUT) {
                    clock.sleep_ms(10U);
                    continue;
                }
                primary = read.error();
                break;
            }
            if (read.value() == 0U) {
                primary = Error::DISCONNECTED;
                break;
            }
            const auto fed = receiver.feed(
                ByteView{read_buffer.data(), read.value()}, reader);
            if (!fed) {
                primary = fed.error();
                break;
            }
        }
        if (primary == Error::OK && !reader.final_valid()) primary = Error::TIMEOUT;
    }
    if (data.value().valid()) data.value().close();

    const auto released =
        wire::release_recording(control_transport, session, Timeout{4000U});
    if (!released && primary == Error::OK) {
        primary = released.error;
    }
    control.value().close();

    if (diagnostics != nullptr) {
        if (reader.final_valid() && reader.final().statistics.entries.count == 2U) {
            diagnostics->stop_counters_valid = true;
            diagnostics->stop_counters = wire_counters(reader.final().statistics);
        }
        diagnostics->stream_end_valid = reader.final_valid();
        if (reader.final_valid()) {
            diagnostics->stream_end.counters =
                wire_counters(reader.final().statistics);
            diagnostics->stream_end.error_code = wire_end_error(reader.final().reason);
        }
        diagnostics->packets_written = reader.packets_written();
    }
    if (reader.failure() != Error::OK) primary = reader.failure();
    if (primary != Error::OK) return Result<Px4TsRunResult>::failure(primary);
    if (!reader.final_valid())
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    const CountersPayload final_counters =
        wire_counters(reader.final().statistics);
    const ErrorCode end_error = wire_end_error(reader.final().reason);
    if (end_error != ErrorCode::OK) {
        if (end_error == ErrorCode::PROTOCOL_ERROR)
            result_failure_kind = Px4TsFailureKind::ts_integrity;
        return Result<Px4TsRunResult>::failure(
            end_error == ErrorCode::SLOW_CONSUMER ? Error::SLOW_CONSUMER :
            end_error == ErrorCode::USB_IO ? Error::USB_IO :
            end_error == ErrorCode::DISCONNECTED ? Error::DISCONNECTED :
            end_error == ErrorCode::PROTOCOL_ERROR ? Error::PROTOCOL_ERROR
                                                    : Error::INTERNAL);
    }
    if (final_counters.packets < reader.packets_written())
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (arguments.packet_count_set &&
        reader.packets_written() != arguments.packet_count)
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (!stop_requested_by_client && !arguments.duration_set &&
        !arguments.packet_count_set)
        return Result<Px4TsRunResult>::failure(Error::DISCONNECTED);
    return Result<Px4TsRunResult>::success(
        Px4TsRunResult{final_counters, signal.stop_requested()});
}

Result<Px4TsRunResult> Px4TsRunner::run_wire(
    const Px4TsArguments& arguments, Px4TsOutput& output, Px4TsClock& clock,
    const Px4TsSignal& signal, Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics) noexcept
{
    return run_wire_profile(arguments, output, clock, signal, failure_kind,
                            diagnostics, nullptr, nullptr);
}

Result<Px4TsRunResult> Px4TsRunner::run_wire(
    const Px4TsArguments& arguments, const TsClientProfile& profile,
    Px4TsOutput& output, Px4TsClock& clock, const Px4TsSignal& signal,
    Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics) noexcept
{
    if (!arguments.common_profile || profile.product_slug == nullptr ||
        profile.tool_name == nullptr)
        return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
    return run_wire_profile(arguments, output, clock, signal, failure_kind,
                            diagnostics, &profile, nullptr);
}

#if defined(DTV_USERLAND_HOST_TS_TEST_ACCESS)
namespace test_access {

void fail_next_frame_allocation() noexcept
{
    fail_next_frame_storage_allocation.store(true, std::memory_order_release);
}

Result<Px4TsRunResult> run_wire_with_roots(
    const Px4TsArguments& arguments, const TsClientProfile& profile,
    Px4TsOutput& output, Px4TsClock& clock, const Px4TsSignal& signal,
    const ipc::posix::RuntimeRootCandidates& roots,
    Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics) noexcept
{
    if (!arguments.common_profile || profile.product_slug == nullptr ||
        profile.tool_name == nullptr)
        return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
    return run_wire_profile(arguments, output, clock, signal, failure_kind,
                            diagnostics, &profile, &roots);
}

}  // namespace test_access
#endif

}  // namespace px4::userland::cli
