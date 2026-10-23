// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/control_workers.h"

#include <atomic>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <bcrypt.h>
#include <windows.h>
#include <cwchar>
#if defined(_MSC_VER)
#include "windows_pthread_compat.h"
#else
#include <pthread.h>
#endif
#include "windows/windows_security.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace px4::userland::ipc::posix {
namespace {

bool operation_lane_valid(ControlWorkerLane lane, const ControlWorkerTask& task,
                          const ControlExecutionLayout& execution) noexcept
{
    switch (task.operation) {
    case ControlWorkerOperation::tuner_authority_job:
        if (task.kind == ControlWorkerTaskKind::authority_cleanup) {
            return task.authority_job.kind ==
                       dtv::userland::detail::AuthorityJobKind::disconnect &&
                   lane == execution.tuner_shutdown_lane;
        }
        if (task.kind != ControlWorkerTaskKind::authority_request ||
            task.authority_job.kind ==
                dtv::userland::detail::AuthorityJobKind::disconnect)
            return false;
        if (task.receiver >= execution.receiver_count ||
            task.receiver >= execution.receiver_lane.size())
            return false;
        return lane == execution.receiver_lane[task.receiver];
    case ControlWorkerOperation::tuner_status:
        return lane == execution.status_lane;
    case ControlWorkerOperation::tuner_acquire:
    case ControlWorkerOperation::tuner_release:
    case ControlWorkerOperation::tuner_tune:
    case ControlWorkerOperation::tuner_start_stream:
    case ControlWorkerOperation::tuner_stop_stream:
    case ControlWorkerOperation::tuner_start_stream_v1:
    case ControlWorkerOperation::tuner_stop_stream_v1:
    case ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1:
    case ControlWorkerOperation::tuner_prepare_attach_v1:
    case ControlWorkerOperation::tuner_run_stream_cleanup_v1:
    case ControlWorkerOperation::tuner_stats:
    case ControlWorkerOperation::tuner_attach_stream:
    case ControlWorkerOperation::tuner_detach_stream:
        if (task.receiver >= execution.receiver_count ||
            task.receiver >= execution.receiver_lane.size()) {
            return false;
        }
        return lane == execution.receiver_lane[task.receiver];
    case ControlWorkerOperation::tuner_shutdown:
        return lane == execution.tuner_shutdown_lane;
    case ControlWorkerOperation::tuner_lnb_power:
        return lane == execution.lnb_lane;
    case ControlWorkerOperation::card_status:
    case ControlWorkerOperation::card_handle_status:
    case ControlWorkerOperation::card_status_combined:
    case ControlWorkerOperation::card_presence:
    case ControlWorkerOperation::card_connect:
    case ControlWorkerOperation::card_reconnect:
    case ControlWorkerOperation::card_disconnect:
    case ControlWorkerOperation::card_reset:
    case ControlWorkerOperation::card_transmit:
    case ControlWorkerOperation::card_begin_transaction:
    case ControlWorkerOperation::card_end_transaction:
    case ControlWorkerOperation::card_release_connection:
    case ControlWorkerOperation::card_shutdown:
        return lane == ControlWorkerLane::card;
    }
    return false;
}

#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
ControlWorkerStartupOptions* test_startup_options = nullptr;
#endif

#if defined(_WIN32)
bool ensure_winsock() noexcept
{
    static std::atomic<int> state{0};  // 0=init, 1=ready, 2=failed
    int current = state.load(std::memory_order_acquire);
    if (current == 1) {
        return true;
    }
    if (current == 2) {
        return false;
    }
    int expected = 0;
    if (state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            state.store(1, std::memory_order_release);
            return true;
        }
        state.store(2, std::memory_order_release);
        return false;
    }
    while ((current = state.load(std::memory_order_acquire)) == 0) {
        ::Sleep(0);
    }
    return current == 1;
}
#endif

#if !defined(_WIN32)

class WakePipe final {
public:
    WakePipe() noexcept = default;
    ~WakePipe() noexcept { close(); }

    Result<void> open(const char* /* runtime_directory */) noexcept
    {
        int descriptors[2] = {-1, -1};
        while (::pipe(descriptors) != 0) {
            if (errno == EINTR) continue;
            return Result<void>::failure(Error::INTERNAL);
        }
        if (!set_flag(descriptors[0], F_GETFL, F_SETFL, O_NONBLOCK) ||
            !set_flag(descriptors[1], F_GETFL, F_SETFL, O_NONBLOCK) ||
            !set_flag(descriptors[0], F_GETFD, F_SETFD, FD_CLOEXEC) ||
            !set_flag(descriptors[1], F_GETFD, F_SETFD, FD_CLOEXEC)) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return Result<void>::failure(Error::INTERNAL);
        }
        read_fd_ = descriptors[0];
        write_fd_ = descriptors[1];
        return Result<void>::success();
    }

    void close() noexcept
    {
        if (read_fd_ >= 0) ::close(read_fd_);
        if (write_fd_ >= 0) ::close(write_fd_);
        read_fd_ = -1;
        write_fd_ = -1;
    }

    void signal(std::atomic<Error>& error) noexcept
    {
        const std::uint8_t byte = 1U;
        while (true) {
            const ssize_t written = ::write(write_fd_, &byte, sizeof(byte));
            if (written == static_cast<ssize_t>(sizeof(byte))) return;
            if (written < 0 && errno == EINTR) continue;
            if (written < 0 && errno == EAGAIN) return;
            error.store(Error::INTERNAL);
            return;
        }
    }

    void drain() noexcept
    {
        std::array<std::uint8_t, 64U> bytes{};
        while (true) {
            const ssize_t read_count = ::read(read_fd_, bytes.data(), bytes.size());
            if (read_count > 0) continue;
            if (read_count < 0 && errno == EINTR) continue;
            return;
        }
    }

    WakeHandle read_fd() const noexcept { return read_fd_; }

private:
    static bool set_flag(int descriptor, int get_command, int set_command,
                         int flag) noexcept
    {
        int current = 0;
        while ((current = ::fcntl(descriptor, get_command)) < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        while (::fcntl(descriptor, set_command, current | flag) < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        return true;
    }

    int read_fd_ = -1;
    int write_fd_ = -1;
};

#else

bool set_socket_nonblocking(SOCKET handle) noexcept
{
    u_long enabled = 1UL;
    return ::ioctlsocket(handle, FIONBIO, &enabled) == 0;
}

bool clear_socket_inherit(SOCKET handle) noexcept
{
    return ::SetHandleInformation(reinterpret_cast<HANDLE>(handle),
                                  HANDLE_FLAG_INHERIT, 0) != 0;
}

bool random_hex_token(wchar_t* out, std::size_t capacity) noexcept
{
    std::array<std::uint8_t, 8U> bytes{};
    if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return false;
    }
    if (capacity < bytes.size() * 2U + 1U) {
        return false;
    }
    static const wchar_t digits[] = L"0123456789abcdef";
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        out[index * 2U] = digits[bytes[index] >> 4U];
        out[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    out[bytes.size() * 2U] = L'\0';
    return true;
}

bool path_to_utf8(const wchar_t* path, char* out, std::size_t capacity) noexcept
{
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                             out, static_cast<int>(capacity),
                                             nullptr, nullptr);
    return length > 0 && static_cast<std::size_t>(length) <= capacity;
}

