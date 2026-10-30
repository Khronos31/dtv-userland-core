// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/unix_pcsc_ifd/pcsc_ifd_adapter.h"

#include "dtv/userland/control_client.h"
#include "dtv/userland/host_runtime/control_socket.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

namespace dtv::userland::unix_pcsc_ifd {
namespace {

constexpr std::uint32_t kRequestTimeoutMs = 4000U;

// The IFD stays free of the device core, so endpoint validation is restated here.
bool valid_serial(std::string_view value) noexcept
{
    if (value.size() != 14U && value.size() != 15U) return false;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

bool valid_instance(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 80U || value == "." || value == ".." ||
        valid_serial(value))
        return false;
    for (const char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')
            continue;
        return false;
    }
    return true;
}

bool valid_field_value(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (const unsigned char character : value) {
        // pcsc-lite's unquoted DEVICENAME token permits ':', so it is the
        // field separator.  Reject it inside values to keep parsing unique.
        if (character < 0x20U || character == 0x7fU || character == ':') {
            return false;
        }
    }
    return true;
}

bool channel_failure(Error error, const IfdProfile& profile) noexcept
{
    switch (error) {
    case Error::VERSION_MISMATCH:
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::TIMEOUT:
    case Error::USB_IO:
    case Error::DISCONNECTED:
    case Error::PROTOCOL_ERROR:
    case Error::FIRMWARE_REJECTED:
    case Error::SLOW_CONSUMER:
    case Error::INTERNAL:
        return true;
    case Error::OK:
    case Error::INVALID_ARGUMENT:
    case Error::BUSY:
        return false;
    case Error::RESOURCE_LIMIT:
        return profile.resource_limit_is_channel_failure;
    case Error::UNSUPPORTED:
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL:
        return false;
    }
    return true;
}

IfdResult copy_value(ByteView value, MutableByteView output,
                     std::size_t& length) noexcept
{
    const std::size_t capacity = length;
    length = value.size;
    if (value.size == 0U) return IfdResult::success;
    if (value.data == nullptr || output.data == nullptr ||
        capacity < value.size || output.size < value.size) {
        return IfdResult::insufficient_buffer;
    }
    std::memcpy(output.data, value.data, value.size);
    return IfdResult::success;
}

IfdResult copy_u8(std::uint8_t value, MutableByteView output,
                  std::size_t& length) noexcept
{
    return copy_value(ByteView{&value, 1U}, output, length);
}

IfdResult copy_u32(std::uint32_t value, MutableByteView output,
                   std::size_t& length) noexcept
{
    const std::array<std::uint8_t, 4U> bytes{
        static_cast<std::uint8_t>(value & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 24U) & 0xffU)};
    return copy_value(ByteView{bytes.data(), bytes.size()}, output, length);
}

class PosixIfdCardClient final : public IfdCardClient {
public:
    PosixIfdCardClient(std::unique_ptr<dtv::userland::wire::TimedClientTransport> client,
                       std::string instance, std::string runtime_directory,
                       dtv::userland::host_runtime::EndpointAccess access,
                       std::string product, const IfdProfile& profile) noexcept
        : client_(std::move(client)), instance_(std::move(instance)),
          runtime_directory_(std::move(runtime_directory)),
          product_(std::move(product)), access_(access),
          validate_status_identity_(profile.validate_status_identity),
          leave_initial_status_unread_(profile.leave_initial_status_unread)
    {
    }

