// SPDX-License-Identifier: GPL-2.0-only
#include "windows/winscard_card_client.h"
#include "windows/winscard_product.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include "px4/posix_ipc.h"
#include "windows/windows_security.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

namespace px4::userland::windows_card {
namespace {

using namespace ipc;
using namespace ipc::posix;
namespace wire = dtv::userland::wire;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxDiscoveryEntries = 64U;
constexpr std::uint32_t kDiscoveryTimeoutMs = 1500U;
constexpr std::uint32_t kEndpointTimeoutMs = 200U;
constexpr std::uint32_t kRequestTimeoutMs = 4000U;
// UDTV 1.0 publishes exactly one card reader for the local reader namespace.
constexpr std::uint16_t kReaderId = 0U;

Timeout remaining_timeout(Clock::time_point deadline) noexcept
{
    const auto now = Clock::now();
    if (now >= deadline) return Timeout{0U};
    // Round down so a sequence of requests cannot each extend the budget.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return Timeout{static_cast<std::uint32_t>(remaining)};
}

// Adapts the product's Windows SocketStream to the shared core's byte
// transport. Framing and every typed request/response stay in the core client;
// this only moves bytes and reports a monotonic clock in the same units.
class SocketTransport final : public wire::ClientTransport {
public:
    SocketTransport() noexcept = default;
    explicit SocketTransport(SocketStream stream) noexcept : stream_(std::move(stream)) {}
    ~SocketTransport() noexcept override { stream_.close(); }
    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    Result<void> write_all(ByteView bytes, Timeout timeout) noexcept override
    {
        return stream_.write_all(bytes, timeout);
    }
    Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept override
    {
        return stream_.read_some(bytes, timeout);
    }
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count());
    }
    void close() noexcept override { stream_.close(); }

private:
    SocketStream stream_;
};

bool reserved_device_name(std::string_view value) noexcept
{
    // Win32 device names are aliases even inside an ordinary directory and
    // even when followed by an extension. Never pass one to a path API.
    const auto dot = value.find('.');
    const auto base = value.substr(0U, dot);
    std::array<char, 5U> upper{};
    if (base.size() < upper.size()) {
        for (std::size_t i = 0U; i < base.size(); ++i) {
            const char c = base[i];
            upper[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }
        const std::string_view name(upper.data(), base.size());
        if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" ||
            (name.size() == 4U && name[3] >= '1' && name[3] <= '9' &&
             (name.substr(0U, 3U) == "COM" || name.substr(0U, 3U) == "LPT")))
            return true;
    }
    return false;
}

bool valid_instance(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 80U || value == "." || value == ".." ||
        value.back() == '.' || reserved_device_name(value)) return false;
    for (const char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.')
            continue;
        return false;
    }
    return true;
}

Result<std::wstring> local_runtime_path(const std::string& input) noexcept
{
    // Check syntax before any filesystem call, including FindFirstFile.
    // Accept only a canonical drive-absolute path. UNC, extended/device
    // namespaces, ADS, drive-relative paths and normalization aliases fail
    // closed rather than accidentally probing a remote host.
    if (input.size() < 3U || input.size() >= kStoredPathCapacity ||
        input.find('\0') != std::string::npos ||
        !((input[0] >= 'A' && input[0] <= 'Z') ||
          (input[0] >= 'a' && input[0] <= 'z')) ||
        input[1] != ':' || input[2] != '\\') {
        return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
    }
    std::array<wchar_t, kStoredPathCapacity> converted{};
    const int count = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        converted.data(), static_cast<int>(converted.size() - 1U));
    if (count <= 0) return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
    const std::wstring path(converted.data(), static_cast<std::size_t>(count));
    if (path.size() > 3U && path.back() == L'\\')
        return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
    std::size_t start = 3U;
    for (std::size_t i = start; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\') {
            const wchar_t c = path[i];
            if (c < 0x20 || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                c == L'"' || c == L'<' || c == L'>' || c == L'|')
                return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
            continue;
        }
        if (i == start || path[i - 1U] == L'.' || path[i - 1U] == L' ')
            return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
        std::string component;
        for (std::size_t j = start; j < i; ++j) {
            if (path[j] > 0x7f) { component.clear(); break; }
            component.push_back(static_cast<char>(path[j]));
        }
        if (reserved_device_name(component))
            return Result<std::wstring>::failure(Error::INVALID_ARGUMENT);
        start = i + 1U;
    }
    return Result<std::wstring>::success(path);
}

