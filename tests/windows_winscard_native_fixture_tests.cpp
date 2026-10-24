// SPDX-License-Identifier: GPL-2.0-only
// Native Windows API fixture for the shared WinSCard DLL and a mocked Core peer.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <winscard.h>

#include <dtv/userland/control_connection.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <windows/windows_security.h>
#include <dtv/userland/wire.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace wire = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::MutableByteView;
using dtv::userland::Result;
namespace windows_security = px4::userland::windows_security;

int failures = 0;

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #value); \
    ++failures; } } while (false)

template <std::size_t N>
void set_text(wire::WString<N>& target, const char* value) noexcept
{
    const std::size_t length = std::strlen(value);
    if (length > target.bytes.size()) return;
    target.length = static_cast<std::uint16_t>(length);
    std::memcpy(target.bytes.data(), value, length);
}

bool create_secure_directory(const std::filesystem::path& path)
{
    return ::CreateDirectoryW(path.c_str(), nullptr) != 0 &&
           windows_security::set_current_user_owner_and_dacl(path.c_str());
}

bool send_all(SOCKET socket, const std::uint8_t* bytes, std::size_t length) noexcept
{
    while (length != 0U) {
        const int written = ::send(socket, reinterpret_cast<const char*>(bytes),
                                   static_cast<int>(length), 0);
        if (written <= 0) return false;
        bytes += written;
        length -= static_cast<std::size_t>(written);
    }
    return true;
}

bool receive_exact(SOCKET socket, std::uint8_t* bytes, std::size_t length) noexcept
{
    while (length != 0U) {
        const int received = ::recv(socket, reinterpret_cast<char*>(bytes),
                                    static_cast<int>(length), 0);
        if (received <= 0) return false;
        bytes += received;
        length -= static_cast<std::size_t>(received);
    }
    return true;
}

class MockControl final {
public:
    MockControl(const std::filesystem::path& directory, const char* product,
                const char* instance) : directory_(directory), product_name_(product)
    {
        for (std::size_t i = 0U; i < hello_.epoch.size(); ++i)
            hello_.epoch[i] = static_cast<std::uint8_t>(0x40U + i);
        set_text(hello_.producer.product, product);
        set_text(hello_.producer.version, "1.0.0-test");
        set_text(list_.producer.product, product);
        set_text(list_.producer.version, "1.0.0-test");
        list_.epoch = hello_.epoch;
        list_.snapshot = wire::Snapshot{1U, 1U};
        list_.topology.state = wire::TopologyState::complete;
        list_.topology.expected_function_count.present = true;
        list_.topology.expected_function_count.value = 1U;
        list_.enclosure.present = true;
        list_.enclosure.value.id = 0U;
        list_.enclosure.value.identity.serial_state = wire::IdentitySerialState::absent;
        list_.enclosure.value.identity.uniqueness = wire::Uniqueness::not_applicable;
        list_.functions.count = 1U;
        list_.functions.items[0U].id = 0U;
        list_.functions.items[0U].vid = 0x1234U;
        list_.functions.items[0U].pid = 0x5678U;
        list_.functions.items[0U].mode = wire::Mode::runtime;
        list_.functions.items[0U].serial_state = wire::SerialState::absent;
        list_.functions.items[0U].identification_reason =
            wire::IdentificationReason::model_unconfirmed;
        list_.functions.items[0U].present = true;
        list_.receivers.present = true;
        list_.readers.present = true;
        list_.readers.value.count = 1U;
        list_.readers.value.items[0U].id = 0U;
        list_.readers.value.items[0U].binding.present = true;
        list_.readers.value.items[0U].binding.value = wire::Binding{0U, 0U};
        context_.epoch = hello_.epoch;
        context_.state = wire::ContextState::ready;
        context_.instance.present = true;
        set_text(context_.instance.value, instance);
        server_socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_ == INVALID_SOCKET) return;
        const std::string endpoint = (directory_ / L"control.sock").u8string();
        SOCKADDR_UN address{};
        address.sun_family = AF_UNIX;
        if (endpoint.size() >= sizeof(address.sun_path)) return;
        std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1U);
        if (::bind(server_socket_, reinterpret_cast<const SOCKADDR*>(&address),
                   sizeof(address)) != 0 || ::listen(server_socket_, 8) != 0) return;
        valid_ = true;
        worker_ = std::thread([this]() noexcept { run(); });
    }

    ~MockControl() noexcept
    {
        stopping_.store(true);
        if (server_socket_ != INVALID_SOCKET) {
            (void)::closesocket(server_socket_);
            server_socket_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) worker_.join();
        (void)::DeleteFileW((directory_ / L"control.sock").c_str());
    }

    bool valid() const noexcept { return valid_; }

