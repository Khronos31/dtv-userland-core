// SPDX-License-Identifier: GPL-2.0-only
#include "detail/listener_io.h"
#include "dtv/userland/host_listener_context/enclosure_context.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>

using namespace dtv::userland;
using namespace dtv::userland::host_listener_context::detail;
using dtv::userland::host_listener_context::ListenerDiagnostic;
using dtv::userland::host_listener_context::ListenerDiagnosticReason;

namespace {

bool check(bool value, const char* label) noexcept
{
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", label);
    return false;
}

bool make_root(std::string& output) noexcept
{
    std::array<char, 64U> path{};
    const char* pattern = "/tmp/dtv-listener-XXXXXX";
    std::memcpy(path.data(), pattern, std::strlen(pattern) + 1U);
    char* created = ::mkdtemp(path.data());
    if (created == nullptr) return false;
    output = created;
    return true;
}

host_runtime::EndpointConfig config(const std::string& root, const char* name,
                                   host_runtime::EndpointAccess access =
                                       host_runtime::EndpointAccess::private_user) noexcept
{
    return host_runtime::EndpointConfig{root, "listener-test", "instance", access, -1, name};
}

bool path_for(const host_runtime::EndpointConfig& endpoint, std::string& path) noexcept
{
    const auto result = host_runtime::endpoint_path(endpoint);
    if (!result) return false;
    path = result.value();
    return true;
}

int connect_peer(const std::string& path) noexcept
{
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        (void)::close(socket);
        return -1;
    }
    return socket;
}

