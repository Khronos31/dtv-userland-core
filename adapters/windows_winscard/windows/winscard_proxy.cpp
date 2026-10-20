// SPDX-License-Identifier: GPL-2.0-only
// Deliberately bounded B25 client ABI, not a system-wide WinSCard replacement.
// All initialization is lazy, outside DllMain. No USB or daemon startup here.
#define WINSCARDDATA
#include "windows/winscard_native.h"
#include "windows/winscard_card_client.h"
#include "windows/winscard_product.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
extern const SCARD_IO_REQUEST g_rgSCardT1Pci = {SCARD_PROTOCOL_T1, sizeof(SCARD_IO_REQUEST)};
}
static_assert(sizeof(SCARD_IO_REQUEST) == 8U, "WinSCard PCI ABI must stay 8 bytes");
static_assert(sizeof(DWORD) == 4U, "WinSCard DWORD must stay 32 bits");

namespace px4::userland::windows_card {
namespace {

constexpr std::size_t kMaxContexts = 64U;
constexpr std::size_t kMaxCards = 256U;
constexpr std::size_t kMaxReaderCharacters = 65536U;
constexpr std::size_t kMaxReaderName = 1024U;
constexpr std::size_t kMaxAllocations = 64U;
constexpr std::size_t kMaxAllocatedBytes = 1024U * 1024U;

LONG map_error(Error error) noexcept
{
    switch (error) {
    case Error::OK: return SCARD_S_SUCCESS;
    case Error::INVALID_ARGUMENT: return SCARD_E_INVALID_PARAMETER;
    case Error::BUSY: return SCARD_E_SHARING_VIOLATION;
    case Error::NO_CARD: return SCARD_E_NO_SMARTCARD;
    case Error::CARD_REMOVED:
    case Error::DISCONNECTED: return SCARD_W_REMOVED_CARD;
    case Error::NOT_FOUND: return SCARD_E_INVALID_HANDLE;
    case Error::NOT_READY: return SCARD_E_READER_UNAVAILABLE;
    case Error::TIMEOUT: return SCARD_E_TIMEOUT;
    case Error::BUFFER_TOO_SMALL: return SCARD_E_INSUFFICIENT_BUFFER;
    case Error::UNSUPPORTED: return SCARD_E_UNSUPPORTED_FEATURE;
    default: return SCARD_F_COMM_ERROR;
    }
}

template <typename Function>
Function native_function(HMODULE module, const char* name) noexcept
{
    const FARPROC raw = ::GetProcAddress(module, name);
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

[[maybe_unused]] bool is_expected_module(HMODULE module, const wchar_t* expected) noexcept
{
    std::array<wchar_t, 32768U> path{};
    const DWORD size = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0U || size >= path.size()) return false;
    // Compare file identities, not spelling: WOW64 can map System32 to
    // SysWOW64. An application .local redirection must never become a provider.
    const HANDLE intended = ::CreateFileW(expected, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0U, nullptr);
    if (intended == INVALID_HANDLE_VALUE) return false;
    const HANDLE actual = ::CreateFileW(path.data(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0U, nullptr);
    BY_HANDLE_FILE_INFORMATION left{}, right{};
    const bool matches = actual != INVALID_HANDLE_VALUE &&
        ::GetFileInformationByHandle(intended, &left) && ::GetFileInformationByHandle(actual, &right) &&
        left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
    if (actual != INVALID_HANDLE_VALUE) (void)::CloseHandle(actual);
    (void)::CloseHandle(intended);
    return matches;
}

NativeApi& native_api() noexcept
{
    static NativeApi api;
#if !defined(PX4_WINSCARD_TESTING)
    static std::once_flag initialized;
    std::call_once(initialized, [&]() noexcept {
        // Pin the proxy once a native module is loaded: a decoder may unload
        // and reload, and a surviving System32 module must not replace us on
        // the next basename import. Windows releases both at process exit.
        HMODULE self = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_PIN,
                                 reinterpret_cast<LPCWSTR>(&native_api), &self)) return;
        std::array<wchar_t, 32768U> directory{};
        const UINT length = ::GetSystemDirectoryW(directory.data(),
                                                   static_cast<UINT>(directory.size()));
        if (length == 0U || length >= directory.size()) return;
        std::wstring path(directory.data(), length);
        path.append(L"\\WinSCard.dll");
        // GetSystemDirectoryW plus WOW64 redirection selects the matching OS
        // architecture. Do not replace this with a basename search or an env var.
        const HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr,
                                                LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) return;
        if (module == self || !is_expected_module(module, path.c_str())) {
            (void)::FreeLibrary(module);
            return;
        }
        api.establish = native_function<decltype(api.establish)>(module, "SCardEstablishContext");
        api.release = native_function<decltype(api.release)>(module, "SCardReleaseContext");
        api.list_a = native_function<decltype(api.list_a)>(module, "SCardListReadersA");
        api.list_w = native_function<decltype(api.list_w)>(module, "SCardListReadersW");
        api.connect_a = native_function<decltype(api.connect_a)>(module, "SCardConnectA");
        api.connect_w = native_function<decltype(api.connect_w)>(module, "SCardConnectW");
        api.disconnect = native_function<decltype(api.disconnect)>(module, "SCardDisconnect");
        api.transmit = native_function<decltype(api.transmit)>(module, "SCardTransmit");
        api.status_w = native_function<decltype(api.status_w)>(module, "SCardStatusW");
        api.change_w = native_function<decltype(api.change_w)>(module, "SCardGetStatusChangeW");
        if (!api.complete()) api = NativeApi{};
    });
#endif
    return api;
}

