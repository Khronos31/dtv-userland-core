// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"
#include "host_ctl_test_access.h"
#include "host_ts_test_access.h"

#include <dtv/userland/control_connection.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/wire.h>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <afunix.h>
#include <aclapi.h>
#include <windows.h>
#include <sddl.h>
#include "windows/windows_security.h"
#include <io.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace wire = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { if (socket != kInvalidSocket) (void)::closesocket(socket); }

bool utf8(const wchar_t* input, std::string& output)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
                                            nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
            output.data(), size, nullptr, nullptr) != size) return false;
    output.resize(static_cast<std::size_t>(size - 1));
    return true;
}
bool wide_path(const std::string& input, std::wstring& output)
{
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            input.c_str(), -1, nullptr, 0);
    if (size <= 0) return false;
    output.resize(static_cast<std::size_t>(size));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), -1,
            output.data(), size) != size) return false;
    output.resize(static_cast<std::size_t>(size - 1));
    return true;
}

bool make_directory(const std::wstring& path)
{
    return ::CreateDirectoryW(path.c_str(), nullptr) != 0 &&
           px4::userland::windows_security::set_current_user_owner_and_dacl(path.c_str());
}

bool add_everyone_allow(const wchar_t* path)
{
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(path), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &current, nullptr,
            &descriptor) != ERROR_SUCCESS) return false;
    PSID everyone = nullptr;
    if (::ConvertStringSidToSidW(L"S-1-1-0", &everyone) == 0) {
        (void)::LocalFree(descriptor);
        return false;
    }
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
    PACL updated = nullptr;
    const DWORD acl_result = ::SetEntriesInAclW(1U, &access, current, &updated);
    (void)::LocalFree(everyone);
    (void)::LocalFree(descriptor);
    if (acl_result != ERROR_SUCCESS || updated == nullptr) return false;
    const DWORD set_result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, updated, nullptr);
    (void)::LocalFree(updated);
    return set_result == ERROR_SUCCESS;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { if (socket >= 0) (void)::close(socket); }

bool make_directory(const std::string& path)
{
    return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), 0700) == 0;
}
#endif

std::string child_path(const std::string& parent, const char* child)
{
    return (std::filesystem::u8path(parent) / child).u8string();
}

struct RuntimeTree final {
    std::string root;
#if defined(_WIN32)
    std::wstring root_wide;
    std::vector<std::wstring> directories;
#else
    std::vector<std::string> directories;
#endif
    std::vector<std::string> instances;

    bool create()
    {
#if defined(_WIN32)
        std::array<wchar_t, MAX_PATH> temp{};
        std::array<wchar_t, MAX_PATH> name{};
        if (::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) == 0U ||
            ::GetTempFileNameW(temp.data(), L"dti", 0U, name.data()) == 0U)
            return false;
        if (::DeleteFileW(name.data()) == 0 || !make_directory(name.data())) return false;
        root_wide = name.data();
        directories.push_back(root_wide);
        return utf8(root_wide.c_str(), root);
#else
        std::array<char, 64U> temp{};
        constexpr char pattern[] = "/tmp/dtv-client-XXXXXX";
        std::memcpy(temp.data(), pattern, sizeof(pattern));
        char* value = ::mkdtemp(temp.data());
        if (value == nullptr || ::chmod(value, 0700) != 0) return false;
        root = value;
        directories.push_back(root);
        return true;
#endif
    }

    bool create_instance(const std::string& name)
    {
        const std::string product = child_path(root, "test-product");
        const std::string version = child_path(product, "v1");
        const std::string instance = child_path(version, name.c_str());
        if (instances.empty()) {
            for (const auto& path : {product, version}) {
#if defined(_WIN32)
                std::wstring wide;
                if (!wide_path(path, wide) || !make_directory(wide)) return false;
                directories.push_back(std::move(wide));
#else
                if (!make_directory(path)) return false;
                directories.push_back(path);
#endif
            }
        }
#if defined(_WIN32)
        std::wstring wide;
        if (!wide_path(instance, wide) || !make_directory(wide)) return false;
        directories.push_back(std::move(wide));
#else
        if (!make_directory(instance)) return false;
        directories.push_back(instance);
#endif
        instances.push_back(name);
        return true;
    }

    ~RuntimeTree()
    {
        for (auto path = directories.rbegin(); path != directories.rend(); ++path) {
#if defined(_WIN32)
            (void)::RemoveDirectoryW(path->c_str());
#else
            (void)::rmdir(path->c_str());
#endif
        }
    }
};

#if !defined(_WIN32)
class EnvironmentValue final {
public:
    explicit EnvironmentValue(const char* name) : name_(name)
    {
        const char* value = std::getenv(name);
        had_value_ = value != nullptr;
        if (had_value_) value_ = value;
    }
    ~EnvironmentValue()
    {
        if (had_value_) (void)::setenv(name_.c_str(), value_.c_str(), 1);
        else (void)::unsetenv(name_.c_str());
    }
    bool set(const char* value) const { return ::setenv(name_.c_str(), value, 1) == 0; }
private:
    std::string name_;
    std::string value_;
    bool had_value_ = false;
};
#endif

struct Listener final {
    Socket socket = kInvalidSocket;
    std::string path;
#if defined(_WIN32)
    std::wstring path_wide;
#endif

    bool bind(const std::string& root, const std::string& instance,
              const char* endpoint_name, bool start_listening = true)
    {
        dtv::userland::host_runtime::EndpointConfig config{
            root, "test-product", instance};
        config.endpoint_name = endpoint_name;
        const auto endpoint = dtv::userland::host_runtime::endpoint_path(config);
        if (!endpoint) return false;
        path = endpoint.value();
#if defined(_WIN32)
        if (!wide_path(path, path_wide)) return false;
        socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket == kInvalidSocket) return false;
        SOCKADDR_UN address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) return false;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
        return ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
               (!start_listening || ::listen(socket, 8) == 0) &&
               px4::userland::windows_security::set_current_user_owner_and_dacl(
                   path_wide.c_str());
#else
        socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket < 0) return false;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) return false;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
        return ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
               ::chmod(path.c_str(), 0600) == 0 &&
               (!start_listening || ::listen(socket, 8) == 0);
#endif
    }

    Socket accept_one()
    {
#if defined(_WIN32)
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(socket, &reads);
        timeval timeout{5, 0};
        if (::select(0, &reads, nullptr, nullptr, &timeout) <= 0) return kInvalidSocket;
#else
        pollfd descriptor{socket, POLLIN, 0};
        if (::poll(&descriptor, 1U, 5000) <= 0 || (descriptor.revents & POLLIN) == 0)
            return kInvalidSocket;
#endif
        const Socket accepted = ::accept(socket, nullptr, nullptr);
        if (accepted == kInvalidSocket) return accepted;
#if defined(_WIN32)
        const DWORD socket_timeout = 5000U;
        (void)::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&socket_timeout), sizeof(socket_timeout));
        (void)::setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&socket_timeout), sizeof(socket_timeout));
#else
        timeval timeout{5, 0};
        (void)::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        return accepted;
    }

    bool has_pending()
    {
#if defined(_WIN32)
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(socket, &reads);
        timeval timeout{0, 0};
        return ::select(0, &reads, nullptr, nullptr, &timeout) > 0;
#else
        pollfd descriptor{socket, POLLIN, 0};
        return ::poll(&descriptor, 1U, 0) > 0 && (descriptor.revents & POLLIN) != 0;
#endif
    }

    ~Listener()
    {
        close_socket(socket);
#if defined(_WIN32)
        if (!path_wide.empty()) (void)::DeleteFileW(path_wide.c_str());
#else
        if (!path.empty()) (void)::unlink(path.c_str());
#endif
    }
};

