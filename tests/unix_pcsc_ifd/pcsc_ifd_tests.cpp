// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/unix_pcsc_ifd/pcsc_ifd_adapter.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::unix_pcsc_ifd;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

struct MockState final {
    bool present = false;
    bool initialized = false;
    Error factory_connect_error = Error::OK;
    Error status_error = Error::OK;
    Error connect_error = Error::OK;
    Error reset_error = Error::OK;
    Error transmit_error = Error::OK;
    Error disconnect_error = Error::OK;
    std::uint64_t generation = 1U;
    std::size_t factory_connects = 0U;
    std::size_t card_connects = 0U;
    std::size_t resets = 0U;
    std::size_t transmits = 0U;
    std::size_t disconnects = 0U;
    std::size_t closes = 0U;
};

IfdCardStatus mock_status(const MockState& state) noexcept
{
    IfdCardStatus status{};
    status.present = state.present;
    status.initialized = state.initialized;
    status.reader_generation = state.generation;
    if (state.initialized) {
        status.atr[0] = 0x3bU;
        status.atr[1] = 0x10U;
        status.atr[2] = 0x12U;
        status.atr_length = 3U;
    }
    return status;
}

class MockClient final : public IfdCardClient {
public:
    explicit MockClient(MockState& state) noexcept : state_(state) {}

    Result<IfdCardStatus> status() noexcept override
    {
        return state_.status_error == Error::OK ?
                   Result<IfdCardStatus>::success(mock_status(state_)) :
                   Result<IfdCardStatus>::failure(state_.status_error);
    }

    Result<IfdCardConnectResult> connect_shared() noexcept override
    {
        ++state_.card_connects;
        if (state_.connect_error != Error::OK) {
            return Result<IfdCardConnectResult>::failure(state_.connect_error);
        }
        if (!state_.present) {
            return Result<IfdCardConnectResult>::failure(Error::NO_CARD);
        }
        state_.initialized = true;
        const auto status = mock_status(state_);
        IfdCardConnectResult result{};
        result.handle = 0x1234U;
        result.atr = status.atr;
        result.atr_length = status.atr_length;
        return Result<IfdCardConnectResult>::success(result);
    }

    Result<void> disconnect(std::uint64_t handle) noexcept override
    {
        ++state_.disconnects;
        if (handle != 0x1234U) return Result<void>::failure(Error::NOT_FOUND);
        if (state_.disconnect_error != Error::OK) {
            return Result<void>::failure(state_.disconnect_error);
        }
        state_.initialized = false;
        return Result<void>::success();
    }

    Result<IfdCardConnectResult> reset(std::uint64_t handle) noexcept override
    {
        ++state_.resets;
        if (handle != 0x1234U) {
            return Result<IfdCardConnectResult>::failure(Error::NOT_FOUND);
        }
        if (state_.reset_error != Error::OK) {
            return Result<IfdCardConnectResult>::failure(state_.reset_error);
        }
        if (!state_.present) {
            return Result<IfdCardConnectResult>::failure(Error::NO_CARD);
        }
        state_.initialized = true;
        const auto status = mock_status(state_);
        IfdCardConnectResult result{};
        result.handle = handle;
        result.atr = status.atr;
        result.atr_length = status.atr_length;
        return Result<IfdCardConnectResult>::success(result);
    }

    Result<std::size_t> transmit(std::uint64_t handle, ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        ++state_.transmits;
        if (state_.transmit_error != Error::OK) {
            return Result<std::size_t>::failure(state_.transmit_error);
        }
        if (handle != 0x1234U || apdu.size == 0U || response.size < 2U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        response.data[0] = 0x90U;
        response.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }

    void close() noexcept override { ++state_.closes; }

private:
    MockState& state_;
};

class MockFactory final : public IfdCardClientFactory {
public:
    explicit MockFactory(MockState& state) noexcept : state_(state) {}