    Result<IfdCardStatus> status() noexcept override
    {
        IfdCardStatus result{};
        if (!card_session_started_) {
            const auto snapshot = dtv::userland::wire::fetch_status(
                *client_, dtv::userland::Timeout{kRequestTimeoutMs},
                dtv::userland::wire::kFeatureCardService);
            if (!snapshot) return Result<IfdCardStatus>::failure(
                static_cast<Error>(snapshot.error));
            if (validate_status_identity_) {
                const auto& product = snapshot.snapshot.status.producer.product;
                const auto& actual_instance = snapshot.snapshot.status.context.instance;
                const auto& listed_readers = snapshot.snapshot.list.readers;
                bool reader_found = false;
                if (listed_readers.present) {
                    for (std::size_t index = 0U; index < listed_readers.value.count; ++index) {
                        if (listed_readers.value.items[index].id == 0U) reader_found = true;
                    }
                }
                if (product.length != product_.size() ||
                    std::memcmp(product.bytes.data(), product_.data(), product_.size()) != 0 ||
                    !actual_instance.present ||
                    actual_instance.value.length != instance_.size() ||
                    std::memcmp(actual_instance.value.bytes.data(), instance_.data(),
                                instance_.size()) != 0 || !reader_found) {
                    return Result<IfdCardStatus>::failure(Error::NOT_FOUND);
                }
            }
            const auto& readers = snapshot.snapshot.status.readers;
            if (!readers.present || readers.value.count == 0U)
                return Result<IfdCardStatus>::failure(Error::NOT_FOUND);
            const auto reader = std::find_if(
                readers.value.items.begin(),
                readers.value.items.begin() + readers.value.count,
                [](const auto& item) { return item.descriptor.id == 0U; });
            if (reader == readers.value.items.begin() + readers.value.count) {
                return Result<IfdCardStatus>::failure(Error::NOT_FOUND);
            }
            session_ = dtv::userland::wire::CardClientSession{};
            session_.epoch = snapshot.snapshot.status.context.epoch;
            session_.reader_id = reader->descriptor.id;
            session_.next_request_id = 4U;
            card_session_started_ = true;
            if (leave_initial_status_unread_) {
                return Result<IfdCardStatus>::success(result);
            }
            const auto status = dtv::userland::wire::card_status(
                *client_, session_, dtv::userland::Timeout{kRequestTimeoutMs});
            if (!status)
                return Result<IfdCardStatus>::failure(static_cast<Error>(status.error));
            card_session_started_ = true;
            result.present = status.response.present;
            result.initialized = status.response.initialized;
            result.reader_generation = status.response.current_generation;
            if (result.present && result.initialized && status.response.atr.present) {
                result.atr_length = status.response.atr.value.length;
                if (result.atr_length > result.atr.size())
                    return Result<IfdCardStatus>::failure(Error::BUFFER_TOO_SMALL);
                if (result.atr_length != 0U)
                    std::memcpy(result.atr.data(), status.response.atr.value.bytes.data(),
                                result.atr_length);
            }
            return Result<IfdCardStatus>::success(result);
        }
        const auto status = dtv::userland::wire::card_status(
            *client_, session_, dtv::userland::Timeout{kRequestTimeoutMs});
        if (!status) return Result<IfdCardStatus>::failure(
            static_cast<Error>(status.error));
        const auto& reader = status.response;
        result.present = reader.present;
        result.initialized = reader.initialized;
        result.reader_generation = reader.current_generation;
        if (result.present && result.initialized && reader.atr.present) {
            result.atr_length = reader.atr.value.length;
            if (result.atr_length > result.atr.size())
                return Result<IfdCardStatus>::failure(Error::BUFFER_TOO_SMALL);
            if (result.atr_length != 0U)
                std::memcpy(result.atr.data(), reader.atr.value.bytes.data(),
                            result.atr_length);
        }
        return Result<IfdCardStatus>::success(result);
    }

    Result<IfdCardConnectResult> connect_shared() noexcept override
    {
        using namespace dtv::userland::host_runtime;
        const EndpointConfig config{runtime_directory_, product_, instance_, access_};
        auto reconnected = connect_control(config, dtv::userland::Timeout{kRequestTimeoutMs});
        if (!reconnected) return Result<IfdCardConnectResult>::failure(
            static_cast<Error>(reconnected.error()));
        if (client_) client_->close();
        client_ = std::move(reconnected.value());
        dtv::userland::wire::CardClientOptions options;
        options.reader_id = 0U;
        options.share = dtv::userland::wire::Share::shared;
        options.expected_product = {reinterpret_cast<const std::uint8_t*>(product_.data()),
                                    product_.size()};
        options.expected_instance = {
            reinterpret_cast<const std::uint8_t*>(instance_.data()), instance_.size()};
        const auto opened = dtv::userland::wire::begin_card(
            *client_, options, dtv::userland::Timeout{kRequestTimeoutMs});
        if (!opened) return Result<IfdCardConnectResult>::failure(
            static_cast<Error>(opened.error));
        session_ = opened.session;
        const auto& atr = opened.connection.atr;
        atr_length_ = atr.present ? atr.value.length : 0U;
        if (atr_length_ > atr_.size())
            return Result<IfdCardConnectResult>::failure(Error::BUFFER_TOO_SMALL);
        if (atr_length_ != 0U)
            std::memcpy(atr_.data(), atr.value.bytes.data(), atr_length_);
        card_session_started_ = true;
        IfdCardConnectResult result{};
        result.handle = session_.card_handle;
        result.atr_length = atr_length_;
        std::copy_n(atr_.begin(), atr_length_, result.atr.begin());
        return Result<IfdCardConnectResult>::success(result);
    }