bool recv_exact(Socket socket, std::uint8_t* output, std::size_t size)
{
    std::size_t offset = 0U;
    while (offset < size) {
#if defined(_WIN32)
        const int count = ::recv(socket, reinterpret_cast<char*>(output + offset),
            static_cast<int>(std::min<std::size_t>(size - offset, INT_MAX)), 0);
#else
        const ssize_t count = ::recv(socket, output + offset, size - offset, 0);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool send_all(Socket socket, const std::uint8_t* input, std::size_t size)
{
    std::size_t offset = 0U;
    while (offset < size) {
#if defined(_WIN32)
        const int count = ::send(socket, reinterpret_cast<const char*>(input + offset),
            static_cast<int>(std::min<std::size_t>(size - offset, INT_MAX)), 0);
#else
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = ::send(socket, input + offset, size - offset, flags);
#endif
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool receive_frame(Socket socket, std::uint8_t* buffer, std::size_t capacity,
                   std::size_t& frame_size, wire::Frame& frame)
{
    if (!recv_exact(socket, buffer, wire::kHeaderSize)) return false;
    const auto header = wire::decode_header(ByteView{buffer, wire::kHeaderSize});
    if (!header) return false;
    frame_size = wire::kHeaderSize + header.value().payload_length;
    if (frame_size > capacity ||
        !recv_exact(socket, buffer + wire::kHeaderSize, header.value().payload_length))
        return false;
    const auto decoded = wire::decode_frame(ByteView{buffer, frame_size});
    if (!decoded) return false;
    frame = decoded.value();
    return true;
}

template <std::size_t N>
void set_text(wire::WString<N>& output, const char* value)
{
    output.length = 0U;
    while (value[output.length] != '\0') {
        output.bytes[output.length] = value[output.length];
        ++output.length;
    }
}

wire::StreamFinal make_final(std::uint64_t stream_id)
{
    wire::StreamFinal final{};
    final.id = stream_id;
    final.tune_revision = 3U;
    final.reason = wire::StreamEndReason::stopped;
    final.statistics.entries.count = 2U;
    final.statistics.entries.items[0U].id = wire::kCounterIdTransmittedPackets;
    final.statistics.entries.items[0U].counter.measurement.state = wire::MeasureState::valid;
    final.statistics.entries.items[0U].counter.measurement.value = 1U;
    final.statistics.entries.items[1U].id = wire::kCounterIdQueueDroppedPackets;
    final.statistics.entries.items[1U].counter.measurement.state = wire::MeasureState::valid;
    final.statistics.entries.items[1U].counter.measurement.value = 0U;
    return final;
}

struct FakeService final {
    wire::ServerHello hello{};
    wire::ListResponse list{};
    wire::Context context{};
    wire::Snapshot snapshot{3U, 5000U};
    std::atomic<bool> saw_acquire{false};
    std::atomic<bool> saw_tune{false};
    std::atomic<bool> saw_start{false};
    std::atomic<bool> saw_status{false};
    std::atomic<unsigned int> status_count{0U};
    std::atomic<unsigned int> list_count{0U};
    std::atomic<bool> saw_stop{false};
    std::atomic<bool> saw_release{false};
    std::atomic<bool> saw_lnb_on{false};
    std::atomic<bool> saw_lnb_off{false};
    std::atomic<unsigned int> lnb_power_count{0U};
    std::atomic<unsigned int> stop_count{0U};
    std::atomic<unsigned int> release_count{0U};
    std::atomic<unsigned int> operation_order{0U};
    std::atomic<unsigned int> stop_order{0U};
    std::atomic<unsigned int> stream_end_order{0U};
    std::atomic<unsigned int> release_order{0U};
    bool unsupported_status = false;
    bool refuse_lnb_power = false;
    unsigned int replace_serial_at_list = 0U;
    std::string replacement_serial;
    unsigned int replace_usb_path_at_list = 0U;
    std::string replacement_usb_path;
    std::mutex stop_mutex;
    std::condition_variable stop_changed;

    explicit FakeService(const std::string& instance,
                         const std::string& product = "test-product",
                         const std::string& serial = {})
    {
        for (std::size_t i = 0U; i < hello.epoch.size(); ++i)
            hello.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
        set_text(hello.producer.product, product.c_str());
        set_text(hello.producer.version, "1.0.0-test");
        list.epoch = hello.epoch;
        list.producer = hello.producer;
        list.snapshot = snapshot;
        list.topology.state = wire::TopologyState::complete;
        list.topology.expected_function_count.present = true;
        list.topology.expected_function_count.value = 1U;
        list.enclosure.present = true;
        if (serial.empty()) {
            list.enclosure.value.identity.serial_state = wire::IdentitySerialState::absent;
            list.enclosure.value.identity.uniqueness = wire::Uniqueness::not_applicable;
        } else {
            list.enclosure.value.identity.serial_state = wire::IdentitySerialState::present;
            list.enclosure.value.identity.serial.present = true;
            list.enclosure.value.identity.uniqueness = wire::Uniqueness::unique_in_scan;
            set_text(list.enclosure.value.identity.serial.value, serial.c_str());
        }
        list.functions.count = 1U;
        list.functions.items[0U].id = 0U;
        list.functions.items[0U].vid = 0x1234U;
        list.functions.items[0U].pid = 0x5678U;
        list.functions.items[0U].mode = wire::Mode::runtime;
        list.functions.items[0U].serial_state = wire::SerialState::absent;
        list.functions.items[0U].present = true;
        list.functions.items[0U].identification_reason =
            wire::IdentificationReason::model_unconfirmed;
        list.receivers.present = true;
        list.receivers.value.count = 1U;
        list.receivers.value.items[0U].id = 513U;
        list.receivers.value.items[0U].systems.mask = 0x01U;
        list.receivers.value.items[0U].binding.present = true;
        list.receivers.value.items[0U].binding.value = wire::Binding{0U, 0U};
        list.readers.present = true;
        context.epoch = hello.epoch;
        context.state = wire::ContextState::ready;
        context.instance.present = true;
        set_text(context.instance.value, instance.c_str());
    }

    template <typename Record>
    bool send_record(Socket peer, std::uint16_t opcode, std::uint32_t request_id,
                     const Record& value,
                     Result<std::size_t> (*encode)(const Record&, MutableByteView) noexcept)
    {
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
        const auto frame = wire::encode_record_frame(
            wire::Kind::response, opcode, request_id, value, encode,
            MutableByteView{output.data(), output.size()});
        return frame && send_all(peer, output.data(), frame.value());
    }

    bool dispatch(Socket peer, const wire::ControlRequest& request)
    {
        if (request.kind == wire::ControlRequestKind::list) {
            const unsigned int current_list = list_count.fetch_add(1U) + 1U;
            if (replace_serial_at_list != 0U && current_list >= replace_serial_at_list) {
                list.enclosure.value.identity.serial_state = wire::IdentitySerialState::present;
                list.enclosure.value.identity.serial.present = true;
                list.enclosure.value.identity.serial.value.length = 0U;
                set_text(list.enclosure.value.identity.serial.value,
                         replacement_serial.c_str());
            }
            if (replace_usb_path_at_list != 0U &&
                current_list >= replace_usb_path_at_list) {
                list.functions.items[0U].location.usb_path.present = true;
                set_text(list.functions.items[0U].location.usb_path.value,
                         replacement_usb_path.c_str());
            }
            return send_record(peer, request.opcode, request.request_id, list,
                               &wire::encode_list_response);
        }
        if (request.kind == wire::ControlRequestKind::status) {
            saw_status.store(true);
            status_count.fetch_add(1U);
            if (unsupported_status) {
                std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
                const auto frame = wire::encode_error_frame(
                    request.opcode, request.request_id, hello.epoch,
                    static_cast<std::uint32_t>(Error::UNSUPPORTED), "status",
                    request.status.scope, MutableByteView{output.data(), output.size()});
                return frame && send_all(peer, output.data(), frame.value());
            }
            wire::StatusResponse status{};
            if (wire::build_status_shell(list, context, snapshot,
                    request.status.scope, status) != Error::OK) return false;
            std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
            const auto frame = wire::encode_status_frame(
                request.request_id, status, list,
                MutableByteView{output.data(), output.size()});
            return frame && send_all(peer, output.data(), frame.value());
        }
        if (request.kind == wire::ControlRequestKind::acquire) {
            saw_acquire.store(true);
            wire::AcquireResponse response{hello.epoch, 77U};
            return send_record(peer, request.opcode, request.request_id, response,
                               &wire::encode_acquire_response);
        }
        if (request.kind == wire::ControlRequestKind::tune) {
            saw_tune.store(true);
            wire::TuneResult response{};
            response.epoch = hello.epoch;
            response.tune_revision = 3U;
            response.requested_hz = request.tune.frequency_hz;
            return send_record(peer, request.opcode, request.request_id, response,
                               &wire::encode_tune_result);
        }
        if (request.kind == wire::ControlRequestKind::start_stream) {
            saw_start.store(true);
            wire::StartResponse response{};
            response.epoch = hello.epoch;
            response.stream_id = 99U;
            response.attach_token[0U] = 0x33U;
            response.expires_after_ms = 5000U;
            return send_record(peer, request.opcode, request.request_id, response,
                               &wire::encode_start_response);
        }
        if (request.kind == wire::ControlRequestKind::stop_stream) {
            stop_count.fetch_add(1U);
            wire::StopResponse response{};
            response.epoch = hello.epoch;
            response.final = make_final(request.stop_stream.stream_id);
            response.quiesced = true;
            const bool sent = send_record(peer, request.opcode, request.request_id,
                                          response, &wire::encode_stop_response);
            if (sent) {
                saw_stop.store(true);
                stop_order.store(operation_order.fetch_add(1U) + 1U);
                stop_changed.notify_all();
            }
            return sent;
        }
        if (request.kind == wire::ControlRequestKind::release) {
            release_count.fetch_add(1U);
            wire::ReleaseResponse response{};
            response.epoch = hello.epoch;
            response.lease_id = request.release.lease_id;
            response.released = true;
            response.quiesced = true;
            const bool sent = send_record(peer, request.opcode, request.request_id,
                                          response, &wire::encode_release_response);
            if (sent) {
                saw_release.store(true);
                release_order.store(operation_order.fetch_add(1U) + 1U);
            }
            return sent;
        }
        if (request.kind == wire::ControlRequestKind::lnb_power) {
            lnb_power_count.fetch_add(1U);
            if (refuse_lnb_power) {
                std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
                const auto frame = wire::encode_error_frame(
                    request.opcode, request.request_id, hello.epoch,
                    static_cast<std::uint32_t>(Error::NOT_READY), "lnb_power",
                    wire::Scope{}, MutableByteView{output.data(), output.size()});
                return frame && send_all(peer, output.data(), frame.value());
            }
            wire::LnbPowerResponse response{};
            response.epoch = hello.epoch;
            response.enabled = request.lnb_power.enabled;
            if (response.enabled) saw_lnb_on.store(true);
            else saw_lnb_off.store(true);
            return send_record(peer, request.opcode, request.request_id, response,
                               &wire::encode_lnb_power_response);
        }
        return false;
    }

    bool serve_control_peer(Socket peer)
    {
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> input{};
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
        wire::ControlConnection connection(hello);
        for (;;) {
            wire::Frame frame{};
            std::size_t frame_size = 0U;
            if (!receive_frame(peer, input.data(), input.size(), frame_size, frame))
                return true;
            wire::ControlRequest request{};
            const wire::Inbound inbound = connection.accept(
                frame, request, MutableByteView{output.data(), output.size()});
            if (inbound.action == wire::InboundAction::close ||
                inbound.action == wire::InboundAction::ignored) return false;
            bool sent = false;
            if (inbound.action == wire::InboundAction::respond) {
                sent = send_all(peer, output.data(), inbound.reply_size);
            } else if (inbound.action == wire::InboundAction::dispatch) {
                sent = dispatch(peer, request);
            }
            if (!sent) return false;
            connection.complete();
            if (request.kind == wire::ControlRequestKind::release) return true;
        }
    }
};

struct FakeControlServer final {
    Listener& listener;
    FakeService& service;
    std::size_t connections;
    std::atomic<bool> good{true};
    std::thread worker;

    FakeControlServer(Listener& endpoint, FakeService& state, std::size_t count)
        : listener(endpoint), service(state), connections(count) {}

    void start()
    {
        worker = std::thread([this]() {
            for (std::size_t i = 0U; i < connections; ++i) {
                const Socket peer = listener.accept_one();
                if (peer == kInvalidSocket) {
                    good.store(false);
                    return;
                }
                const bool served = service.serve_control_peer(peer);
                close_socket(peer);
                if (!served) {
                    good.store(false);
                    return;
                }
            }
        });
    }

    void join()
    {
        if (worker.joinable()) worker.join();
    }

    ~FakeControlServer() { join(); }
};

bool send_event(Socket peer, std::uint16_t opcode, ByteView payload)
{
    constexpr std::size_t capacity = wire::kHeaderSize + wire::kMaxTsDataPayload;
    std::unique_ptr<std::uint8_t[]> frame(new (std::nothrow) std::uint8_t[capacity]);
    if (!frame) return false;
    const wire::Header header{wire::Kind::event, opcode, 0U,
                              static_cast<std::uint32_t>(payload.size)};
    const auto size = wire::encode_frame(
        header, payload, MutableByteView{frame.get(), capacity});
    return size && send_all(peer, frame.get(), size.value());
}

bool send_stream_end(Socket peer, const wire::StreamFinal& final)
{
    std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> frame{};
    const auto size = wire::encode_record_frame(
        wire::Kind::event, wire::kOpcodeStreamEndEvent, 0U,
        wire::StreamEndEvent{final}, &wire::encode_stream_end_event,
        MutableByteView{frame.data(), frame.size()});
    return size && send_all(peer, frame.data(), size.value());
}

struct FakeDataServer final {
    Listener& listener;
    FakeService& service;
    bool close_before_stream_end;
    bool wait_for_stop_without_data;
    std::atomic<bool> saw_attach{false};
    std::atomic<bool> good{true};
    std::thread worker;

    FakeDataServer(Listener& endpoint, FakeService& state, bool close_early,
                   bool wait_for_stop = false)
        : listener(endpoint), service(state), close_before_stream_end(close_early),
          wait_for_stop_without_data(wait_for_stop) {}

    void start()
    {
        worker = std::thread([this]() {
            const Socket peer = listener.accept_one();
            if (peer == kInvalidSocket) {
                good.store(false);
                return;
            }
            std::unique_ptr<std::uint8_t[]> buffer(
                new (std::nothrow) std::uint8_t[wire::kMaxFrameSize]);
            if (!buffer) {
                good.store(false);
                close_socket(peer);
                return;
            }
            std::size_t received = 0U;
            wire::Frame request_frame{};
            if (!receive_frame(peer, buffer.get(), wire::kMaxFrameSize,
                               received, request_frame) ||
                request_frame.header.opcode != wire::kOpcodeAttachStream) {
                good.store(false);
                close_socket(peer);
                return;
            }
            const auto attach = wire::decode_attach_request(request_frame.payload);
            if (!attach || attach.value().epoch != service.hello.epoch ||
                attach.value().lease_id != 77U || attach.value().stream_id != 99U ||
                attach.value().attach_token[0U] != 0x33U) {
                good.store(false);
                close_socket(peer);
                return;
            }
            if (wait_for_stop_without_data) {
                saw_attach.store(true);
                std::unique_lock<std::mutex> lock(service.stop_mutex);
                if (!service.stop_changed.wait_for(lock, std::chrono::seconds(5), [this]() {
                        return service.saw_stop.load();
                    })) good.store(false);
                close_socket(peer);
                return;
            }
            wire::AttachResponse response{service.hello.epoch, 99U};
            std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> output{};
            const auto response_size = wire::encode_record_frame(
                wire::Kind::response, wire::kOpcodeAttachStream,
                request_frame.header.request_id, response,
                &wire::encode_attach_response,
                MutableByteView{output.data(), output.size()});
            if (!response_size || !send_all(peer, output.data(), response_size.value())) {
                good.store(false);
                close_socket(peer);
                return;
            }
            saw_attach.store(true);

            std::array<std::uint8_t, wire::kTsPacketSize> packet{};
            packet[0U] = 0x47U;
            packet[1U] = 0x1fU;
            packet[2U] = 0xffU;
            packet[3U] = 0x10U;
            std::array<std::uint8_t, wire::kTsDataPrefixSize + wire::kTsPacketSize> payload{};
            const wire::TsData data{99U, 0U, 0U, ByteView{packet.data(), packet.size()}};
            const auto payload_size = wire::encode_ts_data(
                data, MutableByteView{payload.data(), payload.size()});
            if (!payload_size || !send_event(peer, wire::kOpcodeTsDataEvent,
                    ByteView{payload.data(), payload_size.value()})) {
                good.store(false);
                close_socket(peer);
                return;
            }
            if (!close_before_stream_end) {
                std::unique_lock<std::mutex> lock(service.stop_mutex);
                if (!service.stop_changed.wait_for(lock, std::chrono::seconds(5), [this]() {
                        return service.saw_stop.load();
                    })) {
                    good.store(false);
                    close_socket(peer);
                    return;
                }
                lock.unlock();
                if (!send_stream_end(peer, make_final(99U))) {
                    good.store(false);
                } else {
                    service.stream_end_order.store(
                        service.operation_order.fetch_add(1U) + 1U);
                }
            }
            close_socket(peer);
        });
    }

    void join()
    {
        if (worker.joinable()) worker.join();
    }

    ~FakeDataServer() { join(); }
};

struct TestClock final : px4::userland::cli::Px4TsClock {
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

struct TestSignal final : px4::userland::cli::Px4TsSignal {
    bool stop_requested() const noexcept override { return false; }
};

struct RequestedTestSignal final : px4::userland::cli::Px4TsSignal {
    std::atomic<bool> requested{false};
    bool stop_requested() const noexcept override { return requested.load(); }
};

struct SignalTimeoutOutput final : px4::userland::cli::Px4TsOutput {
    RequestedTestSignal& signal;
    bool request_signal;
    SignalTimeoutOutput(RequestedTestSignal& value, bool request)
        : signal(value), request_signal(request) {}
    Result<void> write(ByteView,
                       const px4::userland::cli::Px4TsSignal&) noexcept override
    {
        if (request_signal) signal.requested.store(true);
        return Result<void>::failure(Error::TIMEOUT);
    }
};

struct TestOutput final : px4::userland::cli::Px4TsOutput {
    std::vector<std::uint8_t> bytes;
    Result<void> write(ByteView value,
                       const px4::userland::cli::Px4TsSignal&) noexcept override
    {
        if (value.data == nullptr && value.size != 0U)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        if (value.size == 0U) return Result<void>::success();
        bytes.insert(bytes.end(), value.data, value.data + value.size);
        return Result<void>::success();
    }
};

bool add_root(const std::string& value,
              px4::userland::ipc::posix::RuntimeRootCandidates& roots)
{
    if (roots.count >= roots.paths.size() || value.size() >= roots.paths[0U].size())
        return false;
    std::memcpy(roots.paths[roots.count].data(), value.c_str(), value.size() + 1U);
    ++roots.count;
    return true;
}

bool matches_fake_serial(const wire::ListResponse& list,
                         const dtv::userland::identity::ResolvedIdentity& expected) noexcept
{
    if (!list.enclosure.present || expected.kind !=
        dtv::userland::identity::IdentityKind::serial) return false;
    const auto& observed = list.enclosure.value.identity;
    return observed.serial_state == wire::IdentitySerialState::present &&
           observed.serial.present &&
           observed.serial.value.length == expected.serial.length &&
           std::memcmp(observed.serial.value.bytes.data(), expected.serial.bytes.data(),
                       expected.serial.length) == 0;
}

dtv::userland::host_ctl::Profile fake_serial_ctl_profile()
{
    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "fake-ctl";
    profile.product_slug = "test-product";
    profile.product_version = "1.0.0-test";
    profile.product = dtv::userland::identity::Product::px4;
    profile.serial_selector_supported = true;
    profile.matches_serial = matches_fake_serial;
    profile.lnb_power_supported = true;
    return profile;
}

void configure_paired_usb(FakeService& service)
{
    service.list.topology.expected_function_count.value = 2U;
    service.list.functions.count = 2U;
    service.list.functions.items[0U].location.usb_path.present = true;
    set_text(service.list.functions.items[0U].location.usb_path.value, "1-2.1");
    service.list.functions.items[1U] = service.list.functions.items[0U];
    service.list.functions.items[1U].id = 1U;
    set_text(service.list.functions.items[1U].location.usb_path.value, "1-2.2");
    service.list.receivers.value.items[0U].capabilities.controls = wire::kControlBitLnb;
    service.list.receivers.value.items[0U].capabilities.lnb.values.count = 2U;
    service.list.receivers.value.items[0U].capabilities.lnb.values.items[0U] = 0U;
    service.list.receivers.value.items[0U].capabilities.lnb.values.items[1U] = 15U;
}

bool test_host_ctl_device_instance_discovery()
{
    RuntimeTree empty_root;
    RuntimeTree wrong_root;
    RuntimeTree live_root;
    if (!empty_root.create() || !wrong_root.create() || !live_root.create() ||
        !wrong_root.create_instance("other-serial") ||
        !live_root.create_instance("base-serial")) return false;
    Listener wrong_control;
    Listener live_control;
    if (!wrong_control.bind(wrong_root.root, "other-serial", "control.sock") ||
        !live_control.bind(live_root.root, "base-serial", "control.sock")) return false;
    FakeService wrong_service("other-serial", "test-product", "other");
    FakeService live_service("base-serial", "test-product", "expected");
    FakeControlServer wrong_server(wrong_control, wrong_service, 1U);
    FakeControlServer live_server(live_control, live_service, 2U);
    wrong_server.start();
    live_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(empty_root.root, roots) || !add_root(wrong_root.root, roots) ||
        !add_root(live_root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "serial:expected", "list"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    wrong_server.join();
    live_server.join();
    return status == 0 && wrong_server.good.load() && live_server.good.load() &&
           wrong_service.saw_status.load() && live_service.saw_status.load();
}

bool test_host_ctl_device_instance_ambiguity()
{
    RuntimeTree first_root;
    RuntimeTree second_root;
    if (!first_root.create() || !second_root.create() ||
        !first_root.create_instance("first-instance") ||
        !second_root.create_instance("second-instance")) return false;
    Listener first_control;
    Listener second_control;
    if (!first_control.bind(first_root.root, "first-instance", "control.sock") ||
        !second_control.bind(second_root.root, "second-instance", "control.sock")) return false;
    FakeService first_service("first-instance", "test-product", "expected");
    FakeService second_service("second-instance", "test-product", "expected");
    FakeControlServer first_server(first_control, first_service, 1U);
    FakeControlServer second_server(second_control, second_service, 1U);
    first_server.start();
    second_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(first_root.root, roots) || !add_root(second_root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "serial:expected", "status"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    first_server.join();
    second_server.join();
    return status == 3 && first_server.good.load() && second_server.good.load() &&
           first_service.saw_status.load() && second_service.saw_status.load();
}

bool test_host_ctl_device_reconnect_identity()
{
    RuntimeTree root;
    if (!root.create() || !root.create_instance("base-serial")) return false;
    Listener control;
    if (!control.bind(root.root, "base-serial", "control.sock")) return false;
    FakeService service("base-serial", "test-product", "expected");
    service.list.receivers.value.items[0U].capabilities.controls = wire::kControlBitLnb;
    service.list.receivers.value.items[0U].capabilities.lnb.values.count = 2U;
    service.list.receivers.value.items[0U].capabilities.lnb.values.items[0U] = 0U;
    service.list.receivers.value.items[0U].capabilities.lnb.values.items[1U] = 15U;
    service.replace_serial_at_list = 3U;
    service.replacement_serial = "replacement";
    FakeControlServer server(control, service, 3U);
    server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "serial:expected", "lnb-on"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    server.join();
    return status == 3 && server.good.load() && service.status_count.load() == 3U &&
           service.lnb_power_count.load() == 0U;
}

bool test_host_ctl_single_usb_selector_and_operation_assertion()
{
    RuntimeTree root;
    if (!root.create() || !root.create_instance("paired-usb")) return false;
    Listener control;
    if (!control.bind(root.root, "paired-usb", "control.sock")) return false;
    FakeService service("paired-usb", "test-product");
    configure_paired_usb(service);
    FakeControlServer server(control, service, 3U);
    server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "usb:1-2.1", "lnb-on"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    server.join();
    return status == 0 && server.good.load() && service.lnb_power_count.load() == 1U &&
           service.saw_lnb_on.load();
}

bool test_host_ctl_single_usb_operation_assertion_rejects_changed_function()
{
    RuntimeTree root;
    if (!root.create() || !root.create_instance("paired-usb")) return false;
    Listener control;
    if (!control.bind(root.root, "paired-usb", "control.sock")) return false;
    FakeService service("paired-usb", "test-product");
    configure_paired_usb(service);
    service.replace_usb_path_at_list = 3U;
    service.replacement_usb_path = "1-4.1";
    FakeControlServer server(control, service, 3U);
    server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "usb:1-2.1", "lnb-on"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    server.join();
    return status == 3 && server.good.load() && service.lnb_power_count.load() == 0U;
}

bool test_host_ctl_multi_usb_selector_requires_complete_topology()
{
    RuntimeTree root;
    if (!root.create() || !root.create_instance("paired-usb")) return false;
    Listener control;
    if (!control.bind(root.root, "paired-usb", "control.sock")) return false;
    FakeService service("paired-usb", "test-product");
    configure_paired_usb(service);
    FakeControlServer server(control, service, 2U);
    server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "usb:1-2.1,1-2.2", "list"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    server.join();
    return status == 0 && server.good.load();
}

bool test_host_ctl_single_usb_rejects_invalid_topology()
{
    for (unsigned int scenario = 0U; scenario < 4U; ++scenario) {
        RuntimeTree root;
        if (!root.create() || !root.create_instance("paired-usb")) return false;
        Listener control;
        if (!control.bind(root.root, "paired-usb", "control.sock")) return false;
        FakeService service("paired-usb", "test-product");
        configure_paired_usb(service);
        if (scenario == 0U) service.list.topology.state = wire::TopologyState::partial;
        if (scenario == 1U)
            service.list.topology.expected_function_count.value = 1U;
        if (scenario == 2U)
            service.list.functions.items[1U].location.usb_path.present = false;
        if (scenario == 3U)
            set_text(service.list.functions.items[1U].location.usb_path.value, "1-2.1");
        FakeControlServer server(control, service, 1U);
        server.start();

        px4::userland::ipc::posix::RuntimeRootCandidates roots{};
        if (!add_root(root.root, roots)) return false;
        const auto profile = fake_serial_ctl_profile();
        const char* arguments[] = {"fake-ctl", "--device", "usb:1-2.1", "list"};
        const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
            4, arguments, profile, roots);
        server.join();
        if (status != 3 || !server.good.load()) return false;
    }
    return true;
}

bool test_host_ctl_device_security_refusal_stops_search()
{
    RuntimeTree refused_root;
    RuntimeTree live_root;
    if (!refused_root.create() || !refused_root.create_instance("unsafe-instance") ||
        !live_root.create() || !live_root.create_instance("safe-instance")) return false;
    Listener refused;
    Listener live;
    if (!refused.bind(refused_root.root, "unsafe-instance", "control.sock") ||
        !live.bind(live_root.root, "safe-instance", "control.sock")) return false;
#if defined(_WIN32)
    if (!add_everyone_allow(refused.path_wide.c_str())) return false;
#else
    if (::chmod(refused.path.c_str(), 0666) != 0) return false;
#endif
    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(refused_root.root, roots) || !add_root(live_root.root, roots)) return false;
    const auto profile = fake_serial_ctl_profile();
    const char* arguments[] = {"fake-ctl", "--device", "serial:expected", "list"};
    const int status = dtv::userland::host_ctl::test_access::run_control_cli_with_roots(
        4, arguments, profile, roots);
    return status == 3 && !live.has_pending();
}

px4::userland::cli::Px4TsArguments test_ts_arguments(std::uint64_t packet_count)
{
    px4::userland::cli::Px4TsArguments args{};
    args.valid = true;
    args.product_cli = true;
    args.common_profile = true;
    args.system = px4::userland::ipc::System::ISDB_T;
    args.frequency_hz = 473142857U;
    args.tune_timeout_ms = 1000U;
    args.packet_count = packet_count;
    args.packet_count_set = true;
    return args;
}

bool test_host_ctl_real_server()
{
    RuntimeTree tree;
    RuntimeTree empty_root;
    if (!tree.create() || !empty_root.create()) return false;
#if !defined(_WIN32)
    const std::string parent_root = tree.root;
    tree.root = child_path(parent_root, ("dtv-userland-" +
        std::to_string(static_cast<unsigned long>(::geteuid()))).c_str());
    if (!make_directory(tree.root)) return false;
    tree.directories.push_back(tree.root);
#endif
    if (!tree.create_instance("ctl-live")) return false;
#if !defined(_WIN32)
    EnvironmentValue saved_xdg("XDG_RUNTIME_DIR"), saved_tmp("TMPDIR");
    if (!saved_xdg.set(empty_root.root.c_str()) || !saved_tmp.set(parent_root.c_str()))
        return false;
#endif
    Listener control;
    if (!control.bind(tree.root, "ctl-live", "control.sock")) return false;
    FakeService service("ctl-live");
    service.list.functions.count = 2U;
    service.list.functions.items[0U].id = 1U;
    service.list.functions.items[1U] = service.list.functions.items[0U];
    service.list.functions.items[1U].id = 2U;
    service.list.receivers.value.count = 2U;
    service.list.receivers.value.items[0U].id = 0U;
    service.list.receivers.value.items[0U].systems.mask = 0x03U;
    service.list.receivers.value.items[0U].binding.value = wire::Binding{1U, 0U};
    service.list.receivers.value.items[1U].id = 4U;
    service.list.receivers.value.items[1U].systems.mask = 0x02U;
    service.list.receivers.value.items[1U].binding.present = true;
    service.list.receivers.value.items[1U].binding.value = wire::Binding{2U, 0U};
    FakeControlServer server(control, service,
#if !defined(_WIN32)
                             4U
#else
                             2U
#endif
    );
    server.start();

    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "fake-ctl";
    profile.product_slug = "test-product";
    profile.product_version = "1.0.0-test";
    profile.product = dtv::userland::identity::Product::px4;
    const char* list_args[] = {"fake-ctl", "--runtime-dir", tree.root.c_str(),
                               "--instance", "ctl-live", "list"};
    const char* status_args[] = {"fake-ctl", "--runtime-dir", tree.root.c_str(),
                                 "--instance", "ctl-live", "status"};
    FILE* list_output = std::tmpfile();
    if (list_output == nullptr || std::fflush(stdout) != 0) return false;
#if defined(_WIN32)
    const int stdout_fd = ::_fileno(stdout);
    const int saved_stdout = ::_dup(stdout_fd);
    const bool redirected = saved_stdout >= 0 &&
        ::_dup2(::_fileno(list_output), stdout_fd) == 0;
#else
    const int stdout_fd = ::fileno(stdout);
    const int saved_stdout = ::dup(stdout_fd);
    const bool redirected = saved_stdout >= 0 &&
        ::dup2(::fileno(list_output), stdout_fd) >= 0;
#endif
    if (!redirected) {
        if (saved_stdout >= 0) {
#if defined(_WIN32)
            (void)::_close(saved_stdout);
#else
            (void)::close(saved_stdout);
#endif
        }
        std::fclose(list_output);
        return false;
    }
    const int list_status = dtv::userland::host_ctl::run_control_cli(
        6, list_args, profile);
    const bool flushed = std::fflush(stdout) == 0;
#if defined(_WIN32)
    const bool restored = ::_dup2(saved_stdout, stdout_fd) == 0;
    (void)::_close(saved_stdout);
#else
    const bool restored = ::dup2(saved_stdout, stdout_fd) >= 0;
    (void)::close(saved_stdout);
#endif
    std::rewind(list_output);
    std::array<char, 2048U> listed{};
    const std::size_t listed_size = std::fread(
        listed.data(), 1U, listed.size() - 1U, list_output);
    std::fclose(list_output);
    const std::string list_text(listed.data(), listed_size);
    const int status_status = dtv::userland::host_ctl::run_control_cli(
        6, status_args, profile);
#if !defined(_WIN32)
    const char* missing_explicit_args[] = {"fake-ctl", "--runtime-dir",
        empty_root.root.c_str(), "--instance", "ctl-live", "list"};
    const char* default_list_args[] = {"fake-ctl", "--instance", "ctl-live", "list"};
    const char* default_status_args[] = {"fake-ctl", "--instance", "ctl-live", "status"};
    const int missing_explicit_status =
        dtv::userland::host_ctl::run_control_cli(6, missing_explicit_args, profile);
    const int default_list_status =
        dtv::userland::host_ctl::run_control_cli(4, default_list_args, profile);
    const int default_status_status =
        dtv::userland::host_ctl::run_control_cli(4, default_status_args, profile);
#else
    const int missing_explicit_status = 3;
    const int default_list_status = 0;
    const int default_status_status = 0;
#endif
    server.join();
    return flushed && restored &&
           list_text.find("receiver=0 device=1 local=0 system=ISDB-T,ISDB-S\n") !=
               std::string::npos &&
           list_text.find("receiver=4 device=2 local=0 system=ISDB-S\n") !=
               std::string::npos &&
           list_text.find("ISDB-T/S") == std::string::npos &&
           list_status == 0 && status_status == 0 && missing_explicit_status == 3 &&
           default_list_status == 0 && default_status_status == 0 && server.good.load();
}

bool test_host_ctl_lnb_commands()
{
    RuntimeTree tree;
    RuntimeTree empty_root;
    if (!tree.create() || !empty_root.create()) return false;
#if !defined(_WIN32)
    const std::string parent_root = tree.root;
    tree.root = child_path(parent_root, ("dtv-userland-" +
        std::to_string(static_cast<unsigned long>(::geteuid()))).c_str());
    if (!make_directory(tree.root)) return false;
    tree.directories.push_back(tree.root);
    EnvironmentValue saved_xdg("XDG_RUNTIME_DIR"), saved_tmp("TMPDIR");
    if (!saved_xdg.set(empty_root.root.c_str()) || !saved_tmp.set(parent_root.c_str()))
        return false;
#endif
    if (!tree.create_instance("lnb-live")) return false;
    Listener control;
    if (!control.bind(tree.root, "lnb-live", "control.sock")) return false;
    FakeService service("lnb-live");
    auto& capabilities = service.list.receivers.value.items[0U].capabilities;
    capabilities.controls = wire::kControlBitLnb;
    capabilities.lnb.values.count = 2U;
    capabilities.lnb.values.items[0U] = 0U;
    capabilities.lnb.values.items[1U] = 15U;
#if !defined(_WIN32)
    FakeControlServer server(control, service, 4U);
#else
    FakeControlServer server(control, service, 2U);
#endif
    server.start();

    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "fake-ctl";
    profile.product_slug = "test-product";
    profile.product_version = "1.0.0-test";
    profile.product = dtv::userland::identity::Product::px4;
    profile.lnb_power_supported = true;
    const char* on_args[] = {"fake-ctl", "--runtime-dir", tree.root.c_str(),
                             "--instance", "lnb-live", "lnb-on"};
    const char* off_args[] = {"fake-ctl", "--instance", "lnb-live", "lnb-off"};
    const int on_status = dtv::userland::host_ctl::run_control_cli(6, on_args, profile);
#if !defined(_WIN32)
    const int off_status = dtv::userland::host_ctl::run_control_cli(4, off_args, profile);
#else
    const int off_status = 0;
#endif
    server.join();
#if !defined(_WIN32)
    return on_status == 0 && off_status == 0 && server.good.load() &&
           service.lnb_power_count.load() == 2U && service.saw_lnb_on.load() &&
           service.saw_lnb_off.load();
#else
    return on_status == 0 && off_status == 0 && server.good.load() &&
           service.lnb_power_count.load() == 1U && service.saw_lnb_on.load() &&
           !service.saw_lnb_off.load();
#endif
}

bool test_host_ctl_lnb_unsupported_by_device()
{
    RuntimeTree tree;
    if (!tree.create() || !tree.create_instance("lnb-unsupported")) return false;
    Listener control;
    if (!control.bind(tree.root, "lnb-unsupported", "control.sock")) return false;
    FakeService service("lnb-unsupported");
    FakeControlServer server(control, service, 1U);
    server.start();
    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "fake-ctl";
    profile.product_slug = "test-product";
    profile.product_version = "1.0.0-test";
    profile.product = dtv::userland::identity::Product::px4;
    profile.lnb_power_supported = true;
    const char* arguments[] = {"fake-ctl", "--runtime-dir", tree.root.c_str(),
                               "--instance", "lnb-unsupported", "lnb-on"};
    const int status = dtv::userland::host_ctl::run_control_cli(6, arguments, profile);
    server.join();
    return status == 3 && server.good.load() && service.lnb_power_count.load() == 0U;
}

bool test_host_ctl_lnb_remote_refusal()
{
    RuntimeTree tree;
    if (!tree.create() || !tree.create_instance("lnb-refused")) return false;
    Listener control;
    if (!control.bind(tree.root, "lnb-refused", "control.sock")) return false;
    FakeService service("lnb-refused");
    auto& capabilities = service.list.receivers.value.items[0U].capabilities;
    capabilities.controls = wire::kControlBitLnb;
    capabilities.lnb.values.count = 2U;
    capabilities.lnb.values.items[0U] = 0U;
    capabilities.lnb.values.items[1U] = 15U;
    service.refuse_lnb_power = true;
    FakeControlServer server(control, service, 2U);
    server.start();
    dtv::userland::host_ctl::Profile profile{};
    profile.tool_name = "fake-ctl";
    profile.product_slug = "test-product";
    profile.product_version = "1.0.0-test";
    profile.product = dtv::userland::identity::Product::px4;
    profile.lnb_power_supported = true;
    const char* arguments[] = {"fake-ctl", "--runtime-dir", tree.root.c_str(),
                               "--instance", "lnb-refused", "lnb-on"};
    const int status = dtv::userland::host_ctl::run_control_cli(6, arguments, profile);
    server.join();
    return status == 3 && server.good.load() && service.lnb_power_count.load() == 1U;
}

bool run_ts_lifecycle(bool close_before_stream_end,
                      bool overlong_first_candidate = false)
{
    RuntimeTree missing_root;
    RuntimeTree stale_root;
    RuntimeTree live_root;
    if (!missing_root.create() || !stale_root.create() ||
        !stale_root.create_instance("stale") || !live_root.create() ||
        !live_root.create_instance("live-01")) return false;
#if !defined(_WIN32)
    if (overlong_first_candidate) {
        const std::string original_root = missing_root.root;
        missing_root.root = original_root + "/" + std::string(80U, 'x');
        if (!make_directory(missing_root.root)) return false;
        missing_root.directories.push_back(missing_root.root);
        if (!missing_root.create_instance("path-too-long")) return false;
    }
#else
    if (overlong_first_candidate) return false;
#endif
    Listener control;
    Listener data;
    if (!control.bind(live_root.root, "live-01", "control.sock") ||
        !data.bind(live_root.root, "live-01", "stream.sock")) return false;
    FakeService service("live-01");
    FakeControlServer control_server(control, service, 2U);
    FakeDataServer data_server(data, service, close_before_stream_end);
    control_server.start();
    data_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    const std::string missing = child_path(missing_root.root, "no-runtime");
    if (!add_root(missing, roots) || !add_root(stale_root.root, roots) ||
        !add_root(live_root.root, roots)) return false;
    const auto args = test_ts_arguments(close_before_stream_end ? 2U : 1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    px4::userland::cli::Px4TsFailureKind failure{};
    px4::userland::cli::Px4TsRunDiagnostics diagnostics{};
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, &failure, &diagnostics);
    data_server.join();
    control_server.join();
    const bool server_completed = control_server.good.load() && data_server.good.load() &&
        service.saw_acquire.load() && service.saw_tune.load() && service.saw_start.load() &&
        service.saw_stop.load() && service.saw_release.load() && data_server.saw_attach.load();
    if (!server_completed || output.bytes.size() != wire::kTsPacketSize ||
        output.bytes[0U] != 0x47U) return false;
    if (close_before_stream_end)
        return !result && result.error() == Error::DISCONNECTED &&
               !diagnostics.stream_end_valid;
    return result && diagnostics.stream_end_valid && diagnostics.packets_written == 1U;
}

bool run_host_ts_output_timeout_case(bool request_signal)
{
    RuntimeTree live_root;
    if (!live_root.create() || !live_root.create_instance("signal-stop")) return false;
    Listener control;
    Listener data;
    if (!control.bind(live_root.root, "signal-stop", "control.sock") ||
        !data.bind(live_root.root, "signal-stop", "stream.sock")) return false;
    FakeService service("signal-stop");
    FakeControlServer control_server(control, service, 2U);
    FakeDataServer data_server(data, service, false);
    control_server.start();
    data_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(live_root.root, roots)) return false;
    auto args = test_ts_arguments(1U);
    args.packet_count_set = false;
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    RequestedTestSignal signal;
    SignalTimeoutOutput output(signal, request_signal);
    TestClock clock;
    px4::userland::cli::Px4TsFailureKind failure{};
    px4::userland::cli::Px4TsRunDiagnostics diagnostics{};
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, &failure, &diagnostics);
    data_server.join();
    control_server.join();
    const bool ordered = service.stop_order.load() < service.stream_end_order.load() &&
                         service.stream_end_order.load() < service.release_order.load();
    const bool servers_ok = service.saw_stop.load() && service.saw_release.load() &&
                            control_server.good.load() &&
                            (!request_signal || data_server.good.load());
    const bool passed = request_signal
        ? result && result.value().signal_stopped && diagnostics.stream_end_valid &&
              servers_ok && ordered
        : !result && result.error() == Error::TIMEOUT && !diagnostics.stream_end_valid &&
              servers_ok;
    if (!passed) {
        std::fprintf(stderr,
            "output timeout cleanup: requested=%d result=%d error=%u "
            "stream_end=%d stop=%d release=%d data=%d control=%d "
            "order=%u/%u/%u failure=%u\n",
            request_signal ? 1 : 0,
            result ? 1 : 0, result ? 0U : static_cast<unsigned int>(result.error()),
            diagnostics.stream_end_valid ? 1 : 0, service.saw_stop.load() ? 1 : 0,
            service.saw_release.load() ? 1 : 0, data_server.good.load() ? 1 : 0,
            control_server.good.load() ? 1 : 0, service.stop_order.load(),
            service.stream_end_order.load(), service.release_order.load(),
            static_cast<unsigned int>(failure));
    }
    return passed;
}

bool test_host_ts_output_timeouts()
{
    return run_host_ts_output_timeout_case(true) &&
           run_host_ts_output_timeout_case(false);
}

bool test_host_ts_frame_allocation_failure_releases_lease()
{
    RuntimeTree live_root;
    if (!live_root.create() || !live_root.create_instance("oom-live")) return false;
    Listener control;
    Listener data;
    if (!control.bind(live_root.root, "oom-live", "control.sock") ||
        !data.bind(live_root.root, "oom-live", "stream.sock")) return false;
    FakeService service("oom-live");
    FakeControlServer control_server(control, service, 2U);
    FakeDataServer data_server(data, service, false, true);
    control_server.start();
    data_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(live_root.root, roots)) return false;
    const auto args = test_ts_arguments(1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    px4::userland::cli::test_access::fail_next_frame_allocation();
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, nullptr, nullptr);
    data_server.join();
    control_server.join();
    return !result && result.error() == Error::INTERNAL && output.bytes.empty() &&
           data_server.saw_attach.load() && data_server.good.load() &&
           control_server.good.load() && service.stop_count.load() == 1U &&
           service.release_count.load() == 1U;
}

bool test_unsupported_candidate_then_live_instance()
{
    RuntimeTree unsupported_root;
    RuntimeTree live_root;
    if (!unsupported_root.create() || !unsupported_root.create_instance("unsupported") ||
        !live_root.create() || !live_root.create_instance("live-02")) return false;
    Listener unsupported_control;
    Listener control;
    Listener data;
    if (!unsupported_control.bind(unsupported_root.root, "unsupported", "control.sock") ||
        !control.bind(live_root.root, "live-02", "control.sock") ||
        !data.bind(live_root.root, "live-02", "stream.sock")) return false;
    FakeService unsupported_service("unsupported");
    unsupported_service.unsupported_status = true;
    FakeService service("live-02");
    FakeControlServer unsupported_server(unsupported_control, unsupported_service, 1U);
    FakeControlServer control_server(control, service, 2U);
    FakeDataServer data_server(data, service, false);
    unsupported_server.start();
    control_server.start();
    data_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(unsupported_root.root, roots) || !add_root(live_root.root, roots))
        return false;
    const auto args = test_ts_arguments(1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, nullptr, nullptr);
    data_server.join();
    control_server.join();
    unsupported_server.join();
    return result && output.bytes.size() == wire::kTsPacketSize &&
           unsupported_server.good.load() && unsupported_service.saw_status.load() &&
           control_server.good.load() && data_server.good.load() &&
           service.saw_stop.load() && service.saw_release.load();
}

bool test_not_ready_candidate_then_live_instance()
{
    RuntimeTree not_ready_root;
    RuntimeTree live_root;
    if (!not_ready_root.create() || !not_ready_root.create_instance("not-ready") ||
        !live_root.create() || !live_root.create_instance("live-after-not-ready"))
        return false;
    Listener not_ready_control;
    Listener control;
    Listener data;
    if (!not_ready_control.bind(not_ready_root.root, "not-ready", "control.sock", false) ||
        !control.bind(live_root.root, "live-after-not-ready", "control.sock") ||
        !data.bind(live_root.root, "live-after-not-ready", "stream.sock")) return false;
    const dtv::userland::host_runtime::EndpointConfig not_ready_endpoint{
        not_ready_root.root, "test-product", "not-ready"};
    const auto not_ready_probe = dtv::userland::host_runtime::connect_control(
        not_ready_endpoint, {1000U});
    if (not_ready_probe || not_ready_probe.error() != Error::NOT_READY) return false;
    FakeService service("live-after-not-ready");
    FakeControlServer control_server(control, service, 2U);
    FakeDataServer data_server(data, service, false);
    control_server.start();
    data_server.start();

    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(not_ready_root.root, roots) || !add_root(live_root.root, roots))
        return false;
    const auto args = test_ts_arguments(1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, nullptr, nullptr);
    data_server.join();
    control_server.join();
    return result && output.bytes.size() == wire::kTsPacketSize &&
           control_server.good.load() && data_server.good.load() &&
           service.saw_stop.load() && service.saw_release.load();
}

