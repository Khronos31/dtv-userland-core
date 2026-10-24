// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/enclosure_context.h"

#include "dtv/userland/control_client.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "px4/tuner_service.h"

#if defined(_WIN32)
#include "windows/windows_security.h"
#else
#include "px4/posix_ipc.h"
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::host_listener_context;
using namespace px4::userland;
namespace w = dtv::userland::wire;

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> result;
    while (value[result.length] != '\0') {
        result.bytes[result.length] = value[result.length];
        ++result.length;
    }
    return result;
}

class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 8U; }
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t, std::uint64_t, std::uint32_t) noexcept override
    {
        return Result<TunerHzProgramming>::success({});
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        TunerStrictProfile profile;
        if ((receiver == 2U || receiver == 3U) && system == ipc::System::ISDB_T) {
            profile.native_hz = true;
            profile.min_hz = 400000000U;
            profile.max_hz = 800000000U;
        }
        return profile;
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> start_capture(std::uint8_t receiver, ipc::System) noexcept override
    {
        return fail_capture_start && receiver == 2U
            ? Result<void>::failure(Error::UNSUPPORTED)
            : Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, ipc::System) noexcept override
    {
        return Result<void>::success();
    }
    bool fail_capture_start = false;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success({{1U}});
    }
};

class Clock final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t duration) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    }
};

class Stream final : public TunerStreamControl {
public:
    explicit Stream(bool missing_final_when_unattached = false) noexcept
        : missing_final_when_unattached_(missing_final_when_unattached)
    {
    }
    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        identity_ = attachment;
        attached_ = true;
        ever_attached_ = true;
        packets_remaining_ = 1U;
        counters_ = TunerStreamCounters{};
        counters_.packets = 1U;
        counters_.bytes = 188U;
        final_ = TunerStreamFinalSnapshot{};
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        attached_ = false;
        final_.counters = counters_;
        final_.terminal = static_cast<std::uint8_t>(TunerStreamTerminal::stopped);
        return Result<void>::success();
    }
    Result<TunerStreamCounters> stats(const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return same(attachment) ? Result<TunerStreamCounters>::success(counters_)
                                : Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& attachment) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!attached_ && missing_final_when_unattached_ && !ever_attached_)
            return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
        return same(attachment) ? Result<TunerStreamFinalSnapshot>::success(final_)
                                : Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }
    Result<void> release_final(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return same(attachment) ? Result<void>::success()
                                : Result<void>::failure(Error::NOT_FOUND);
    }
    Result<TunerStreamReadResult> read(const TunerAttachment& attachment,
                                        MutableByteView output,
                                        Timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!same(attachment)) return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
        if (packets_remaining_ != 0U && output.size >= 188U) {
            for (std::size_t i = 0U; i < 188U; ++i)
                output.data[i] = static_cast<std::uint8_t>(i);
            --packets_remaining_;
            return Result<TunerStreamReadResult>::success({188U, false, false,
                TunerStreamTerminal::none});
        }
        return Result<TunerStreamReadResult>::success({0U, !attached_, attached_,
            attached_ ? TunerStreamTerminal::none : TunerStreamTerminal::stopped});
    }
private:
    bool same(const TunerAttachment& other) const noexcept
    {
        return identity_.owner_client_id == other.owner_client_id &&
            identity_.lease_id == other.lease_id &&
            identity_.attachment_id == other.attachment_id &&
            identity_.nonce == other.nonce;
    }
    mutable std::mutex mutex_;
    TunerAttachment identity_{};
    TunerStreamCounters counters_{};
    TunerStreamFinalSnapshot final_{};
    std::size_t packets_remaining_ = 0U;
    bool missing_final_when_unattached_ = false;
    bool ever_attached_ = false;
    bool attached_ = false;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(true); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        initialized_ = true;
        atr_.bytes[0U] = 0x3bU;
        atr_.bytes[1U] = 0x00U;
        atr_.length = 2U;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView, MutableByteView response) noexcept override
    {
        if (response.data == nullptr || response.size < 2U)
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        response.data[0U] = 0x90U;
        response.data[1U] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override
    {
        initialized_ = false;
        atr_ = CardAtrBytes{};
    }
private:
    CardAtrBytes atr_{};
    bool initialized_ = false;
};

