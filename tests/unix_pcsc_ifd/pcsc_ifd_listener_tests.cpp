// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/enclosure_context.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/unix_pcsc_ifd/pcsc_ifd_adapter.h"
#include "dtv/userland/wire.h"
#include "px4/card_service.h"
#include "px4/tuner_service.h"

extern "C" {
#include <ifdhandler.h>
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace w = dtv::userland::wire;
namespace hr = dtv::userland::host_runtime;
namespace hlc = dtv::userland::host_listener_context;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;
using dtv::userland::Timeout;
using namespace px4::userland;

#if defined(DTV_USERLAND_UNIX_PCSC_ASICEN_PROFILE)
constexpr const auto& kProfile = dtv::userland::unix_pcsc_ifd::kAsicenIfdProfile;
#else
constexpr const auto& kProfile = dtv::userland::unix_pcsc_ifd::kPx4IfdProfile;
#endif

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

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

class TempRuntime final {
public:
    TempRuntime()
    {
        std::string pattern = "/tmp/dtv-core-ifd-XXXXXX";
        char* made = ::mkdtemp(pattern.data());
        if (made != nullptr) {
            path_ = made;
            valid_ = ::chmod(path_.c_str(), 0700) == 0;
        }
    }
    ~TempRuntime() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }
private:
    std::string path_;
    bool valid_ = false;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override
    { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override
    { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    { return Result<bool>::success(present.load()); }
    std::atomic<bool> present{true};
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        if (!backend_ || !backend_->present.load())
            return Result<void>::failure(Error::NO_CARD);
        initialized_ = true;
        atr_.bytes[0U] = 0x3bU;
        atr_.bytes[1U] = 0x00U;
        atr_.length = 2U;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        if (!initialized_ || apdu.size == 0U || response.data == nullptr ||
            response.size < 2U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        ++transmits;
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
    void bind(CardBackend& backend) noexcept { backend_ = &backend; }
    std::atomic<std::size_t> transmits{0U};
private:
    CardBackend* backend_ = nullptr;
    CardAtrBytes atr_{};
    bool initialized_ = false;
};

class TunerBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> value{};
        value[0U] = 1U;
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(value);
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
    { std::this_thread::sleep_for(std::chrono::milliseconds(duration)); }
};

class Family final : public hr::EnclosureFamily {
public:
    Family(CardBackend& card, CardSession& session, TunerBackend& tuner,
           Clock& clock) noexcept
        : card_(card), session_(session), tuner_(tuner), clock_(clock) {}
    TunerServiceBackend& tuner_backend() noexcept override { return tuner_; }
    CardServiceBackend& card_backend() noexcept override { return card_; }
    CardProtocolSession& card_protocol() noexcept override { return session_; }
    TunerServiceTime& time() noexcept override { return clock_; }
    TunerStreamControl* stream_control() noexcept override { return nullptr; }
private:
    CardBackend& card_;
    CardSession& session_;
    TunerBackend& tuner_;
    Clock& clock_;
};

w::ListResponse make_list()
{
    w::ListResponse list{};
    for (std::size_t i = 0U; i < list.epoch.size(); ++i)
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    list.producer.product = text<32U>(kProfile.product);
    list.producer.version = text<64U>("test");
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
    receiver.id = 0U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    return list;
}

hlc::ContextConfig make_config(const std::string& root, const char* instance)
{
    hlc::ContextConfig config{};
    config.runtime_directory = root;
    config.product = kProfile.product;
    config.instance = instance;
    config.canonical_list = make_list();
    config.server.epoch = config.canonical_list.epoch;
    config.server.producer = config.canonical_list.producer;
    config.server.features = w::kFeatureStateEvent | w::kFeatureCardService;
    config.context.epoch = config.server.epoch;
    config.context.state = w::ContextState::ready;
    config.context.instance.present = true;
    config.context.instance.value =
        text<dtv::userland::detail::kMaxInstanceBytes>(instance);
    config.stream_engine_epoch = 42U;
    config.stream_arm_budget_ms = 5000U;
    return config;
}

class ServerRunner final {
public:
    explicit ServerRunner(hlc::EnclosureContext& context) noexcept : context_(context)
    {
        thread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                if (!context_.poll_once(Timeout{10U})) {
                    healthy_.store(false, std::memory_order_release);
                    return;
                }
            }
        });
    }
    ~ServerRunner() noexcept { stop_and_join(); }
    void stop_and_join() noexcept
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
    bool healthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