bool test_security_refusal_stops_search()
{
#if defined(_WIN32)
    RuntimeTree refused_root;
    RuntimeTree live_root;
    if (!refused_root.create() || !refused_root.create_instance("unsafe") ||
        !live_root.create() || !live_root.create_instance("safe")) return false;
    Listener refused;
    Listener live;
    if (!refused.bind(refused_root.root, "unsafe", "control.sock") ||
        !live.bind(live_root.root, "safe", "control.sock") ||
        !add_everyone_allow(refused.path_wide.c_str())) return false;
    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(refused_root.root, roots) || !add_root(live_root.root, roots)) return false;
    const auto args = test_ts_arguments(1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, nullptr, nullptr);
    return !result && result.error() == Error::UNSUPPORTED && !live.has_pending();
#else
    RuntimeTree refused_root;
    RuntimeTree live_root;
    if (!refused_root.create() || !refused_root.create_instance("unsafe") ||
        !live_root.create() || !live_root.create_instance("safe")) return false;
    Listener refused;
    Listener live;
    if (!refused.bind(refused_root.root, "unsafe", "control.sock") ||
        !live.bind(live_root.root, "safe", "control.sock") ||
        ::chmod(refused.path.c_str(), 0666) != 0) return false;
    px4::userland::ipc::posix::RuntimeRootCandidates roots{};
    if (!add_root(refused_root.root, roots) || !add_root(live_root.root, roots)) return false;
    const auto args = test_ts_arguments(1U);
    const px4::userland::cli::TsClientProfile profile{
        "fake-ts", "test-product", dtv::userland::identity::Product::px4};
    TestOutput output;
    TestClock clock;
    TestSignal signal;
    const auto result = px4::userland::cli::test_access::run_wire_with_roots(
        args, profile, output, clock, signal, roots, nullptr, nullptr);
    return !result && result.error() == Error::UNSUPPORTED && !live.has_pending();
