// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_runtime/posix_ipc.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

namespace {

bool make_directory(const std::string& path) noexcept
{
    return ::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), 0700) == 0;
}

}  // namespace

int main()
{
    const char* temporary = std::getenv("TMPDIR");
    if (temporary == nullptr || temporary[0] != '/') temporary = "/tmp";
    std::array<char, 512U> base{};
    const char* external_root = std::getenv("DTV_RUNTIME_EXPECTED_ROOT");
    char* made = nullptr;
    std::string expected;
    if (external_root != nullptr && external_root[0] == '/') {
        expected = external_root;
    } else {
        std::string pattern = std::string(temporary) + "/r-XXXXXX";
        if (pattern.size() >= base.size()) return 2;
        std::memcpy(base.data(), pattern.c_str(), pattern.size() + 1U);
        made = ::mkdtemp(base.data());
        if (made == nullptr || ::unsetenv("XDG_RUNTIME_DIR") != 0 ||
            ::setenv("TMPDIR", made, 1) != 0) return 3;
        expected = std::string(made) + "/dtv-userland-" +
            std::to_string(static_cast<unsigned long>(::geteuid()));
    }
    const auto published = select_publish_runtime_root("fake-native", "x");
    bool ok = published && published.value() == expected;
    if (!ok) std::fprintf(stderr, "publish_failed=%u expected=%s actual=%s\n",
        published ? 0U : static_cast<unsigned>(published.error()), expected.c_str(),
        published ? published.value().c_str() : "");
    const std::string product = expected + "/fake-native";
    const std::string version = product + "/v1";
    const std::string instance = version + "/x";
    ok = ok && make_directory(product) && make_directory(version) &&
        make_directory(instance);
    if (!ok) std::fprintf(stderr, "namespace_creation_failed root=%s\n", expected.c_str());

    int listener = -1;
    std::string socket_path;
    if (ok) {
        const auto path = dtv::userland::host_runtime::control_endpoint_path(
            {expected, "fake-native", "x"});
        ok = static_cast<bool>(path);
        if (path) socket_path = path.value();
        else std::fprintf(stderr, "endpoint_path_failed=%u\n",
            static_cast<unsigned>(path.error()));
    }
    if (ok) {
        listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
        ok = listener >= 0 &&
            ::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            ::chmod(socket_path.c_str(), 0600) == 0 && ::listen(listener, 1) == 0;
    }
    auto client = ok ? SocketStream::connect(
        {nullptr, "x", "control.sock", EndpointAccess::private_user,
         "fake-native"}, Timeout{1000U})
        : Result<SocketStream>::failure(Error::INTERNAL);
    const int peer = client ? ::accept(listener, nullptr, nullptr) : -1;
    ok = ok && client && client.value().valid() && peer >= 0;
    if (!client) std::fprintf(stderr, "default_connect_failed=%u\n",
        static_cast<unsigned>(client.error()));
    if (client && peer < 0) std::fprintf(stderr, "fake_listener_accept_failed\n");

    if (peer >= 0) (void)::close(peer);
    if (client) client.value().close();
    if (listener >= 0) (void)::close(listener);
    if (!socket_path.empty()) (void)::unlink(socket_path.c_str());
    (void)::rmdir(instance.c_str());
    (void)::rmdir(version.c_str());
    (void)::rmdir(product.c_str());
    if (made != nullptr) {
        (void)::rmdir(expected.c_str());
        (void)::rmdir(made);
    }
    if (ok) std::puts("posix_runtime_default_fake_connect=PASS");
    return ok ? 0 : 4;
}
