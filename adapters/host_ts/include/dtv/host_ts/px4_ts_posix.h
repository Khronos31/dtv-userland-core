// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_HOST_TS_PX4_TS_POSIX_H
#define DTV_HOST_TS_PX4_TS_POSIX_H

#include <dtv/host_ts/px4_ts_core.h>

namespace px4::userland::cli {

// POSIX output is deliberately packet-sized and nonblocking. A pipe/socket
// which stops accepting data is polled in bounded intervals so signal
// cancellation can return to the runner without leaving a partial packet.
class Px4TsFdOutput final : public Px4TsOutput {
public:
    explicit Px4TsFdOutput(int fd, bool close_fd) noexcept;
    ~Px4TsFdOutput() noexcept override;

    Result<void> write(ByteView bytes, const Px4TsSignal& signal) noexcept override;
    bool ready() const noexcept { return ready_; }

private:
    int fd_ = -1;
    int original_flags_ = -1;
    bool close_fd_ = false;
    bool ready_ = false;
};

}  // namespace px4::userland::cli

#endif  // DTV_HOST_TS_PX4_TS_POSIX_H