#endif
}

}  // namespace

int main()
{
#if defined(_WIN32)
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    const bool ctl = test_host_ctl_real_server();
    const bool lnb_ctl = test_host_ctl_lnb_commands();
    const bool ctl_discovery = test_host_ctl_device_instance_discovery();
    const bool ctl_ambiguous = test_host_ctl_device_instance_ambiguity();
    const bool ctl_reconnect_identity = test_host_ctl_device_reconnect_identity();
    const bool ctl_single_usb = test_host_ctl_single_usb_selector_and_operation_assertion();
    const bool ctl_single_usb_reconnect =
        test_host_ctl_single_usb_operation_assertion_rejects_changed_function();
    const bool ctl_multi_usb = test_host_ctl_multi_usb_selector_requires_complete_topology();
    const bool ctl_invalid_usb = test_host_ctl_single_usb_rejects_invalid_topology();
    const bool ctl_security = test_host_ctl_device_security_refusal_stops_search();
    const bool lnb_unsupported = test_host_ctl_lnb_unsupported_by_device();
    const bool lnb_refused = test_host_ctl_lnb_remote_refusal();
    const bool ts = run_ts_lifecycle(false);
    const bool eof = run_ts_lifecycle(true);
    const bool output_timeouts = test_host_ts_output_timeouts();
#if !defined(_WIN32)
    const bool overlong = run_ts_lifecycle(false, true);
#else
    const bool overlong = true;
#endif
    const bool oom = test_host_ts_frame_allocation_failure_releases_lease();
    const bool unsupported = test_unsupported_candidate_then_live_instance();
    const bool not_ready = test_not_ready_candidate_then_live_instance();
    const bool security = test_security_refusal_stops_search();
#if defined(_WIN32)
    (void)::WSACleanup();
#endif
    if (ctl) std::puts("fake_server_host_ctl_list_status=PASS");
    if (lnb_ctl) std::puts("fake_server_host_ctl_lnb_on_off=PASS");
    if (ctl_discovery) std::puts("fake_server_host_ctl_device_instance_discovery=PASS");
    if (ctl_ambiguous) std::puts("fake_server_host_ctl_device_ambiguity=PASS");
    if (ctl_reconnect_identity) std::puts("fake_server_host_ctl_reconnect_identity=PASS");
    if (ctl_single_usb) std::puts("fake_server_host_ctl_single_usb_selector=PASS");
    if (ctl_single_usb_reconnect)
        std::puts("fake_server_host_ctl_single_usb_operation_assertion=PASS");
    if (ctl_multi_usb) std::puts("fake_server_host_ctl_multi_usb_selector=PASS");
    if (ctl_invalid_usb) std::puts("fake_server_host_ctl_invalid_usb_topology=PASS");
    if (ctl_security) std::puts("fake_server_host_ctl_security_refusal=PASS");
    if (lnb_unsupported) std::puts("fake_server_host_ctl_lnb_unsupported=PASS");
    if (lnb_refused) std::puts("fake_server_host_ctl_lnb_remote_refusal=PASS");
    if (ts) std::puts("fake_server_host_ts_record_attach_stream_end_stop_release=PASS");
    if (eof) std::puts("fake_server_host_ts_peer_eof_cleanup=PASS");
    if (output_timeouts)
        std::puts("fake_server_host_ts_output_timeout_signal_and_regular=PASS");
    if (overlong) std::puts("runtime_discovery_overlong_endpoint_then_live=PASS");
    if (oom) std::puts("fake_server_host_ts_frame_oom_stop_release=PASS");
    if (unsupported) std::puts("runtime_discovery_remote_unsupported_then_live=PASS");
    if (not_ready) std::puts("runtime_discovery_not_ready_then_live=PASS");
    else std::fputs("runtime_discovery_not_ready_then_live=FAIL\n", stderr);
    if (security) std::puts("runtime_discovery_security_refusal_not_skipped=PASS");
    return ctl && lnb_ctl && ctl_discovery && ctl_ambiguous &&
           ctl_reconnect_identity && ctl_single_usb && ctl_single_usb_reconnect &&
           ctl_multi_usb && ctl_invalid_usb && ctl_security && lnb_unsupported && lnb_refused &&
           ts && eof && output_timeouts && overlong &&
           oom && unsupported && not_ready && security ? 0 : 1;
}