bool test_pair_io_and_interest() noexcept
{
    std::string root;
    if (!check(make_root(root), "private runtime root")) return false;
    auto io = create_listener_io();
    auto control = config(root, "control.sock");
    auto data = config(root, "stream.sock");
    if (!check(io != nullptr, "listener factory") ||
        !check(io->reserve_pair(control, data) == Error::OK, "reserve private pair")) {
        std::filesystem::remove_all(root);
        return false;
    }
    ListenerPair pair{};
    if (!check(io->publish_pair(pair) == Error::OK, "publish private pair")) {
        (void)io->release_namespace();
        std::filesystem::remove_all(root);
        return false;
    }
    if (!check(io->release_namespace() == Error::BUSY, "published pair retains namespace lease")) return false;
    std::string control_path;
    struct stat info{};
    const bool mode_ok = path_for(control, control_path) &&
        ::lstat(control_path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) &&
        info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0;
    if (!check(mode_ok, "private endpoint owner and mode")) return false;

    const int peer = connect_peer(control_path);
    if (!check(peer >= 0, "connect control peer")) return false;
    std::array<ReadyEvent, kListenerIoEventCapacity> events{};
    std::size_t count = 0U;
    if (!check(io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
                        events.data(), events.size(), count) == Error::OK && count == 1U &&
               events[0].kind == ReadyKind::listener && events[0].listener_kind == ListenerKind::control,
               "control listener readiness")) return false;
    const AcceptResult accepted = io->accept(events[0].listener);
    if (!check(accepted.status == IoStatus::progress, "accept control peer")) return false;

    std::array<std::uint8_t, 16U> read_buffer{};
    IoResult result = io->read_some(accepted.connection, MutableByteView{read_buffer.data(), read_buffer.size()});
    if (!check(result.status == IoStatus::would_block && result.error == Error::OK,
               "nonblocking would-block")) return false;
    const auto before = std::chrono::steady_clock::now();
    if (!check(io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{25U},
                        events.data(), events.size(), count) == Error::OK && count == 0U &&
               std::chrono::steady_clock::now() - before >= std::chrono::milliseconds(15),
               "idle read-only poll waits without writable busy loop")) return false;
    if (!check(io->set_connection_interest(accepted.connection, ready_write) == Error::OK &&
               io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{100U},
                        events.data(), events.size(), count) == Error::OK && count == 1U &&
               (events[0].flags & ready_write) != 0U,
               "explicit write readiness")) return false;
    if (!check(io->set_connection_interest(accepted.connection, ready_read) == Error::OK,
               "restore read interest")) return false;

    int wake[2] = {-1, -1};
    const char wake_byte = 1;
    if (!check(::pipe(wake) == 0 && ::write(wake[1], &wake_byte, 1U) == 1,
               "prepare worker wake")) return false;
    if (!check(io->poll(wake[0], Timeout{100U}, events.data(), events.size(), count) == Error::OK &&
               count == 1U && events[0].kind == ReadyKind::worker_wake,
               "worker wake readiness")) return false;
    char drained = 0;
    (void)::read(wake[0], &drained, 1U);
    (void)::close(wake[0]);
    (void)::close(wake[1]);

    const char first = 'a';
    if (!check(::send(peer, &first, 1U, 0) == 1, "send first fragment")) return false;
    result = io->read_some(accepted.connection, MutableByteView{read_buffer.data(), read_buffer.size()});
    if (!check(result.status == IoStatus::progress && result.transferred == 1U && read_buffer[0] == 'a',
               "read first fragment")) return false;
    const char second[] = {'b', 'c'};
    if (!check(::send(peer, second, sizeof(second), 0) == static_cast<ssize_t>(sizeof(second)),
               "send second fragment")) return false;
    result = io->read_some(accepted.connection, MutableByteView{read_buffer.data(), read_buffer.size()});
    if (!check(result.status == IoStatus::progress && result.transferred == sizeof(second),
               "read second fragment")) return false;

    std::vector<std::uint8_t> large(8U * 1024U * 1024U, 0x5aU);
    const IoResult written = io->write_some(accepted.connection, ByteView{large.data(), large.size()});
    if (!check(written.status == IoStatus::progress && written.transferred > 0U &&
               written.transferred < large.size(), "partial nonblocking write")) return false;
    if (!check(io->close_connection(accepted.connection) == Error::OK &&
               io->set_connection_interest(accepted.connection, ready_read) == Error::INVALID_ARGUMENT,
               "closed token cannot target reused native handle")) return false;
    (void)::close(peer);

    const int eof_peer = connect_peer(control_path);
    if (!check(eof_peer >= 0 &&
               io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
                        events.data(), events.size(), count) == Error::OK && count == 1U,
               "connect EOF peer")) return false;
    const AcceptResult eof_connection = io->accept(events[0].listener);
    if (!check(eof_connection.status == IoStatus::progress && ::shutdown(eof_peer, SHUT_WR) == 0,
               "accept and half-close EOF peer")) return false;
    result = io->read_some(eof_connection.connection, MutableByteView{read_buffer.data(), read_buffer.size()});
    if (!check(result.status == IoStatus::eof && result.transferred == 0U,
               "clean EOF distinct from WouldBlock")) return false;
    (void)io->close_connection(eof_connection.connection);
    (void)::close(eof_peer);

    const int reset_peer = connect_peer(control_path);
    if (!check(reset_peer >= 0 &&
               io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
                        events.data(), events.size(), count) == Error::OK && count == 1U,
               "connect reset peer")) return false;
    const AcceptResult reset_connection = io->accept(events[0].listener);
    if (!check(reset_connection.status == IoStatus::progress, "accept reset peer")) return false;
    const linger reset{1, 0};
    (void)::setsockopt(reset_peer, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    (void)::close(reset_peer);
    (void)io->poll(::px4::userland::ipc::posix::kInvalidWakeHandle, Timeout{1000U},
                   events.data(), events.size(), count);
    result = io->read_some(reset_connection.connection, MutableByteView{read_buffer.data(), read_buffer.size()});
    const IoResult reset_write = result.status == IoStatus::failure ? result :
        io->write_some(reset_connection.connection, ByteView{large.data(), 1U});
    if (!check(reset_write.status == IoStatus::failure && reset_write.error == Error::DISCONNECTED,
               "reset maps to typed disconnect")) return false;
    (void)io->unpublish_pair(pair);
    const Error released = io->release_namespace();
    std::filesystem::remove_all(root);
    return check(released == Error::OK, "release private namespace");
}