Result<std::string> runtime_root() noexcept
{
    std::array<wchar_t, 512U> buffer{};
    ::SetLastError(ERROR_SUCCESS);
    DWORD size = ::GetEnvironmentVariableW(kRuntimeEnv, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (size == 0U && ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        size = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    }
    if (size == 0U || size >= buffer.size()) {
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    }
    // Reject UNC, rooted-relative and device paths before any discovery I/O.
    // Custom roots must use the same ordinary drive-absolute form on all clients.
    const bool drive = (buffer[0] >= L'A' && buffer[0] <= L'Z') ||
                       (buffer[0] >= L'a' && buffer[0] <= L'z');
    if (!drive || size < 3U || buffer[1] != L':' || buffer[2] != L'\\') {
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            buffer.data(), static_cast<int>(size),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes >= 512) return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(),
                              static_cast<int>(size), result.data(), bytes,
                              nullptr, nullptr) != bytes) {
        return Result<std::string>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<std::string>::success(std::move(result));
}

struct Context final {
    std::mutex mutex;
    bool released = false;
    DWORD scope = SCARD_SCOPE_USER;
    SCARDCONTEXT native = 0U;
    std::string root;
    std::map<const void*, std::size_t> allocations;
    std::size_t allocated_bytes = 0U;
};

struct Card final {
    std::mutex mutex;
    bool active = true;
    std::shared_ptr<Context> owner;
    SCARDHANDLE native = 0U;
    std::unique_ptr<CardClient> local;
    std::wstring reader_name;
};

void* allocate_memory(Context& context, std::size_t bytes) noexcept
{
    bytes = std::max<std::size_t>(1U, bytes);
    if (context.allocations.size() >= kMaxAllocations ||
        bytes > kMaxAllocatedBytes - context.allocated_bytes) return nullptr;
    void* memory = ::HeapAlloc(::GetProcessHeap(), 0U, bytes);
    if (!memory) return nullptr;
    context.allocations.emplace(memory, bytes);
    context.allocated_bytes += bytes;
    return memory;
}

bool free_memory(Context& context, const void* memory) noexcept
{
    const auto found = context.allocations.find(memory);
    if (found == context.allocations.end()) return false;
    if (!::HeapFree(::GetProcessHeap(), 0U, const_cast<void*>(memory))) return false;
    context.allocated_bytes -= found->second;
    context.allocations.erase(found);
    return true;
}

struct Registry final {
    std::mutex mutex;
    std::map<SCARDCONTEXT, std::shared_ptr<Context>> contexts;
    std::map<SCARDHANDLE, std::shared_ptr<Card>> cards;
    std::uintptr_t next = 0x50580000U;

    std::uintptr_t allocate() noexcept
    {
        // Never reuse a public handle, including after integer wrap. The wire
        // u64 card handle is separate and is never narrowed into this value.
        if (next == std::numeric_limits<std::uintptr_t>::max()) return 0U;
        return next++;
    }
};

Registry& registry() noexcept
{
    // Intentionally process-lifetime storage. CRT process-detach runs under
    // the loader lock: do not destroy active clients/mutexes there. Explicit
    // ReleaseContext owns cleanup; the OS closes abandoned sockets at exit.
    static Registry* const value = new Registry;
    return *value;
}

std::shared_ptr<Context> find_context(SCARDCONTEXT handle) noexcept
{
    Registry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.contexts.find(handle);
    return found == state.contexts.end() ? nullptr : found->second;
}