private:
    template <typename Record>
    bool encode_record(std::uint16_t opcode, std::uint32_t request_id,
                       const Record& record,
                       Result<std::size_t> (*encode)(const Record&, MutableByteView) noexcept,
                       MutableByteView output, std::size_t& reply_size)
    {
        const auto size = wire::encode_record_frame(wire::Kind::response, opcode,
            request_id, record, encode, output);
        if (!size) return false;
        reply_size = size.value();
        return true;
    }

    bool dispatch(const wire::ControlRequest& request, bool& connected,
                  MutableByteView output, std::size_t& reply_size)
    {
        if (request.kind == wire::ControlRequestKind::list)
            return encode_record(request.opcode, request.request_id, list_,
                                 &wire::encode_list_response, output, reply_size);
        if (request.kind == wire::ControlRequestKind::status) {
            wire::StatusResponse status{};
            if (wire::build_status_shell(list_, context_, list_.snapshot,
                                          request.status.scope, status) !=
                dtv::userland::Error::OK) return false;
            const auto size = wire::encode_status_frame(request.request_id, status, list_,
                output);
            if (!size) return false;
            reply_size = size.value();
            return true;
        }
        if (request.kind == wire::ControlRequestKind::card_status) {
            wire::CardStatusResponse response{};
            response.key = request.card_status.key;
            response.current_generation = 1U;
            response.present = true;
            response.initialized = connected;
            if (connected) {
                response.protocol.present = true;
                response.protocol.value = wire::CardProtocol::t1;
                response.atr.present = true;
                response.atr.value.length = 2U;
                response.atr.value.bytes[0U] = 0x3bU;
                response.atr.value.bytes[1U] = 0x00U;
            }
            return encode_record(request.opcode, request.request_id, response,
                                 &wire::encode_card_status_response, output, reply_size);
        }
        if (request.kind == wire::ControlRequestKind::card_connect) {
            connected = true;
            wire::CardSessionResult response{};
            response.epoch = request.card_connect.key.epoch;
            response.reader_id = request.card_connect.key.reader_id;
            response.card_handle = 0x1234U;
            response.bound_generation = 1U;
            response.current_generation = 1U;
            response.present = true;
            response.initialized = true;
            response.usable = true;
            response.protocol.present = true;
            response.protocol.value = wire::CardProtocol::t1;
            response.atr.present = true;
            response.atr.value.length = 2U;
            response.atr.value.bytes[0U] = 0x3bU;
            response.atr.value.bytes[1U] = 0x00U;
            return encode_record(request.opcode, request.request_id, response,
                                 &wire::encode_card_session_result, output, reply_size);
        }
        if (request.kind == wire::ControlRequestKind::card_transmit) {
            wire::CardTransmitResponse response{};
            response.key = request.card_transmit.key;
            response.response.length = 2U;
            response.response.bytes[0U] = 0x90U;
            response.response.bytes[1U] = 0x00U;
            return encode_record(request.opcode, request.request_id, response,
                                 &wire::encode_card_transmit_response, output, reply_size);
        }
        if (request.kind == wire::ControlRequestKind::card_disconnect) {
            wire::CardDisconnectResponse response{};
            response.key.epoch = request.card_disconnect.key.epoch;
            response.key.reader_id = request.card_disconnect.key.reader_id;
            response.card_handle = request.card_disconnect.key.card_handle;
            response.consumed = true;
            response.quiesced = true;
            return encode_record(request.opcode, request.request_id, response,
                                 &wire::encode_card_disconnect_response, output, reply_size);
        }
        return false;
    }

    void serve(SOCKET peer) noexcept
    {
        wire::ServerHello server = hello_;
        server.features = wire::kFeatureCardService;
        wire::ControlConnection connection(server);
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> input{};
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
        bool connected = false;
        while (!stopping_.load()) {
            if (!receive_exact(peer, input.data(), wire::kHeaderSize)) break;
            const auto header = wire::decode_header(ByteView{input.data(), wire::kHeaderSize});
            if (!header) break;
            const std::size_t payload_size = header.value().payload_length;
            if (payload_size > input.size() - wire::kHeaderSize ||
                !receive_exact(peer, input.data() + wire::kHeaderSize, payload_size)) break;
            const auto frame = wire::decode_frame(ByteView{input.data(),
                wire::kHeaderSize + payload_size});
            if (!frame) break;
            wire::ControlRequest request{};
            const auto inbound = connection.accept(frame.value(), request,
                MutableByteView{output.data(), output.size()});
            std::size_t reply_size = inbound.reply_size;
            if (inbound.action == wire::InboundAction::dispatch) {
                if (!dispatch(request, connected,
                        MutableByteView{output.data(), output.size()}, reply_size)) break;
            } else if (inbound.action != wire::InboundAction::respond) {
                break;
            }
            if (!send_all(peer, output.data(), reply_size)) break;
            connection.complete();
        }
        (void)::closesocket(peer);
    }

    void run() noexcept
    {
        while (!stopping_.load()) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(server_socket_, &readable);
            timeval timeout{0, 100000};
            if (::select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            const SOCKET peer = ::accept(server_socket_, nullptr, nullptr);
            if (peer == INVALID_SOCKET) continue;
            serve(peer);
        }
    }

    std::filesystem::path directory_;
    std::string product_name_;
    wire::ServerHello hello_{};
    wire::ListResponse list_{};
    wire::Context context_{};
    SOCKET server_socket_ = INVALID_SOCKET;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    bool valid_ = false;
};