Error path_error(DWORD error) noexcept
{
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return Error::NOT_FOUND;
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
        error == ERROR_INVALID_NAME) return Error::INVALID_ARGUMENT;
    return Error::INTERNAL;
}

class DirectoryPin final {
public:
    DirectoryPin() noexcept = default;
    ~DirectoryPin() noexcept { if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_); }
    DirectoryPin(const DirectoryPin&) = delete;
    DirectoryPin& operator=(const DirectoryPin&) = delete;
    HANDLE get() const noexcept { return handle_; }

    Result<void> open(const std::wstring& path, bool read_security) noexcept
    {
        // No FILE_SHARE_WRITE/DELETE: hold the checked ancestor in place and
        // prevent converting it into a reparse point until discovery/connect
        // finishes. OPEN_REPARSE_POINT prevents following the final component.
        handle_ = ::CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | (read_security ? READ_CONTROL : 0U),
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return Result<void>::failure(path_error(::GetLastError()));
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileType(handle_) != FILE_TYPE_DISK ||
            ::GetFileInformationByHandle(handle_, &info) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        return Result<void>::success();
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

bool verified_security(HANDLE handle, bool private_directory) noexcept
{
    windows_security::SidBuffer sid_storage{};
    PSID current = windows_security::current_user_sid(sid_storage);
    if (current == nullptr) return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) return false;
    bool valid = windows_security::sid_equal(owner, current) &&
        windows_security::acl_allows_only_trusted_writers(dacl, current);
    if (valid && private_directory) {
        SECURITY_DESCRIPTOR_CONTROL control = 0U;
        DWORD revision = 0U;
        valid = ::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
                (control & SE_DACL_PROTECTED) != 0U;
        ACL_SIZE_INFORMATION information{};
        valid = valid && ::GetAclInformation(dacl, &information, sizeof(information),
                                             AclSizeInformation) != 0;
        for (DWORD i = 0U; valid && i < information.AceCount; ++i) {
            void* raw = nullptr;
            valid = ::GetAce(dacl, i, &raw) != 0 && raw != nullptr;
            if (!valid) break;
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            valid = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                windows_security::sid_equal(
                    reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart)), current);
        }
    }
    ::LocalFree(descriptor);
    return valid;
}

struct RuntimePins final {
    std::array<DirectoryPin, 64U> ancestors;
    DirectoryPin product;
    DirectoryPin version;
    std::wstring product_path;
    std::wstring version_path;

