// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_CONTROL_SERVER_TEST_ACCESS_H
#define PX4_USERLAND_CONTROL_SERVER_TEST_ACCESS_H

#include <cstddef>
#include <cstdint>

namespace px4::userland::ipc::posix {

// This header is included only by the test-only control_server translation
// unit.  The callbacks observe bounded socket progress; they do not alter
// server behavior or expose Client objects.
struct ControlServerTestHooks final {
    void (*stream_count_changed)(void*, std::size_t, std::size_t) noexcept = nullptr;
    void (*stream_partial_frame)(void*, std::uint64_t) noexcept = nullptr;
    void (*stream_write_blocked)(void*, std::uint64_t) noexcept = nullptr;
    // Control responses: nonzero caps the bytes one flush may send, so a
    // response stays partially written across poll cycles; the callback
    // reports each flush that leaves bytes pending.
    std::size_t control_write_limit = 0U;
    void (*control_write_partial)(void*, std::uint64_t) noexcept = nullptr;
    void (*worker_task_submitted)(void*, std::uint8_t, std::uint8_t,
                                  std::uint64_t, std::uint32_t,
                                  std::uint8_t) noexcept = nullptr;
    void (*worker_task_completed)(void*, std::uint8_t, std::uint64_t,
                                  std::uint32_t, std::uint8_t,
                                  std::uint32_t) noexcept = nullptr;
    void (*workers_joined)(void*, std::size_t) noexcept = nullptr;
    void* context = nullptr;
};

void set_control_server_test_hooks(ControlServerTestHooks* hooks) noexcept;

}  // namespace px4::userland::ipc::posix

#endif  // PX4_USERLAND_CONTROL_SERVER_TEST_ACCESS_H