    Result<void> disconnect(std::uint64_t handle) noexcept override
    {
        if (handle == 0U || handle != session_.card_handle)
            return Result<void>::failure(Error::NOT_FOUND);
        const auto disconnected = dtv::userland::wire::card_disconnect(
            *client_, session_, dtv::userland::wire::Disposition::leave,
            dtv::userland::Timeout{kRequestTimeoutMs});
        if (!disconnected) return Result<void>::failure(
            static_cast<Error>(disconnected.error));
        session_.card_handle = 0U;
        session_.bound_generation = 0U;
        atr_length_ = 0U;
        atr_.fill(0U);
        return Result<void>::success();
    }

    Result<IfdCardConnectResult> reset(std::uint64_t handle) noexcept override
    {
        if (handle == 0U || handle != session_.card_handle)
            return Result<IfdCardConnectResult>::failure(Error::NOT_FOUND);
        const auto reset = dtv::userland::wire::card_reset(
            *client_, session_, dtv::userland::Timeout{kRequestTimeoutMs});
        if (!reset) return Result<IfdCardConnectResult>::failure(
            static_cast<Error>(reset.error));
        const auto& atr = reset.response.atr;
        atr_length_ = atr.present ? atr.value.length : 0U;
        if (atr_length_ > atr_.size())
            return Result<IfdCardConnectResult>::failure(Error::BUFFER_TOO_SMALL);
        if (atr_length_ != 0U)
            std::memcpy(atr_.data(), atr.value.bytes.data(), atr_length_);
        IfdCardConnectResult result{};
        result.handle = handle;
        result.atr_length = atr_length_;
        std::copy_n(atr_.begin(), atr_length_, result.atr.begin());
        return Result<IfdCardConnectResult>::success(result);
    }

    Result<std::size_t> transmit(std::uint64_t handle, ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        if (handle == 0U || handle != session_.card_handle)
            return Result<std::size_t>::failure(Error::NOT_FOUND);
        const auto transmitted = dtv::userland::wire::card_transmit(
            *client_, session_, dtv::userland::wire::CardProtocol::t1,
            {apdu.data, apdu.size}, dtv::userland::Timeout{kRequestTimeoutMs});
        if (!transmitted) return Result<std::size_t>::failure(
            static_cast<Error>(transmitted.error));
        const auto& bytes = transmitted.response.response;
        if (bytes.length > response.size || (bytes.length != 0U && response.data == nullptr)) {
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        }
        if (bytes.length != 0U) std::memcpy(response.data, bytes.bytes.data(), bytes.length);
        return Result<std::size_t>::success(bytes.length);
    }

    void close() noexcept override
    {
        if (client_) client_->close();
    }

private:
    std::unique_ptr<dtv::userland::wire::TimedClientTransport> client_;
    dtv::userland::wire::CardClientSession session_{};
    std::array<std::uint8_t, kIfdAtrMaxLength> atr_{};
    std::size_t atr_length_ = 0U;
    bool card_session_started_ = false;
    std::string instance_;
    std::string runtime_directory_;
    std::string product_;
    dtv::userland::host_runtime::EndpointAccess access_ =
        dtv::userland::host_runtime::EndpointAccess::private_user;
    bool validate_status_identity_ = false;
    bool leave_initial_status_unread_ = false;
};

}  // namespace

