// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_posix.h>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace px4::userland::cli {
namespace {

constexpr int kOutputPollMilliseconds = 100;

}  // namespace

Px4TsFdOutput::Px4TsFdOutput(int fd, bool close_fd) noexcept
    : fd_(fd), close_fd_(close_fd)
{
    if (fd_ < 0) return;
    original_flags_ = ::fcntl(fd_, F_GETFL);
    if (original_flags_ < 0 ||
        ::fcntl(fd_, F_SETFL, original_flags_ | O_NONBLOCK) < 0) {
        return;
    }
    ready_ = true;
}

Px4TsFdOutput::~Px4TsFdOutput() noexcept
{
    if (fd_ >= 0 && original_flags_ >= 0)
        (void)::fcntl(fd_, F_SETFL, original_flags_);
    if (close_fd_ && fd_ >= 0) (void)::close(fd_);
}

Result<void> Px4TsFdOutput::write(ByteView bytes,
                                  const Px4TsSignal& signal) noexcept
{
    if (!ready_ || bytes.data == nullptr || bytes.size != 188U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (signal.stop_requested()) return Result<void>::failure(Error::TIMEOUT);

    std::size_t offset = 0U;
    while (offset < bytes.size) {
        const ssize_t written = ::write(fd_, bytes.data + offset, bytes.size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            if (offset < bytes.size && signal.stop_requested())
                return Result<void>::failure(Error::INTERNAL);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            if (signal.stop_requested())
                return offset == 0U ? Result<void>::failure(Error::TIMEOUT) :
                                     Result<void>::failure(Error::INTERNAL);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (signal.stop_requested())
                return offset == 0U ? Result<void>::failure(Error::TIMEOUT) :
                                     Result<void>::failure(Error::INTERNAL);
            pollfd descriptor{fd_, POLLOUT, 0};
            int polled = 0;
            do {
                polled = ::poll(&descriptor, 1U, kOutputPollMilliseconds);
            } while (polled < 0 && errno == EINTR && !signal.stop_requested());
            if (signal.stop_requested())
                return offset == 0U ? Result<void>::failure(Error::TIMEOUT) :
                                     Result<void>::failure(Error::INTERNAL);
            if (polled < 0) return Result<void>::failure(Error::INTERNAL);
            if (polled > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return Result<void>::failure(Error::INTERNAL);
            continue;
        }
        return Result<void>::failure(Error::INTERNAL);
    }
    return Result<void>::success();
}

}  // namespace px4::userland::cli