struct PathIdentity final {
    std::uint64_t device = 0U;
    std::uint64_t index = 0U;
    bool valid = false;
};

bool path_identity(const wchar_t* path, DWORD flags, PathIdentity& output) noexcept
{
    const HANDLE file = ::CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(file, &info);
    (void)::CloseHandle(file);
    if (ok == 0) {
        return false;
    }
    output.device = info.dwVolumeSerialNumber;
    output.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
                   static_cast<std::uint64_t>(info.nFileIndexLow);
    output.valid = true;
    return true;
}

bool same_identity(const PathIdentity& left, const PathIdentity& right) noexcept
{
    return left.valid && right.valid && left.device == right.device &&
           left.index == right.index;
}

// Windows worker wake is a same-user AF_UNIX SOCK_STREAM pair created inside a
// freshly-created random directory owned by the current user with a protected
// current-user-only DACL. No preexisting path is reused or blindly deleted, no
// network socket is involved, and drain is bounded.
class WakePipe final {
public:
    WakePipe() noexcept = default;
    ~WakePipe() noexcept { close(); }

    Result<void> open(const char* runtime_directory) noexcept
    {
        if (!ensure_winsock()) {
            return Result<void>::failure(Error::INTERNAL);
        }
        std::array<wchar_t, MAX_PATH> base{};
        if (runtime_directory == nullptr) {
            const DWORD base_length =
                ::GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(),
                                          static_cast<DWORD>(base.size()));
            if (base_length == 0U || base_length >= base.size()) {
                return Result<void>::failure(Error::INTERNAL);
            }
        } else {
            // Reject empty, overlong or malformed UTF-8 rather than truncate
            // it or fall back to a different runtime namespace.
            constexpr std::size_t max_utf8 = MAX_PATH * 4U;
            std::size_t length = 0U;
            while (length < max_utf8 && runtime_directory[length] != '\0') ++length;
            if (length == 0U || length == max_utf8 ||
                ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      runtime_directory, static_cast<int>(length),
                                      base.data(), static_cast<int>(base.size() - 1U)) <= 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
        }
        // Require a drive-absolute local path; never a relative or truncated
        // one that could resolve elsewhere.
        const bool absolute = ((base[0] >= L'A' && base[0] <= L'Z') ||
                               (base[0] >= L'a' && base[0] <= L'z')) &&
                              base[1] == L':' && base[2] == L'\\';
        if (!absolute) {
            return Result<void>::failure(Error::INTERNAL);
        }
        // The parent must itself satisfy the same-user runtime policy; an
        // untrusted user with write/delete-child rights could otherwise
        // replace our private child directory.
        if (!windows_security::verify_runtime_parent(base.data())) {
            return Result<void>::failure(Error::INTERNAL);
        }
        if (!create_private_directory(base.data())) {
            return cleanup(Error::INTERNAL);
        }
        (void)path_identity(directory_.data(), FILE_FLAG_BACKUP_SEMANTICS,
                            directory_identity_);
        if (std::swprintf(endpoint_.data(), endpoint_.size(), L"%s\\w.sock",
                          directory_.data()) <= 0) {
            return cleanup(Error::INTERNAL);
        }
        SOCKADDR_UN address{};
        address.sun_family = AF_UNIX;
        if (!path_to_utf8(endpoint_.data(), address.sun_path,
                          sizeof(address.sun_path))) {
            return cleanup(Error::INTERNAL);
        }