std::shared_ptr<Card> find_card(SCARDHANDLE handle) noexcept
{
    Registry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto found = state.cards.find(handle);
    return found == state.cards.end() ? nullptr : found->second;
}

LONG ensure_native_context(Context& context) noexcept
{
    NativeApi& api = native_api();
    if (!api.complete()) return SCARD_E_NO_SERVICE;
    if (context.native != 0U) return SCARD_S_SUCCESS;
    // A service absent at context creation may start later. Retry only context
    // creation, never a failed card operation or a stale native card.
    SCARDCONTEXT candidate = 0U;
    const LONG result = api.establish(context.scope, nullptr, nullptr, &candidate);
    if (result == SCARD_S_SUCCESS) {
        if (candidate == 0U) return SCARD_F_COMM_ERROR;
        context.native = candidate;
    }
    return result;
}

template <typename Character>
struct ReaderEntry final {
    std::basic_string<Character> name;
    std::string instance;  // Empty means Windows owns the reader.
};

template <typename Character>
LONG native_list(SCARDCONTEXT context, Character* output, LPDWORD size) noexcept;
template <>
LONG native_list<char>(SCARDCONTEXT context, char* output, LPDWORD size) noexcept
{
    return native_api().list_a(context, nullptr, output, size);
}
template <>
LONG native_list<wchar_t>(SCARDCONTEXT context, wchar_t* output, LPDWORD size) noexcept
{
    return native_api().list_w(context, nullptr, output, size);
}

template <typename Character>
LONG collect_readers(Context& context, std::vector<ReaderEntry<Character>>& readers,
                     Error* local_failure = nullptr) noexcept
{
    const auto local = list_readers(context.root);
    if (local_failure) *local_failure = local ? Error::OK : local.error();
    std::vector<ReaderEntry<Character>> native_readers;
    LONG native_result = ensure_native_context(context);
    if (native_result == SCARD_S_SUCCESS) {
        DWORD size = 0U;
        native_result = native_list<Character>(context.native, nullptr, &size);
        if (native_result == SCARD_S_SUCCESS) {
            // Reader lists can change between the size query and copy. Bound
            // both allocation and retries, and never trust a malformed MULTI_SZ.
            for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
                if (size < 2U || size > kMaxReaderCharacters) return SCARD_F_COMM_ERROR;
                std::vector<Character> names(size, static_cast<Character>(0));
                DWORD used = size;
                native_result = native_list<Character>(context.native, names.data(), &used);
                if (native_result == SCARD_E_INSUFFICIENT_BUFFER) {
                    size = used;
                    continue;
                }
                if (native_result != SCARD_S_SUCCESS) break;
                if (used < 2U || used > names.size() || names[used - 1U] != 0 ||
                    names[used - 2U] != 0) return SCARD_F_COMM_ERROR;
                std::size_t offset = 0U;
                while (offset < used - 1U && names[offset] != 0) {
                    const auto end = std::find(names.begin() + static_cast<std::ptrdiff_t>(offset),
                                               names.begin() + used, static_cast<Character>(0));
                    if (end == names.begin() + used) return SCARD_F_COMM_ERROR;
                    const auto length = static_cast<std::size_t>(end - names.begin()) - offset;
                    native_readers.push_back({std::basic_string<Character>(names.data() + offset,
                                                                           length), {}});
                    offset += length + 1U;
                }
                break;
            }
        }
    }
    if (local) for (const Reader& item : local.value()) {
        const std::basic_string<Character> name(item.display_name.begin(), item.display_name.end());
        // Preserve access to a Windows-owned reader if an unusual native name
        // collides. Never silently route that reader to a virtual device.
        if (std::none_of(native_readers.begin(), native_readers.end(),
                         [&](const auto& item_native) { return item_native.name == name; })) {
            readers.push_back({name, item.instance});
        }
    }
    readers.insert(readers.end(), native_readers.begin(), native_readers.end());
    if (!readers.empty()) return SCARD_S_SUCCESS;
    if (!local) return map_error(local.error());
    return native_result == SCARD_S_SUCCESS || native_result == SCARD_E_NO_SERVICE ||
                   native_result == SCARD_E_SERVICE_STOPPED ?
               SCARD_E_NO_READERS_AVAILABLE : native_result;
}

