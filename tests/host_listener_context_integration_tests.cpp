// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/enclosure_context.h"

#include "dtv/userland/control_client.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "px4/tuner_service.h"

#if defined(_WIN32)
#include "windows/windows_security.h"
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
        if (receiver == 2U && system == ipc::System::ISDB_T) {
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
    Result<void> start_capture(std::uint8_t, ipc::System) noexcept override
    {
        return fail_capture_start ? Result<void>::failure(Error::UNSUPPORTED)
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
    Result<void> attach(const TunerAttachment& attachment) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        identity_ = attachment;
        attached_ = true;
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
    bool attached_ = false;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    bool initialized() const noexcept override { return false; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtrBytes atr_{};
};

class Family final : public host_runtime::EnclosureFamily {
public:
    explicit Family(std::shared_ptr<std::atomic<bool>> shutdown,
                    bool fail_capture_start = false)
        : shutdown_(std::move(shutdown))
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
    list.receivers.value.count = 1U;
    auto& receiver = list.receivers.value.items[0U];
    receiver.id = 2U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    receiver.binding.value.local_id = 0U;
    list.readers.present = true;
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

bool lifecycle(bool fail_capture_start = false)
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
    const std::wstring runtime_path = std::filesystem::path(path).wstring();
    if (!px4::userland::windows_security::set_current_user_owner_and_dacl(
            runtime_path.c_str()) ||
        !px4::userland::windows_security::verify_runtime_parent(runtime_path.c_str())) {
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
    auto shutdown = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<Family> family(new Family(shutdown, fail_capture_start));
    ContextConfig config;
    config.runtime_directory = path;
    config.product = "listener-test";
    config.instance = "one";
    config.server.epoch = make_list().epoch;
    config.server.producer = make_list().producer;
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
        path, "listener-test", "one", host_runtime::EndpointAccess::private_user,
        -1, "control.sock"};
    auto control = host_runtime::connect_control(control_endpoint, Timeout{5000U});
    std::unique_ptr<wire::TimedClientTransport> data_transport;
    bool passed = false;
    const char* stage = "control connect";
    do {
        if (!control) break;
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
            path, "listener-test", "one", host_runtime::EndpointAccess::private_user,
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
            stage = "failed ATTACH other connection isolation";
            auto other_control = host_runtime::connect_control(
                control_endpoint, Timeout{5000U});
            if (!other_control) break;
            const auto status = w::fetch_status(*other_control.value(), Timeout{5000U});
            if (!status) break;
            auto other_data = host_runtime::connect_endpoint(
                data_endpoint, Timeout{5000U});
            if (!other_data) break;
            other_data.value()->close();
            other_control.value()->close();
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
    if (control) control.value()->close();
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
    if (fail_capture_start)
        return passed && !closed && closed.error() == Error::INTERNAL &&
            !shutdown->load(std::memory_order_acquire);
    return passed && closed && closed.value() == CloseState::quiesced &&
        shutdown->load(std::memory_order_acquire);
}

}  // namespace

int main()
{
    return lifecycle() && lifecycle(true) ? 0 : 1;
}