    Result<std::unique_ptr<IfdCardClient>> connect(
        const IfdEndpoint& endpoint) noexcept override
    {
        ++state_.factory_connects;
        if (state_.factory_connect_error != Error::OK) {
            return Result<std::unique_ptr<IfdCardClient>>::failure(
                state_.factory_connect_error);
        }
        if (endpoint.device_instance != "00001205000960") {
            return Result<std::unique_ptr<IfdCardClient>>::failure(Error::NOT_FOUND);
        }
        return Result<std::unique_ptr<IfdCardClient>>::success(
            std::unique_ptr<IfdCardClient>(new MockClient(state_)));
    }

private:
    MockState& state_;
};


bool test_device_name_and_error_mapping()
{
    const auto parsed = parse_ifd_device_name(
        "px4-userland:runtime=/run/user/1000:device=00001205000960:access=user");
    CHECK(parsed && parsed.value().runtime_directory == "/run/user/1000" &&
          parsed.value().device_instance == "00001205000960" &&
          !parsed.value().group_access);
    const auto default_runtime = parse_ifd_device_name(
        "px4-userland:device=00001205000960:access=group");
    CHECK(default_runtime && default_runtime.value().runtime_directory.empty() &&
          default_runtime.value().group_access);
    const auto named = parse_ifd_device_name(
        "px4-userland:runtime=/run/user/1000:instance=receiver.left:access=group");
    CHECK(named && named.value().runtime_directory == "/run/user/1000" &&
          named.value().device_instance == "receiver.left" && named.value().group_access);
    CHECK(!parse_ifd_device_name(
        "px4-userland:device=00001205000960:instance=receiver.left"));
    CHECK(!parse_ifd_device_name("px4-userland:instance=."));
    CHECK(!parse_ifd_device_name("px4-userland:instance=.."));
    CHECK(!parse_ifd_device_name("px4-userland:instance=bad/path"));
    CHECK(!parse_ifd_device_name("px4-userland:instance=bad:token"));
    CHECK(!parse_ifd_device_name("px4-userland:instance=999999999999999"));
    CHECK(!parse_ifd_device_name(
        "px4-userland:instance=receiver.left:instance=receiver.right"));
    CHECK(!parse_ifd_device_name(nullptr));
    CHECK(!parse_ifd_device_name("px4-userland:runtime=relative:device=00001205000960"));
    CHECK(!parse_ifd_device_name(
        "px4-userland:runtime=/tmp/px4:ifd:device=00001205000960"));
    CHECK(!parse_ifd_device_name("px4-userland:device=0000120500096"));
    CHECK(!parse_ifd_device_name(
        "px4-userland:device=00001205000960:device=00001205000960"));
    CHECK(!parse_ifd_device_name("px4-userland:device=00001205000960:unknown=x"));
    CHECK(!parse_ifd_device_name(
        "\"px4-userland:device=00001205000960:access=user\""));
    CHECK(map_ifd_error(Error::TIMEOUT, IfdOperation::transmit) ==
          IfdResult::response_timeout);
    CHECK(map_ifd_error(Error::NO_CARD, IfdOperation::power) ==
          IfdResult::error_power_action);
    CHECK(map_ifd_error(Error::CARD_REMOVED, IfdOperation::transmit) ==
          IfdResult::icc_not_present);
    CHECK(map_ifd_error(Error::DISCONNECTED, IfdOperation::status) ==
          IfdResult::no_such_device);
    CHECK(map_ifd_error(Error::BUFFER_TOO_SMALL, IfdOperation::transmit) ==
          IfdResult::insufficient_buffer);
    CHECK(map_ifd_error(Error::RESOURCE_LIMIT, IfdOperation::transmit) ==
          IfdResult::communication_error);
    const auto asicen = parse_ifd_device_name(
        "asicen-userland:instance=receiver.left", kAsicenIfdProfile);
    CHECK(asicen && asicen.value().product == "asicen-userland" &&
          asicen.value().device_instance == "receiver.left");
    CHECK(!parse_ifd_device_name(
        "px4-userland:instance=receiver.left", kAsicenIfdProfile));
    CHECK(map_ifd_error(Error::RESOURCE_LIMIT, IfdOperation::transmit) ==
          IfdResult::communication_error);
    return true;
}

bool test_profile_capabilities_and_presence()
{
    MockState state;
    state.factory_connect_error = Error::DISCONNECTED;
    PosixIfdCardClientFactory factory(kAsicenIfdProfile);
    IfdAdapter adapter(factory, kAsicenIfdProfile);
    CHECK(adapter.create_channel_by_name(
              0U, "asicen-userland:instance=receiver.left") == IfdResult::success);
    std::array<std::uint8_t, 4U> value{};
    std::size_t length = value.size();
    CHECK(adapter.get_capability(0U, kAttrMaxDataRate,
                                MutableByteView{value.data(), value.size()}, length) ==
          IfdResult::success);
    CHECK(length == 4U && value[0] == 0x00U && value[1] == 0x4bU &&
          value[2] == 0x00U && value[3] == 0x00U);
    std::array<std::uint8_t, 40U> type{};
    length = type.size();
    CHECK(adapter.get_capability(0U, kAttrVendorIfdType,
                                MutableByteView{type.data(), type.size()}, length) ==
          IfdResult::success);
    CHECK(std::string_view(reinterpret_cast<const char*>(type.data()), length) ==
          "ASICEN card reader via asicend");
    const IfdResult profile_presence = adapter.presence(0U);
    CHECK(profile_presence == IfdResult::icc_not_present);
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    return true;
}

bool test_profile_connect_failure_recovery()
{
    MockState px4_state;
    px4_state.present = true;
    px4_state.connect_error = Error::UNSUPPORTED;
    MockFactory px4_factory(px4_state);
    IfdAdapter px4_adapter(px4_factory, kPx4IfdProfile);
    CHECK(px4_adapter.create_channel_by_name(
              0U, "px4-userland:device=00001205000960") == IfdResult::success);
    std::array<std::uint8_t, kIfdAtrMaxLength> atr{};
    std::size_t atr_length = atr.size();
    CHECK(px4_adapter.power(0U, IfdPowerAction::power_up,
                            MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::not_supported);
    CHECK(px4_state.closes == 1U);

    MockState asicen_state;
    asicen_state.present = true;
    asicen_state.connect_error = Error::UNSUPPORTED;
    MockFactory asicen_factory(asicen_state);
    IfdAdapter asicen_adapter(asicen_factory, kAsicenIfdProfile);
    CHECK(asicen_adapter.create_channel_by_name(
              0U, "asicen-userland:device=00001205000960") == IfdResult::success);
    atr_length = atr.size();
    CHECK(asicen_adapter.power(0U, IfdPowerAction::power_up,
                               MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::not_supported);
    CHECK(asicen_state.closes == 0U);
    return true;
}

bool test_adapter_state_and_recovery()
{
    MockState state;
    MockFactory factory(state);
    IfdAdapter adapter(factory);
    constexpr const char* name =
        "px4-userland:runtime=/tmp:device=00001205000960:access=user";
    CHECK(adapter.create_channel(0U, 0U) == IfdResult::not_supported);
    CHECK(adapter.create_channel_by_name(1U, name) == IfdResult::no_such_device);
    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    CHECK(adapter.create_channel_by_name(0U, name) ==
          IfdResult::communication_error);
    CHECK(adapter.presence(0U) == IfdResult::icc_not_present);

    std::array<std::uint8_t, kIfdAtrMaxLength> atr{};
    std::size_t atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::error_power_action);
    CHECK(atr_length == 0U);

    state.present = true;
    ++state.generation;
    CHECK(adapter.presence(0U) == IfdResult::icc_present);
    atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);
    CHECK(atr_length == 3U && atr[0] == 0x3bU && atr[2] == 0x12U);
    CHECK(adapter.set_protocol(0U, kIfdSetProtocolT1, 0U, 0U, 0U, 0U) ==
          IfdResult::success);
    CHECK(adapter.set_protocol(0U, kIfdSetProtocolT1, kIfdNegotiatePts1,
                               0x12U, 0U, 0U) == IfdResult::success);
    CHECK(adapter.set_protocol(0U, kIfdSetProtocolT1, kIfdNegotiatePts1,
                               0x11U, 0U, 0U) == IfdResult::error_pts_failure);
    CHECK(adapter.set_protocol(0U, 1U, 0U, 0U, 0U, 0U) ==
          IfdResult::protocol_not_supported);

    std::array<std::uint8_t, 8U> capability{};
    std::size_t capability_length = 0U;
    CHECK(adapter.get_capability(0U, kTagIfdAtr,
                                 MutableByteView{nullptr, 0U}, capability_length) ==
          IfdResult::insufficient_buffer);
    CHECK(capability_length == 3U);
    capability_length = capability.size();
    CHECK(adapter.get_capability(
              0U, kAttrAsyncProtocolTypes,
              MutableByteView{capability.data(), capability.size()}, capability_length) ==
          IfdResult::success);
    CHECK(capability_length == 4U && capability[0] == 2U);
    CHECK(adapter.get_capability(
              0U, 0xffffffffU,
              MutableByteView{capability.data(), capability.size()}, capability_length) ==
          IfdResult::error_tag);
    const std::array<std::uint8_t, 1U> slot{0U};
    CHECK(adapter.set_capability(0U, kTagIfdSlotNumber,
                                 ByteView{slot.data(), slot.size()}) ==
          IfdResult::success);
    CHECK(adapter.set_capability(0U, kTagIfdAtr,
                                 ByteView{slot.data(), slot.size()}) ==
          IfdResult::error_value_read_only);

    const std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    for (const std::uint32_t wrong_protocol :
         std::array<std::uint32_t, 2U>{0U, kIfdSetProtocolT1}) {
        std::size_t response_length = response.size();
        CHECK(adapter.transmit(
                  0U, wrong_protocol, ByteView{apdu.data(), apdu.size()},
                  MutableByteView{response.data(), response.size()}, response_length) ==
              IfdResult::protocol_not_supported);
        CHECK(response_length == 0U);
    }
    for (std::size_t iteration = 0U; iteration < 100U; ++iteration) {
        std::size_t response_length = response.size();
        CHECK(adapter.transmit(
                  0U, kIfdTransmitProtocolT1,
                  ByteView{apdu.data(), apdu.size()},
                  MutableByteView{response.data(), response.size()}, response_length) ==
              IfdResult::success);
        CHECK(response_length == 2U && response[0] == 0x90U && response[1] == 0x00U);
    }
    CHECK(state.transmits == 100U);

    std::atomic<bool> concurrent_ok{true};
    std::array<std::thread, 4U> workers;
    for (std::size_t worker = 0U; worker < workers.size(); ++worker) {
        workers[worker] = std::thread([&]() {
            std::array<std::uint8_t, 8U> worker_response{};
            for (std::size_t iteration = 0U; iteration < 25U; ++iteration) {
                std::size_t worker_length = worker_response.size();
                if (adapter.transmit(
                        0U, kIfdTransmitProtocolT1,
                        ByteView{apdu.data(), apdu.size()},
                        MutableByteView{worker_response.data(), worker_response.size()},
                        worker_length) != IfdResult::success || worker_length != 2U) {
                    concurrent_ok.store(false);
                    return;
                }
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    CHECK(concurrent_ok.load() && state.transmits == 200U);

    state.present = false;
    state.initialized = false;
    ++state.generation;
    CHECK(adapter.presence(0U) == IfdResult::icc_not_present);
    std::size_t response_length = response.size();
    CHECK(adapter.transmit(
              0U, kIfdTransmitProtocolT1,
              ByteView{apdu.data(), apdu.size()},
              MutableByteView{response.data(), response.size()}, response_length) ==
          IfdResult::icc_not_present);
    state.present = true;
    ++state.generation;
    CHECK(adapter.presence(0U) == IfdResult::icc_present);
    atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::reset,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);
    CHECK(state.resets == 1U);

    atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_down,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);
    CHECK(atr_length == 0U && state.disconnects == 1U);
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    CHECK(state.closes == 2U);

    state.status_error = Error::OK;
    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    state.status_error = Error::DISCONNECTED;
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    state.status_error = Error::NOT_READY;
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    state.status_error = Error::NOT_FOUND;
    CHECK(adapter.presence(0U) == IfdResult::no_such_device);
    state.status_error = Error::TIMEOUT;
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    state.status_error = Error::UNSUPPORTED;
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    state.status_error = Error::NO_CARD;
    CHECK(adapter.presence(0U) == IfdResult::icc_not_present);
    state.status_error = Error::CARD_REMOVED;
    CHECK(adapter.presence(0U) == IfdResult::icc_not_present);
    state.factory_connect_error = Error::TIMEOUT;
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    state.factory_connect_error = Error::UNSUPPORTED;
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    CHECK(state.factory_connects == 11U && state.closes == 8U);
    return true;
}

bool test_adapter_lazy_reconnect()
{
    MockState state;
    state.present = true;
    state.factory_connect_error = Error::NOT_READY;
    MockFactory factory(state);
    IfdAdapter adapter(factory);
    constexpr const char* name =
        "px4-userland:device=00001205000960:access=user";

    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    CHECK(state.factory_connects == 1U);
    CHECK(adapter.presence(0U) == IfdResult::communication_error);
    CHECK(state.factory_connects == 2U);

    std::array<std::uint8_t, 1U> attribute{};
    std::size_t attribute_length = attribute.size();
    CHECK(adapter.get_capability(
              0U, kAttrIccPresence,
              MutableByteView{attribute.data(), attribute.size()},
              attribute_length) == IfdResult::success);
    CHECK(attribute_length == 1U && attribute[0] == 0U);

    std::array<std::uint8_t, kIfdAtrMaxLength> atr{};
    std::size_t atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::icc_not_present);
    CHECK(atr_length == 0U);
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 2U> response{};
    std::size_t response_length = response.size();
    CHECK(adapter.transmit(
              0U, kIfdTransmitProtocolT1, ByteView{apdu.data(), apdu.size()},
              MutableByteView{response.data(), response.size()}, response_length) ==
          IfdResult::icc_not_present);
    CHECK(response_length == 0U);
    atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_down,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);
    CHECK(atr_length == 0U);

    state.factory_connect_error = Error::OK;
    CHECK(adapter.presence(0U) == IfdResult::icc_present);
    CHECK(state.factory_connects == 6U);
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    CHECK(state.closes == 1U);
    CHECK(adapter.close_channel(0U) == IfdResult::no_such_device);
    return true;
}

bool test_adapter_timeout_and_buffer_errors()
{
    MockState state;
    state.present = true;
    MockFactory factory(state);
    IfdAdapter adapter(factory);
    constexpr const char* name =
        "px4-userland:device=00001205000960:access=user";
    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    std::array<std::uint8_t, kIfdAtrMaxLength> atr{};
    std::size_t atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);