template <typename Character>
LONG list(SCARDCONTEXT handle, const Character* groups, Character* output,
          LPDWORD length) noexcept
{
    if (length == nullptr) return SCARD_E_INVALID_PARAMETER;
    if (groups != nullptr) return SCARD_E_UNSUPPORTED_FEATURE;
    const auto context = find_context(handle);
    if (!context) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    std::vector<ReaderEntry<Character>> readers;
    const LONG result = collect_readers(*context, readers);
    if (result != SCARD_S_SUCCESS) {
        if (result == SCARD_E_NO_READERS_AVAILABLE) *length = 0U;
        return result;
    }
    std::size_t required = 1U;
    for (const auto& reader : readers) {
        if (reader.name.size() + 1U > kMaxReaderCharacters - required) return SCARD_E_NO_MEMORY;
        required += reader.name.size() + 1U;
    }
    const DWORD capacity = output ? *length : 0U;
    *length = static_cast<DWORD>(required);
    if (output == nullptr) return SCARD_S_SUCCESS;
    const bool automatic = capacity == SCARD_AUTOALLOCATE;
    Character* destination = output;
    if (automatic) {
        destination = static_cast<Character*>(allocate_memory(*context, required * sizeof(Character)));
        if (!destination) return SCARD_E_NO_MEMORY;
    } else if (capacity < required) return SCARD_E_INSUFFICIENT_BUFFER;
    std::size_t offset = 0U;
    for (const auto& reader : readers) {
        std::copy(reader.name.begin(), reader.name.end(), destination + offset);
        offset += reader.name.size();
        destination[offset++] = 0;
    }
    destination[offset] = 0;
    if (automatic) std::memcpy(output, &destination, sizeof(destination));
    return SCARD_S_SUCCESS;
}

template <typename Character>
LONG native_connect(SCARDCONTEXT context, const Character* reader, DWORD share,
                    DWORD protocols, LPSCARDHANDLE card, LPDWORD active) noexcept;
template <>
LONG native_connect<char>(SCARDCONTEXT context, const char* reader, DWORD share,
                         DWORD protocols, LPSCARDHANDLE card, LPDWORD active) noexcept
{
    return native_api().connect_a(context, reader, share, protocols, card, active);
}
template <>
LONG native_connect<wchar_t>(SCARDCONTEXT context, const wchar_t* reader, DWORD share,
                            DWORD protocols, LPSCARDHANDLE card, LPDWORD active) noexcept
{
    return native_api().connect_w(context, reader, share, protocols, card, active);
}

LONG close_card(Card& card, DWORD disposition) noexcept
{
    if (!card.active) return SCARD_E_INVALID_HANDLE;
    card.active = false;
    if (card.local) {
        const auto result = card.local->disconnect(disposition == SCARD_RESET_CARD ?
                                                      ipc::Disposition::reset : ipc::Disposition::leave);
        card.local.reset();
        return result ? SCARD_S_SUCCESS : map_error(result.error());
    }
    return native_api().disconnect(card.native, disposition);
}

