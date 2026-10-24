// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"

#include <px4/posix_ipc.h>
#include <dtv/userland/control_client.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/json.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dtv::userland::host_ctl {
namespace {

class SocketStreamTransport final : public wire::TimedClientTransport {
public:
    explicit SocketStreamTransport(px4::userland::ipc::posix::SocketStream& stream) noexcept
        : stream_(stream) {}
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
    px4::userland::ipc::posix::SocketStream& stream_;
};

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
    std::string instance;
    std::string runtime_directory;
    std::string device;
    std::string_view error;
    Command command = Command::none;
    std::array<std::uint8_t, wire::kMaxApduBytes> apdu{};
    std::size_t apdu_length = 0U;
};

Arguments invalid(std::string_view message) noexcept
{
    Arguments value{};
    value.error = message;
    return value;
}

int hex_nibble(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_apdu(std::string_view value, Arguments& result) noexcept
{
    if (value.empty()) return false;
    std::size_t offset = 0U;
    while (offset < value.size()) {
        if (result.apdu_length == result.apdu.size() || offset + 2U > value.size())
            return false;
        const int high = hex_nibble(value[offset]);
        const int low = hex_nibble(value[offset + 1U]);
        if (high < 0 || low < 0) return false;
        result.apdu[result.apdu_length++] =
            static_cast<std::uint8_t>((high << 4) | low);
        offset += 2U;
        if (offset == value.size()) break;
        if (value[offset++] != ':' || offset == value.size()) return false;
    }
    return result.apdu_length != 0U;
}

Command parse_command(std::string_view value) noexcept
{
    if (value == "list") return Command::list;
    if (value == "status") return Command::status;
    if (value == "card-status") return Command::card_status;
    if (value == "card-atr") return Command::card_atr;
    if (value == "card-reset") return Command::card_reset;
    if (value == "card-apdu") return Command::card_apdu;
    return Command::none;
}

bool is_card_command(Command command) noexcept
{
    return command == Command::card_status || command == Command::card_atr ||
           command == Command::card_reset || command == Command::card_apdu;
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
        value.command = parse_command(argument);
        if (value.command == Command::none) return invalid("unsupported command");
        if (value.command == Command::card_apdu) {
            if (index + 1 >= argc || argv[index + 1] == nullptr ||
                !parse_apdu(argv[++index], value))
                return invalid("card-apdu requires 1..4096 colon-separated bytes");
        }
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

int exit_status(Error error, bool card_operation = false) noexcept
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
    case Error::VERSION_MISMATCH: return 6;
    case Error::PROTOCOL_ERROR: return card_operation ? 9 : 6;
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
    std::fprintf(output, "usage: %s [--runtime-dir PATH] [--instance TOKEN] "
        "[--device usb:BUS-PORT[,BUS-PORT...]] [--group] COMMAND\n"
        "  --runtime-dir overrides the standard runtime root; --instance is required unless --device is supplied\n"
        "       %s --help | --version\ncommands: list, status",
        profile.tool_name, profile.tool_name);
    if (profile.card_supported || profile.internal_card_status_supported)
        std::fputs(", card-status", output);
    else
        std::fputs(", card-status (unsupported)", output);
    if (profile.card_supported)
        std::fputs(", card-atr, card-reset, card-apdu HEX", output);
    else
        std::fputs(", card-atr (unsupported), card-reset (unsupported), "
                   "card-apdu HEX (unsupported)", output);
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
    if ((arguments.command == Command::card_status && !profile.card_supported &&
         !profile.internal_card_status_supported) ||
        (arguments.command != Command::card_status && is_card_command(arguments.command) &&
         !profile.card_supported)) {
        std::fprintf(stderr, "%s: CARD commands are unsupported by this product\n",
                     profile.tool_name);
        return exit_status(Error::UNSUPPORTED, true);
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
    if (instance.empty() || !normalized_instance(instance, normalized, profile)) {
        std::fprintf(stderr, "%s: --instance is required and must be valid\n",
                     profile.tool_name);
        return 2;
    }
    instance.assign(normalized.bytes.data(), normalized.length);
    const auto access = arguments.group ?
        px4::userland::ipc::posix::EndpointAccess::shared_group :
        px4::userland::ipc::posix::EndpointAccess::private_user;
    const px4::userland::ipc::posix::EndpointConfig default_endpoint{
        nullptr, instance.c_str(), "control.sock", access, profile.product_slug};
    const host_runtime::EndpointConfig explicit_endpoint{
        arguments.runtime_directory, profile.product_slug, instance,
        arguments.group ? host_runtime::EndpointAccess::shared_group :
                          host_runtime::EndpointAccess::private_user};
    px4::userland::ipc::posix::SocketStream default_stream;
    SocketStreamTransport default_transport(default_stream);
    std::unique_ptr<wire::TimedClientTransport> owned_transport;
    wire::TimedClientTransport* transport = nullptr;
    const auto connect_control = [&]() {
        if (arguments.runtime_directory.empty()) {
            auto connected = px4::userland::ipc::posix::SocketStream::connect(
                default_endpoint, Timeout{5000U});
            if (!connected) return connected.error();
            default_stream = std::move(connected.value());
            transport = &default_transport;
            return Error::OK;
        }
        auto connected = host_runtime::connect_control(explicit_endpoint, Timeout{5000U});
        if (!connected) {
            return connected.error();
        }
        owned_transport = std::move(connected.value());
        transport = owned_transport.get();
        return Error::OK;
    };
    if (const auto error = connect_control(); error != Error::OK) {
        std::fprintf(stderr, "%s: connect failed (%u)\n", profile.tool_name,
                     static_cast<unsigned int>(error));
        return exit_status(error);
    }
    const std::uint64_t required_features =
        arguments.command == Command::card_status && profile.card_supported
            ? wire::kFeatureCardService : 0U;
    const auto snapshot = wire::fetch_status(*transport, Timeout{5000U}, required_features);
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
    if (arguments.command == Command::card_status) {
        if (profile.internal_card_status_supported && !profile.card_supported) {
            const wire::Optional<bool>& inserted = snapshot.snapshot.status.card_inserted;
            std::printf("present=%s\n", !inserted.present ? "unknown"
                                         : inserted.value ? "yes" : "no");
            return std::fflush(stdout) == 0 ? 0 : 70;
        }
        if (!snapshot.snapshot.list.readers.present ||
            snapshot.snapshot.list.readers.value.count == 0U) {
            std::fprintf(stderr, "%s: no card reader is available\n", profile.tool_name);
            return 3;
        }
        wire::CardClientSession session{};
        session.epoch = snapshot.snapshot.status.context.epoch;
        session.reader_id = snapshot.snapshot.list.readers.value.items[0U].id;
        session.next_request_id = 4U;
        const auto card = wire::card_status(
            *transport, session, Timeout{5000U});
        if (!card) {
            std::fprintf(stderr, "%s: card-status failed (%u, remote=%u)\n",
                         profile.tool_name, static_cast<unsigned int>(card.error),
                         static_cast<unsigned int>(card.remote_error_code));
            return exit_status(card.error, true);
        }
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
        if (!snapshot.snapshot.list.readers.present ||
            snapshot.snapshot.list.readers.value.count == 0U) {
            std::fprintf(stderr, "%s: no card reader is available\n", profile.tool_name);
            return 3;
        }
        const std::uint16_t reader_id =
            snapshot.snapshot.list.readers.value.items[0U].id;
        transport->close();
        owned_transport.reset();
        if (const auto error = connect_control(); error != Error::OK) {
            std::fprintf(stderr, "%s: connect failed (%u)\n", profile.tool_name,
                         static_cast<unsigned int>(error));
            return exit_status(error, true);
        }
        wire::CardClientOptions options{};
        options.reader_id = reader_id;
        options.expected_product = ByteView{
            reinterpret_cast<const std::uint8_t*>(profile.product_slug), product_length};
        options.expected_instance = ByteView{
            reinterpret_cast<const std::uint8_t*>(instance.data()), instance.size()};
        const auto opened = wire::begin_card(
            *transport, options, Timeout{5000U});
        if (!opened) {
            std::fprintf(stderr, "%s: card connect failed (%u, remote=%u)\n",
                         profile.tool_name, static_cast<unsigned int>(opened.error),
                         static_cast<unsigned int>(opened.remote_error_code));
            return exit_status(opened.error, true);
        }
        wire::CardClientSession session = opened.session;
        Error operation_error = Error::OK;
        if (session.epoch != snapshot.snapshot.status.context.epoch ||
            session.reader_id != reader_id) {
            const auto released = wire::card_disconnect(
                *transport, session, wire::Disposition::leave,
                Timeout{5000U});
            if (!released)
                std::fprintf(stderr, "%s: stale card handle release failed (%u)\n",
                             profile.tool_name, static_cast<unsigned int>(released.error));
            return exit_status(Error::NOT_FOUND, true);
        }
        if (arguments.command == Command::card_atr) {
            if (!opened.connection.atr.present)
                operation_error = Error::NOT_READY;
            else
                print_hex("atr", opened.connection.atr.value.bytes.data(),
                          opened.connection.atr.value.length);
        } else if (arguments.command == Command::card_reset) {
            const auto reset = wire::card_reset(
                *transport, session, Timeout{5000U});
            if (!reset) {
                operation_error = reset.error;
            } else if (!reset.response.atr.present) {
                operation_error = Error::NOT_READY;
            } else {
                print_hex("atr", reset.response.atr.value.bytes.data(),
                          reset.response.atr.value.length);
            }
        } else {
            const auto transmitted = wire::card_transmit(
                *transport, session, wire::CardProtocol::t1,
                ByteView{arguments.apdu.data(), arguments.apdu_length},
                Timeout{5000U});
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
                *transport, session, wire::Disposition::leave,
                Timeout{5000U});
            if (!disconnected && operation_error == Error::OK)
                operation_error = disconnected.error;
        }
        if (operation_error != Error::OK) {
            std::fprintf(stderr, "%s: card operation failed (%u)\n", profile.tool_name,
                         static_cast<unsigned int>(operation_error));
            return exit_status(operation_error, true);
        }
        return std::fflush(stdout) == 0 ? 0 : 70;
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