        const SOCKET listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener == INVALID_SOCKET) {
            return cleanup(Error::INTERNAL);
        }
        if (!clear_socket_inherit(listener) ||
            ::bind(listener, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0 ||
            ::listen(listener, 1) != 0) {
            ::closesocket(listener);
            return cleanup(Error::INTERNAL);
        }
        (void)path_identity(endpoint_.data(), FILE_FLAG_OPEN_REPARSE_POINT,
                            endpoint_identity_);

        // Nonblocking connect with a bounded poll; a live local listener
        // completes immediately, but never block indefinitely.
        SOCKET writer = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (writer == INVALID_SOCKET || !clear_socket_inherit(writer) ||
            !set_socket_nonblocking(writer)) {
            if (writer != INVALID_SOCKET) {
                ::closesocket(writer);
            }
            ::closesocket(listener);
            return cleanup(Error::INTERNAL);
        }
        int connect_result = ::connect(writer,
                                       reinterpret_cast<const sockaddr*>(&address),
                                       sizeof(address));
        if (connect_result != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
            ::closesocket(writer);
            ::closesocket(listener);
            return cleanup(Error::INTERNAL);
        }
        if (connect_result != 0) {
            WSAPOLLFD descriptor{};
            descriptor.fd = writer;
            descriptor.events = POLLWRNORM;
            const int polled = ::WSAPoll(&descriptor, 1UL, 2000);
            if (polled <= 0) {
                ::closesocket(writer);
                ::closesocket(listener);
                return cleanup(Error::INTERNAL);
            }
            int socket_error = 0;
            int socket_error_size = static_cast<int>(sizeof(socket_error));
            if (::getsockopt(writer, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&socket_error),
                             &socket_error_size) != 0 ||
                socket_error != 0) {
                ::closesocket(writer);
                ::closesocket(listener);
                return cleanup(Error::INTERNAL);
            }
        }
        // Bounded accept: the connection is already queued, but never block
        // indefinitely on a hostile listener state.
        if (!set_socket_nonblocking(listener)) {
            ::closesocket(writer);
            ::closesocket(listener);
            return cleanup(Error::INTERNAL);
        }
        SOCKET reader = INVALID_SOCKET;
        for (int attempt = 0; attempt < 200; ++attempt) {
            reader = ::accept(listener, nullptr, nullptr);
            if (reader != INVALID_SOCKET) {
                break;
            }
            if (::WSAGetLastError() != WSAEWOULDBLOCK) {
                break;
            }
            WSAPOLLFD descriptor{};
            descriptor.fd = listener;
            descriptor.events = POLLRDNORM;
            (void)::WSAPoll(&descriptor, 1UL, 10);
        }
        ::closesocket(listener);
        // Remove the endpoint before exposing the pair; identity-checked so we
        // never delete a replacement.
        remove_endpoint();
        if (reader == INVALID_SOCKET || !clear_socket_inherit(reader) ||
            !set_socket_nonblocking(reader) || !set_socket_nonblocking(writer)) {
            if (reader != INVALID_SOCKET) {
                ::closesocket(reader);
            }
            ::closesocket(writer);
            return cleanup(Error::INTERNAL);
        }
        read_socket_ = reader;
        write_socket_ = writer;
        return Result<void>::success();
    }

    void close() noexcept
    {
        if (read_socket_ != INVALID_SOCKET) ::closesocket(read_socket_);
        if (write_socket_ != INVALID_SOCKET) ::closesocket(write_socket_);
        read_socket_ = INVALID_SOCKET;
        write_socket_ = INVALID_SOCKET;
        remove_endpoint();
        if (created_directory_ && directory_[0] != L'\0') {
            PathIdentity current{};
            // Only remove the exact directory we created; a missing saved
            // identity fails open as a safe orphan rather than a blind delete.
            if (directory_identity_.valid &&
                path_identity(directory_.data(), FILE_FLAG_BACKUP_SEMANTICS,
                              current) &&
                same_identity(directory_identity_, current)) {
                (void)::RemoveDirectoryW(directory_.data());
            }
            created_directory_ = false;
        }
    }

    void signal(std::atomic<Error>& error) noexcept
    {
        const char byte = 1;
        if (::send(write_socket_, &byte, 1, 0) == 1) {
            return;
        }
        if (::WSAGetLastError() == WSAEWOULDBLOCK) {
            return;
        }
        error.store(Error::INTERNAL);
    }

    void drain() noexcept
    {
        std::array<char, 512U> bytes{};
        for (int iteration = 0; iteration < 64; ++iteration) {
            const int read =
                ::recv(read_socket_, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (read <= 0) {
                return;
            }
        }
    }

    WakeHandle read_fd() const noexcept
    {
        return static_cast<WakeHandle>(read_socket_);
    }

private:
    bool create_private_directory(const wchar_t* base) noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::array<wchar_t, 32U> token{};
            if (!random_hex_token(token.data(), token.size())) {
                return false;
            }
            if (std::swprintf(directory_.data(), directory_.size(), L"%s\\px4-wake-%s",
                              base, token.data()) <= 0) {
                directory_[0] = L'\0';
                return false;
            }
            SECURITY_ATTRIBUTES attributes{};
            SECURITY_DESCRIPTOR descriptor{};
            PACL acl = nullptr;
            windows_security::SidBuffer sid_storage{};
            if (!windows_security::build_current_user_security(attributes,
                                                               descriptor, acl,
                                                               sid_storage)) {
                return false;
            }
            const BOOL made = ::CreateDirectoryW(directory_.data(), &attributes);
            ::LocalFree(acl);
            if (made != 0) {
                created_directory_ = true;
                return true;
            }
            if (::GetLastError() != ERROR_ALREADY_EXISTS) {
                directory_[0] = L'\0';
                return false;
            }
        }
        return false;
    }

    void remove_endpoint() noexcept
    {
        if (endpoint_[0] == L'\0') {
            return;
        }
        PathIdentity current{};
        if (endpoint_identity_.valid &&
            path_identity(endpoint_.data(), FILE_FLAG_OPEN_REPARSE_POINT, current) &&
            same_identity(endpoint_identity_, current)) {
            (void)::DeleteFileW(endpoint_.data());
        }
        endpoint_[0] = L'\0';
        endpoint_identity_ = PathIdentity{};
    }

    Result<void> cleanup(Error error) noexcept
    {
        close();
        return Result<void>::failure(error);
    }

    SOCKET read_socket_ = INVALID_SOCKET;
    SOCKET write_socket_ = INVALID_SOCKET;
    std::array<wchar_t, MAX_PATH> directory_{};
    std::array<wchar_t, MAX_PATH> endpoint_{};
    bool created_directory_ = false;
    PathIdentity directory_identity_{};
    PathIdentity endpoint_identity_{};
};

#endif  // _WIN32

}  // namespace

struct ControlWorkerLanes::Impl final {
    struct Lane final {
        Impl& owner;
        ControlWorkerLane lane;
        pthread_t thread{};
        pthread_mutex_t mutex{};
        pthread_cond_t task_ready{};
        pthread_cond_t completion_room{};
        bool mutex_initialized = false;
        bool task_ready_initialized = false;
        bool completion_room_initialized = false;
        bool started = false;
        bool joined = false;
        bool stopping = false;
        std::array<ControlWorkerTask, kControlWorkerQueueCapacity> tasks{};
        std::size_t task_head = 0U;
        std::size_t task_count = 0U;
        std::array<ControlWorkerCompletion, kControlWorkerQueueCapacity> completions{};
        std::size_t completion_head = 0U;
        std::size_t completion_count = 0U;
        std::atomic<std::size_t> active_tasks{0U};

        Lane(Impl& owner_value, ControlWorkerLane lane_value) noexcept
            : owner(owner_value), lane(lane_value)
        {
        }

        static void* run(void* context) noexcept
        {
            static_cast<Lane*>(context)->run_loop();
            return nullptr;
        }