template <typename Character>
LONG connect(SCARDCONTEXT handle, const Character* reader, DWORD share, DWORD protocols,
             LPSCARDHANDLE output, LPDWORD active) noexcept
{
    if (!reader || !output || !active) return SCARD_E_INVALID_PARAMETER;
    *output = 0U;
    *active = SCARD_PROTOCOL_UNDEFINED;
    std::size_t name_length = 0U;
    while (name_length < kMaxReaderName && reader[name_length] != 0) ++name_length;
    if (name_length == 0U || name_length == kMaxReaderName) return SCARD_E_INVALID_PARAMETER;
    const auto context = find_context(handle);
    if (!context) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    std::vector<ReaderEntry<Character>> readers;
    Error local_failure = Error::OK;
    const LONG listed = collect_readers(*context, readers, &local_failure);
    if (listed != SCARD_S_SUCCESS && listed != SCARD_E_NO_READERS_AVAILABLE) return listed;
    const std::basic_string<Character> name(reader, name_length);
    const auto found = std::find_if(readers.begin(), readers.end(),
                                     [&](const auto& entry) { return entry.name == name; });
    const auto card = std::make_shared<Card>();
    card->owner = context;
    DWORD negotiated = SCARD_PROTOCOL_UNDEFINED;
    if (found != readers.end() && !found->instance.empty()) {
        if (share != SCARD_SHARE_SHARED && share != SCARD_SHARE_EXCLUSIVE) return SCARD_E_UNSUPPORTED_FEATURE;
        if ((protocols & ~(SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1)) != 0U) return SCARD_E_INVALID_PARAMETER;
        if ((protocols & SCARD_PROTOCOL_T1) == 0U) return SCARD_E_PROTO_MISMATCH;
        auto local = CardClient::connect(context->root, found->instance,
                                           share == SCARD_SHARE_EXCLUSIVE ?
                                               ipc::ShareMode::exclusive : ipc::ShareMode::shared);
        if (!local) return map_error(local.error());
        card->local = std::move(local.value());
        card->reader_name.assign(found->name.begin(), found->name.end());
        negotiated = SCARD_PROTOCOL_T1;
    } else {
        const std::basic_string<Character> prefix(std::begin(kReaderPrefix), std::end(kReaderPrefix) - 1);
        if (found == readers.end() && name.compare(0U, prefix.size(), prefix) == 0) {
            return local_failure != Error::OK ? map_error(local_failure) : SCARD_E_UNKNOWN_READER;
        }
        const LONG available = ensure_native_context(*context);
        if (available != SCARD_S_SUCCESS) return available;
        const LONG result = native_connect(context->native, reader, share, protocols,
                                            &card->native, &negotiated);
        if (result != SCARD_S_SUCCESS) return result;
    }
    Registry& state = registry();
    std::uintptr_t id = 0U;
    {
        std::lock_guard<std::mutex> registry_lock(state.mutex);
        if (state.cards.size() < kMaxCards) id = state.allocate();
        if (id != 0U) state.cards.emplace(id, card);
    }
    if (id == 0U) {
        (void)close_card(*card, SCARD_LEAVE_CARD);
        return SCARD_E_NO_MEMORY;
    }
    *output = id;
    *active = negotiated;
    return SCARD_S_SUCCESS;
}

struct StatusSnapshot final {
    std::vector<wchar_t> names;
    std::vector<std::uint8_t> atr;
    DWORD state = 0U;
    DWORD protocol = SCARD_PROTOCOL_UNDEFINED;
};

LONG card_status(Card& card, StatusSnapshot& snapshot) noexcept
{
    if (card.local) {
        const auto status = card.local->status();
        if (!status) return map_error(status.error());
        snapshot.names.assign(card.reader_name.begin(), card.reader_name.end());
        snapshot.names.push_back(0); snapshot.names.push_back(0);
        snapshot.atr = status.value().atr;
        snapshot.state = SCARD_SPECIFIC;
        snapshot.protocol = SCARD_PROTOCOL_T1;
        return SCARD_S_SUCCESS;
    }
    std::array<BYTE, 36U> atr{};
    DWORD atr_size = static_cast<DWORD>(atr.size()), name_size = 0U;
    LONG result = native_api().status_w(card.native, nullptr, &name_size,
        &snapshot.state, &snapshot.protocol, atr.data(), &atr_size);
    if (result != SCARD_S_SUCCESS) return result;
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        if (name_size == 0U || name_size > kMaxReaderCharacters) return SCARD_F_COMM_ERROR;
        snapshot.names.assign(name_size, 0);
        DWORD used = name_size;
        atr_size = static_cast<DWORD>(atr.size());
        result = native_api().status_w(card.native, snapshot.names.data(), &used,
            &snapshot.state, &snapshot.protocol, atr.data(), &atr_size);
        if (result == SCARD_E_INSUFFICIENT_BUFFER && used > name_size && atr_size <= atr.size()) {
            name_size = used;
            continue;
        }
        if (result != SCARD_S_SUCCESS) return result;
        if (used == 0U || used > snapshot.names.size() || snapshot.names[used - 1U] != 0 ||
            atr_size > atr.size()) return SCARD_F_COMM_ERROR;
        snapshot.names.resize(used);
        // A single-name provider may report one final NUL. Publish MULTI_SZ.
        if (used == 1U || snapshot.names[used - 2U] != 0) {
            if (used == kMaxReaderCharacters) return SCARD_F_COMM_ERROR;
            snapshot.names.push_back(0);
        }
        snapshot.atr.assign(atr.begin(), atr.begin() + atr_size);
        return SCARD_S_SUCCESS;
    }
    return SCARD_E_INSUFFICIENT_BUFFER;
}

}  // namespace

#if defined(PX4_WINSCARD_TESTING)
void set_native_api_for_tests(const NativeApi& api) noexcept { native_api() = api; }
#endif

}  // namespace px4::userland::windows_card

