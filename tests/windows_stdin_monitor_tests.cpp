// SPDX-License-Identifier: GPL-2.0-only
// Windows tests for the cooperative stdin-EOF monitor: already-EOF input,
// EOF after data, a read failure, and bounded cancel/join when a never-closing
// pipe is stopped by the main thread. Uses anonymous pipes only.
#include <dtv/userland/host_facade/windows_stdin.h>

#if defined(_WIN32)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using px4::userland::cli::StdinEofMonitor;

int failures = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                               \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

void on_eof(void* context) noexcept
{
    static_cast<std::atomic<int>*>(context)->fetch_add(1, std::memory_order_acq_rel);
}

bool wait_for_count(std::atomic<int>& count, int target, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (count.load(std::memory_order_acquire) < target &&
           std::chrono::steady_clock::now() < deadline) {
        ::Sleep(5U);
    }
    return count.load(std::memory_order_acquire) >= target;
}

void test_already_eof()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    if (read_end == nullptr) {
        return;
    }
    ::CloseHandle(write_end);  // immediate EOF
    std::atomic<int> count{0};
    StdinEofMonitor monitor;
    CHECK(monitor.start(read_end, &on_eof, &count));
    CHECK(wait_for_count(count, 1, 2000));
    const auto start = std::chrono::steady_clock::now();
    monitor.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(elapsed.count() < 2000);
    ::CloseHandle(read_end);
}

void test_eof_after_data()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    if (read_end == nullptr) {
        return;
    }
    std::atomic<int> count{0};
    StdinEofMonitor monitor;
    CHECK(monitor.start(read_end, &on_eof, &count));
    DWORD written = 0U;
    CHECK(::WriteFile(write_end, "x", 1, &written, nullptr) != 0);
    ::CloseHandle(write_end);
    CHECK(wait_for_count(count, 1, 2000));
    monitor.stop();
    ::CloseHandle(read_end);
}

void test_read_failure()
{
    // A write-only handle makes ReadFile fail, which the monitor treats as a
    // stop condition.
    wchar_t temp[MAX_PATH];
    const DWORD length = ::GetTempPathW(MAX_PATH, temp);
    CHECK(length != 0U);
    std::wstring path(temp, length);
    path += L"px4-stdin-monitor.bin";
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    std::atomic<int> count{0};
    StdinEofMonitor monitor;
    CHECK(monitor.start(handle, &on_eof, &count));
    CHECK(wait_for_count(count, 1, 2000));
    monitor.stop();
    ::CloseHandle(handle);
    (void)::DeleteFileW(path.c_str());
}

void test_never_closing_pipe_bounded_stop()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    if (read_end == nullptr) {
        return;
    }
    std::atomic<int> count{0};
    StdinEofMonitor monitor;
    CHECK(monitor.start(read_end, &on_eof, &count));
    ::Sleep(100U);
    const auto start = std::chrono::steady_clock::now();
    monitor.stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(elapsed.count() < 2000);
    CHECK(!monitor.running());
    CHECK(count.load(std::memory_order_acquire) == 0);
    ::CloseHandle(read_end);
    ::CloseHandle(write_end);
}

}  // namespace

int main()
{
    test_already_eof();
    test_eof_after_data();
    test_read_failure();
    test_never_closing_pipe_bounded_stop();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("windows stdin monitor tests: PASS\n");
    return 0;
}

#endif  // _WIN32