private:
    hlc::EnclosureContext& context_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> healthy_{true};
    std::thread thread_;
};

bool transmit(std::size_t& response_count)
{
    const std::array<UCHAR, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<UCHAR, 16U> response{};
    DWORD response_length = static_cast<DWORD>(response.size());
    const SCARD_IO_HEADER send_pci{1U, sizeof(SCARD_IO_HEADER)};
    SCARD_IO_HEADER receive_pci{};
    const RESPONSECODE result = IFDHTransmitToICC(
        0U, send_pci, const_cast<UCHAR*>(apdu.data()),
        static_cast<DWORD>(apdu.size()), response.data(), &response_length,
        &receive_pci);
    if (result == IFD_SUCCESS && response_length == 2U &&
        response[0U] == 0x90U && response[1U] == 0x00U) {
        ++response_count;
        return true;
    }
    return false;
}

bool test_listener_reinsert()
{
    constexpr const char* instance = "ifd-reinsert-test";
    TempRuntime runtime;
    CHECK(runtime.valid());

    CardBackend card;
    CardSession session;
    session.bind(card);
    TunerBackend tuner;
    Clock clock;
    std::unique_ptr<hr::EnclosureFamily> family(
        new Family(card, session, tuner, clock));
    const auto config = make_config(runtime.path(), instance);
    auto assembled = hlc::EnclosureContext::assemble(
        std::move(family), std::unique_ptr<hr::TunerNonceSource>(new Nonce()), config);
    if (!assembled) {
        std::fprintf(stderr, "EnclosureContext::assemble error=%u\n",
                     static_cast<unsigned>(assembled.error()));
    }
    CHECK(assembled);
    auto context = std::move(assembled.value());
    CHECK(context->reserve_namespace());
    CHECK(context->publish());
    ServerRunner runner(*context);

    std::string device_name = std::string(kProfile.device_prefix) +
        "runtime=" + runtime.path() + ":instance=" + instance + ":access=user";
    CHECK(IFDHCreateChannelByName(0U, device_name.data()) == IFD_SUCCESS);
    CHECK(IFDHICCPresence(0U) == IFD_ICC_PRESENT);

    std::array<UCHAR, MAX_ATR_SIZE> atr{};
    DWORD atr_length = static_cast<DWORD>(atr.size());
    CHECK(IFDHPowerICC(0U, IFD_POWER_UP, atr.data(), &atr_length) == IFD_SUCCESS);
    CHECK(atr_length != 0U);
    CHECK(IFDHSetProtocolParameters(0U, SCARD_PROTOCOL_T1, 0U, 0U, 0U, 0U) ==
          IFD_SUCCESS);

    std::size_t apdu_count = 0U;
    CHECK(transmit(apdu_count));
    CHECK(apdu_count == 1U && session.transmits.load() == 1U);

    card.present.store(false, std::memory_order_release);
    CHECK(IFDHICCPresence(0U) == IFD_ICC_NOT_PRESENT);
    const std::size_t after_removal = session.transmits.load();
    CHECK(!transmit(apdu_count));
    CHECK(apdu_count == 1U && session.transmits.load() == after_removal);

    const hr::EndpointConfig endpoint{runtime.path(), kProfile.product, instance,
                                      hr::EndpointAccess::private_user};
    auto exclusive_transport = hr::connect_control(endpoint, Timeout{5000U});
    CHECK(exclusive_transport);
    w::CardClientOptions exclusive_options{};
    exclusive_options.reader_id = 0U;
    exclusive_options.share = w::Share::exclusive;
    exclusive_options.expected_product = {
        reinterpret_cast<const std::uint8_t*>(kProfile.product),
        std::strlen(kProfile.product)};
    exclusive_options.expected_instance = {
        reinterpret_cast<const std::uint8_t*>(instance), std::strlen(instance)};
    auto exclusive = w::begin_card(*exclusive_transport.value(), exclusive_options,
                                   Timeout{5000U});
    if (exclusive) {
        auto exclusive_session = exclusive.session;
        const auto disconnected = w::card_disconnect(
            *exclusive_transport.value(), exclusive_session, w::Disposition::leave,
            Timeout{5000U});
        CHECK(disconnected && disconnected.response.consumed &&
              disconnected.response.quiesced);
    } else {
        CHECK(exclusive.error == Error::NO_CARD);
    }
    exclusive_transport.value()->close();

    card.present.store(true, std::memory_order_release);
    CHECK(IFDHICCPresence(0U) == IFD_ICC_PRESENT);
    const std::size_t before_reset = session.transmits.load();
    CHECK(!transmit(apdu_count));
    CHECK(apdu_count == 1U && session.transmits.load() == before_reset);
    atr.fill(0U);
    atr_length = static_cast<DWORD>(atr.size());
    CHECK(IFDHPowerICC(0U, IFD_RESET, atr.data(), &atr_length) == IFD_SUCCESS);
    CHECK(atr_length != 0U);
    CHECK(transmit(apdu_count));
    CHECK(apdu_count == 2U && session.transmits.load() == 2U);
    CHECK(IFDHCloseChannel(0U) == IFD_SUCCESS);

    CHECK(IFDHCreateChannelByName(0U, device_name.data()) == IFD_SUCCESS);
    atr_length = static_cast<DWORD>(atr.size());
    CHECK(IFDHPowerICC(0U, IFD_POWER_UP, atr.data(), &atr_length) == IFD_SUCCESS);
    CHECK(atr_length != 0U);

    runner.stop_and_join();
    CHECK(runner.healthy());
    const auto closed = context->close(Timeout{5000U});
    CHECK(closed && closed.value() == hlc::CloseState::quiesced);
    const std::array<UCHAR, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<UCHAR, 16U> response{};
    DWORD response_length = static_cast<DWORD>(response.size());
    const SCARD_IO_HEADER send_pci{1U, sizeof(SCARD_IO_HEADER)};
    SCARD_IO_HEADER receive_pci{};
    const std::size_t before_stopped_transmit = session.transmits.load();
    const RESPONSECODE stopped_transmit = IFDHTransmitToICC(
        0U, send_pci, const_cast<UCHAR*>(apdu.data()),
        static_cast<DWORD>(apdu.size()), response.data(), &response_length,
        &receive_pci);
    CHECK(stopped_transmit == IFD_NO_SUCH_DEVICE);
    CHECK(response_length == 0U);
    CHECK(session.transmits.load() == before_stopped_transmit);
#if defined(DTV_USERLAND_UNIX_PCSC_ASICEN_PROFILE)
    constexpr RESPONSECODE expected_presence = IFD_ICC_NOT_PRESENT;
#else
    constexpr RESPONSECODE expected_presence = IFD_COMMUNICATION_ERROR;
#endif
    const RESPONSECODE stopped_presence = IFDHICCPresence(0U);
    CHECK(stopped_presence == expected_presence);
    atr_length = static_cast<DWORD>(atr.size());
    const RESPONSECODE stopped_reset =
        IFDHPowerICC(0U, IFD_RESET, atr.data(), &atr_length);
    CHECK(stopped_reset == IFD_ICC_NOT_PRESENT);
    CHECK(atr_length == 0U);
    const RESPONSECODE stopped_close = IFDHCloseChannel(0U);
    std::printf("listener_stop profile=%s transmit=%lu presence=%lu reset=%lu atr=%lu close=%lu apdus=%zu\n",
                kProfile.product, static_cast<unsigned long>(stopped_transmit),
                static_cast<unsigned long>(stopped_presence),
                static_cast<unsigned long>(stopped_reset),
                static_cast<unsigned long>(atr_length),
                static_cast<unsigned long>(stopped_close),
                session.transmits.load());
    return true;
}

}  // namespace

int main()
{
    if (!test_listener_reinsert()) return 1;
    std::printf("PASS unix_pcsc_ifd_listener\n");
    return 0;
}