Result<IfdEndpoint> parse_ifd_device_name(const char* device_name,
                                          const IfdProfile& profile) noexcept
{
    if (device_name == nullptr) {
        return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
    }
    std::size_t length = 0U;
    while (length <= kIfdDeviceNameMaxLength && device_name[length] != '\0') {
        ++length;
    }
    if (length == 0U || length > kIfdDeviceNameMaxLength) {
        return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
    }
    const std::string_view input(device_name, length);
    const std::string_view device_prefix(profile.device_prefix);
    if (input.size() <= device_prefix.size() ||
        input.substr(0U, device_prefix.size()) != device_prefix) {
        return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
    }

    IfdEndpoint endpoint{};
    endpoint.product = profile.product;
    bool have_runtime = false;
    bool have_device = false;
    bool have_instance = false;
    bool have_access = false;
    std::size_t offset = device_prefix.size();
    while (offset < input.size()) {
        const std::size_t separator = input.find(':', offset);
        const std::size_t end = separator == std::string_view::npos ?
                                    input.size() : separator;
        const std::string_view field = input.substr(offset, end - offset);
        const std::size_t equals = field.find('=');
        if (field.empty() || equals == std::string_view::npos || equals == 0U ||
            equals + 1U == field.size()) {
            return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
        }
        const std::string_view key = field.substr(0U, equals);
        const std::string_view value = field.substr(equals + 1U);
        if (!valid_field_value(value)) {
            return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
        }
        if (key == "runtime") {
            if (have_runtime || value.front() != '/') {
                return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
            }
            have_runtime = true;
            endpoint.runtime_directory.assign(value.data(), value.size());
        } else if (key == "device") {
            if (have_device || !valid_serial(value)) {
                return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
            }
            have_device = true;
            endpoint.device_instance.assign(value.data(), value.size());
        } else if (key == "instance") {
            if (have_instance || !valid_instance(value)) {
                return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
            }
            have_instance = true;
            endpoint.device_instance.assign(value.data(), value.size());
        } else if (key == "access") {
            if (have_access || (value != "user" && value != "group")) {
                return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
            }
            have_access = true;
            endpoint.group_access = value == "group";
        } else {
            return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
        }
        if (separator == std::string_view::npos) break;
        offset = separator + 1U;
        if (offset == input.size()) {
            return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
        }
    }
    if (have_device == have_instance) {
        return Result<IfdEndpoint>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<IfdEndpoint>::success(std::move(endpoint));
}

IfdResult map_ifd_error(Error error, IfdOperation operation) noexcept
{
    switch (error) {
    case Error::OK: return IfdResult::success;
    case Error::TIMEOUT: return IfdResult::response_timeout;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::DISCONNECTED:
    case Error::FIRMWARE_REJECTED:
        return IfdResult::no_such_device;
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
        return operation == IfdOperation::power ? IfdResult::error_power_action :
               IfdResult::icc_not_present;
    case Error::BUFFER_TOO_SMALL: return IfdResult::insufficient_buffer;
    case Error::UNSUPPORTED: return IfdResult::not_supported;
    case Error::INVALID_ARGUMENT:
    case Error::VERSION_MISMATCH:
    case Error::BUSY:
    case Error::RESOURCE_LIMIT:
    case Error::USB_IO:
    case Error::PROTOCOL_ERROR:
    case Error::SLOW_CONSUMER:
    case Error::INTERNAL:
        return IfdResult::communication_error;
    }
    return IfdResult::communication_error;
}

Result<std::unique_ptr<IfdCardClient>> PosixIfdCardClientFactory::connect(
    const IfdEndpoint& endpoint) noexcept
{
    using namespace dtv::userland::host_runtime;
    const EndpointAccess access = endpoint.group_access ? EndpointAccess::shared_group :
                                                           EndpointAccess::private_user;
    const EndpointConfig config{endpoint.runtime_directory, profile_.product,
                                endpoint.device_instance, access};
    auto client = connect_control(config, dtv::userland::Timeout{kRequestTimeoutMs});
    if (!client) {
        return Result<std::unique_ptr<IfdCardClient>>::failure(
            static_cast<Error>(client.error()));
    }
    return Result<std::unique_ptr<IfdCardClient>>::success(
        std::unique_ptr<IfdCardClient>(
            new PosixIfdCardClient(std::move(client.value()), endpoint.device_instance,
                                   endpoint.runtime_directory, access,
                                   profile_.product, profile_)));
}

void IfdAdapter::clear_card_state() noexcept
{
    atr_.fill(0U);
    atr_length_ = 0U;
    present_ = false;
    powered_ = false;
}

void IfdAdapter::invalidate_client() noexcept
{
    if (client_) client_->close();
    client_.reset();
    card_handle_ = 0U;
    reader_generation_ = 0U;
    clear_card_state();
}

void IfdAdapter::invalidate_channel() noexcept
{
    invalidate_client();
    endpoint_ = IfdEndpoint{};
    channel_configured_ = false;
}

Result<void> IfdAdapter::ensure_client() noexcept
{
    if (!channel_configured_) return Result<void>::failure(Error::NOT_READY);
    if (client_) return Result<void>::success();
    auto client = factory_.connect(endpoint_);
    if (!client) {
        clear_card_state();
        card_handle_ = 0U;
        reader_generation_ = 0U;
        return Result<void>::failure(client.error());
    }
    client_ = std::move(client.value());
    return Result<void>::success();
}

IfdResult IfdAdapter::fail(Error error, IfdOperation operation) noexcept
{
    if (error == Error::NO_CARD || error == Error::CARD_REMOVED) {
        clear_card_state();
    } else if (channel_failure(error, profile_)) {
        invalidate_client();
    }
    return map_ifd_error(error, operation);
}

Result<IfdCardStatus> IfdAdapter::refresh_status() noexcept
{
    if (!client_) return Result<IfdCardStatus>::failure(Error::NOT_READY);
    const auto status = client_->status();
    if (!status) return status;
    const bool generation_changed =
        reader_generation_ != 0U &&
        reader_generation_ != status.value().reader_generation;
    if (!status.value().present || generation_changed) {
        if (card_handle_ != 0U) {
            invalidate_client();
        } else {
            clear_card_state();
        }
        reader_generation_ = status.value().reader_generation;
        present_ = status.value().present;
        return status;
    }
    reader_generation_ = status.value().reader_generation;
    present_ = status.value().present;
    if (status.value().present && status.value().initialized) {
        atr_ = status.value().atr;
        atr_length_ = status.value().atr_length;
        powered_ = true;
    }
    return status;
}

Result<IfdCardConnectResult> IfdAdapter::power_or_reset(bool force_reset) noexcept
{
    if (!client_) {
        return Result<IfdCardConnectResult>::failure(Error::NOT_READY);
    }
    Result<IfdCardConnectResult> result =
        Result<IfdCardConnectResult>::failure(Error::INTERNAL);
    if (card_handle_ == 0U) {
        result = client_->connect_shared();
        if (!result) {
            if (profile_.invalidate_client_on_connect_failure) invalidate_client();
            return result;
        }
    } else if (force_reset || !powered_) {
        result = client_->reset(card_handle_);
    } else {
        IfdCardConnectResult cached{};
        cached.handle = card_handle_;
        cached.atr = atr_;
        cached.atr_length = atr_length_;
        return Result<IfdCardConnectResult>::success(cached);
    }
    if (!result) return result;
    card_handle_ = result.value().handle;
    atr_ = result.value().atr;
    atr_length_ = result.value().atr_length;
    present_ = true;
    powered_ = true;
    return result;
}

IfdResult IfdAdapter::create_channel_by_name(std::uint64_t lun,
                                             const char* device_name) noexcept
{
    const auto parsed = parse_ifd_device_name(device_name, profile_);
    if (!parsed) return IfdResult::communication_error;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun)) return IfdResult::no_such_device;
    if (channel_configured()) return IfdResult::communication_error;
    endpoint_ = parsed.value();
    channel_configured_ = true;
    if (!ensure_client()) return IfdResult::success;
    const auto status = refresh_status();
    if (!status) invalidate_client();
    return IfdResult::success;
}