    state.transmit_error = Error::TIMEOUT;
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    std::size_t response_length = response.size();
    CHECK(adapter.transmit(
              0U, kIfdTransmitProtocolT1,
              ByteView{apdu.data(), apdu.size()},
              MutableByteView{response.data(), response.size()}, response_length) ==
          IfdResult::response_timeout);
    CHECK(response_length == 0U && state.closes == 1U);
    CHECK(adapter.presence(0U) == IfdResult::icc_present);
    CHECK(state.factory_connects == 2U);

    state.transmit_error = Error::OK;
    state.initialized = false;
    std::array<std::uint8_t, 1U> short_atr{};
    atr_length = short_atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
                        MutableByteView{short_atr.data(), short_atr.size()}, atr_length) ==
          IfdResult::insufficient_buffer);
    CHECK(atr_length == 3U);
    atr_length = atr.size();
    CHECK(adapter.power(0U, IfdPowerAction::reset,
                        MutableByteView{atr.data(), atr.size()}, atr_length) ==
          IfdResult::success);
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    return true;
}

}  // namespace

int main()
{
    if (!test_device_name_and_error_mapping() ||
        !test_profile_capabilities_and_presence() ||
        !test_profile_connect_failure_recovery() ||
        !test_adapter_state_and_recovery() ||
        !test_adapter_lazy_reconnect() ||
        !test_adapter_timeout_and_buffer_errors()) {
        return 1;
    }
    std::printf("PASS pcsc_ifd\n");
    return 0;
}