class Family final : public host_runtime::EnclosureFamily {
public:
    explicit Family(std::shared_ptr<std::atomic<bool>> shutdown,
                    bool fail_capture_start = false,
                    bool missing_final_when_unattached = false)
        : shutdown_(std::move(shutdown)),
          stream_(missing_final_when_unattached)
    {
        tuner_.fail_capture_start = fail_capture_start;
    }
    Result<void> shutdown() noexcept override
    {
        shutdown_->store(true, std::memory_order_release);
        return Result<void>::success();
    }
    TunerServiceBackend& tuner_backend() noexcept override { return tuner_; }
    CardServiceBackend& card_backend() noexcept override { return card_; }
    CardProtocolSession& card_protocol() noexcept override { return card_session_; }
    TunerServiceTime& time() noexcept override { return clock_; }
    TunerStreamControl* stream_control() noexcept override { return &stream_; }
private:
    std::shared_ptr<std::atomic<bool>> shutdown_;
    Backend tuner_;
    CardBackend card_;
    CardSession card_session_;
    Clock clock_;
    Stream stream_;
};

w::ListResponse make_list()
{
    w::ListResponse list;
    for (std::size_t i = 0U; i < list.epoch.size(); ++i)
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    list.producer.product = text<32U>("listener-test");
    list.producer.version = text<64U>("1");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 2U;
    auto& receiver = list.receivers.value.items[0U];
    receiver.id = 2U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    receiver.binding.value.local_id = 0U;
    auto& other_receiver = list.receivers.value.items[1U];
    other_receiver.id = 3U;
    other_receiver.systems.mask = 1U;
    other_receiver.binding.present = true;
    other_receiver.binding.value.function_id = 1U;
    other_receiver.binding.value.local_id = 1U;
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    return list;
}

bool read_frame(w::TimedClientTransport& transport, w::Frame& frame) noexcept
{
    std::array<std::uint8_t, w::kHeaderSize> header_bytes{};
    const auto read_exact = [&transport](std::uint8_t* bytes,
                                          std::size_t size) noexcept {
        std::size_t offset = 0U;
        while (offset < size) {
            const auto read = transport.read_some({bytes + offset, size - offset},
                                                   Timeout{5000U});
            if (!read || read.value() == 0U) return false;
            offset += read.value();
        }
        return true;
    };
    if (!read_exact(header_bytes.data(), header_bytes.size())) return false;
    const auto header = w::decode_header({header_bytes.data(), header_bytes.size()});
    if (!header || header.value().payload_length > w::kMaxTsDataPayload) return false;
    static std::array<std::uint8_t, w::kMaxTsDataPayload> payload{};
    if (!read_exact(payload.data(), header.value().payload_length)) return false;
    frame.header = header.value();
    frame.payload = {payload.data(), header.value().payload_length};
    return true;
}

template <typename Request>
bool write_record(w::TimedClientTransport& transport, std::uint16_t opcode,
                  std::uint32_t request_id, const Request& request,
                  Result<std::size_t> (*encode)(const Request&, MutableByteView) noexcept)
{
    std::array<std::uint8_t, w::kHeaderSize + w::kMaxControlPayload> bytes{};
    const auto size = w::encode_record_frame(w::Kind::request, opcode, request_id,
                                             request, encode,
                                             {bytes.data(), bytes.size()});
    if (!size) return false;
    return transport.write_all({bytes.data(), size.value()}, Timeout{5000U}).has_value();
}

bool lifecycle(bool fail_capture_start = false,
               bool missing_final_when_unattached = false,
               bool default_runtime_root = false)
{
#if defined(_WIN32)
    const char* runtime_parent = std::getenv("LOCALAPPDATA");
    if (runtime_parent == nullptr || runtime_parent[0] == '\0') {
        std::fprintf(stderr, "LOCALAPPDATA is not set for protected runtime parent\n");
        return false;
    }
    const auto path = (std::filesystem::path(runtime_parent) /
        ("dtv-context-" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()))).string();
#else
    const auto path = (std::filesystem::temp_directory_path() /
        ("dtv-context-" + std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count()))).string();
