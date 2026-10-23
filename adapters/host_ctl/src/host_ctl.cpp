// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

#include <dtv/userland/control_client.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/json.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace dtv::userland::host_ctl {
namespace {

enum class Command : std::uint8_t { none, list, status };

struct Arguments final {
    bool valid = false;
    bool help = false;
    bool group = false;
    std::string instance;
    std::string runtime_directory;
    std::string device;
    std::string_view error;
    Command command = Command::none;
};

Arguments invalid(std::string_view message) noexcept
{
    Arguments value{};
    value.error = message;
    return value;
}

Arguments parse(int argc, const char* const* argv) noexcept
{
    Arguments value{};
    bool have_instance = false;
    bool have_runtime = false;
    bool have_device = false;
    if (argc < 0 || argv == nullptr) return invalid("invalid arguments");
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            if (argc != 2) return invalid("help cannot be combined");
            value.valid = true;
            value.help = true;
            return value;
        }
        if (argument == "--version") return invalid("--version must be the only argument");
        if (argument == "--group") {
            if (value.group) return invalid("duplicate --group");
            value.group = true;
            continue;
        }
        if (argument == "--instance" || argument == "--runtime-dir" ||
            argument == "--device" || argument == "-d") {
            if (index + 1 >= argc || argv[index + 1] == nullptr)
                return invalid("option requires a value");
            const std::string_view text(argv[++index]);
            if (argument == "--instance") {
                if (have_instance) return invalid("duplicate --instance");
                have_instance = true;
                value.instance.assign(text);
            } else if (argument == "--runtime-dir") {
                if (have_runtime) return invalid("duplicate --runtime-dir");
                have_runtime = true;
                value.runtime_directory.assign(text);
            } else {
                if (have_device) return invalid("duplicate --device");
                have_device = true;
                value.device.assign(text);
            }
            continue;
        }
        if (value.command != Command::none) return invalid("unexpected argument");
        if (argument == "list") value.command = Command::list;
        else if (argument == "status") value.command = Command::status;
        else return invalid("unsupported command");
    }
    if (value.command == Command::none) return invalid("a command is required");
    if ((have_instance && value.instance.empty()) ||
        (have_runtime && value.runtime_directory.empty()) ||
        (have_device && value.device.empty())) return invalid("selector must not be empty");
#if !defined(_WIN32)
    if (value.group && !have_runtime) return invalid("--group requires --runtime-dir");
#endif
    value.valid = true;
    return value;
}

bool normalized_instance(std::string_view input, identity::InstanceText& output,
                         const Profile& profile) noexcept
{
    if (profile.normalize_instance != nullptr) return profile.normalize_instance(input, output);
    const auto result = identity::canonicalize_instance(input);
    if (!result) return false;
    output = result.value();
    return true;
}

bool matches_device(const wire::ListResponse& list,
                    const identity::ResolvedIdentity& expected,
                    const Profile& profile) noexcept
{
    if (!list.enclosure.present) return false;
    if (expected.kind == identity::IdentityKind::serial)
        return profile.matches_serial != nullptr && profile.matches_serial(list, expected);
    for (std::size_t index = 0U; index < expected.topology.count; ++index) {
        bool found = false;
        for (std::size_t candidate = 0U; candidate < list.functions.count; ++candidate) {
            const auto& path = list.functions.items[candidate].location.usb_path;
            const auto& expected_path = expected.topology.paths[index];
            if (path.present && path.value.length == expected_path.length &&
                std::memcmp(path.value.bytes.data(), expected_path.bytes.data(),
                            expected_path.length) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return list.functions.count == expected.topology.count;
}

int exit_status(Error error) noexcept
{
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY:
    case Error::RESOURCE_LIMIT: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH:
    case Error::PROTOCOL_ERROR: return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::SLOW_CONSUMER: return 8;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::INTERNAL: return 70;
    }
    return 70;
}

void usage(FILE* output, const Profile& profile) noexcept
{
    std::fprintf(output, "usage: %s --runtime-dir PATH [--instance TOKEN] "
        "[--device usb:BUS-PORT[,BUS-PORT...]] [--group] list|status\n"
        "  --runtime-dir is required; --instance is required unless --device is supplied\n"
        "       %s --help | --version\n", profile.tool_name, profile.tool_name);
}

bool print_list(const wire::ListResponse& list, const wire::StatusResponse& status) noexcept
{
    if (std::fprintf(stdout, "ready=%u usb-functions=%zu receivers=%zu readers=%zu\n",
            static_cast<unsigned int>(status.context.state),
            static_cast<std::size_t>(list.functions.count),
            list.receivers.present ? static_cast<std::size_t>(list.receivers.value.count) : 0U,
            list.readers.present ? static_cast<std::size_t>(list.readers.value.count) : 0U) < 0)
        return false;
    if (list.enclosure.present && list.enclosure.value.model.present &&
        std::fprintf(stdout, "model=%.*s\n",
            static_cast<int>(list.enclosure.value.model.value.name.length),
            list.enclosure.value.model.value.name.bytes.data()) < 0) return false;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        const auto& function = list.functions.items[index];
        if (std::fprintf(stdout, "function=%u usb=%04x:%04x present=%s\n",
                static_cast<unsigned int>(function.id),
                static_cast<unsigned int>(function.vid),
                static_cast<unsigned int>(function.pid),
                function.present ? "yes" : "no") < 0) return false;
    }
    return std::fflush(stdout) == 0;
}

}  // namespace