extern "C" LONG WINAPI SCardEstablishContext(DWORD scope, LPCVOID reserved1,
                                               LPCVOID reserved2, LPSCARDCONTEXT output)
{
    using namespace px4::userland::windows_card;
    if (!output || reserved1 || reserved2) return SCARD_E_INVALID_PARAMETER;
    *output = 0U;
    if (scope != SCARD_SCOPE_USER && scope != SCARD_SCOPE_SYSTEM) return SCARD_E_INVALID_VALUE;
    const auto root = runtime_root();
    const auto context = std::make_shared<Context>();
    context->scope = scope;
    // An invalid local configuration must not disable unrelated OS readers.
    // Empty root is rejected by the local client before any filesystem I/O.
    if (root) context->root = root.value();
    const LONG native_result = ensure_native_context(*context);
    if (native_result != SCARD_S_SUCCESS && native_result != SCARD_E_NO_SERVICE &&
        native_result != SCARD_E_SERVICE_STOPPED) return native_result;
    if (!root && native_result != SCARD_S_SUCCESS) return map_error(root.error());
    Registry& state = registry();
    std::uintptr_t id = 0U;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.contexts.size() < kMaxContexts) id = state.allocate();
        if (id != 0U) state.contexts.emplace(id, context);
    }
    if (id == 0U) {
        if (context->native != 0U) (void)native_api().release(context->native);
        return SCARD_E_NO_MEMORY;
    }
    *output = id;
    return SCARD_S_SUCCESS;
}

extern "C" LONG WINAPI SCardReleaseContext(SCARDCONTEXT handle)
{
    using namespace px4::userland::windows_card;
    const auto context = find_context(handle);
    if (!context) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> context_lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    context->released = true;
    std::vector<std::shared_ptr<Card>> owned;
    Registry& state = registry();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.contexts.erase(handle);
        for (auto it = state.cards.begin(); it != state.cards.end();) {
            if (it->second->owner == context) {
                owned.push_back(it->second);
                it = state.cards.erase(it);
            } else ++it;
        }
    }
    for (const auto& card : owned) {
        std::lock_guard<std::mutex> lock(card->mutex);
        card->active = false;
        // Closing each owning control connection releases its daemon handles.
        // No sequential per-card roundtrips (up to 256 timeout intervals).
        card->local.reset();
    }
    LONG cleanup_result = SCARD_S_SUCCESS;
    while (!context->allocations.empty()) {
        const auto first = context->allocations.begin();
        if (!free_memory(*context, first->first)) {
            cleanup_result = SCARD_F_INTERNAL_ERROR;
            // Do not spin or skip native cleanup if the OS refuses a free.
            context->allocations.erase(first);
        }
    }
    const LONG native_result = context->native != 0U ?
        native_api().release(context->native) : SCARD_S_SUCCESS;
    return cleanup_result != SCARD_S_SUCCESS ? cleanup_result : native_result;
}