        Result<void> initialize() noexcept
        {
            if (pthread_mutex_init(&mutex, nullptr) != 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
            mutex_initialized = true;
            if (pthread_cond_init(&task_ready, nullptr) != 0) {
                pthread_mutex_destroy(&mutex);
                mutex_initialized = false;
                return Result<void>::failure(Error::INTERNAL);
            }
            task_ready_initialized = true;
            if (pthread_cond_init(&completion_room, nullptr) != 0) {
                pthread_cond_destroy(&task_ready);
                task_ready_initialized = false;
                pthread_mutex_destroy(&mutex);
                mutex_initialized = false;
                return Result<void>::failure(Error::INTERNAL);
            }
            completion_room_initialized = true;
            owner.live_threads.fetch_add(1U);
            if (pthread_create(&thread, nullptr, &Lane::run, this) != 0) {
                owner.live_threads.fetch_sub(1U);
                pthread_cond_destroy(&completion_room);
                completion_room_initialized = false;
                pthread_cond_destroy(&task_ready);
                task_ready_initialized = false;
                pthread_mutex_destroy(&mutex);
                mutex_initialized = false;
                return Result<void>::failure(Error::INTERNAL);
            }
            started = true;
            return Result<void>::success();
        }

        void destroy_sync() noexcept
        {
            if (completion_room_initialized) pthread_cond_destroy(&completion_room);
            if (task_ready_initialized) pthread_cond_destroy(&task_ready);
            if (mutex_initialized) pthread_mutex_destroy(&mutex);
            completion_room_initialized = false;
            task_ready_initialized = false;
            mutex_initialized = false;
        }

        void run_loop() noexcept
        {
            while (true) {
                ControlWorkerTask task{};
                if (!pop_task(task)) break;
                ControlWorkerCompletion completion = owner.execute(task);
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
                if (task.operation == ControlWorkerOperation::tuner_authority_job &&
                    task.authority_job.kind ==
                        dtv::userland::detail::AuthorityJobKind::acquire &&
                    owner.mismatch_next_authority_acquire_receiver.exchange(false)) {
                    ++completion.authority_completion.receiver;
                }
#endif
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
                if ((task.operation == ControlWorkerOperation::tuner_attach_stream ||
                     task.operation == ControlWorkerOperation::tuner_prepare_attach_v1) &&
                    owner.before_tuner_attach_completion != nullptr) {
                    owner.before_tuner_attach_completion(
                        owner.before_tuner_attach_completion_context);
                }
#endif
                (void)push_completion(completion);
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
                if (task.operation == ControlWorkerOperation::tuner_authority_job &&
                    owner.duplicate_next_authority_completion.exchange(false)) {
                    (void)push_completion(completion);
                }
                if (task.operation == ControlWorkerOperation::tuner_authority_job &&
                    owner.fatal_after_authority_lane.load() == static_cast<int>(lane)) {
                    owner.worker_error.store(Error::INTERNAL);
                    owner.fatal_after_authority_lane.store(-1);
                }
#endif
                active_tasks.fetch_sub(1U);
            }
            owner.live_threads.fetch_sub(1U);
        }

        bool pop_task(ControlWorkerTask& task) noexcept
        {
            if (pthread_mutex_lock(&mutex) != 0) {
                owner.worker_error.store(Error::INTERNAL);
                return false;
            }
            while (task_count == 0U && !stopping) {
                if (pthread_cond_wait(&task_ready, &mutex) != 0) {
                    owner.worker_error.store(Error::INTERNAL);
                    (void)pthread_mutex_unlock(&mutex);
                    return false;
                }
            }
            if (task_count == 0U && stopping) {
                (void)pthread_mutex_unlock(&mutex);
                return false;
            }
            task = tasks[task_head];
            task_head = (task_head + 1U) % tasks.size();
            --task_count;
            // Dequeue and active transition are one mutex-protected state
            // change, so idle() cannot observe a taken-but-not-active task.
            active_tasks.fetch_add(1U);
            (void)pthread_mutex_unlock(&mutex);
            return true;
        }

        bool push_completion(const ControlWorkerCompletion& completion) noexcept
        {
            if (pthread_mutex_lock(&mutex) != 0) {
                owner.worker_error.store(Error::INTERNAL);
                return false;
            }
            while (completion_count == completions.size()) {
                if (pthread_cond_wait(&completion_room, &mutex) != 0) {
                    owner.worker_error.store(Error::INTERNAL);
                    (void)pthread_mutex_unlock(&mutex);
                    return false;
                }
            }
            const std::size_t position =
                (completion_head + completion_count) % completions.size();
            completions[position] = completion;
            ++completion_count;
            (void)pthread_mutex_unlock(&mutex);
            owner.wake.signal(owner.worker_error);
            return true;
        }

        Result<void> submit(const ControlWorkerTask& task) noexcept
        {
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
            if (owner.fail_tuner_attach_submit &&
                (task.operation == ControlWorkerOperation::tuner_attach_stream ||
                 task.operation == ControlWorkerOperation::tuner_prepare_attach_v1)) {
                return Result<void>::failure(Error::BUSY);
            }
            if (task.operation == ControlWorkerOperation::tuner_authority_job &&
                task.kind == ControlWorkerTaskKind::authority_cleanup &&
                owner.reject_next_authority_cleanup_submit.exchange(false)) {
                return Result<void>::failure(Error::BUSY);
            }
            if (task.authority_stream &&
                (task.operation == ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1 ||
                 task.operation == ControlWorkerOperation::tuner_run_stream_cleanup_v1) &&
                owner.reject_next_authority_stream_cleanup_submit.exchange(false)) {
                return Result<void>::failure(Error::BUSY);
            }
#endif
            if (pthread_mutex_lock(&mutex) != 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
            if (stopping) {
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(Error::NOT_READY);
            }
            if (task_count == tasks.size()) {
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(Error::BUSY);
            }
            const std::size_t position =
                (task_head + task_count) % tasks.size();
            tasks[position] = task;
            ++task_count;
            if (pthread_cond_signal(&task_ready) != 0) {
                --task_count;
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(Error::INTERNAL);
            }
            (void)pthread_mutex_unlock(&mutex);
            return Result<void>::success();
        }

        // R2 bounded TUNE admission and publication.  The lane mutex is held
        // across the capacity check, the callback-free registry try-lock
        // admission, the fixed-size task copy and the signal, so normal
        // execution/stop cannot interleave before publication.
        Result<void> submit_tune(const ControlWorkerTask& task) noexcept
        {
            if (pthread_mutex_lock(&mutex) != 0) {
                return Result<void>::failure(Error::INTERNAL);
            }
            if (stopping) {
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(Error::NOT_READY);
            }
            if (task_count == tasks.size()) {
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(Error::BUSY);
            }
            // The prepared outcome was computed before this lock; admission
            // only consumes it (no planner or profile query here).
            const auto admitted = task.tune_prepared
                ? owner.tuner_service.try_admit_prepared_tune(
                      task.client_id, task.prepared_tune, task.accepted_at_ms)
                : owner.tuner_service.try_admit_tune(
                      task.client_id, task.tune, task.accepted_at_ms);
            if (!admitted) {
                (void)pthread_mutex_unlock(&mutex);
                return Result<void>::failure(admitted.error());
            }
            ControlWorkerTask entry = task;
            entry.tune_admitted = true;
            entry.admitted_tune = admitted.value();
            const std::size_t position =
                (task_head + task_count) % tasks.size();
            tasks[position] = entry;
            ++task_count;
            bool signal_failed = pthread_cond_signal(&task_ready) != 0;
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
            if (owner.fail_tune_signal) signal_failed = true;
#endif
            if (signal_failed) {
                --task_count;
                (void)pthread_mutex_unlock(&mutex);
                // The admitted ticket was accepted but never published: abort
                // it exactly once after unlock with an internal error.  Its
                // advanced revision and invalidated current state stay.
                (void)owner.tuner_service.abort_unstarted_tune(admitted.value(),
                                                               Error::INTERNAL);
                return Result<void>::failure(Error::INTERNAL);
            }
            (void)pthread_mutex_unlock(&mutex);
            return Result<void>::success();
        }

        bool try_pop(ControlWorkerCompletion& completion) noexcept
        {
            if (pthread_mutex_lock(&mutex) != 0) {
                owner.worker_error.store(Error::INTERNAL);
                return false;
            }
            if (completion_count == 0U) {
                (void)pthread_mutex_unlock(&mutex);
                return false;
            }
            completion = completions[completion_head];
            completion_head = (completion_head + 1U) % completions.size();
            --completion_count;
            (void)pthread_cond_signal(&completion_room);
            (void)pthread_mutex_unlock(&mutex);
            return true;
        }

        std::size_t pending_completions() const noexcept
        {
            if (pthread_mutex_lock(const_cast<pthread_mutex_t*>(&mutex)) != 0) {
                return 0U;
            }
            const std::size_t result = completion_count;
            (void)pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&mutex));
            return result;
        }

        void stop() noexcept
        {
            if (pthread_mutex_lock(&mutex) != 0) {
                owner.worker_error.store(Error::INTERNAL);
                return;
            }
            stopping = true;
            (void)pthread_cond_broadcast(&task_ready);
            (void)pthread_cond_broadcast(&completion_room);
            (void)pthread_mutex_unlock(&mutex);
        }