struct RuntimeRoot final {
    std::filesystem::path path;
    std::filesystem::path runtime;
    bool create()
    {
        std::array<wchar_t, MAX_PATH> temp{};
        std::array<std::uint8_t, 4U> random{};
        std::array<wchar_t, 9U> token{};
        if (::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) == 0U ||
            !windows_security::random_hex(random.data(), random.size(), token.data(),
                                           token.size())) return false;
        path = std::filesystem::path(temp.data()) / (std::wstring(L"dws-") + token.data());
        if (!create_secure_directory(path)) return false;
        runtime = path / L"runtime";
        return create_secure_directory(runtime);
    }
    ~RuntimeRoot() noexcept
    {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

bool make_product_tree(const std::filesystem::path& root, const wchar_t* product,
                       const wchar_t* instance, std::filesystem::path& endpoint_dir)
{
    const auto product_dir = root / product;
    const auto version_dir = product_dir / L"v1";
    endpoint_dir = version_dir / instance;
    return create_secure_directory(product_dir) && create_secure_directory(version_dir) &&
           create_secure_directory(endpoint_dir);
}

template <typename Function>
Function load_function(HMODULE module, const char* name) noexcept
{
    const FARPROC raw = ::GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

bool exercise_reader(HMODULE module, SCARDCONTEXT context, const std::wstring& name)
{
    using Connect = decltype(&::SCardConnectW);
    using Status = decltype(&::SCardStatusW);
    using Transmit = decltype(&::SCardTransmit);
    using Disconnect = decltype(&::SCardDisconnect);
    const auto connect = load_function<Connect>(module, "SCardConnectW");
    const auto status = load_function<Status>(module, "SCardStatusW");
    const auto transmit = load_function<Transmit>(module, "SCardTransmit");
    const auto disconnect = load_function<Disconnect>(module, "SCardDisconnect");
    if (!connect || !status || !transmit || !disconnect) return false;
    SCARDHANDLE card = 0U;
    DWORD protocol = 0U;
    if (connect(context, name.c_str(), SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1,
                &card, &protocol) != SCARD_S_SUCCESS || protocol != SCARD_PROTOCOL_T1)
        return false;
    std::array<wchar_t, 128U> actual_name{};
    DWORD name_length = static_cast<DWORD>(actual_name.size());
    std::array<std::uint8_t, 33U> atr{};
    DWORD atr_length = static_cast<DWORD>(atr.size());
    DWORD state = 0U;
    DWORD active_protocol = 0U;
    if (status(card, actual_name.data(), &name_length, &state, &active_protocol,
               atr.data(), &atr_length) != SCARD_S_SUCCESS || atr_length != 2U ||
        atr[0U] != 0x3bU || atr[1U] != 0x00U || active_protocol != SCARD_PROTOCOL_T1)
        return false;
    const std::array<std::uint8_t, 5U> command{0x00U, 0x84U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 8U> response{};
    DWORD response_length = static_cast<DWORD>(response.size());
    SCARD_IO_REQUEST receive_pci{};
    const SCARD_IO_REQUEST send_pci{SCARD_PROTOCOL_T1, sizeof(SCARD_IO_REQUEST)};
    const LONG transmitted = transmit(card, &send_pci,
        command.data(), static_cast<DWORD>(command.size()), &receive_pci,
        response.data(), &response_length);
    const bool transmit_ok = transmitted == SCARD_S_SUCCESS && response_length == 2U &&
                             response[0U] == 0x90U && response[1U] == 0x00U;
    const LONG disconnected = disconnect(card, SCARD_LEAVE_CARD);
    return transmit_ok && disconnected == SCARD_S_SUCCESS;
}

int run(const wchar_t* dll_path)
{
    WSADATA winsock{};
    CHECK(::WSAStartup(MAKEWORD(2U, 2U), &winsock) == 0);
    RuntimeRoot root;
    CHECK(root.create());
    if (root.runtime.empty()) return failures + 1;
    CHECK(::SetEnvironmentVariableW(L"DTV_USERLAND_RUNTIME_DIR", root.runtime.c_str()) != 0);
    std::filesystem::path px4_dir;
    std::filesystem::path asicen_dir;
    CHECK(make_product_tree(root.runtime, L"px4-userland", L"fixture-p", px4_dir));
    CHECK(make_product_tree(root.runtime, L"asicen-userland", L"fixture-a", asicen_dir));
    MockControl px4(px4_dir, "px4-userland", "fixture-p");
    MockControl asicen(asicen_dir, "asicen-userland", "fixture-a");
    CHECK(px4.valid() && asicen.valid());
    HMODULE module = ::LoadLibraryW(dll_path);
    CHECK(module != nullptr);
    if (module == nullptr) return failures + 1;
    using Establish = decltype(&::SCardEstablishContext);
    using Release = decltype(&::SCardReleaseContext);
    using List = decltype(&::SCardListReadersW);
    const auto establish = load_function<Establish>(module, "SCardEstablishContext");
    const auto release = load_function<Release>(module, "SCardReleaseContext");
    const auto list = load_function<List>(module, "SCardListReadersW");
    CHECK(establish && release && list);
    if (establish && release && list) {
        SCARDCONTEXT context = 0U;
        CHECK(establish(SCARD_SCOPE_USER, nullptr, nullptr, &context) == SCARD_S_SUCCESS);
        DWORD characters = 0U;
        CHECK(list(context, nullptr, nullptr, &characters) == SCARD_S_SUCCESS);
        std::vector<wchar_t> readers(characters);
        CHECK(list(context, nullptr, readers.data(), &characters) == SCARD_S_SUCCESS);
        const std::wstring px4_name = L"DTV userland (px4:fixture-p/reader-0)";
        const std::wstring asicen_name = L"DTV userland (asicen:fixture-a/reader-0)";
        bool found_px4 = false;
        bool found_asicen = false;
        for (const wchar_t* reader = readers.data(); reader && *reader;
             reader += std::wcslen(reader) + 1U) {
            found_px4 = found_px4 || px4_name == reader;
            found_asicen = found_asicen || asicen_name == reader;
        }
        CHECK(found_px4 && found_asicen);
        CHECK(exercise_reader(module, context, px4_name));
        CHECK(exercise_reader(module, context, asicen_name));
        CHECK(release(context) == SCARD_S_SUCCESS);
    }
    (void)::FreeLibrary(module);
    (void)::SetEnvironmentVariableW(L"DTV_USERLAND_RUNTIME_DIR", nullptr);
    (void)::WSACleanup();
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1,
                                            nullptr, 0);
    if (size <= 0) return 2;
    std::vector<wchar_t> path(static_cast<std::size_t>(size));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1,
            path.data(), size) != size) return 2;
    return run(path.data());
}