extern "C" LONG WINAPI SCardListReadersA(SCARDCONTEXT context, LPCSTR groups,
                                           LPSTR output, LPDWORD length)
{
    return px4::userland::windows_card::list(context, groups, output, length);
}
extern "C" LONG WINAPI SCardListReadersW(SCARDCONTEXT context, LPCWSTR groups,
                                           LPWSTR output, LPDWORD length)
{
    return px4::userland::windows_card::list(context, groups, output, length);
}
extern "C" LONG WINAPI SCardConnectA(SCARDCONTEXT context, LPCSTR reader, DWORD share,
                                      DWORD protocols, LPSCARDHANDLE output, LPDWORD active)
{
    return px4::userland::windows_card::connect(context, reader, share, protocols, output, active);
}
extern "C" LONG WINAPI SCardConnectW(SCARDCONTEXT context, LPCWSTR reader, DWORD share,
                                      DWORD protocols, LPSCARDHANDLE output, LPDWORD active)
{
    return px4::userland::windows_card::connect(context, reader, share, protocols, output, active);
}
extern "C" LONG WINAPI SCardDisconnect(SCARDHANDLE handle, DWORD disposition)
{
    using namespace px4::userland::windows_card;
    const auto card = find_card(handle);
    if (!card) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(card->mutex);
    if (!card->active) return SCARD_E_INVALID_HANDLE;
    if (disposition != SCARD_LEAVE_CARD && disposition != SCARD_RESET_CARD &&
        disposition != SCARD_UNPOWER_CARD && disposition != SCARD_EJECT_CARD) {
        return SCARD_E_INVALID_VALUE;
    }
    if (card->local && disposition != SCARD_LEAVE_CARD && disposition != SCARD_RESET_CARD) {
        return SCARD_E_UNSUPPORTED_FEATURE;
    }
    LONG result = SCARD_S_SUCCESS;
    if (card->local) result = close_card(*card, disposition);
    else {
        result = native_api().disconnect(card->native, disposition);
        // Failed native disconnect need not release a card. Retain ownership
        // so the caller can retry or ReleaseContext can clean up the provider.
        if (result != SCARD_S_SUCCESS) return result;
        card->active = false;
    }
    {
        Registry& state = registry();
        std::lock_guard<std::mutex> registry_lock(state.mutex);
        state.cards.erase(handle);
    }
    return result;
}
extern "C" LONG WINAPI SCardTransmit(SCARDHANDLE handle, LPCSCARD_IO_REQUEST send_pci,
                                      LPCBYTE send, DWORD send_length,
                                      LPSCARD_IO_REQUEST receive_pci, LPBYTE receive,
                                      LPDWORD receive_length)
{
    using namespace px4::userland;
    using namespace px4::userland::windows_card;
    const auto card = find_card(handle);
    if (!card) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(card->mutex);
    if (!card->active) return SCARD_E_INVALID_HANDLE;
    if (!card->local) return native_api().transmit(card->native, send_pci, send,
                                                   send_length, receive_pci, receive, receive_length);
    if (!send_pci || !send || send_length == 0U || send_length > 4096U ||
        !receive || !receive_length || send_pci->cbPciLength != sizeof(SCARD_IO_REQUEST) ||
        (receive_pci && receive_pci->cbPciLength < sizeof(SCARD_IO_REQUEST))) {
        return SCARD_E_INVALID_PARAMETER;
    }
    if (*receive_length == SCARD_AUTOALLOCATE) return SCARD_E_INVALID_PARAMETER;
    if (send_pci->dwProtocol != SCARD_PROTOCOL_T1) return SCARD_E_PROTO_MISMATCH;
    const auto response = card->local->transmit(ByteView{send, send_length});
    if (!response) return map_error(response.error());
    const DWORD capacity = *receive_length;
    *receive_length = static_cast<DWORD>(response.value().size());
    if (capacity < response.value().size()) return SCARD_E_INSUFFICIENT_BUFFER;
    std::copy(response.value().begin(), response.value().end(), receive);
    if (receive_pci) *receive_pci = g_rgSCardT1Pci;
    return SCARD_S_SUCCESS;
}

extern "C" LONG WINAPI SCardIsValidContext(SCARDCONTEXT handle)
{
    using namespace px4::userland::windows_card;
    const auto context = find_context(handle);
    if (!context) return ERROR_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(context->mutex);
    return context->released ? ERROR_INVALID_HANDLE : SCARD_S_SUCCESS;
}

extern "C" LONG WINAPI SCardFreeMemory(SCARDCONTEXT handle, LPCVOID memory)
{
    using namespace px4::userland::windows_card;
    const auto context = find_context(handle);
    if (!context) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    return free_memory(*context, memory) ? SCARD_S_SUCCESS : SCARD_E_INVALID_PARAMETER;
}

extern "C" LONG WINAPI SCardStatusW(SCARDHANDLE handle, LPWSTR names, LPDWORD name_length,
                                     LPDWORD state, LPDWORD protocol, LPBYTE atr, LPDWORD atr_length)
{
    using namespace px4::userland::windows_card;
    if ((names && !name_length) || (atr && !atr_length)) return SCARD_E_INVALID_PARAMETER;
    const auto card = find_card(handle);
    if (!card) return SCARD_E_INVALID_HANDLE;
    // Same ordering as ReleaseContext. Never acquire context after card.
    const auto context = card->owner;
    std::lock_guard<std::mutex> context_lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> card_lock(card->mutex);
    if (!card->active) return SCARD_E_INVALID_HANDLE;
    StatusSnapshot snapshot;
    const LONG result = card_status(*card, snapshot);
    if (result != SCARD_S_SUCCESS) return result;
    const DWORD name_capacity = names ? *name_length : 0U;
    const DWORD atr_capacity = atr ? *atr_length : 0U;
    if (name_length) *name_length = static_cast<DWORD>(snapshot.names.size());
    if (atr_length) *atr_length = static_cast<DWORD>(snapshot.atr.size());
    if (state) *state = snapshot.state;
    if (protocol) *protocol = snapshot.protocol;
    const bool allocate_names = names && name_capacity == SCARD_AUTOALLOCATE;
    const bool allocate_atr = atr && atr_capacity == SCARD_AUTOALLOCATE;
    if ((names && !allocate_names && name_capacity < snapshot.names.size()) ||
        (atr && !allocate_atr && atr_capacity < snapshot.atr.size())) return SCARD_E_INSUFFICIENT_BUFFER;
    void* name_memory = nullptr;
    void* atr_memory = nullptr;
    if (allocate_names) {
        name_memory = allocate_memory(*context, snapshot.names.size() * sizeof(wchar_t));
        if (!name_memory) return SCARD_E_NO_MEMORY;
    }
    if (allocate_atr) {
        atr_memory = allocate_memory(*context, snapshot.atr.size());
        if (!atr_memory) {
            if (name_memory) (void)free_memory(*context, name_memory);
            return SCARD_E_NO_MEMORY;
        }
    }
    if (names) {
        void* destination = allocate_names ? name_memory : names;
        std::memcpy(destination, snapshot.names.data(), snapshot.names.size() * sizeof(wchar_t));
        if (allocate_names) std::memcpy(names, &name_memory, sizeof(name_memory));
    }
    if (atr) {
        void* destination = allocate_atr ? atr_memory : atr;
        if (!snapshot.atr.empty()) std::memcpy(destination, snapshot.atr.data(), snapshot.atr.size());
        if (allocate_atr) std::memcpy(atr, &atr_memory, sizeof(atr_memory));
    }
    return SCARD_S_SUCCESS;
}