        bool idle() const noexcept
        {
            if (pthread_mutex_lock(const_cast<pthread_mutex_t*>(&mutex)) != 0) {
                return false;
            }
            const bool result = task_count == 0U && completion_count == 0U &&
                                active_tasks.load() == 0U;
            (void)pthread_mutex_unlock(const_cast<pthread_mutex_t*>(&mutex));
            return result;
        }
    };

    CardService& card_service;
    TunerService& tuner_service;
    ControlExecutionLayout execution;
    WakePipe wake;
    std::array<std::unique_ptr<Lane>, 3U> lanes;
    std::atomic<std::size_t> live_threads{0U};
    std::atomic<Error> worker_error{Error::OK};
    bool joined = false;
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
    bool fail_tuner_attach_submit = false;
    bool fail_tune_signal = false;
    std::atomic<int> fatal_after_authority_lane{-1};
    std::atomic<bool> duplicate_next_authority_completion{false};
    std::atomic<bool> mismatch_next_authority_acquire_receiver{false};
    std::atomic<bool> reject_next_authority_cleanup_submit{false};
    std::atomic<bool> reject_next_authority_stream_cleanup_submit{false};
    void (*before_tuner_attach_completion)(void*) noexcept = nullptr;
    void* before_tuner_attach_completion_context = nullptr;
#endif

    Impl(CardService& card, TunerService& tuner, ControlExecutionLayout execution_layout
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
         , const ControlWorkerStartupOptions* options = nullptr
#endif
         ) noexcept
        : card_service(card), tuner_service(tuner), execution(execution_layout)
    {
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
        if (options != nullptr) {
            fail_tuner_attach_submit = options->fail_tuner_attach_submit;
            fail_tune_signal = options->fail_tune_signal;
            fatal_after_authority_lane.store(options->fatal_after_authority_lane);
            duplicate_next_authority_completion.store(
                options->duplicate_next_authority_completion);
            mismatch_next_authority_acquire_receiver.store(
                options->mismatch_next_authority_acquire_receiver);
            reject_next_authority_cleanup_submit.store(
                options->reject_next_authority_cleanup_submit);
            reject_next_authority_stream_cleanup_submit.store(
                options->reject_next_authority_stream_cleanup_submit);
            before_tuner_attach_completion =
                options->before_tuner_attach_completion;
            before_tuner_attach_completion_context =
                options->before_tuner_attach_completion_context;
        }
#endif
    }

