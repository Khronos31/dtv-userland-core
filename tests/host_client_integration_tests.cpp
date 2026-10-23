// SPDX-License-Identifier: GPL-2.0-only
#include "host_ctl.h"
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

struct Listener final {
    Socket socket = kInvalidSocket;
    std::string path;
#if defined(_WIN32)
    std::wstring path_wide;
#endif

    bool bind(const std::string& root, const std::string& instance,
              const char* endpoint_name)
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
               ::listen(socket, 8) == 0 &&
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
               ::chmod(path.c_str(), 0600) == 0 && ::listen(socket, 8) == 0;
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
        const ssize_t count = ::send(socket, input + offset, size - offset, 0);
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
    std::atomic<bool> saw_stop{false};
    std::atomic<bool> saw_release{false};
    std::atomic<unsigned int> stop_count{0U};
    std::atomic<unsigned int> release_count{0U};
    bool unsupported_status = false;
    std::mutex stop_mutex;
    std::condition_variable stop_changed;

    explicit FakeService(const std::string& instance)
    {
        for (std::size_t i = 0U; i < hello.epoch.size(); ++i)
            hello.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
        set_text(hello.producer.product, "test-product");
        set_text(hello.producer.version, "1.0.0-test");
        list.epoch = hello.epoch;
        list.producer = hello.producer;
        list.snapshot = snapshot;
        list.topology.state = wire::TopologyState::complete;
        list.topology.expected_function_count.present = true;
        list.topology.expected_function_count.value = 1U;
        list.enclosure.present = true;
        list.enclosure.value.identity.serial_state = wire::IdentitySerialState::absent;
        list.enclosure.value.identity.uniqueness = wire::Uniqueness::not_applicable;
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
            return send_record(peer, request.opcode, request.request_id, list,
                               &wire::encode_list_response);
        }
        if (request.kind == wire::ControlRequestKind::status) {
            saw_status.store(true);
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
            if (sent) saw_release.store(true);
            return sent;
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
                if (!send_stream_end(peer, make_final(99U))) good.store(false);
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
    if (!tree.create() || !tree.create_instance("ctl-live")) return false;
    Listener control;
    if (!control.bind(tree.root, "ctl-live", "control.sock")) return false;
    FakeService service("ctl-live");
    FakeControlServer server(control, service, 2U);
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
    const int list_status = dtv::userland::host_ctl::run_control_cli(
        6, list_args, profile);
    const int status_status = dtv::userland::host_ctl::run_control_cli(
        6, status_args, profile);
    server.join();
    return list_status == 0 && status_status == 0 && server.good.load();
}

bool run_ts_lifecycle(bool close_before_stream_end)
{
    RuntimeTree missing_root;
    RuntimeTree stale_root;
    RuntimeTree live_root;
    if (!missing_root.create() || !stale_root.create() ||
        !stale_root.create_instance("stale") || !live_root.create() ||
        !live_root.create_instance("live-01")) return false;
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
    const bool ts = run_ts_lifecycle(false);
    const bool eof = run_ts_lifecycle(true);
    const bool oom = test_host_ts_frame_allocation_failure_releases_lease();
    const bool unsupported = test_unsupported_candidate_then_live_instance();
    const bool security = test_security_refusal_stops_search();
#if defined(_WIN32)
    (void)::WSACleanup();
#endif
    if (ctl) std::puts("fake_server_host_ctl_list_status=PASS");
    if (ts) std::puts("fake_server_host_ts_record_attach_stream_end_stop_release=PASS");
    if (eof) std::puts("fake_server_host_ts_peer_eof_cleanup=PASS");
    if (oom) std::puts("fake_server_host_ts_frame_oom_stop_release=PASS");
    if (unsupported) std::puts("runtime_discovery_remote_unsupported_then_live=PASS");
    if (security) std::puts("runtime_discovery_security_refusal_not_skipped=PASS");
    return ctl && ts && eof && oom && unsupported && security ? 0 : 1;
}