    Result<void> open(const std::string& runtime) noexcept
    {
        const auto parsed = local_runtime_path(runtime);
        if (!parsed) return Result<void>::failure(parsed.error());
        const auto& path = parsed.value();
        const std::wstring drive = path.substr(0U, 3U);
        const UINT type = ::GetDriveTypeW(drive.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        std::size_t count = 0U;
        std::size_t end = 3U;
        for (;;) {
            if (count == ancestors.size())
                return Result<void>::failure(Error::INVALID_ARGUMENT);
            const bool last = end == path.size();
            const auto opened = ancestors[count].open(path.substr(0U, end), last);
            if (!opened) return opened;
            if (last) {
                if (!verified_security(ancestors[count].get(), false))
                    return Result<void>::failure(Error::INVALID_ARGUMENT);
                break;
            }
            ++count;
            const auto separator = path.find(L'\\', end + (end == 3U ? 0U : 1U));
            end = separator == std::wstring::npos ? path.size() : separator;
        }
        product_path = path;
        if (product_path.back() != L'\\') product_path += L'\\';
        product_path += kProductNamespaceW;
        const auto product_opened = product.open(product_path, true);
        if (!product_opened) return product_opened;
        if (!verified_security(product.get(), true))
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        // The published layout is <runtime>/px4-userland/v1/<instance>.
        version_path = product_path + L"\\" + kProductVersionW;
        const auto version_opened = version.open(version_path, true);
        if (!version_opened) return version_opened;
        if (!verified_security(version.get(), true))
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        return Result<void>::success();
    }
};

Result<void> pin_instance(const RuntimePins& runtime, const std::string& instance,
                          DirectoryPin& pin) noexcept
{
    if (!valid_instance(instance)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    const std::wstring wide(instance.begin(), instance.end());
    const auto opened = pin.open(runtime.version_path + L'\\' + wide, true);
    if (!opened) return opened;
    return verified_security(pin.get(), true) ? Result<void>::success() :
                                              Result<void>::failure(Error::INVALID_ARGUMENT);
}

Result<std::unique_ptr<SocketTransport>> open_transport(
    const std::string& runtime, const std::string& instance,
    Clock::time_point deadline) noexcept
{
    const Timeout timeout = remaining_timeout(deadline);
    if (timeout.milliseconds == 0U)
        return Result<std::unique_ptr<SocketTransport>>::failure(Error::TIMEOUT);
    const EndpointConfig endpoint{runtime.c_str(), instance.c_str(),
                                   kControlEndpointName, EndpointAccess::private_user};
    auto stream = SocketStream::connect(endpoint, timeout);
    if (!stream) return Result<std::unique_ptr<SocketTransport>>::failure(stream.error());
    return Result<std::unique_ptr<SocketTransport>>::success(
        std::make_unique<SocketTransport>(std::move(stream.value())));
}

ByteView byte_view(const char* bytes, std::size_t size) noexcept
{
    return ByteView{reinterpret_cast<const std::uint8_t*>(bytes), size};
}

ByteView product_slug_view() noexcept
{
    return byte_view(kProductSlug, sizeof(kProductSlug) - 1U);
}

bool matching_identity(const wire::StatusSnapshot& snapshot,
                       const std::string& instance) noexcept
{
    return wire::status_identity_matches(
        snapshot, product_slug_view(), byte_view(instance.data(), instance.size()));
}

bool listed_reader(const wire::ListResponse& list, std::uint16_t reader_id) noexcept
{
    if (!list.readers.present) return false;
    for (std::size_t index = 0U; index < list.readers.value.count; ++index) {
        if (list.readers.value.items[index].id == reader_id) return true;
    }
    return false;
}

ReaderStatus to_reader_status(const wire::CardStatusResponse& response) noexcept
{
    ReaderStatus result;
    result.present = response.present;
    result.initialized = response.initialized;
    result.generation = response.current_generation;
    if (response.atr.present && response.atr.value.length != 0U) {
        result.atr.assign(response.atr.value.bytes.data(),
                          response.atr.value.bytes.data() + response.atr.value.length);
    }
    return result;
}

bool terminal_error(Error error) noexcept
{
    switch (error) {
    case Error::BUSY:
    case Error::UNSUPPORTED:
    case Error::INVALID_ARGUMENT:
    case Error::BUFFER_TOO_SMALL:
        return false;
    default:
        return true;
    }
}

}  // namespace

Result<std::vector<Reader>> list_readers(const std::string& runtime_root) noexcept
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(kDiscoveryTimeoutMs);
    RuntimePins runtime;
    const auto opened = runtime.open(runtime_root);
    if (!opened) {
        if (opened.error() == Error::NOT_FOUND)
            return Result<std::vector<Reader>>::success({});
        return Result<std::vector<Reader>>::failure(opened.error());
    }
    std::vector<std::string> instances;
    const std::wstring pattern = runtime.version_path + L"\\*";
    WIN32_FIND_DATAW data{};
    const HANDLE search = ::FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? Result<std::vector<Reader>>::success({})
            : Result<std::vector<Reader>>::failure(path_error(error));
    }
    Error scan_error = Error::OK;
    std::size_t entries = 0U;
    do {
        if (remaining_timeout(deadline).milliseconds == 0U) {
            scan_error = Error::TIMEOUT;
            break;
        }
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") continue;
        if (++entries > kMaxDiscoveryEntries) {
            scan_error = Error::BUFFER_TOO_SMALL;
            break;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            name.empty() || name.size() > 80U) continue;
        std::string instance;
        for (const wchar_t c : name) {
            if (c > 0x7f) { instance.clear(); break; }
            instance.push_back(static_cast<char>(c));
        }
        if (valid_instance(instance)) instances.push_back(std::move(instance));
    } while (::FindNextFileW(search, &data) != 0);
    const DWORD find_error = ::GetLastError();
    ::FindClose(search);
    if (scan_error != Error::OK)
        return Result<std::vector<Reader>>::failure(scan_error);
    if (find_error != ERROR_NO_MORE_FILES)
        return Result<std::vector<Reader>>::failure(path_error(find_error));
    std::sort(instances.begin(), instances.end());
    std::vector<Reader> readers;
    for (const auto& instance : instances) {
        if (remaining_timeout(deadline).milliseconds == 0U)
            return Result<std::vector<Reader>>::failure(Error::TIMEOUT);
        DirectoryPin pin;
        if (!pin_instance(runtime, instance, pin)) continue;
        const auto endpoint_deadline = std::min(
            deadline, Clock::now() + std::chrono::milliseconds(kEndpointTimeoutMs));
        auto transport = open_transport(runtime_root, instance, endpoint_deadline);
        if (!transport) continue;
        const Timeout timeout = remaining_timeout(endpoint_deadline);
        if (timeout.milliseconds == 0U) continue;
        const auto status = wire::fetch_status(*transport.value(), timeout,
                                               wire::kFeatureCardService);
        // A CARD_SERVICE endpoint that is not ready, or that does not publish
        // the reader, is not exposed as a usable reader.
        if (!status || status.snapshot.status.context.state != wire::ContextState::ready ||
            !listed_reader(status.snapshot.list, kReaderId) ||
            !matching_identity(status.snapshot, instance))
            continue;
        readers.push_back(Reader{instance, std::string(kReaderPrefix) + instance + ")"});
    }
    if (remaining_timeout(deadline).milliseconds == 0U)
        return Result<std::vector<Reader>>::failure(Error::TIMEOUT);
    return Result<std::vector<Reader>>::success(std::move(readers));
}