IfdResult IfdAdapter::create_channel(std::uint64_t lun,
                                     std::uint64_t channel) noexcept
{
    (void)channel;
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_lun(lun) ? IfdResult::not_supported :
                            IfdResult::no_such_device;
}

IfdResult IfdAdapter::close_channel(std::uint64_t lun) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun)) return IfdResult::no_such_device;
    if (!channel_configured()) return IfdResult::no_such_device;
    Error error = Error::OK;
    if (client_ && card_handle_ != 0U) {
        const auto disconnected = client_->disconnect(card_handle_);
        if (!disconnected) error = disconnected.error();
    }
    invalidate_channel();
    return error == Error::OK ? IfdResult::success :
                                map_ifd_error(error, IfdOperation::channel);
}

IfdResult IfdAdapter::copy_atr(MutableByteView output,
                               std::size_t& length) noexcept
{
    return copy_value(ByteView{atr_.data(), atr_length_}, output, length);
}

IfdResult IfdAdapter::get_capability(std::uint64_t lun, std::uint32_t tag,
                                     MutableByteView output,
                                     std::size_t& length) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun) || !channel_configured()) return IfdResult::no_such_device;
    switch (tag) {
    case kTagIfdAtr:
    case kAttrAtrString:
        return copy_atr(output, length);
    case kTagIfdSlotNumber: return copy_u8(0U, output, length);
    case kTagIfdSlotThreadSafe: return copy_u8(0U, output, length);
    case kTagIfdThreadSafe: return copy_u8(1U, output, length);
    case kTagIfdSlotsNumber: return copy_u8(1U, output, length);
    case kTagIfdSimultaneousAccess: return copy_u8(1U, output, length);
    case kAttrVendorName:
        return copy_value(
            ByteView{reinterpret_cast<const std::uint8_t*>(profile_.reader_name),
                     std::strlen(profile_.reader_name)}, output, length);
    case kAttrVendorIfdType:
        return copy_value(
            ByteView{reinterpret_cast<const std::uint8_t*>(profile_.reader_type),
                     std::strlen(profile_.reader_type)}, output, length);
    case kAttrVendorIfdVersion: return copy_u32(0x00010000U, output, length);
    case kAttrVendorIfdSerial:
        return copy_value(
            ByteView{reinterpret_cast<const std::uint8_t*>(
                         endpoint_.device_instance.data()),
                     endpoint_.device_instance.size()}, output, length);
    case kAttrAsyncProtocolTypes:
    case kAttrCurrentProtocolType:
        return copy_u32(kIfdSetProtocolT1, output, length);
    case kAttrDefaultDataRate: return copy_u32(9600U, output, length);
    case kAttrMaxDataRate: return copy_u32(profile_.maximum_baud, output, length);
    case kAttrMaxIfsd: return copy_u32(251U, output, length);
    case kAttrMaxInput: return copy_u32(kIfdApduMaxLength, output, length);
    case kAttrIccPresence: {
        const auto connected = ensure_client();
        if (!connected) return copy_u8(0U, output, length);
        const auto status = refresh_status();
        if (!status) {
            (void)fail(status.error(), IfdOperation::status);
            return copy_u8(0U, output, length);
        }
        return copy_u8(status.value().present ? 1U : 0U, output, length);
    }
    case kAttrIccInterfaceStatus: {
        const auto connected = ensure_client();
        if (!connected) return copy_u8(0U, output, length);
        const auto status = refresh_status();
        if (!status) {
            (void)fail(status.error(), IfdOperation::status);
            return copy_u8(0U, output, length);
        }
        return copy_u8(powered_ ? 1U : 0U, output, length);
    }
    default: return IfdResult::error_tag;
    }
}