    ControlWorkerCompletion execute(const ControlWorkerTask& task) noexcept
    {
        ControlWorkerCompletion completion{};
        completion.opcode = task.opcode;
        completion.kind = task.kind;
        completion.operation = task.operation;
        completion.client_id = task.client_id;
        completion.connection_id = task.connection_id;
        completion.request_id = task.request_id;
        completion.receiver = task.receiver;
        completion.lease_id = task.lease_id;
        completion.stream_ticket = task.stream_ticket;
        completion.data_slot = task.data_slot;
        completion.data_generation = task.data_generation;
        completion.authority_stream = task.authority_stream;
        if (task.operation == ControlWorkerOperation::tuner_authority_job) {
            completion.authority_key = task.authority_job.key;
            completion.authority_job_kind = task.authority_job.kind;
            completion.client_id = task.authority_job.key.client_id;
            completion.request_id = static_cast<std::uint32_t>(
                task.authority_job.key.request_id);
            completion.receiver = task.authority_job.receiver;
            if (task.authority_job.kind ==
                dtv::userland::detail::AuthorityJobKind::tune) {
                completion.receiver = task.authority_job.tune.receiver;
                completion.lease_id = task.authority_job.tune.lease_id;
            } else {
                completion.lease_id = task.authority_job.lease_id;
            }
        }
        completion.card_handle = task.card_handle;
        completion.status_scope = task.status_scope;
        Error operation_error = Error::OK;
        switch (task.operation) {
        case ControlWorkerOperation::tuner_authority_job: {
            completion.authority_completion = ControlWorkerLanes::execute_authority_job(
                tuner_service, task.authority_job);
            operation_error = completion.authority_completion.error;
            completion.lease_id = completion.authority_completion.lease_id;
            completion.receiver = completion.authority_completion.receiver;
            break;
        }
        case ControlWorkerOperation::tuner_acquire: {
            const auto result = tuner_service.acquire(task.client_id, task.receiver);
            if (!result) operation_error = result.error();
            else completion.tuner_acquire = result.value();
            break;
        }
        case ControlWorkerOperation::tuner_release: {
            const auto result = tuner_service.release(task.client_id, task.lease_id);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::tuner_tune: {
            if (task.tune_admitted) {
                const auto result =
                    tuner_service.execute_admitted_tune(task.admitted_tune);
                if (!result) operation_error = result.error();
                else {
                    completion.tune_response = TuneResponsePayload{
                        1U, std::numeric_limits<std::int32_t>::min()};
                    // The typed result reports what the engine committed for
                    // this exact ticket; it is never reconstructed from the
                    // request.
                    completion.tune_revision = task.admitted_tune.tune_revision;
                    const auto metadata = tuner_service.tune_metadata(task.receiver);
                    if (metadata && metadata.value().current_valid &&
                        metadata.value().current_revision ==
                            task.admitted_tune.tune_revision) {
                        completion.tune_metadata = metadata.value();
                        completion.tune_record_valid = true;
                    }
                }
            } else {
                const auto result = tuner_service.tune(task.client_id, task.tune);
                if (!result) operation_error = result.error();
                else completion.tune_response = result.value();
            }
            break;
        }
        case ControlWorkerOperation::tuner_start_stream: {
            const auto result = tuner_service.start_stream(
                task.client_id, task.lease_id);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::tuner_stop_stream: {
            const auto result = tuner_service.stop_stream(
                task.client_id, task.lease_id);
            if (!result) operation_error = result.error();
            else {
                completion.stream_final_snapshot = result.value();
                completion.stream_snapshot_valid = true;
            }
            break;
        }
        case ControlWorkerOperation::tuner_start_stream_v1: {
            const auto result = tuner_service.start_stream_v1(
                task.client_id, task.lease_id, task.tune_revision,
                task.now_ms, task.arm_budget_ms);
            if (!result) operation_error = result.error();
            else {
                completion.v1_start = result.value();
                completion.v1_start_valid = true;
                const std::uint64_t expires = completion.v1_start.expires_at_ms;
                const std::uint64_t now = task.now_ms;
                const std::uint64_t remaining = expires > now ? expires - now : 0U;
                completion.v1_expires_after_ms =
                    remaining > std::numeric_limits<std::uint32_t>::max()
                        ? std::numeric_limits<std::uint32_t>::max()
                        : static_cast<std::uint32_t>(remaining);
            }
            break;
        }
        case ControlWorkerOperation::tuner_stop_stream_v1: {
            const auto result = tuner_service.stop_stream_v1(
                task.client_id, task.lease_id, task.stream_id);
            if (!result) operation_error = result.error();
            else {
                completion.v1_stop = result.value();
                completion.v1_stop_valid = true;
            }
            break;
        }
        case ControlWorkerOperation::tuner_stop_stream_and_cleanup_v1: {
            const auto stopped = task.authority_stream
                ? tuner_service.authority_stop_stream_v1(
                    task.stream_ticket.client_id, task.lease_id,
                    task.stream_ticket.stream_id)
                : tuner_service.stop_stream_v1(
                task.stream_ticket.client_id, task.lease_id,
                task.stream_ticket.stream_id);
            if (!stopped) operation_error = stopped.error();
            else {
                const auto cleaned =
                    task.authority_stream
                        ? tuner_service.authority_run_stream_cleanup_v1(task.stream_ticket)
                        : tuner_service.run_stream_cleanup_v1(task.stream_ticket);
                if (!cleaned && cleaned.error() != Error::NOT_FOUND)
                    operation_error = cleaned.error();
                const auto final = task.authority_stream
                    ? tuner_service.authority_stream_last_terminal_v1(
                        task.stream_ticket.client_id, task.lease_id)
                    : tuner_service.stream_last_terminal_v1(
                        task.stream_ticket.client_id, task.lease_id);
                if (final) {
                    // Keep the exact STOP identity/revision; the cleanup
                    // observation adds the settled quiescence and error.
                    completion.v1_stop = stopped.value();
                    completion.v1_stop.quiesced = final.value().quiesced;
                    completion.v1_stop.cleanup_error = final.value().cleanup_error;
                    completion.v1_stop.terminal = final.value().terminal;
                    completion.v1_stop_valid = true;
                } else if (operation_error == Error::OK) {
                    operation_error = final.error();
                }
            }
            break;
        }
        case ControlWorkerOperation::tuner_prepare_attach_v1: {
            const auto result = task.authority_stream
                ? tuner_service.authority_prepare_attach_v1(task.stream_ticket)
                : tuner_service.prepare_attach_v1(task.stream_ticket);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::tuner_run_stream_cleanup_v1: {
            const auto result = task.authority_stream
                ? tuner_service.authority_run_stream_cleanup_v1(task.stream_ticket)
                : tuner_service.run_stream_cleanup_v1(task.stream_ticket);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::tuner_stats: {
            const auto result = tuner_service.stats(
                task.client_id, task.lease_id);
            if (!result) operation_error = result.error();
            else completion.stream_counters = result.value();
            break;
        }
        case ControlWorkerOperation::tuner_attach_stream: {
            const auto result = tuner_service.attach_stream(task.lease_id, task.nonce);
            if (!result) operation_error = result.error();
            else {
                completion.tuner_attachment = result.value();
                completion.tuner_attachment_valid = true;
            }
            break;
        }
        case ControlWorkerOperation::tuner_detach_stream: {
            const auto result = tuner_service.detach_stream(task.attachment);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::tuner_status: {
            const auto result = tuner_service.status();
            if (!result) operation_error = result.error();
            else {
                completion.tuner_status = result.value();
                completion.tuner_status_valid = true;
                if (tuner_service.supports_internal_card_observation())
                    completion.internal_card_inserted =
                        tuner_service.sample_internal_card_inserted();
            }
            break;
        }
        case ControlWorkerOperation::tuner_lnb_power: {
            const auto result = tuner_service.set_independent_lnb_power(task.lnb_enabled);
            if (!result) operation_error = result.error();
            else completion.lnb_enabled = task.lnb_enabled;
            break;
        }
        case ControlWorkerOperation::tuner_shutdown: {
            const auto result = tuner_service.shutdown();
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::card_status: {
            const auto result = card_service.status();
            if (!result) operation_error = result.error();
            else {
                completion.card_status = result.value();
                completion.card_status_valid = true;
            }
            break;
        }
        case ControlWorkerOperation::card_handle_status: {
            const auto result = card_service.handle_status(task.client_id, task.card_handle);
            if (!result) operation_error = result.error();
            else {
                completion.card_status = result.value();
                completion.card_status_valid = true;
            }
            break;
        }
        case ControlWorkerOperation::card_status_combined: {
            const auto card = card_service.status();
            if (!card) {
                operation_error = card.error();
                break;
            }
            completion.card_status = card.value();
            completion.card_status_valid = true;
            const auto tuner = tuner_service.status();
            if (!tuner) {
                operation_error = tuner.error();
                break;
            }
            completion.tuner_status = tuner.value();
            completion.tuner_status_valid = true;
            break;
        }
        case ControlWorkerOperation::card_presence: {
            const auto result = card_service.poll_presence();
            if (!result) operation_error = result.error();
            else completion.card_presence = result.value();
            break;
        }
        case ControlWorkerOperation::card_connect: {
            const auto result = card_service.connect(
                task.client_id, task.share_mode, task.card_generation_bound);
            if (!result) operation_error = result.error();
            else completion.card_connect = result.value();
            break;
        }
        case ControlWorkerOperation::card_reconnect: {
            const auto result = card_service.reconnect(
                task.client_id, task.card_handle, task.share_mode, task.disposition);
            if (!result) operation_error = result.error();
            else completion.atr = result.value();
            break;
        }
        case ControlWorkerOperation::card_disconnect: {
            const auto result = card_service.disconnect(
                task.client_id, task.card_handle, task.disposition);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::card_reset: {
            const auto result = card_service.reset(task.client_id, task.card_handle);
            if (!result) operation_error = result.error();
            else completion.atr = result.value();
            break;
        }
        case ControlWorkerOperation::card_transmit: {
            const auto result = card_service.transmit(
                task.client_id, task.card_handle,
                ByteView{task.apdu.data(), task.apdu_size},
                MutableByteView{completion.card_response.data(), completion.card_response.size()});
            if (!result) operation_error = result.error();
            else completion.card_response_size = result.value();
            break;
        }
        case ControlWorkerOperation::card_begin_transaction: {
            const auto result = card_service.begin_transaction(
                task.client_id, task.card_handle);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::card_end_transaction: {
            const auto result = card_service.end_transaction(
                task.client_id, task.card_handle, task.disposition);
            if (!result) operation_error = result.error();
            else {
                // END carries no ATR of its own.  Re-read the owned handle
                // afterwards so the typed CardSessionResult can report the
                // settled card honestly; a stale or uninitialized handle is
                // not an END failure.
                const auto status = card_service.handle_status(task.client_id,
                                                               task.card_handle);
                if (status) {
                    completion.card_status = status.value();
                    completion.card_status_valid = true;
                }
            }
            break;
        }
        case ControlWorkerOperation::card_release_connection: {
            const auto result = card_service.release_connection(task.client_id);
            if (!result) operation_error = result.error();
            break;
        }
        case ControlWorkerOperation::card_shutdown: {
            const auto result = card_service.shutdown();
            if (!result) operation_error = result.error();
            break;
        }
        }
        if (task.operation == ControlWorkerOperation::tuner_acquire ||
            task.operation == ControlWorkerOperation::tuner_release ||
            task.operation == ControlWorkerOperation::tuner_tune ||
            task.operation == ControlWorkerOperation::tuner_start_stream ||
            task.operation == ControlWorkerOperation::tuner_stop_stream ||
            task.operation == ControlWorkerOperation::tuner_start_stream_v1 ||
            task.operation == ControlWorkerOperation::tuner_stop_stream_v1 ||
            task.operation == ControlWorkerOperation::tuner_stats ||
            task.operation == ControlWorkerOperation::tuner_attach_stream ||
            task.operation == ControlWorkerOperation::tuner_detach_stream) {
            const auto status = tuner_service.status();
            if (status) {
                completion.tuner_status = status.value();
                completion.tuner_status_valid = true;
            } else if (operation_error == Error::OK) {
                operation_error = status.error();
            }
        }
        if (operation_lane_valid(ControlWorkerLane::card, task, execution)) {
            // Sample the reader generation on the lane for every card
            // operation, including a failed one, so the owning poll thread can
            // bind or invalidate handles without touching CardService.
            completion.card_generation = card_service.reader_generation();
        }
        completion.error = operation_error;
        return completion;
    }
};

ControlWorkerLanes::ControlWorkerLanes(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

Result<std::unique_ptr<ControlWorkerLanes>> ControlWorkerLanes::create(
    CardService& card_service, TunerService& tuner_service,
    const char* runtime_directory, ControlExecutionLayout execution) noexcept
{
#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
    if (test_startup_options != nullptr) {
        ControlWorkerStartupOptions* options = test_startup_options;
        test_startup_options = nullptr;
        return create_for_test(card_service, tuner_service, *options, runtime_directory,
                               execution);
    }
#endif
    auto impl = std::unique_ptr<Impl>(
        new Impl(card_service, tuner_service, execution));
    if (!impl->wake.open(runtime_directory)) {
        return Result<std::unique_ptr<ControlWorkerLanes>>::failure(Error::INTERNAL);
    }
    const std::array<ControlWorkerLane, 3U> lane_ids{
        ControlWorkerLane::tuner_dev1, ControlWorkerLane::tuner_dev2,
        ControlWorkerLane::card};
    for (std::size_t index = 0U; index < lane_ids.size(); ++index) {
        impl->lanes[index] = std::unique_ptr<Impl::Lane>(
            new Impl::Lane(*impl, lane_ids[index]));
        const auto initialized = impl->lanes[index]->initialize();
        if (!initialized) {
            for (std::size_t stop = 0U; stop <= index; ++stop) {
                if (impl->lanes[stop] != nullptr && impl->lanes[stop]->started) {
                    impl->lanes[stop]->stop();
                }
            }
            for (std::size_t join = 0U; join <= index; ++join) {
                if (impl->lanes[join] != nullptr && impl->lanes[join]->started) {
                    (void)pthread_join(impl->lanes[join]->thread, nullptr);
                    impl->lanes[join]->joined = true;
                }
                if (impl->lanes[join] != nullptr) impl->lanes[join]->destroy_sync();
            }
            return Result<std::unique_ptr<ControlWorkerLanes>>::failure(
                Error::INTERNAL);
        }
    }
    return Result<std::unique_ptr<ControlWorkerLanes>>::success(
        std::unique_ptr<ControlWorkerLanes>(new ControlWorkerLanes(std::move(impl))));
}

#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
void ControlWorkerLanes::set_test_startup_options(
    ControlWorkerStartupOptions* options) noexcept
{
    test_startup_options = options;
}

Result<std::unique_ptr<ControlWorkerLanes>> ControlWorkerLanes::create_for_test(
    CardService& card_service, TunerService& tuner_service,
    const ControlWorkerStartupOptions& options, const char* runtime_directory,
    ControlExecutionLayout execution) noexcept
{
    auto impl = std::unique_ptr<Impl>(
        new Impl(card_service, tuner_service, execution, &options));
    if (options.fail_wake_pipe || !impl->wake.open(runtime_directory)) {
        return Result<std::unique_ptr<ControlWorkerLanes>>::failure(Error::INTERNAL);
    }
    const std::array<ControlWorkerLane, 3U> lane_ids{
        ControlWorkerLane::tuner_dev1, ControlWorkerLane::tuner_dev2,
        ControlWorkerLane::card};
    for (std::size_t index = 0U; index < lane_ids.size(); ++index) {
        if (options.fail_lane == static_cast<int>(index)) {
            for (std::size_t stop = 0U; stop < index; ++stop) {
                if (impl->lanes[stop] != nullptr && impl->lanes[stop]->started) {
                    impl->lanes[stop]->stop();
                }
            }
            for (std::size_t join = 0U; join < index; ++join) {
                if (impl->lanes[join] != nullptr && impl->lanes[join]->started) {
                    (void)pthread_join(impl->lanes[join]->thread, nullptr);
                    impl->lanes[join]->joined = true;
                    impl->lanes[join]->destroy_sync();
                }
            }
            return Result<std::unique_ptr<ControlWorkerLanes>>::failure(Error::INTERNAL);
        }
        impl->lanes[index] = std::unique_ptr<Impl::Lane>(
            new Impl::Lane(*impl, lane_ids[index]));
        const auto initialized = impl->lanes[index]->initialize();
        if (!initialized) {
            for (std::size_t stop = 0U; stop <= index; ++stop) {
                if (impl->lanes[stop] != nullptr && impl->lanes[stop]->started) {
                    impl->lanes[stop]->stop();
                }
            }
            for (std::size_t join = 0U; join <= index; ++join) {
                if (impl->lanes[join] != nullptr && impl->lanes[join]->started) {
                    (void)pthread_join(impl->lanes[join]->thread, nullptr);
                    impl->lanes[join]->joined = true;
                }
                if (impl->lanes[join] != nullptr) impl->lanes[join]->destroy_sync();
            }
            return Result<std::unique_ptr<ControlWorkerLanes>>::failure(
                Error::INTERNAL);
        }
    }
    return Result<std::unique_ptr<ControlWorkerLanes>>::success(
        std::unique_ptr<ControlWorkerLanes>(new ControlWorkerLanes(std::move(impl))));
}
#endif

ControlWorkerLanes::~ControlWorkerLanes() noexcept
{
    if (!impl_) return;
    (void)stop_and_join();
    for (auto& lane : impl_->lanes) {
        if (lane != nullptr) lane->destroy_sync();
    }
}

Result<void> ControlWorkerLanes::submit(ControlWorkerLane lane,
                                        const ControlWorkerTask& task) noexcept
{
    if (!impl_) return Result<void>::failure(Error::NOT_READY);
    const std::size_t index = static_cast<std::size_t>(lane);
    if (index >= impl_->lanes.size() || impl_->lanes[index] == nullptr) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!operation_lane_valid(lane, task, impl_->execution)) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (task.operation == ControlWorkerOperation::tuner_authority_job) {
        const auto& job = task.authority_job;
        if (job.deferred) return Result<void>::failure(Error::INVALID_ARGUMENT);
        if (job.kind != dtv::userland::detail::AuthorityJobKind::tune &&
            job.kind != dtv::userland::detail::AuthorityJobKind::acquire &&
            job.kind != dtv::userland::detail::AuthorityJobKind::release &&
            job.kind != dtv::userland::detail::AuthorityJobKind::rollback_release &&
            job.kind != dtv::userland::detail::AuthorityJobKind::disconnect) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        const std::uint64_t job_client =
            job.kind == dtv::userland::detail::AuthorityJobKind::tune
                ? job.tune.client_id : job.client_id;
        const std::uint64_t job_lease =
            job.kind == dtv::userland::detail::AuthorityJobKind::tune
                ? job.tune.lease_id : job.lease_id;
        const std::uint8_t job_receiver =
            job.kind == dtv::userland::detail::AuthorityJobKind::tune
                ? job.tune.receiver : job.receiver;
        if (job.key.client_id < dtv::userland::detail::kAuthorityClientIdFirst ||
            job.key.client_id != job_client ||
            job.key.request_id > std::numeric_limits<std::uint32_t>::max() ||
            task.client_id != job.key.client_id ||
            task.request_id != static_cast<std::uint32_t>(job.key.request_id) ||
            task.lease_id != job_lease ||
            (job.kind != dtv::userland::detail::AuthorityJobKind::disconnect &&
             task.receiver != job_receiver)) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
    }
    // A real strict daemon routes TUNE through the bounded admission and
    // publication transaction.  Callers that did not opt into strict mode keep
    // the legacy direct queue submission.
    if (task.operation == ControlWorkerOperation::tuner_tune &&
        impl_->tuner_service.strict_admission_enabled()) {
        return impl_->lanes[index]->submit_tune(task);
    }
    return impl_->lanes[index]->submit(task);
}

dtv::userland::detail::AuthorityCompletion ControlWorkerLanes::execute_authority_job(
    TunerService& tuner_service,
    const dtv::userland::detail::AuthorityJob& job) noexcept
{
    return dtv::userland::detail::ControlAuthority::execute_job_value(
        tuner_service.engine_, job);
}

bool ControlWorkerLanes::try_pop(ControlWorkerLane lane,
                                 ControlWorkerCompletion& completion) noexcept
{
    if (!impl_) return false;
    const std::size_t index = static_cast<std::size_t>(lane);
    return index < impl_->lanes.size() && impl_->lanes[index] != nullptr &&
           impl_->lanes[index]->try_pop(completion);
}

Result<ControlWorkerLane> ControlWorkerLanes::authority_lane(
    std::uint8_t receiver, bool cleanup) const noexcept
{
    if (!impl_) return Result<ControlWorkerLane>::failure(Error::NOT_READY);
    if (cleanup) {
        return Result<ControlWorkerLane>::success(
            impl_->execution.tuner_shutdown_lane);
    }
    if (receiver >= impl_->execution.receiver_count ||
        receiver >= impl_->execution.receiver_lane.size())
        return Result<ControlWorkerLane>::failure(Error::INVALID_ARGUMENT);
    return Result<ControlWorkerLane>::success(
        impl_->execution.receiver_lane[receiver]);
}

#if defined(PX4_CONTROL_WORKERS_TEST_ACCESS)
std::size_t ControlWorkerLanes::pending_completions_for_test(
    ControlWorkerLane lane) const noexcept
{
    if (!impl_) return 0U;
    const std::size_t index = static_cast<std::size_t>(lane);
    return index < impl_->lanes.size() && impl_->lanes[index] != nullptr ?
               impl_->lanes[index]->pending_completions() : 0U;
}
#endif

WakeHandle ControlWorkerLanes::wake_fd() const noexcept
{
    return impl_ ? impl_->wake.read_fd() : kInvalidWakeHandle;
}

void ControlWorkerLanes::drain_wake() noexcept
{
    if (impl_) impl_->wake.drain();
}

Error ControlWorkerLanes::error() const noexcept
{
    if (!impl_) return Error::NOT_READY;
    const Error wake_error = impl_->worker_error.load();
    return wake_error == Error::OK ? Error::OK : wake_error;
}

bool ControlWorkerLanes::idle() const noexcept
{
    if (!impl_) return true;
    for (const auto& lane : impl_->lanes) {
        if (lane != nullptr && !lane->idle()) return false;
    }
    return true;
}

std::size_t ControlWorkerLanes::live_thread_count() const noexcept
{
    return impl_ ? impl_->live_threads.load() : 0U;
}

Result<void> ControlWorkerLanes::request_stop() noexcept
{
    if (!impl_) return Result<void>::success();
    for (auto& lane : impl_->lanes) {
        if (lane != nullptr && lane->started) lane->stop();
    }
    const Error error = impl_->worker_error.load();
    return error == Error::OK ? Result<void>::success()
                              : Result<void>::failure(error);
}

Result<void> ControlWorkerLanes::stop_and_join() noexcept
{
    if (!impl_) return Result<void>::success();
    Error first = Error::OK;
    const auto stopped = request_stop();
    if (!stopped) first = stopped.error();
    while (impl_->live_threads.load() != 0U) {
        ControlWorkerCompletion completion{};
        bool drained = false;
        for (const auto& lane : impl_->lanes) {
            if (lane != nullptr) {
                while (lane->try_pop(completion)) drained = true;
            }
        }
        if (!drained) {
#if defined(_WIN32)
            WSAPOLLFD descriptor{};
            descriptor.fd = static_cast<SOCKET>(impl_->wake.read_fd());
            descriptor.events = POLLRDNORM;
            (void)::WSAPoll(&descriptor, 1UL, 10);
#else
            pollfd descriptor{impl_->wake.read_fd(), POLLIN, 0};
            (void)::poll(&descriptor, 1U, 10);
#endif
            impl_->wake.drain();
        }
    }
    ControlWorkerCompletion completion{};
    for (const auto& lane : impl_->lanes) {
        if (lane != nullptr) {
            while (lane->try_pop(completion)) {}
        }
    }
    const auto joined = join();
    if (!joined && first == Error::OK) first = joined.error();
    return first == Error::OK ? Result<void>::success() : Result<void>::failure(first);
}

Result<void> ControlWorkerLanes::join() noexcept
{
    if (!impl_ || impl_->joined) return Result<void>::success();
    Error first = impl_->worker_error.load();
    for (auto& lane : impl_->lanes) {
        if (lane == nullptr || !lane->started || lane->joined) continue;
        if (pthread_join(lane->thread, nullptr) != 0 && first == Error::OK) {
            first = Error::INTERNAL;
        }
        lane->joined = true;
    }
    impl_->joined = true;
    return first == Error::OK ? Result<void>::success() : Result<void>::failure(first);
}

}  // namespace px4::userland::ipc::posix
