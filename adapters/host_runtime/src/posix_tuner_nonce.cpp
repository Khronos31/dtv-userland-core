// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_tuner_nonce.h"

#if !defined(_WIN32)

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace px4::userland::ipc::posix {

Result<std::array<std::uint8_t, ipc::kNonceLength>>
PosixTunerNonceSource::generate() noexcept
{
    std::array<std::uint8_t, ipc::kNonceLength> nonce{};
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return Result<decltype(nonce)>::failure(Error::INTERNAL);
    }
    std::size_t offset = 0U;
    while (offset < nonce.size()) {
        const ssize_t count = ::read(descriptor, nonce.data() + offset,
                                     nonce.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || static_cast<std::size_t>(count) > nonce.size() - offset) {
            (void)::close(descriptor);
            return Result<decltype(nonce)>::failure(Error::INTERNAL);
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(descriptor) != 0) {
        return Result<decltype(nonce)>::failure(Error::INTERNAL);
    }
    return Result<decltype(nonce)>::success(nonce);
}

}  // namespace px4::userland::ipc::posix

#endif