IfdResult IfdAdapter::set_capability(std::uint64_t lun, std::uint32_t tag,
                                     ByteView value) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun) || !channel_configured()) return IfdResult::no_such_device;
    if (tag == kTagIfdSlotNumber) {
        return value.data != nullptr && value.size == 1U && value.data[0] == 0U ?
                   IfdResult::success : IfdResult::error_set_failure;
    }
    switch (tag) {
    case kTagIfdAtr:
    case kTagIfdSlotThreadSafe:
    case kTagIfdThreadSafe:
    case kTagIfdSlotsNumber:
    case kTagIfdSimultaneousAccess:
    case kAttrVendorName:
    case kAttrVendorIfdType:
    case kAttrVendorIfdVersion:
    case kAttrVendorIfdSerial:
    case kAttrAsyncProtocolTypes:
    case kAttrDefaultDataRate:
    case kAttrMaxDataRate:
    case kAttrMaxIfsd:
    case kAttrCurrentProtocolType:
    case kAttrIccPresence:
    case kAttrIccInterfaceStatus:
    case kAttrAtrString:
    case kAttrMaxInput:
        return IfdResult::error_value_read_only;
    default: return IfdResult::error_tag;
    }
}

IfdResult IfdAdapter::set_protocol(std::uint64_t lun, std::uint32_t protocol,
                                   std::uint8_t flags, std::uint8_t pts1,
                                   std::uint8_t pts2, std::uint8_t pts3) noexcept
{
    (void)pts2;
    (void)pts3;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun) || !channel_configured()) return IfdResult::no_such_device;
    if (protocol != kIfdSetProtocolT1) return IfdResult::protocol_not_supported;
    if (!present_ || !powered_ || atr_length_ < 2U) {
        return IfdResult::error_power_action;
    }
    if ((flags & static_cast<std::uint8_t>(~kIfdNegotiatePts1)) != 0U) {
        return IfdResult::error_pts_failure;
    }
    std::uint8_t atr_ta1 = 0x11U;
    if ((atr_[1] & 0x10U) != 0U) {
        if (atr_length_ < 3U) return IfdResult::error_pts_failure;
        atr_ta1 = atr_[2];
    }
    if ((flags & kIfdNegotiatePts1) != 0U && pts1 != atr_ta1) {
        return IfdResult::error_pts_failure;
    }
    return IfdResult::success;
}