int run_control_cli(int argc, const char* const* argv, const Profile& profile) noexcept
{
    if (argc == 2 && argv != nullptr && argv[1] != nullptr &&
        std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", profile.product_slug, profile.product_version);
        return 0;
    }
    const Arguments arguments = parse(argc, argv);
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
    if (instance.empty() && !arguments.device.empty()) {
        const auto selector = identity::parse_device_selector(arguments.device);
        if (!selector || (selector.value().kind == identity::SelectorKind::serial &&
                          !profile.serial_selector_supported)) return 2;
        const auto resolved = identity::resolve_selector(profile.product, selector.value());
        if (!resolved) return exit_status(resolved.error());
        const auto automatic = identity::auto_instance(resolved.value());
        if (!automatic) return exit_status(automatic.error());
        instance.assign(automatic.value().bytes.data(), automatic.value().length);
    }
    identity::InstanceText normalized{};
    if (instance.empty() || arguments.runtime_directory.empty() ||
        !normalized_instance(instance, normalized, profile)) {
        std::fprintf(stderr, "%s: --runtime-dir and --instance are required and must be valid\n",
                     profile.tool_name);
        return 2;
    }
    instance.assign(normalized.bytes.data(), normalized.length);
    const host_runtime::EndpointConfig endpoint{
        arguments.runtime_directory, profile.product_slug, instance,
        arguments.group ? host_runtime::EndpointAccess::shared_group :
                          host_runtime::EndpointAccess::private_user};
    auto transport = host_runtime::connect_control(endpoint, Timeout{5000U});
    if (!transport) {
        std::fprintf(stderr, "%s: connect failed (%u)\n", profile.tool_name,
                     static_cast<unsigned int>(transport.error()));
        return exit_status(transport.error());
    }
    const auto snapshot = wire::fetch_status(*transport.value(), Timeout{5000U});
    if (!snapshot) {
        std::fprintf(stderr, "%s: status failed (%u, remote=%u)\n", profile.tool_name,
            static_cast<unsigned int>(snapshot.error),
            static_cast<unsigned int>(snapshot.remote_error_code));
        return exit_status(snapshot.error);
    }
    const auto& producer = snapshot.snapshot.status.producer.product;
    const std::size_t product_length = std::strlen(profile.product_slug);
    if (producer.length != product_length ||
        std::memcmp(producer.bytes.data(), profile.product_slug, product_length) != 0) {
        std::fprintf(stderr, "%s: endpoint belongs to another product\n", profile.tool_name);
        return 3;
    }
    if (!snapshot.snapshot.status.context.instance.present ||
        snapshot.snapshot.status.context.instance.value.length != instance.size() ||
        std::memcmp(snapshot.snapshot.status.context.instance.value.bytes.data(),
                    instance.data(), instance.size()) != 0) {
        std::fprintf(stderr, "%s: endpoint instance mismatch\n", profile.tool_name);
        return 3;
    }
    if (!arguments.device.empty()) {
        const auto selector = identity::parse_device_selector(arguments.device);
        const auto expected = selector ? identity::resolve_selector(profile.product, selector.value())
                                       : Result<identity::ResolvedIdentity>::failure(Error::INVALID_ARGUMENT);
        if (!expected || !matches_device(snapshot.snapshot.list, expected.value(), profile)) return 3;
    }
    if (arguments.command == Command::list)
        return print_list(snapshot.snapshot.list, snapshot.snapshot.status) ? 0 : 70;

    const wire::StatusContext context{&snapshot.snapshot.list};
    const auto size = wire::status_json_size(snapshot.snapshot.status, context);
    if (!size) return exit_status(size.error());
    std::vector<std::uint8_t> output(size.value());
    const auto encoded = wire::encode_status_json(snapshot.snapshot.status, context,
        MutableByteView{output.data(), output.size()});
    if (!encoded || std::fwrite(output.data(), 1U, encoded.value(), stdout) != encoded.value() ||
        std::fflush(stdout) != 0) return 70;
    return 0;
}

}  // namespace dtv::userland::host_ctl