bool test_stale_and_foreign_rollback() noexcept
{
    std::string root;
    if (!check(make_root(root), "stale fixture root")) return false;
    auto control = config(root, "control.sock");
    auto data = config(root, "stream.sock");
    std::string data_path;
    if (!check(path_for(data, data_path), "data path")) return false;
    auto io = create_listener_io();
    if (!check(io->reserve_pair(control, data) == Error::OK, "reserve stale namespace")) return false;
    const std::filesystem::path parent = std::filesystem::path(data_path).parent_path();
    const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, data_path.c_str(), data_path.size() + 1U);
    const bool stale_created = stale >= 0 &&
        ::bind(stale, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    if (stale >= 0) (void)::close(stale);
    if (!check(stale_created, "create stale owned socket")) return false;
    ListenerPair pair{};
    if (!check(io->publish_pair(pair) == Error::OK, "replace stale owned endpoint")) return false;
    (void)io->unpublish_pair(pair);
    (void)io->release_namespace();
    std::filesystem::remove_all(root);

    if (!check(make_root(root), "foreign fixture root")) return false;
    control = config(root, "control.sock");
    data = config(root, "stream.sock");
    std::string control_path;
    std::string foreign_path;
    if (!check(path_for(control, control_path) && path_for(data, foreign_path), "foreign paths")) return false;
    io = create_listener_io();
    if (!check(io->reserve_pair(control, data) == Error::OK, "reserve foreign namespace")) return false;
    const int foreign = ::open(foreign_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (!check(foreign >= 0, "create foreign regular path")) return false;
    const char marker[] = "foreign";
    (void)::write(foreign, marker, sizeof(marker));
    (void)::close(foreign);
    const Error publish = io->publish_pair(pair);
    struct stat control_info{};
    struct stat foreign_info{};
    const bool rollback = publish != Error::OK && ::lstat(control_path.c_str(), &control_info) != 0 &&
        ::lstat(foreign_path.c_str(), &foreign_info) == 0 && S_ISREG(foreign_info.st_mode) &&
        foreign_info.st_uid == ::geteuid();
    (void)io->release_namespace();
    std::filesystem::remove_all(root);
    return check(rollback, "one-side failure rolls back own socket and preserves foreign path");
}

bool test_shared_group_boundary() noexcept
{
    std::string root;
    if (!check(make_root(root), "shared group root")) return false;
    if (!check(::chmod(root.c_str(), 0750) == 0, "set shared group root mode")) return false;
    auto control = config(root, "control.sock", host_runtime::EndpointAccess::shared_group);
    auto data = config(root, "stream.sock", host_runtime::EndpointAccess::shared_group);
    auto io = create_listener_io();
    ListenerPair pair{};
    if (!check(io->reserve_pair(control, data) == Error::OK &&
               io->publish_pair(pair) == Error::OK, "allow safe shared-group namespace")) return false;
    std::string control_path;
    struct stat info{};
    const bool mode = path_for(control, control_path) && ::lstat(control_path.c_str(), &info) == 0 &&
        S_ISSOCK(info.st_mode) && (info.st_mode & 0077) == 0060 &&
        info.st_gid == ::getegid();
    if (!check(mode, "shared endpoint group read-write only")) return false;
    (void)io->unpublish_pair(pair);
    (void)io->release_namespace();
    std::filesystem::remove_all(root);

    if (!check(make_root(root) && ::chmod(root.c_str(), 0777) == 0, "unsafe group root")) return false;
    control = config(root, "control.sock", host_runtime::EndpointAccess::shared_group);
    data = config(root, "stream.sock", host_runtime::EndpointAccess::shared_group);
    io = create_listener_io();
    ListenerDiagnostic diagnostic{};
    const Error denied = io->reserve_pair(control, data, &diagnostic);
    std::filesystem::remove_all(root);
    if (!check(denied == Error::UNSUPPORTED, "reject world-accessible shared-group root") ||
        !check(diagnostic.reason == ListenerDiagnosticReason::runtime_directory_mode,
               "report actual runtime mode refusal")) return false;

    if (!check(make_root(root), "owner fixture root")) return false;
    if (::geteuid() == 0) {
        if (!check(::chown(root.c_str(), 65534, 65534) == 0, "set foreign runtime owner"))
            return false;
        control = config(root, "control.sock");
        data = config(root, "stream.sock");
        diagnostic = ListenerDiagnostic{};
        const Error owner_denied = io->reserve_pair(control, data, &diagnostic);
        const bool refused = owner_denied == Error::UNSUPPORTED &&
            diagnostic.reason == ListenerDiagnosticReason::runtime_directory_owner;
        (void)::chown(root.c_str(), ::geteuid(), ::getegid());
        if (!check(refused, "report actual runtime owner refusal")) return false;
    }
    std::filesystem::remove_all(root);

    const std::string missing_root = root + "/missing";
    control = config(missing_root, "control.sock");
    data = config(missing_root, "stream.sock");
    diagnostic = ListenerDiagnostic{};
    const Error missing_error = io->reserve_pair(control, data, &diagnostic);
    if (!check(missing_error == Error::UNSUPPORTED &&
               diagnostic.reason == ListenerDiagnosticReason::runtime_directory_missing,
               "report absent runtime directory without claiming ACL failure")) return false;

    const std::string overlong_root = "/" + std::string(96U, 'x');
    control = config(overlong_root, "control.sock");
    data = config(overlong_root, "stream.sock");
    diagnostic = ListenerDiagnostic{};
    const Error long_error = io->reserve_pair(control, data, &diagnostic);
    return check(long_error == Error::INVALID_ARGUMENT &&
                 diagnostic.reason == ListenerDiagnosticReason::endpoint_path &&
                 diagnostic.endpoint.reason ==
                    host_runtime::EndpointDiagnosticReason::path_too_long &&
                 diagnostic.endpoint.path_bytes + 1U ==
                    diagnostic.endpoint.required_bytes &&
                 diagnostic.endpoint.required_bytes >
                    diagnostic.endpoint.native_capacity,
                 "report completed listener endpoint path length");
}

bool test_nested_directory_diagnostics() noexcept
{
    using Layer = dtv::userland::host_listener_context::RuntimeDirectoryLayer;
    const Layer layers[] = {Layer::product, Layer::version, Layer::instance};
    for (const Layer layer : layers) {
        std::string root;
        if (!check(make_root(root), "nested runtime root")) return false;
        const std::string product = root + "/listener-test";
        const std::string version = product + "/v1";
        const std::string instance = version + "/instance";
        std::string target = product;
        bool created = true;
        if (layer != Layer::product) {
            created = ::mkdir(product.c_str(), 0700) == 0;
            target = version;
        }
        if (layer == Layer::instance) {
            created = created && ::mkdir(version.c_str(), 0700) == 0;
            target = instance;
        }
        created = created && ::mkdir(target.c_str(), 0700) == 0 &&
                  ::chmod(target.c_str(), 0777) == 0;
        auto io = create_listener_io();
        ListenerDiagnostic diagnostic{};
        const auto control = config(root, "control.sock");
        const auto data = config(root, "stream.sock");
        const Error result = created ? io->reserve_pair(control, data, &diagnostic)
                                     : Error::INTERNAL;
        const bool valid = created && result == Error::INTERNAL &&
            diagnostic.reason == ListenerDiagnosticReason::runtime_directory_mode &&
            diagnostic.directory_layer == layer;
        io.reset();
        (void)::chmod(target.c_str(), 0700);
        std::filesystem::remove_all(root);
        if (!check(valid, "report nested runtime directory mode and layer")) return false;
    }

    if (::geteuid() == 0) {
        std::string root;
        if (!check(make_root(root), "nested owner runtime root")) return false;
        const std::string product = root + "/listener-test";
        if (::mkdir(product.c_str(), 0700) != 0 ||
            ::chown(product.c_str(), 65534, 65534) != 0) {
            std::filesystem::remove_all(root);
            return false;
        }
        auto io = create_listener_io();
        ListenerDiagnostic diagnostic{};
        const auto control = config(root, "control.sock");
        const auto data = config(root, "stream.sock");
        const Error result = io->reserve_pair(control, data, &diagnostic);
        const bool valid = result == Error::INTERNAL &&
            diagnostic.reason == ListenerDiagnosticReason::runtime_directory_owner &&
            diagnostic.directory_layer == Layer::product;
        io.reset();
        (void)::chown(product.c_str(), ::geteuid(), ::getegid());
        std::filesystem::remove_all(root);
        if (!check(valid, "report nested runtime directory owner and layer")) return false;
    }
    return true;
}

}  // namespace

int main() noexcept
{
    return test_pair_io_and_interest() && test_stale_and_foreign_rollback() &&
        test_shared_group_boundary() && test_nested_directory_diagnostics() ? 0 : 1;
}