Result<ReaderStatus> reader_status(const std::string& runtime_root,
                                  const std::string& instance) noexcept
{
    if (!valid_instance(instance)) return Result<ReaderStatus>::failure(Error::INVALID_ARGUMENT);
    const auto deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    RuntimePins runtime;
    const auto opened = runtime.open(runtime_root);
    if (!opened) return Result<ReaderStatus>::failure(opened.error());
    DirectoryPin pin;
    const auto pinned = pin_instance(runtime, instance, pin);
    if (!pinned) return Result<ReaderStatus>::failure(pinned.error());
    auto transport = open_transport(runtime_root, instance, deadline);
    if (!transport) return Result<ReaderStatus>::failure(transport.error());
    const Timeout fetch_timeout = remaining_timeout(deadline);
    if (fetch_timeout.milliseconds == 0U)
        return Result<ReaderStatus>::failure(Error::TIMEOUT);
    const auto status = wire::fetch_status(*transport.value(), fetch_timeout,
                                           wire::kFeatureCardService);
    if (!status) return Result<ReaderStatus>::failure(status.error);
    if (!matching_identity(status.snapshot, instance))
        return Result<ReaderStatus>::failure(Error::NOT_FOUND);
    if (status.snapshot.status.context.state != wire::ContextState::ready)
        return Result<ReaderStatus>::failure(Error::NOT_READY);
    if (!listed_reader(status.snapshot.list, kReaderId))
        return Result<ReaderStatus>::failure(Error::NOT_FOUND);
    // A reader snapshot must not connect or initialize the card: one
    // reader-keyed CARD_STATUS with no owner handle.
    wire::CardClientSession session{};
    session.epoch = status.snapshot.status.context.epoch;
    session.reader_id = kReaderId;
    session.next_request_id = 4U;
    const Timeout status_timeout = remaining_timeout(deadline);
    if (status_timeout.milliseconds == 0U)
        return Result<ReaderStatus>::failure(Error::TIMEOUT);
    const auto result = wire::card_status(*transport.value(), session, status_timeout);
    if (!result) return Result<ReaderStatus>::failure(result.error);
    return Result<ReaderStatus>::success(to_reader_status(result.response));
}