IfdResult IfdAdapter::power(std::uint64_t lun, IfdPowerAction action,
                            MutableByteView atr, std::size_t& atr_length) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun) || !channel_configured()) {
        atr_length = 0U;
        return IfdResult::no_such_device;
    }
    if (action == IfdPowerAction::power_down) {
        if (atr.data != nullptr && atr.size != 0U) {
            std::memset(atr.data, 0, atr.size);
        }
        atr_length = 0U;
        if (!client_ || card_handle_ == 0U) {
            clear_card_state();
            return IfdResult::success;
        }
        const auto disconnected = client_->disconnect(card_handle_);
        if (!disconnected) {
            const IfdResult result = fail(disconnected.error(), IfdOperation::power);
            if (client_ == nullptr) return IfdResult::success;
            return result;
        }
        card_handle_ = 0U;
        clear_card_state();
        return IfdResult::success;
    }
    const auto connected = ensure_client();
    if (!connected) {
        atr_length = 0U;
        return IfdResult::icc_not_present;
    }
    const bool force_reset = action == IfdPowerAction::reset;
    const auto result = power_or_reset(force_reset);
    if (!result) {
        atr_length = 0U;
        return fail(result.error(), IfdOperation::power);
    }
    return copy_atr(atr, atr_length);
}

IfdResult IfdAdapter::transmit(std::uint64_t lun, std::uint32_t protocol,
                               ByteView apdu, MutableByteView response,
                               std::size_t& response_length) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    response_length = 0U;
    if (!valid_lun(lun) || !channel_configured()) return IfdResult::no_such_device;
    const auto connected = ensure_client();
    if (!connected) return IfdResult::icc_not_present;
    if (protocol != kIfdTransmitProtocolT1) {
        return IfdResult::protocol_not_supported;
    }
    if (!present_ || !powered_ || card_handle_ == 0U) {
        return IfdResult::icc_not_present;
    }
    if (apdu.data == nullptr || apdu.size == 0U ||
        apdu.size > kIfdApduMaxLength || response.data == nullptr ||
        response.size == 0U) {
        return IfdResult::communication_error;
    }
    const auto transmitted = client_->transmit(card_handle_, apdu, response);
    if (!transmitted) return fail(transmitted.error(), IfdOperation::transmit);
    response_length = transmitted.value();
    return IfdResult::success;
}

IfdResult IfdAdapter::control(std::uint64_t lun, std::uint32_t control_code,
                              ByteView input, MutableByteView output,
                              std::size_t& returned) noexcept
{
    (void)control_code;
    (void)input;
    (void)output;
    std::lock_guard<std::mutex> lock(mutex_);
    returned = 0U;
    return valid_lun(lun) && channel_configured() ? IfdResult::not_supported :
                                             IfdResult::no_such_device;
}

IfdResult IfdAdapter::presence(std::uint64_t lun) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_lun(lun) || !channel_configured()) return IfdResult::no_such_device;
    const auto connected = ensure_client();
    if (!connected) {
        if (profile_.transport_failure_means_absent) return IfdResult::icc_not_present;
        const IfdResult result = fail(connected.error(), IfdOperation::status);
        return result == IfdResult::no_such_device ||
               result == IfdResult::response_timeout || result == IfdResult::not_supported
            ? IfdResult::communication_error : result;
    }
    const auto status = refresh_status();
    if (!status) {
        if (profile_.transport_failure_means_absent) return IfdResult::icc_not_present;
        const IfdResult result = fail(status.error(), IfdOperation::status);
        if (result == IfdResult::no_such_device && status.error() != Error::NOT_FOUND) {
            return IfdResult::communication_error;
        }
        return result == IfdResult::response_timeout || result == IfdResult::not_supported
            ? IfdResult::communication_error : result;
    }
    return status.value().present ? IfdResult::icc_present :
                                    IfdResult::icc_not_present;
}

}  // namespace dtv::userland::unix_pcsc_ifd
