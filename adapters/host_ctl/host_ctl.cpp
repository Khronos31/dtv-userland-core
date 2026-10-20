// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

#include "px4/posix_ipc.h"

#include <dtv/userland/control_client.h>
#include <dtv/userland/json.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dtv::userland::host_ctl {
namespace {

using namespace px4::userland::ipc::posix;
namespace wire = dtv::userland::wire;
namespace identity = dtv::userland::identity;
using CommonError = dtv::userland::Error;

enum class Command : std::uint8_t {
    none,
    list,
    status,
    card_status,
    card_atr,
    card_reset,
    card_apdu,
};

struct Arguments final {
    bool valid = false;
    bool help = false;
    bool group = false;
    std::string device;
    std::string instance;
    std::string runtime_directory;
    std::string_view error;
    Command command = Command::none;
    std::array<std::uint8_t, wire::kMaxApduBytes> apdu{};
    std::size_t apdu_length = 0U;
};

Arguments invalid(std::string_view message) noexcept
{
    Arguments result;
    result.error = message;
    return result;
}

int hex_nibble(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_hex(std::string_view value, Arguments& result) noexcept
{
    if (value.empty()) return false;
    std::size_t offset = 0U;
    while (offset < value.size()) {
        if (result.apdu_length == result.apdu.size() || offset + 2U > value.size())
            return false;
        const int high = hex_nibble(value[offset]);
        const int low = hex_nibble(value[offset + 1U]);
        if (high < 0 || low < 0) return false;
        result.apdu[result.apdu_length++] = static_cast<std::uint8_t>((high << 4) | low);
        offset += 2U;
        if (offset == value.size()) break;
        if (value[offset++] != ':' || offset == value.size()) return false;
    }
    return result.apdu_length != 0U;
}

Command parse_command(std::string_view value, bool card_supported) noexcept
{
    if (value == "list") return Command::list;
    if (value == "status") return Command::status;
    if (card_supported) {
        if (value == "card-status") return Command::card_status;
        if (value == "card-atr") return Command::card_atr;
        if (value == "card-reset") return Command::card_reset;
        if (value == "card-apdu") return Command::card_apdu;
    }
    return Command::none;
}

Arguments parse_arguments(int argc, const char* const* argv,
                          bool card_supported) noexcept
{
    Arguments result;
    bool have_device = false;
    bool have_instance = false;
    bool have_runtime = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help" || option == "-h") {
            if (argc != 2) return invalid("help cannot be combined");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--version") return invalid("--version must be the only argument");
        if (option == "--group") {
            if (result.group) return invalid("duplicate --group");
            result.group = true;
            continue;
        }
        if (option == "--device" || option == "-d" || option == "--instance" ||
            option == "--runtime-dir") {
            if (index + 1 >= argc || argv[index + 1] == nullptr)
                return invalid("option requires a value");
            const std::string_view value(argv[++index]);
            if (option == "--device" || option == "-d") {
                if (have_device) return invalid("duplicate --device");
                have_device = true;
                result.device.assign(value);
            } else if (option == "--instance") {
                if (have_instance) return invalid("duplicate --instance");
                have_instance = true;
                result.instance.assign(value);
            } else {
                if (have_runtime) return invalid("duplicate --runtime-dir");
                have_runtime = true;
                result.runtime_directory.assign(value);
            }
            continue;
        }
        if (result.command == Command::none) {
            result.command = parse_command(option, card_supported);
            if (result.command == Command::none) return invalid("unknown command");
            if (result.command == Command::card_apdu) {
                if (index + 1 >= argc || argv[index + 1] == nullptr ||
                    !parse_hex(argv[++index], result))
                    return invalid("card-apdu requires 1..4096 colon-separated bytes");
            }
            continue;
        }
        return invalid("unexpected argument");
    }
    if (result.command == Command::none) return invalid("a command is required");
    if ((have_device && result.device.empty()) || (have_instance && result.instance.empty()))
        return invalid("selector value must not be empty");
    if (have_runtime && result.runtime_directory.empty())
        return invalid("--runtime-dir must not be empty");
#if !defined(_WIN32)
    if (result.group && !have_runtime)
        return invalid("--group requires --runtime-dir");
#endif
    result.valid = true;
    return result;
}

bool normalize_instance(std::string_view value, identity::InstanceText& output,
                        const Profile& profile) noexcept
{
    if (profile.normalize_instance != nullptr)
        return profile.normalize_instance(value, output);
    const auto canonical = identity::canonicalize_instance(value);
    if (!canonical) return false;
    output = canonical.value();
    return true;
}

bool is_card_command(Command command) noexcept
{
    return command == Command::card_status || command == Command::card_atr ||
           command == Command::card_reset || command == Command::card_apdu;
}

int exit_status(CommonError error, bool card_operation = false) noexcept
{
    switch (error) {
    case CommonError::OK: return 0;
    case CommonError::INVALID_ARGUMENT: return 2;
    case CommonError::NOT_FOUND:
    case CommonError::NOT_READY:
    case CommonError::UNSUPPORTED: return 3;
    case CommonError::BUSY:
    case CommonError::RESOURCE_LIMIT: return 4;
    case CommonError::TIMEOUT: return 5;
    case CommonError::VERSION_MISMATCH: return 6;
    case CommonError::PROTOCOL_ERROR: return card_operation ? 9 : 6;
    case CommonError::USB_IO:
    case CommonError::DISCONNECTED: return 7;
    case CommonError::SLOW_CONSUMER: return 8;
    case CommonError::NO_CARD:
    case CommonError::CARD_REMOVED:
    case CommonError::BUFFER_TOO_SMALL: return 9;
    case CommonError::FIRMWARE_REJECTED: return 10;
    case CommonError::INTERNAL: return 70;
    }
    return 70;
}

void usage(FILE* output, const Profile& profile) noexcept
{
    std::fprintf(output,
                 "usage: %s [--device serial:SERIAL|usb:BUS-PORT[,BUS-PORT...]] "
                 "[--instance TOKEN] [--runtime-dir PATH] [--group] COMMAND\n"
                 "       %s --help | --version\ncommands: list, status",
                 profile.tool_name, profile.tool_name);
    if (profile.card_supported)
        std::fputs(", card-status, card-atr, card-reset, card-apdu HEX", output);
    std::fputc('\n', output);
}

void print_hex(const char* label, const std::uint8_t* data, std::size_t size) noexcept
{
    std::printf("%s=", label);
    for (std::size_t index = 0U; index < size; ++index)
        std::printf("%s%02x", index == 0U ? "" : ":",
                    static_cast<unsigned int>(data[index]));
    std::putchar('\n');
}

class SocketTransport final : public wire::ClientTransport {
public:
    explicit SocketTransport(SocketStream stream) noexcept : stream_(std::move(stream)) {}

    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept override
    {
        const auto result = stream_.write_all(
            px4::userland::ByteView{bytes.data, bytes.size},
            px4::userland::Timeout{timeout.milliseconds});
        return result ? Result<void>::success() :
                        Result<void>::failure(static_cast<CommonError>(result.error()));
    }

    Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept override
    {
        const auto result = stream_.read_some(
            px4::userland::MutableByteView{bytes.data, bytes.size},
            px4::userland::Timeout{timeout.milliseconds});
        return result ? Result<std::size_t>::success(result.value()) :
                        Result<std::size_t>::failure(
                            static_cast<CommonError>(result.error()));
    }

    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void close() noexcept override { stream_.close(); }

private:
    SocketStream stream_;
};

Result<identity::ResolvedIdentity> resolved_device(std::string_view value,
                                                    const Profile& profile) noexcept
{
    const auto selector = identity::parse_device_selector(value);
    if (!selector)
        return Result<identity::ResolvedIdentity>::failure(selector.error());
    if (selector.value().kind == identity::SelectorKind::serial &&
        !profile.serial_selector_supported)
        return Result<identity::ResolvedIdentity>::failure(CommonError::INVALID_ARGUMENT);
    return identity::resolve_selector(profile.product, selector.value());
}

bool matches_device(const wire::ListResponse& list,
                    const identity::ResolvedIdentity& expected,
                    const Profile& profile) noexcept
{
    if (!list.enclosure.present) return false;
    if (expected.kind == identity::IdentityKind::serial)
        return profile.matches_serial != nullptr && profile.matches_serial(list, expected);
    for (std::size_t i = 0U; i < expected.topology.count; ++i) {
        bool found = false;
        for (std::size_t j = 0U; j < list.functions.count; ++j) {
            const auto& path = list.functions.items[j].location.usb_path;
            if (path.present && path.value.length == expected.topology.paths[i].length &&
                std::memcmp(path.value.bytes.data(), expected.topology.paths[i].bytes.data(),
                            expected.topology.paths[i].length) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return list.functions.count == expected.topology.count;
}

const char* mode_name(wire::Mode mode) noexcept
{
    switch (mode) {
    case wire::Mode::cold: return "cold";
    case wire::Mode::runtime: return "runtime";
    case wire::Mode::unknown: return "unknown";
    }
    return "unknown";
}

const char* context_state_name(wire::ContextState state) noexcept
{
    switch (state) {
    case wire::ContextState::starting: return "starting";
    case wire::ContextState::ready: return "ready";
    case wire::ContextState::stopping: return "stopping";
    case wire::ContextState::error: return "error";
    case wire::ContextState::disconnected: return "disconnected";
    }
    return "unknown";
}

const char* receiver_system_name(wire::Systems systems) noexcept
{
    switch (systems.mask) {
    case 0x01U: return "ISDB-T";
    case 0x02U: return "ISDB-S";
    case 0x03U: return "ISDB-T,ISDB-S";
    case 0x04U: return "ISDB-C";
    case 0x05U: return "ISDB-T,ISDB-C";
    case 0x06U: return "ISDB-S,ISDB-C";
    case 0x07U: return "ISDB-T,ISDB-S,ISDB-C";
    }
    return "unknown";
}

std::size_t device_number(const wire::ListResponse& list,
                          const wire::ReceiverDescriptor& receiver) noexcept
{
    if (!receiver.binding.present) return 0U;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        if (list.functions.items[index].id == receiver.binding.value.function_id)
            return index + 1U;
    }
    return 0U;
}

bool print_list(const wire::ListResponse& list, const wire::StatusResponse& status) noexcept
{
    std::uint8_t usb_present_mask = 0U;
    for (std::size_t index = 0U; index < list.functions.count && index < 8U; ++index) {
        if (list.functions.items[index].present)
            usb_present_mask = static_cast<std::uint8_t>(
                usb_present_mask | static_cast<std::uint8_t>(1U << index));
    }
    if (std::fputs("serial=", stdout) == EOF) return false;
    if (list.enclosure.present) {
        const wire::Identity& device_identity = list.enclosure.value.identity;
        if (device_identity.serial_state == wire::IdentitySerialState::present &&
            device_identity.serial.present) {
            if (std::fprintf(stdout, "%.*s",
                             static_cast<int>(device_identity.serial.value.length),
                             device_identity.serial.value.bytes.data()) < 0)
                return false;
        } else if (std::fputs("unknown", stdout) == EOF) {
            return false;
        }
    } else if (std::fputs("unknown", stdout) == EOF) {
        return false;
    }
    if (std::fprintf(stdout, " ready=%s usb-present-mask=0x%02x\n",
                     context_state_name(status.context.state),
                     static_cast<unsigned int>(usb_present_mask)) < 0)
        return false;
    if (list.enclosure.present) {
        const wire::Enclosure& enclosure = list.enclosure.value;
        if (std::fprintf(stdout, "enclosure id=%u",
                         static_cast<unsigned int>(enclosure.id)) < 0)
            return false;
        if (enclosure.model.present &&
            std::fprintf(stdout, " model=%.*s",
                         static_cast<int>(enclosure.model.value.name.length),
                         enclosure.model.value.name.bytes.data()) < 0)
            return false;
        if (std::fputc('\n', stdout) == EOF) return false;
    } else if (std::fputs("enclosure unavailable\n", stdout) == EOF) {
        return false;
    }
    if (std::fputs("functions:\n", stdout) == EOF) return false;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        const wire::UsbFunction& function = list.functions.items[index];
        if (std::fprintf(stdout, "  function id=%u usb=%04x:%04x mode=%s",
                         static_cast<unsigned int>(function.id),
                         static_cast<unsigned int>(function.vid),
                         static_cast<unsigned int>(function.pid), mode_name(function.mode)) < 0)
            return false;
        if (function.location.usb_path.present &&
            std::fprintf(stdout, " path=%.*s",
                         static_cast<int>(function.location.usb_path.value.length),
                         function.location.usb_path.value.bytes.data()) < 0)
            return false;
        if (std::fputc('\n', stdout) == EOF) return false;
    }
    if (!list.receivers.present) {
        if (std::fputs("receivers=unknown\n", stdout) == EOF) return false;
    } else if (list.receivers.value.count == 0U) {
        if (std::fputs("receivers=0\n", stdout) == EOF) return false;
    } else {
        for (std::size_t index = 0U; index < list.receivers.value.count; ++index) {
            const wire::ReceiverDescriptor& receiver = list.receivers.value.items[index];
            const std::size_t device = device_number(list, receiver);
            if (std::fprintf(stdout, "receiver=%u device=",
                             static_cast<unsigned int>(receiver.id)) < 0)
                return false;
            if (device == 0U) {
                if (std::fputs("unknown", stdout) == EOF) return false;
            } else if (std::fprintf(stdout, "%u", static_cast<unsigned int>(device)) < 0) {
                return false;
            }
            if (receiver.binding.present) {
                if (std::fprintf(stdout, " local=%u",
                                 static_cast<unsigned int>(receiver.binding.value.local_id)) < 0)
                    return false;
            } else if (std::fputs(" local=unknown", stdout) == EOF) {
                return false;
            }
            if (std::fprintf(stdout, " system=%s\n",
                             receiver_system_name(receiver.systems)) < 0)
                return false;
        }
    }
    if (!list.readers.present) {
        if (std::fputs("readers=unknown\n", stdout) == EOF) return false;
    } else if (list.readers.value.count == 0U) {
        if (std::fputs("readers=0\n", stdout) == EOF) return false;
    } else {
        if (std::fputs("readers:\n", stdout) == EOF) return false;
        for (std::size_t index = 0U; index < list.readers.value.count; ++index) {
            const wire::ReaderDescriptor& reader = list.readers.value.items[index];
            if (std::fprintf(stdout, "  reader id=%u",
                             static_cast<unsigned int>(reader.id)) < 0)
                return false;
            if (reader.binding.present &&
                std::fprintf(stdout, " binding=%u:%u",
                             static_cast<unsigned int>(reader.binding.value.function_id),
                             static_cast<unsigned int>(reader.binding.value.local_id)) < 0)
                return false;
            if (std::fputc('\n', stdout) == EOF) return false;
        }
    }
    return std::fflush(stdout) == 0;
}

struct RuntimeCandidate final {
    std::string runtime_directory;
    std::string instance;
};

int report_error(const Profile& profile, std::string_view message,
                 CommonError error) noexcept
{
    std::fprintf(stderr, "%s: %.*s (%u)\n", profile.tool_name,
                 static_cast<int>(message.size()), message.data(),
                 static_cast<unsigned int>(error));
    return exit_status(error);
}

}  // namespace

int run_control_cli(int argc, const char* const* argv, const Profile& profile) noexcept
{
    if (argc == 2 && argv != nullptr && argv[1] != nullptr &&
        std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", profile.product_slug, profile.product_version);
        return 0;
    }
    const Arguments arguments = parse_arguments(argc, argv, profile.card_supported);
    if (!arguments.valid) {
        std::fprintf(stderr, "%s: %.*s\n", profile.tool_name,
                     static_cast<int>(arguments.error.size()), arguments.error.data());
        usage(stderr, profile);
        return 2;
    }
    if (arguments.help) {
        usage(stdout, profile);
        return 0;
    }
#if defined(_WIN32)
    if (arguments.group) {
        std::fprintf(stderr, "%s: --group is unsupported on Windows\n", profile.tool_name);
        return 3;
    }
#endif

    std::string instance = arguments.instance;
    identity::ResolvedIdentity expected{};
    const bool has_device = !arguments.device.empty();
    if (has_device) {
        const auto resolved = resolved_device(arguments.device, profile);
        if (!resolved) {
            std::fprintf(stderr, "%s: invalid --device selector\n", profile.tool_name);
            return 2;
        }
        expected = resolved.value();
        if (instance.empty()) {
            const auto automatic = identity::auto_instance(expected);
            if (!automatic) return exit_status(automatic.error());
            instance.assign(automatic.value().bytes.data(), automatic.value().length);
        }
    }

    std::vector<std::string> runtime_directories;
    const bool search_runtime_roots = arguments.runtime_directory.empty();
    if (!search_runtime_roots) {
        runtime_directories.push_back(arguments.runtime_directory);
    } else {
        const auto candidates = client_runtime_roots();
        if (!candidates) return exit_status(candidates.error());
        for (std::size_t index = 0U; index < candidates.value().count; ++index)
            runtime_directories.emplace_back(candidates.value().paths[index].data());
    }

    const bool discover_instances = instance.empty();
    std::vector<RuntimeCandidate> endpoints;
    if (discover_instances) {
        for (const std::string& runtime_directory : runtime_directories) {
            const auto discovered = discover_runtime_instances(
                runtime_directory.c_str(),
                arguments.group ? EndpointAccess::shared_group : EndpointAccess::private_user);
            if (!discovered) {
                if (search_runtime_roots &&
                    (discovered.error() == CommonError::NOT_FOUND ||
                     discovered.error() == CommonError::NOT_READY))
                    continue;
                return exit_status(discovered.error());
            }
            if (discovered.value().canonical_collision) {
                std::fprintf(stderr, "%s: ambiguous runtime instances\n", profile.tool_name);
                return 3;
            }
            for (std::size_t index = 0U; index < discovered.value().count; ++index)
                endpoints.push_back(RuntimeCandidate{
                    runtime_directory, discovered.value().instances[index].data()});
        }
    } else {
        identity::InstanceText normalized{};
        if (!normalize_instance(instance, normalized, profile)) {
            std::fprintf(stderr, "%s: --instance is invalid\n", profile.tool_name);
            return 2;
        }
        instance.assign(normalized.bytes.data(), normalized.length);
        for (const std::string& runtime_directory : runtime_directories)
            endpoints.push_back(RuntimeCandidate{runtime_directory, instance});
    }
    if (endpoints.empty()) return 3;

    wire::StatusFetchResult snapshot{};
    RuntimeCandidate selected_candidate{};
    std::unique_ptr<SocketTransport> selected_transport;
    bool found = false;
    for (const RuntimeCandidate& candidate : endpoints) {
        const EndpointConfig endpoint{
            candidate.runtime_directory.empty() ? nullptr : candidate.runtime_directory.c_str(),
            candidate.instance.c_str(), kControlEndpointName,
            arguments.group ? EndpointAccess::shared_group : EndpointAccess::private_user};
        auto stream = SocketStream::connect(endpoint, px4::userland::Timeout{2000U});
        if (!stream) {
            if ((discover_instances || search_runtime_roots) &&
                (stream.error() == CommonError::NOT_FOUND ||
                 stream.error() == CommonError::NOT_READY))
                continue;
            return report_error(profile, "connect failed", stream.error());
        }
        auto transport = std::make_unique<SocketTransport>(std::move(stream.value()));
        const std::uint64_t required_features =
            arguments.command == Command::card_status ? wire::kFeatureCardService : 0U;
        auto current = wire::fetch_status(*transport, Timeout{5000U}, required_features);
        if (!current) {
            std::fprintf(stderr, "%s: status failed (%u, remote=%u)\n",
                         profile.tool_name, static_cast<unsigned int>(current.error),
                         static_cast<unsigned int>(current.remote_error_code));
            return exit_status(current.error);
        }
        const std::size_t product_length = std::strlen(profile.product_slug);
        const auto& product = current.snapshot.status.producer.product;
        if (product.length != product_length ||
            std::memcmp(product.bytes.data(), profile.product_slug, product_length) != 0) {
            std::fprintf(stderr, "%s: endpoint belongs to another product\n", profile.tool_name);
            return 3;
        }
        if (has_device && !matches_device(current.snapshot.list, expected, profile)) {
            std::fprintf(stderr, "%s: connected instance does not match --device\n",
                         profile.tool_name);
            return 3;
        }
        if (!current.snapshot.status.context.instance.present) {
            std::fprintf(stderr, "%s: daemon omitted canonical instance\n", profile.tool_name);
            return 3;
        }
        const auto& actual_instance = current.snapshot.status.context.instance.value;
        if (actual_instance.length != candidate.instance.size() ||
            std::memcmp(actual_instance.bytes.data(), candidate.instance.data(),
                        candidate.instance.size()) != 0) {
            std::fprintf(stderr, "%s: connected instance does not match selector\n",
                         profile.tool_name);
            return 3;
        }
        if (found) {
            std::fprintf(stderr, "%s: multiple runtime roots publish this target\n",
                         profile.tool_name);
            return 3;
        }
        snapshot = std::move(current);
        selected_candidate = candidate;
        selected_transport = std::move(transport);
        found = true;
    }
    if (!found) return 3;
    if (arguments.command == Command::list) {
        if (!print_list(snapshot.snapshot.list, snapshot.snapshot.status)) {
            std::fprintf(stderr, "%s: list output failed\n", profile.tool_name);
            return 70;
        }
        return 0;
    }
    if (arguments.command == Command::card_status) {
        if (!snapshot.snapshot.list.readers.present ||
            snapshot.snapshot.list.readers.value.count == 0U) {
            std::fprintf(stderr, "%s: no card reader is available\n", profile.tool_name);
            return 3;
        }
        wire::CardClientSession session{};
        session.epoch = snapshot.snapshot.status.context.epoch;
        session.reader_id = snapshot.snapshot.list.readers.value.items[0U].id;
        session.next_request_id = 4U;
        const auto card = wire::card_status(*selected_transport, session, Timeout{5000U});
        if (!card) return report_error(profile, "card-status failed", card.error);
        const wire::CardStatusResponse& response = card.response;
        std::printf("present=%s initialized=%s reader-generation=%llu\n",
                    response.present ? "yes" : "no",
                    response.initialized ? "yes" : "no",
                    static_cast<unsigned long long>(response.current_generation));
        if (response.atr.present)
            print_hex("atr", response.atr.value.bytes.data(), response.atr.value.length);
        return std::fflush(stdout) == 0 ? 0 : 70;
    }
    if (is_card_command(arguments.command)) {
        const EndpointConfig endpoint{
            selected_candidate.runtime_directory.empty() ? nullptr :
                selected_candidate.runtime_directory.c_str(),
            selected_candidate.instance.c_str(), kControlEndpointName,
            arguments.group ? EndpointAccess::shared_group : EndpointAccess::private_user};
        selected_transport->close();
        auto stream = SocketStream::connect(endpoint, px4::userland::Timeout{2000U});
        if (!stream) return report_error(profile, "connect failed", stream.error());
        SocketTransport card_transport(std::move(stream.value()));
        if (!snapshot.snapshot.list.readers.present ||
            snapshot.snapshot.list.readers.value.count == 0U) {
            std::fprintf(stderr, "%s: no card reader is available\n", profile.tool_name);
            return 3;
        }
        wire::CardClientOptions options{};
        options.reader_id = snapshot.snapshot.list.readers.value.items[0U].id;
        const auto opened = wire::begin_card(card_transport, options, Timeout{5000U});
        if (!opened) return report_error(profile, "card connect failed", opened.error);
        if (opened.session.epoch != snapshot.snapshot.status.context.epoch ||
            opened.session.reader_id != options.reader_id) {
            wire::CardClientSession stale = opened.session;
            if (stale.card_handle != 0U) {
                const auto released = wire::card_disconnect(
                    card_transport, stale, wire::Disposition::leave, Timeout{5000U});
                if (!released)
                    std::fprintf(stderr, "%s: stale card handle release failed (%u)\n",
                                 profile.tool_name, static_cast<unsigned int>(released.error));
            }
            std::fprintf(stderr, "%s: daemon changed during card selection\n",
                         profile.tool_name);
            return 3;
        }
        wire::CardClientSession session = opened.session;
        CommonError operation_error = CommonError::OK;
        if (arguments.command == Command::card_atr) {
            if (!opened.connection.atr.present) {
                operation_error = CommonError::NOT_READY;
            } else {
                print_hex("atr", opened.connection.atr.value.bytes.data(),
                          opened.connection.atr.value.length);
            }
        } else if (arguments.command == Command::card_reset) {
            const auto reset = wire::card_reset(card_transport, session, Timeout{5000U});
            if (!reset) operation_error = reset.error;
            else if (!reset.response.atr.present) operation_error = CommonError::NOT_READY;
            else print_hex("atr", reset.response.atr.value.bytes.data(),
                           reset.response.atr.value.length);
        } else {
            const auto transmitted = wire::card_transmit(
                card_transport, session, wire::CardProtocol::t1,
                ByteView{arguments.apdu.data(), arguments.apdu_length}, Timeout{5000U});
            if (!transmitted) {
                operation_error = transmitted.error;
            } else {
                print_hex("response", transmitted.response.response.bytes.data(),
                          transmitted.response.response.length);
                std::printf("response-length=%zu\ntransmit-count=1\n",
                            static_cast<std::size_t>(
                                transmitted.response.response.length));
            }
        }
        if (session.card_handle != 0U) {
            const auto disconnected = wire::card_disconnect(
                card_transport, session, wire::Disposition::leave, Timeout{5000U});
            if (!disconnected && operation_error == CommonError::OK)
                operation_error = disconnected.error;
        }
        if (operation_error != CommonError::OK) {
            std::fprintf(stderr, "%s: card operation failed (%u)\n", profile.tool_name,
                         static_cast<unsigned int>(operation_error));
            return exit_status(operation_error, true);
        }
        return std::fflush(stdout) == 0 ? 0 : 70;
    }

    wire::StatusContext context{&snapshot.snapshot.list};
    const auto size = wire::status_json_size(snapshot.snapshot.status, context);
    if (!size) return exit_status(size.error());
    std::vector<std::uint8_t> output(size.value());
    const auto encoded = wire::encode_status_json(
        snapshot.snapshot.status, context,
        MutableByteView{output.data(), output.size()});
    if (!encoded || std::fwrite(output.data(), 1U, encoded.value(), stdout) != encoded.value() ||
        std::fflush(stdout) != 0) {
        std::fprintf(stderr, "%s: status output failed\n", profile.tool_name);
        return 70;
    }
    return 0;
}

}  // namespace dtv::userland::host_ctl