Result<std::unique_ptr<CardClient>> CardClient::connect(
    const std::string& runtime_root, const std::string& instance,
    ShareMode mode) noexcept
{
    if (!valid_instance(instance) ||
        (mode != ShareMode::shared && mode != ShareMode::exclusive))
        return Result<std::unique_ptr<CardClient>>::failure(Error::INVALID_ARGUMENT);
    const auto deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    RuntimePins runtime;
    const auto opened = runtime.open(runtime_root);
    if (!opened) return Result<std::unique_ptr<CardClient>>::failure(opened.error());
    DirectoryPin pin;
    const auto pinned = pin_instance(runtime, instance, pin);
    if (!pinned) return Result<std::unique_ptr<CardClient>>::failure(pinned.error());
    auto transport = open_transport(runtime_root, instance, deadline);
    if (!transport) return Result<std::unique_ptr<CardClient>>::failure(transport.error());
    const Timeout timeout = remaining_timeout(deadline);
    if (timeout.milliseconds == 0U)
        return Result<std::unique_ptr<CardClient>>::failure(Error::TIMEOUT);
    // The shared client performs HELLO(CARD_SERVICE)/LIST/STATUS then
    // CARD_STATUS and CARD_CONNECT on this one connection. A non-card daemon
    // is refused with UNSUPPORTED and every failure closes the connection.
    wire::CardClientOptions options{};
    options.reader_id = kReaderId;
    options.share = static_cast<wire::Share>(mode);
    options.preferred_protocols = 1U;
    options.expected_product = product_slug_view();
    options.expected_instance = byte_view(instance.data(), instance.size());
    const auto card = wire::begin_card(*transport.value(), options, timeout);
    if (!card) return Result<std::unique_ptr<CardClient>>::failure(card.error);
    // Never present a stale (usable=false) or ATR-less handle as a live card.
    if (!card.connection.usable || !card.connection.atr.present ||
        card.connection.atr.value.length == 0U)
        return Result<std::unique_ptr<CardClient>>::failure(Error::PROTOCOL_ERROR);
    std::vector<std::uint8_t> atr(
        card.connection.atr.value.bytes.data(),
        card.connection.atr.value.bytes.data() + card.connection.atr.value.length);
    return Result<std::unique_ptr<CardClient>>::success(std::unique_ptr<CardClient>(
        new CardClient(std::move(transport.value()), card.session, std::move(atr))));
}

CardClient::~CardClient() noexcept
{
    // Closing the owning connection releases handles/transactions in px4d.
    // Destruction never resets the card or waits for an extra request.
    if (transport_) transport_->close();
}

void CardClient::forget() noexcept
{
    if (transport_) transport_->close();
    transport_.reset();
    session_.card_handle = 0U;
    session_.bound_generation = 0U;
}

Result<std::vector<std::uint8_t>> CardClient::transmit(ByteView apdu) noexcept
{
    if (apdu.data == nullptr || apdu.size == 0U || apdu.size > wire::kMaxApduBytes)
        return Result<std::vector<std::uint8_t>>::failure(Error::INVALID_ARGUMENT);
    if (!transport_ || session_.card_handle == 0U)
        return Result<std::vector<std::uint8_t>>::failure(Error::NOT_FOUND);
    const auto result = wire::card_transmit(*transport_, session_, wire::CardProtocol::t1,
                                            apdu, Timeout{kRequestTimeoutMs});
    if (!result) {
        if (terminal_error(result.error)) forget();
        return Result<std::vector<std::uint8_t>>::failure(result.error);
    }
    const auto& bytes = result.response.response;
    std::vector<std::uint8_t> response;
    if (bytes.length != 0U) response.assign(bytes.bytes.data(), bytes.bytes.data() + bytes.length);
    return Result<std::vector<std::uint8_t>>::success(std::move(response));
}

Result<ReaderStatus> CardClient::status() noexcept
{
    if (!transport_ || session_.card_handle == 0U)
        return Result<ReaderStatus>::failure(Error::NOT_FOUND);
    const auto result = wire::card_status(*transport_, session_, Timeout{kRequestTimeoutMs});
    if (!result) {
        if (terminal_error(result.error)) forget();
        return Result<ReaderStatus>::failure(result.error);
    }
    ReaderStatus status = to_reader_status(result.response);
    if (!status.present) {
        forget();
        return Result<ReaderStatus>::failure(Error::CARD_REMOVED);
    }
    if (!status.initialized) {
        forget();
        return Result<ReaderStatus>::failure(Error::NOT_READY);
    }
    // Do not borrow a replacement card's ATR. Preserve the ATR that belongs
    // to this successful, validated CARD_CONNECT handle.
    status.atr = atr_;
    return Result<ReaderStatus>::success(std::move(status));
}

Result<void> CardClient::disconnect(Disposition disposition) noexcept
{
    if (disposition != Disposition::leave && disposition != Disposition::reset)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (!transport_ || session_.card_handle == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    const auto result = wire::card_disconnect(
        *transport_, session_, static_cast<wire::Disposition>(disposition),
        Timeout{kRequestTimeoutMs});
    forget();
    return result ? Result<void>::success() : Result<void>::failure(result.error);
}

}  // namespace px4::userland::windows_card

#endif  // _WIN32