#endif
    std::error_code fs_error;
    if (!std::filesystem::create_directory(path, fs_error) || fs_error) {
        std::fprintf(stderr, "runtime directory create error=%u\n",
                     static_cast<unsigned>(fs_error.value()));
        return false;
    }
#if defined(_WIN32)
    const std::wstring runtime_path_wide = std::filesystem::path(path).wstring();
    if (!px4::userland::windows_security::set_current_user_owner_and_dacl(
            runtime_path_wide.c_str()) ||
        !px4::userland::windows_security::verify_runtime_parent(runtime_path_wide.c_str())) {
        std::fprintf(stderr, "runtime directory DACL setup/readback failed\n");
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
#else
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, fs_error);
    if (fs_error) {
        std::fprintf(stderr, "runtime directory permissions error=%u\n",
                     static_cast<unsigned>(fs_error.value()));
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
#endif
#if !defined(_WIN32)
    class EnvironmentValue final {
    public:
        explicit EnvironmentValue(const char* name) : name_(name)
        {
            const char* value = std::getenv(name);
            had_value_ = value != nullptr;
            if (had_value_) value_ = value;
        }
        ~EnvironmentValue()
        {
            if (had_value_) (void)::setenv(name_.c_str(), value_.c_str(), 1);
            else (void)::unsetenv(name_.c_str());
        }
        bool set(const char* value) const { return ::setenv(name_.c_str(), value, 1) == 0; }
    private:
        std::string name_;
        std::string value_;
        bool had_value_ = false;
    } saved_xdg("XDG_RUNTIME_DIR"), saved_tmp("TMPDIR");
    const std::string runtime_path = default_runtime_root
        ? path + "/dtv-userland-" +
            std::to_string(static_cast<unsigned long>(::geteuid()))
        : path;
    if (default_runtime_root) {
        const std::string missing_xdg = path + "/missing-xdg";
        if (!saved_xdg.set(missing_xdg.c_str()) || !saved_tmp.set(path.c_str())) {
            std::filesystem::remove_all(path, fs_error);
            return false;
        }
    }
#else
    if (default_runtime_root) {
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
    const std::string runtime_path = path;
#endif
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<Family> family(new Family(shutdown, fail_capture_start,
                                               missing_final_when_unattached));
    ContextConfig config;
    if (!default_runtime_root) config.runtime_directory = runtime_path;
    config.product = "listener-test";
    config.instance = "one";
    config.server.epoch = make_list().epoch;
    config.server.producer = make_list().producer;
    config.server.features = w::kFeatureCardService;
    config.canonical_list = make_list();
    config.context.epoch = config.server.epoch;
    config.stream_engine_epoch = 0x5349414e4fULL;
    config.stream_arm_budget_ms = 5000U;
    auto assembled = EnclosureContext::assemble(std::move(family),
        std::unique_ptr<host_runtime::TunerNonceSource>(new Nonce()), config);
    if (!assembled) {
        std::fprintf(stderr, "Context assemble error=%u\n",
                     static_cast<unsigned>(assembled.error()));
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
    if (const auto reserved = assembled.value()->reserve_namespace(); !reserved) {
        std::fprintf(stderr, "Context namespace reserve error=%u\n",
                     static_cast<unsigned>(reserved.error()));
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
    if (const auto published = assembled.value()->publish(); !published) {
        std::fprintf(stderr, "Context publish error=%u\n",
                     static_cast<unsigned>(published.error()));
        std::filesystem::remove_all(path, fs_error);
        return false;
    }
    auto context = std::move(assembled.value());
    std::atomic<bool> serving{true};
    std::atomic<unsigned> poll_errors{0U};
    std::atomic<unsigned> last_poll_error{0U};
    std::unique_ptr<w::TimedClientTransport> surviving_control;
    std::unique_ptr<w::TimedClientTransport> same_receiver_control_transport;
    std::unique_ptr<w::TimedClientTransport> other_receiver_control_transport;
    std::unique_ptr<w::TimedClientTransport> other_data_transport;
    std::thread server([&] {
        while (serving.load(std::memory_order_acquire)) {
            const auto result = context->poll_once(Timeout{100U});
            if (!result) {
                poll_errors.fetch_add(1U, std::memory_order_relaxed);
                last_poll_error.store(static_cast<unsigned>(result.error()),
                                      std::memory_order_relaxed);
            }
        }
    });
    const host_runtime::EndpointConfig control_endpoint{
        runtime_path, "listener-test", "one", host_runtime::EndpointAccess::private_user,
        -1, "control.sock"};
    bool default_client_ok = true;
#if !defined(_WIN32)
    if (default_runtime_root) {
        const px4::userland::ipc::posix::EndpointConfig default_endpoint{
            nullptr, "one", "control.sock",
            px4::userland::ipc::posix::EndpointAccess::private_user,
            "listener-test"};
        auto default_stream = px4::userland::ipc::posix::SocketStream::connect(
            default_endpoint, Timeout{5000U});
        default_client_ok = static_cast<bool>(default_stream);
        if (default_stream) default_stream.value().close();
    }
#endif
    auto control = host_runtime::connect_control(control_endpoint, Timeout{5000U});
    std::unique_ptr<wire::TimedClientTransport> data_transport;
    bool passed = false;
    const char* stage = default_client_ok ? "control connect" :
        "default runtime root client connect";
    do {
        if (!default_client_ok || !control) break;
        stage = "CARD status/connect/reset/APDU/disconnect";
        auto card_control = host_runtime::connect_control(control_endpoint, Timeout{5000U});
        if (!card_control) break;
        w::CardClientOptions card_options;
        card_options.reader_id = 0U;
        const auto card = w::begin_card(*card_control.value(), card_options,
                                        Timeout{5000U});
        if (!card) {
            std::fprintf(stderr, "card connect error=%u remote=%u\n",
                static_cast<unsigned>(card.error), card.remote_error_code);
            break;
        }
        auto card_session = card.session;
        stage = "CARD HANDLE_STATUS";
        w::CardHandleStatusRequest handle_status{};
        handle_status.key.epoch = card_session.epoch;
        handle_status.key.reader_id = card_session.reader_id;
        handle_status.key.bound_generation = card_session.bound_generation;
        handle_status.key.card_handle = card_session.card_handle;
        w::Frame card_frame;
        if (!write_record(*card_control.value(), w::kOpcodeCardHandleStatus, 6U,
                          handle_status, &w::encode_card_handle_status_request) ||
            !read_frame(*card_control.value(), card_frame) ||
            card_frame.header.opcode != w::kOpcodeCardHandleStatus ||
            !w::decode_card_session_result(card_frame.payload)) break;
        w::BeginTransactionRequest begin_transaction{};
        stage = "CARD BEGIN_TRANSACTION";
        begin_transaction.key = handle_status.key;
        if (!write_record(*card_control.value(), w::kOpcodeBeginTransaction, 7U,
                          begin_transaction, &w::encode_begin_transaction_request) ||
            !read_frame(*card_control.value(), card_frame) ||
            card_frame.header.opcode != w::kOpcodeBeginTransaction ||
            !w::decode_begin_transaction_response(card_frame.payload)) break;
        w::EndTransactionRequest end_transaction{};
        stage = "CARD END_TRANSACTION";
        end_transaction.key = handle_status.key;
        if (!write_record(*card_control.value(), w::kOpcodeEndTransaction, 8U,
                          end_transaction, &w::encode_end_transaction_request) ||
            !read_frame(*card_control.value(), card_frame) ||
            card_frame.header.opcode != w::kOpcodeEndTransaction ||
            card_frame.payload.size != 0U) break;
        w::CardReconnectRequest reconnect{};
        stage = "CARD RECONNECT";
        reconnect.key = handle_status.key;
        reconnect.preferred_protocols = 1U;
        if (!write_record(*card_control.value(), w::kOpcodeCardReconnect, 9U,
                          reconnect, &w::encode_card_reconnect_request) ||
            !read_frame(*card_control.value(), card_frame) ||
            card_frame.header.opcode != w::kOpcodeCardReconnect ||
            !w::decode_card_session_result(card_frame.payload)) break;
        card_session.next_request_id = 10U;
        stage = "CARD RESET";
        const auto card_reset = w::card_reset(*card_control.value(), card_session,
                                               Timeout{5000U});
        if (!card_reset) {
            std::fprintf(stderr, "card reset error=%u remote=%u\n",
                static_cast<unsigned>(card_reset.error), card_reset.remote_error_code);
            break;
        }
        const std::array<std::uint8_t, 4U> apdu{{0x00U, 0x84U, 0x00U, 0x00U}};
        const auto card_response = w::card_transmit(*card_control.value(), card_session,
            w::CardProtocol::t1, ByteView{apdu.data(), apdu.size()}, Timeout{5000U});
        if (!card_response || card_response.response.response.length != 2U ||
            card_response.response.response.bytes[0U] != 0x90U ||
            card_response.response.response.bytes[1U] != 0x00U) {
            std::fprintf(stderr, "card APDU error=%u remote=%u\n",
                static_cast<unsigned>(card_response.error), card_response.remote_error_code);
            break;
        }
        const auto card_disconnect = w::card_disconnect(*card_control.value(), card_session,
            w::Disposition::leave, Timeout{5000U});
        if (!card_disconnect || !card_disconnect.response.consumed ||
            !card_disconnect.response.quiesced) {
            std::fprintf(stderr, "card disconnect error=%u remote=%u\n",
                static_cast<unsigned>(card_disconnect.error), card_disconnect.remote_error_code);
            break;
        }
        card_options.share = w::Share::exclusive;
        auto abandoned_control = host_runtime::connect_control(
            control_endpoint, Timeout{5000U});
        if (!abandoned_control) break;
        const auto abandoned_card = w::begin_card(*abandoned_control.value(), card_options,
                                                   Timeout{5000U});
        if (!abandoned_card) break;
        abandoned_control.value()->close();
        bool recovered = false;
        for (unsigned attempt = 0U; attempt < 100U && !recovered; ++attempt) {
            auto recovery_control = host_runtime::connect_control(
                control_endpoint, Timeout{5000U});
            if (!recovery_control) break;
            const auto recovery_card = w::begin_card(*recovery_control.value(), card_options,
                                                      Timeout{5000U});
            if (recovery_card) {
                auto recovery_session = recovery_card.session;
                const auto released = w::card_disconnect(*recovery_control.value(),
                    recovery_session, w::Disposition::leave, Timeout{5000U});
                recovered = released && released.response.consumed &&
                            released.response.quiesced;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!recovered) {
            std::fprintf(stderr, "card owner was not released after control disconnect\n");
            break;
        }
        stage = "HELLO/LIST/STATUS/ACQUIRE/TUNE/START";
        w::RecordingOptions options;
        options.receiver_id = 2U;
        options.tune.epoch = config.server.epoch;
        options.tune.system = w::System::isdb_t;
        options.tune.frequency_hz = 473142857U;
        options.tune.timeout_ms = 1000U;
        auto recording = w::begin_recording(*control.value(), options, Timeout{10000U});
        if (!recording) {
            std::fprintf(stderr, "recording error=%u remote=%u\n",
                static_cast<unsigned>(recording.error), recording.remote_error_code);
            break;
        }
        stage = "data connect";
        const host_runtime::EndpointConfig data_endpoint{
            runtime_path, "listener-test", "one", host_runtime::EndpointAccess::private_user,
            -1, "stream.sock"};
        auto data = host_runtime::connect_endpoint(data_endpoint, Timeout{5000U});
        if (!data) break;
        stage = "ATTACH encode/write";
        data_transport = std::move(data.value());
        w::AttachRequest attach;
        attach.epoch = recording.session.epoch;
        attach.lease_id = recording.session.lease_id;
        attach.stream_id = recording.session.stream_id;
        attach.attach_token = recording.session.attach_token;
        std::array<std::uint8_t, 64U> attach_payload{};
        const auto attach_size = w::encode_attach_request(attach,
            {attach_payload.data(), attach_payload.size()});
        std::array<std::uint8_t, w::kHeaderSize + 64U> attach_frame{};
        if (!attach_size || !w::encode_header({w::Kind::request, w::kOpcodeAttachStream,
                1U, static_cast<std::uint32_t>(attach_size.value())},
                {attach_frame.data(), w::kHeaderSize})) break;
        std::memcpy(attach_frame.data() + w::kHeaderSize, attach_payload.data(),
                    attach_size.value());
        if (!data_transport->write_all({attach_frame.data(), w::kHeaderSize + attach_size.value()},
                                       Timeout{5000U})) break;
        stage = "ATTACH response";
        w::Frame attached_frame;
        if (!read_frame(*data_transport, attached_frame) ||
            attached_frame.header.opcode != w::kOpcodeAttachStream) {
            std::fprintf(stderr, "attach frame kind=%u opcode=%u len=%zu\n",
                static_cast<unsigned>(attached_frame.header.kind),
                attached_frame.header.opcode, attached_frame.payload.size);
            break;
        }
        if (fail_capture_start) {
            if (attached_frame.header.kind != w::Kind::error_response) break;
            const auto error = w::decode_error_response(attached_frame.payload);
            if (!error || error.value().error.code !=
                    static_cast<std::uint32_t>(Error::UNSUPPORTED)) break;
            data_transport->close();
            data_transport.reset();
            stage = "failed ATTACH owner disconnect";
            auto other_control = host_runtime::connect_control(
                control_endpoint, Timeout{5000U});
            if (!other_control) break;
            control.value()->close();
            control.value().reset();
            surviving_control = std::move(other_control.value());
            const auto status = w::fetch_status(*surviving_control, Timeout{5000U});
            if (!status) break;

            stage = "failed receiver lease remains isolated";
            auto same_receiver_control = host_runtime::connect_control(
                control_endpoint, Timeout{5000U});
            if (!same_receiver_control) break;
            same_receiver_control_transport =
                std::move(same_receiver_control.value());
            w::RecordingOptions same_receiver_options;
            same_receiver_options.receiver_id = 2U;
            same_receiver_options.tune.epoch = config.server.epoch;
            same_receiver_options.tune.system = w::System::isdb_t;
            same_receiver_options.tune.frequency_hz = 473142857U;
            same_receiver_options.tune.timeout_ms = 1000U;
            const auto blocked = w::begin_recording(*same_receiver_control_transport,
                same_receiver_options, Timeout{5000U});
            if (blocked || blocked.error != Error::BUSY ||
                blocked.remote_error_code != static_cast<std::uint32_t>(Error::BUSY))
                break;

            stage = "other receiver control connect";
            auto other_receiver_control = host_runtime::connect_control(
                control_endpoint, Timeout{5000U});
            if (!other_receiver_control) break;
            other_receiver_control_transport =
                std::move(other_receiver_control.value());
            stage = "other receiver begin recording";
            w::RecordingOptions other_options = same_receiver_options;
            other_options.receiver_id = 3U;
            auto other_recording = w::begin_recording(
                *other_receiver_control_transport, other_options, Timeout{10000U});
            if (!other_recording) {
                std::fprintf(stderr, "other receiver begin error=%u remote=%u\n",
                    static_cast<unsigned>(other_recording.error),
                    other_recording.remote_error_code);
                break;
            }
            stage = "other receiver data connect";
            auto other_data = host_runtime::connect_endpoint(
                data_endpoint, Timeout{5000U});
            if (!other_data) break;
            other_data_transport = std::move(other_data.value());
            stage = "other receiver attach write";
            w::AttachRequest other_attach;
            other_attach.epoch = other_recording.session.epoch;
            other_attach.lease_id = other_recording.session.lease_id;
            other_attach.stream_id = other_recording.session.stream_id;
            other_attach.attach_token = other_recording.session.attach_token;
            std::array<std::uint8_t, 64U> other_attach_payload{};
            const auto other_attach_size = w::encode_attach_request(other_attach,
                {other_attach_payload.data(), other_attach_payload.size()});
            std::array<std::uint8_t, w::kHeaderSize + 64U> other_attach_frame{};
            if (!other_attach_size || !w::encode_header({w::Kind::request,
                    w::kOpcodeAttachStream, 1U,
                    static_cast<std::uint32_t>(other_attach_size.value())},
                    {other_attach_frame.data(), w::kHeaderSize})) break;
            std::memcpy(other_attach_frame.data() + w::kHeaderSize,
                        other_attach_payload.data(), other_attach_size.value());
            if (!other_data_transport->write_all(
                    {other_attach_frame.data(), w::kHeaderSize + other_attach_size.value()},
                    Timeout{5000U})) break;
            stage = "other receiver attach response";
            w::Frame other_attach_response;
            if (!read_frame(*other_data_transport, other_attach_response) ||
                other_attach_response.header.kind != w::Kind::response ||
                other_attach_response.header.opcode != w::kOpcodeAttachStream) break;
            const auto other_attached = w::decode_attach_response(
                other_attach_response.payload);
            if (!other_attached ||
                other_attached.value().stream_id != other_recording.session.stream_id) break;
            stage = "other receiver TS_DATA";
            w::Frame other_ts_frame;
            if (!read_frame(*other_data_transport, other_ts_frame) ||
                other_ts_frame.header.kind != w::Kind::event ||
                other_ts_frame.header.opcode != w::kOpcodeTsDataEvent) break;
            const auto other_ts = w::decode_ts_data(other_ts_frame.payload);
            if (!other_ts || other_ts.value().stream_id !=
                    other_recording.session.stream_id ||
                other_ts.value().packets.size != 188U) break;
            passed = true;
            break;
        }
        if (attached_frame.header.kind != w::Kind::response) break;
        const auto attached = w::decode_attach_response(attached_frame.payload);
        if (!attached || attached.value().stream_id != recording.session.stream_id) break;
        stage = "TS_DATA";
        w::Frame ts_frame;
        if (!read_frame(*data_transport, ts_frame) || ts_frame.header.kind != w::Kind::event ||
            ts_frame.header.opcode != w::kOpcodeTsDataEvent) break;
        const auto ts = w::decode_ts_data(ts_frame.payload);
        if (!ts || ts.value().stream_id != recording.session.stream_id ||
            ts.value().packets.size != 188U || ts.value().packets.data[187U] != 187U) break;
        stage = "STOP";
        const auto stopped = w::stop_recording(*control.value(), recording.session,
                                                Timeout{5000U});
        if (!stopped || stopped.response.final.id != recording.session.stream_id) break;
        stage = "STREAM_END";
        w::Frame end_frame;
        if (!read_frame(*data_transport, end_frame) || end_frame.header.kind != w::Kind::event ||
            end_frame.header.opcode != w::kOpcodeStreamEndEvent) break;
        const auto ended = w::decode_stream_end_event(end_frame.payload);
        if (!ended || ended.value().final.id != recording.session.stream_id) break;
        stage = "RELEASE";
        const auto released = w::release_recording(*control.value(), recording.session,
                                                    Timeout{5000U});
        if (!released) break;
        passed = true;
    } while (false);
    if (data_transport) data_transport->close();
    if (control && control.value()) control.value()->close();
    serving.store(false, std::memory_order_release);
    const auto closed = context->close(Timeout{5000U});
    server.join();
    context.reset();
    std::filesystem::remove_all(path, fs_error);
    if (!passed) std::fprintf(stderr, "FAIL: %s\n", stage);
    if (!passed) std::fprintf(stderr, "poll errors=%u\n",
        poll_errors.load(std::memory_order_relaxed));
    if (!passed) std::fprintf(stderr, "last poll error=%u\n",
        last_poll_error.load(std::memory_order_relaxed));
    if (passed && (!closed || closed.value() != CloseState::quiesced ||
        !shutdown->load(std::memory_order_acquire)))
        std::fprintf(stderr, "close result=%u state=%u family=%u\n",
            static_cast<unsigned>(closed.error()),
            closed ? static_cast<unsigned>(closed.value()) : 255U,
            static_cast<unsigned>(shutdown->load(std::memory_order_acquire)));
    return passed && closed && closed.value() == CloseState::quiesced &&
        shutdown->load(std::memory_order_acquire);
}

}  // namespace

int main()
{
#if defined(_WIN32)
    return lifecycle() && lifecycle(true, false) && lifecycle(true, true) ? 0 : 1;
#else
    return lifecycle() && lifecycle(true, false) && lifecycle(true, true) &&
        lifecycle(false, false, true) ? 0 : 1;
#endif
}