extern "C" LONG WINAPI SCardGetStatusChangeW(SCARDCONTEXT handle, DWORD timeout,
                                              LPSCARD_READERSTATEW states, DWORD count)
{
    using namespace px4::userland;
    using namespace px4::userland::windows_card;
    if (!states || count == 0U) return SCARD_E_INVALID_PARAMETER;
    // Only the pinned TvCas non-waiting, one-reader query is part of this ABI.
    // No silently shortened infinite waits, partial multi-reader results, or
    // invented cancellation semantics.
    if (timeout != 0U || count != 1U || states[0].dwCurrentState != SCARD_STATE_UNAWARE) {
        return SCARD_E_UNSUPPORTED_FEATURE;
    }
    if (!states[0].szReader) return SCARD_E_INVALID_PARAMETER;
    std::size_t length = 0U;
    while (length < kMaxReaderName && states[0].szReader[length] != 0) ++length;
    if (length == 0U || length == kMaxReaderName) return SCARD_E_INVALID_PARAMETER;
    const auto context = find_context(handle);
    if (!context) return SCARD_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->released) return SCARD_E_INVALID_HANDLE;
    std::vector<ReaderEntry<wchar_t>> readers;
    Error local_failure = Error::OK;
    const LONG listed = collect_readers(*context, readers, &local_failure);
    if (listed != SCARD_S_SUCCESS && listed != SCARD_E_NO_READERS_AVAILABLE) return listed;
    const std::wstring name(states[0].szReader, length);
    const auto found = std::find_if(readers.begin(), readers.end(),
                                   [&](const auto& entry) { return entry.name == name; });
    if (found != readers.end() && !found->instance.empty()) {
        const auto status = reader_status(context->root, found->instance);
        if (!status) return map_error(status.error());
        if (status.value().atr.size() > sizeof(states[0].rgbAtr)) return SCARD_F_COMM_ERROR;
        states[0].dwEventState = SCARD_STATE_CHANGED |
            (status.value().present ? SCARD_STATE_PRESENT : SCARD_STATE_EMPTY);
        states[0].cbAtr = static_cast<DWORD>(status.value().atr.size());
        std::memset(states[0].rgbAtr, 0, sizeof(states[0].rgbAtr));
        if (!status.value().atr.empty()) {
            std::memcpy(states[0].rgbAtr, status.value().atr.data(), status.value().atr.size());
        }
        return SCARD_S_SUCCESS;
    }
    const std::wstring prefix(std::begin(kReaderPrefix), std::end(kReaderPrefix) - 1);
    if (found == readers.end() && name.compare(0U, prefix.size(), prefix) == 0) {
        states[0].dwEventState = SCARD_STATE_UNKNOWN | SCARD_STATE_IGNORE | SCARD_STATE_CHANGED;
        states[0].cbAtr = 0U;
        return local_failure != Error::OK ? map_error(local_failure) : SCARD_E_UNKNOWN_READER;
    }
    const LONG available = ensure_native_context(*context);
    return available == SCARD_S_SUCCESS ? native_api().change_w(context->native, 0U, states, 1U) : available;
}
