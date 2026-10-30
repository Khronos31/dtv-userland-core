// SPDX-License-Identifier: GPL-2.0-only
//
// ASICEN SerialEndpointLease::acquire_identity() semantics (e2a86ca1) with
// the Core host_listener_context safe-directory helper reused. The product
// keeps the acquisition point, USB identity and lock prefix policy.
#include "dtv/userland/host_listener_context/identity_lease.h"

#include "detail/directory_security.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace dtv::userland::host_listener_context {
namespace {

namespace directory_security = detail::directory_security;

constexpr std::size_t kMaxStaleInodeRetries = 8U;

class FileDescriptor final {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    ~FileDescriptor() noexcept { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : descriptor_(other.release()) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const noexcept { return descriptor_; }
    int release() noexcept
    {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }
    void reset(int descriptor = -1) noexcept
    {
        if (descriptor_ >= 0) (void)::close(descriptor_);
        descriptor_ = descriptor;
    }

private:
    int descriptor_ = -1;
};

bool valid_component(std::string_view value) noexcept
{
    if (value.empty() || value == "." || value == "..") return false;
    for (const char character : value) {
        const bool accepted =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.';
        if (!accepted) return false;
    }
    return true;
}

bool build_lock_filename(std::string_view prefix, std::string_view identity,
                         std::array<char, 64U>& filename) noexcept
{
    const int written = std::snprintf(filename.data(), filename.size(),
                                      "%.*s%.*s.lock",
                                      static_cast<int>(prefix.size()), prefix.data(),
                                      static_cast<int>(identity.size()), identity.data());
    return written > 0 && static_cast<std::size_t>(written) < filename.size();
}

Error directory_error(directory_security::Status status) noexcept
{
    switch (status) {
    case directory_security::Status::missing:
        return Error::NOT_FOUND;
    case directory_security::Status::lookup:
        return Error::INTERNAL;
    case directory_security::Status::type:
    case directory_security::Status::owner:
    case directory_security::Status::mode:
    case directory_security::Status::security:
        return Error::INVALID_ARGUMENT;
    case directory_security::Status::ok:
        break;
    }
    return Error::INTERNAL;
}

Error file_error(int error) noexcept
{
    if (error == ENOENT) return Error::NOT_FOUND;
    if (error == EACCES || error == EPERM) return Error::UNSUPPORTED;
    if (error == EADDRINUSE || error == EBUSY || error == EAGAIN ||
        error == EWOULDBLOCK) return Error::BUSY;
    if (error == EINVAL || error == ENAMETOOLONG || error == ELOOP ||
        error == EISDIR) return Error::INVALID_ARGUMENT;
    return Error::INTERNAL;
}

bool same_identity(const struct stat& left, const struct stat& right) noexcept
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool resolve_runtime(std::string_view configured, std::string& runtime) noexcept
{
    if (configured.empty()) {
        const char* environment = std::getenv("XDG_RUNTIME_DIR");
        if (environment == nullptr || environment[0] == '\0') return false;
        runtime = environment;
    } else {
        runtime.assign(configured.data(), configured.size());
    }
    if (runtime.empty() || runtime.front() != '/') return false;
    while (runtime.size() > 1U && runtime.back() == '/') runtime.pop_back();
    return true;
}

}  // namespace

IdentityLease::~IdentityLease() noexcept
{
    close();
}

IdentityLease::IdentityLease(IdentityLease&& other) noexcept
    : lock_handle_(other.lock_handle_), directory_handle_(other.directory_handle_),
      filename_(other.filename_)
{
    other.lock_handle_ = detail::kInvalidIdentityLeaseHandle;
    other.directory_handle_ = detail::kInvalidIdentityLeaseHandle;
}

IdentityLease& IdentityLease::operator=(IdentityLease&& other) noexcept
{
    if (this != &other) {
        close();
        lock_handle_ = other.lock_handle_;
        directory_handle_ = other.directory_handle_;
        filename_ = other.filename_;
        other.lock_handle_ = detail::kInvalidIdentityLeaseHandle;
        other.directory_handle_ = detail::kInvalidIdentityLeaseHandle;
    }
    return *this;
}

Result<IdentityLease> IdentityLease::acquire(
    const IdentityLeaseConfig& config, std::string_view identity) noexcept
{
    return acquire(config, identity, nullptr);
}

Result<IdentityLease> IdentityLease::acquire(
    const IdentityLeaseConfig& config, std::string_view identity,
    IdentityLeaseDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr) *diagnostic = IdentityLeaseDiagnostic{};
    if (!valid_component(config.lock_prefix)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::lock_prefix;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    if (!valid_component(identity)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }
    std::array<char, 64U> filename{};
    if (!build_lock_filename(config.lock_prefix, identity, filename)) {
        if (diagnostic != nullptr) diagnostic->reason = IdentityLeaseDiagnosticReason::identity;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    std::string runtime;
    if (!resolve_runtime(config.runtime_directory, runtime)) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory;
        return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
    }

    const directory_security::Policy policy{
        config.access, true,
        config.access == host_runtime::EndpointAccess::shared_group,
        config.require_effective_user_owner};
    const auto check = directory_security::inspect(
        runtime, policy, ::geteuid(), ::getegid());
    if (check.status != directory_security::Status::ok) {
        if (diagnostic != nullptr) {
            switch (check.status) {
            case directory_security::Status::missing:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_missing;
                break;
            case directory_security::Status::lookup:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_lookup;
                break;
            case directory_security::Status::type:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_type;
                break;
            case directory_security::Status::owner:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_owner;
                break;
            case directory_security::Status::mode:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_mode;
                break;
            case directory_security::Status::security:
                diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_security;
                break;
            case directory_security::Status::ok:
                break;
            }
        }
        return Result<IdentityLease>::failure(directory_error(check.status));
    }

    FileDescriptor directory(::open(runtime.c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_lookup;
        return Result<IdentityLease>::failure(file_error(errno));
    }
    struct stat directory_status {};
    if (::fstat(directory.get(), &directory_status) != 0 ||
        !directory_security::same_identity(check.identity, directory_status)) {
        if (diagnostic != nullptr)
            diagnostic->reason = IdentityLeaseDiagnosticReason::runtime_directory_security;
        return Result<IdentityLease>::failure(Error::BUSY);
    }

    for (std::size_t attempt = 0U; attempt < kMaxStaleInodeRetries; ++attempt) {
        int descriptor = ::openat(directory.get(), filename.data(),
                                  O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                  0600);
        if (descriptor >= 0 && ::fchmod(descriptor, 0600) != 0) {
            const int error = errno;
            (void)::close(descriptor);
            return Result<IdentityLease>::failure(file_error(error));
        }
        if (descriptor < 0 && errno == EEXIST) {
            descriptor = ::openat(directory.get(), filename.data(),
                                  O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            if (descriptor < 0 && errno == ENOENT) continue;
        }
        if (descriptor < 0) {
            const int error = errno;
            if (error == ELOOP || error == EISDIR) {
                if (diagnostic != nullptr)
                    diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_type;
                return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
            }
            return Result<IdentityLease>::failure(file_error(error));
        }
        FileDescriptor lock(descriptor);

        struct stat lock_status {};
        if (::fstat(lock.get(), &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
            if (diagnostic != nullptr)
                diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_type;
            return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
        }
        if (lock_status.st_uid != ::geteuid()) {
            if (diagnostic != nullptr)
                diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_owner;
            return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
        }
        if ((lock_status.st_mode & 07777) != 0600) {
            if (diagnostic != nullptr)
                diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_mode;
            return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
        }
        if (lock_status.st_nlink != 1) {
            if (diagnostic != nullptr)
                diagnostic->reason = IdentityLeaseDiagnosticReason::lease_file_links;
            return Result<IdentityLease>::failure(Error::INVALID_ARGUMENT);
        }
        if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            struct stat named_status {};
            const bool stale =
                ::fstatat(directory.get(), filename.data(), &named_status,
                          AT_SYMLINK_NOFOLLOW) != 0 ||
                !same_identity(named_status, lock_status);
            if (stale) continue;
            return Result<IdentityLease>::failure(
                lock_error == EWOULDBLOCK || lock_error == EAGAIN
                    ? Error::BUSY : file_error(lock_error));
        }
        struct stat named_status {};
        if (::fstatat(directory.get(), filename.data(), &named_status,
                      AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(named_status.st_mode) &&
            same_identity(named_status, lock_status)) {
            IdentityLease lease;
            lease.lock_handle_ = lock.release();
            lease.directory_handle_ = directory.release();
            lease.filename_ = filename;
            return Result<IdentityLease>::success(std::move(lease));
        }
    }
    return Result<IdentityLease>::failure(Error::BUSY);
}

void IdentityLease::close() noexcept
{
    if (lock_handle_ >= 0) {
        if (::flock(lock_handle_, LOCK_EX | LOCK_NB) == 0 && directory_handle_ >= 0) {
            struct stat held_status {};
            struct stat named_status {};
            if (::fstat(lock_handle_, &held_status) == 0 &&
                ::fstatat(directory_handle_, filename_.data(), &named_status,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(named_status.st_mode) &&
                same_identity(named_status, held_status)) {
                (void)::unlinkat(directory_handle_, filename_.data(), 0);
            }
        }
        (void)::close(lock_handle_);
        lock_handle_ = detail::kInvalidIdentityLeaseHandle;
    }
    if (directory_handle_ >= 0) {
        (void)::close(directory_handle_);
        directory_handle_ = detail::kInvalidIdentityLeaseHandle;
    }
    filename_.fill('\0');
}

}  // namespace dtv::userland::host_listener_context
