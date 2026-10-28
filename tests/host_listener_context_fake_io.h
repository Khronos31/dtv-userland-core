// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_HOST_LISTENER_CONTEXT_FAKE_IO_H
#define DTV_USERLAND_HOST_LISTENER_CONTEXT_FAKE_IO_H

#include "detail/listener_io.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class FakeListenerIoControl final {
public:
    FakeListenerIoControl();
    ~FakeListenerIoControl();
    FakeListenerIoControl(const FakeListenerIoControl&) = delete;
    FakeListenerIoControl& operator=(const FakeListenerIoControl&) = delete;

    dtv::userland::host_listener_context::detail::ConnectionToken connect(
        dtv::userland::host_listener_context::detail::ListenerKind listener,
        dtv::userland::ByteView input = {nullptr, 0U});
    void send(dtv::userland::host_listener_context::detail::ConnectionToken connection,
              dtv::userland::ByteView input);
    void disconnect(dtv::userland::host_listener_context::detail::ConnectionToken connection);
    std::vector<std::uint8_t> output(
        dtv::userland::host_listener_context::detail::ConnectionToken connection) const;
    bool closed(dtv::userland::host_listener_context::detail::ConnectionToken connection) const;
    std::size_t accepted(
        dtv::userland::host_listener_context::detail::ListenerKind listener) const;
    void set_chunks(std::size_t read_bytes, std::size_t write_bytes);
    void block_writes(dtv::userland::host_listener_context::detail::ConnectionToken connection,
                      bool blocked);
    void block_unpublish();
    void fail_unpublish_after_cleanup();
    bool wait_unpublish();
    bool wait_poll();
    void release_unpublish();
    bool reserved() const;
    bool published() const;
    std::size_t unpublish_count() const;
    std::size_t namespace_release_count() const;
    std::uint32_t last_poll_timeout_ms() const;

private:
    friend void set_fake_listener_io_control(FakeListenerIoControl*) noexcept;
    friend std::unique_ptr<dtv::userland::host_listener_context::detail::ListenerIo>
        dtv::userland::host_listener_context::detail::create_listener_io() noexcept;
    friend class FakeListenerIo;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void set_fake_listener_io_control(FakeListenerIoControl* control) noexcept;
std::size_t host_listener_context_ts_buffer_current_for_test() noexcept;
std::size_t host_listener_context_ts_buffer_high_water_for_test() noexcept;

#endif  // DTV_USERLAND_HOST_LISTENER_CONTEXT_FAKE_IO_H
