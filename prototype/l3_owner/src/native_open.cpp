/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_usb.h"

#include <charconv>
#include <algorithm>
#include <array>
#include <new>
#include <string_view>

namespace {

using l3_owner_prototype::NativeUsbProvider;

struct ParsedLocation {
    uint8_t bus = 0;
    std::vector<uint8_t> ports;
};

bool parse_number(std::string_view text, unsigned int *value)
{
    if (text.empty() || value == nullptr) return false;
    unsigned int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_location(const char *text, ParsedLocation *location)
{
    if (text == nullptr || location == nullptr) return false;
    const std::string_view value(text);
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos || dash == 0U ||
        dash + 1U >= value.size() || value.find('-', dash + 1U) !=
                                         std::string_view::npos) {
        return false;
    }
    unsigned int bus = 0;
    if (!parse_number(value.substr(0, dash), &bus) || bus == 0U || bus > 255U) {
        return false;
    }
    std::size_t begin = dash + 1U;
    while (begin < value.size()) {
        const std::size_t end = value.find('.', begin);
        const auto part = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin
                                                  : end - begin);
        unsigned int port = 0;
        if (!parse_number(part, &port) || port == 0U || port > 255U ||
            location->ports.size() >= 16U ||
            (end != std::string_view::npos && end + 1U == value.size())) {
            return false;
        }
        location->ports.push_back(static_cast<uint8_t>(port));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    if (location->ports.empty()) return false;
    location->bus = static_cast<uint8_t>(bus);
    return true;
}

bool matches_location(libusb_device *device, const ParsedLocation &location)
{
    if (libusb_get_bus_number(device) != location.bus) return false;
    std::array<uint8_t, 16U> ports{};
    const int count = libusb_get_port_numbers(device, ports.data(),
                                               static_cast<int>(ports.size()));
    return count == static_cast<int>(location.ports.size()) &&
           std::equal(location.ports.begin(), location.ports.end(), ports.begin());
}

struct PublicLogger {
    dtv_l3_public_logger logger{};
};

void public_logger_callback(void *userdata, dtv_l3_log_level level,
                            const char *function, const char *message)
{
    auto *holder = static_cast<PublicLogger *>(userdata);
    if (holder != nullptr && holder->logger.fn != nullptr) {
        holder->logger.fn(holder->logger.userdata,
                          static_cast<dtv_l3_public_log_level>(level),
                          function, message);
    }
}

dtv_l3_logger to_internal_logger(PublicLogger *holder)
{
    dtv_l3_logger result{};
    result.fn = public_logger_callback;
    result.userdata = holder;
    result.min_level = static_cast<dtv_l3_log_level>(holder->logger.min_level);
    return result;
}

dtv_l3_firmware to_internal_firmware(
    const dtv_l3_public_firmware &firmware,
    dtv_l3_firmware_manifest *manifest)
{
    dtv_l3_firmware result{};
    if (firmware.bytes == nullptr || firmware.size == 0U) return result;
    manifest->format = firmware.format;
    manifest->length_bytes = firmware.size;
    manifest->sha256 = firmware.sha256;
    manifest->flags = 0U;
    result.source = DTV_L3_FIRMWARE_BYTES;
    result.manifest = manifest;
    result.bytes = firmware.bytes;
    result.size = firmware.size;
    return result;
}

dtv_l3_result create_native_context(const dtv_l3_public_open_args *args,
                                    dtv_l3_context **owner_out,
                                    dtv_l3_op **op_out)
{
    if (args == nullptr || args->product == nullptr || args->location == nullptr ||
        args->firmware.bytes == nullptr || args->firmware.size == 0U ||
        args->logger.min_level > DTV_L3_PUBLIC_LOG_DEBUG) {
        return DTV_L3_INVALID;
    }
    const auto *product = args->product;
    if (product->create == nullptr || product->vendor_id == 0U ||
        product->product_id == 0U) {
        return DTV_L3_INVALID;
    }
    auto logger_owner = std::make_shared<PublicLogger>();
    logger_owner->logger = args->logger;
    ParsedLocation location{};
    if (!parse_location(args->location, &location)) return DTV_L3_INVALID;

    libusb_context *usb_context = nullptr;
    if (libusb_init(&usb_context) != 0) return DTV_L3_FAILED;
    libusb_device **devices = nullptr;
    const ssize_t device_count = libusb_get_device_list(usb_context, &devices);
    if (device_count < 0) {
        libusb_exit(usb_context);
        return DTV_L3_FAILED;
    }
    libusb_device_handle *handle = nullptr;
    bool matched = false;
    for (ssize_t index = 0; index < device_count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != 0 ||
            descriptor.idVendor != product->vendor_id ||
            descriptor.idProduct != product->product_id ||
            !matches_location(devices[index], location)) {
            continue;
        }
        matched = true;
        if (libusb_open(devices[index], &handle) == 0) break;
    }
    libusb_free_device_list(devices, 1);
    if (handle == nullptr) {
        libusb_exit(usb_context);
        return matched ? DTV_L3_BUSY : DTV_L3_DISCONNECTED;
    }

    dtv_l3_firmware_manifest manifest{};
    const dtv_l3_firmware firmware =
        to_internal_firmware(args->firmware, &manifest);
    std::array<dtv_l3_native_claim, 8> claims{};
    uint32_t claim_count = 0U;
    const dtv_l3_product_adapter *adapter = nullptr;
    void *adapter_owner = nullptr;
    dtv_l3_result created = DTV_L3_FAILED;
    try {
        created = product->create(
            product->product_context, handle, &firmware, &adapter, claims.data(),
            static_cast<uint32_t>(claims.size()), &claim_count, &adapter_owner);
    } catch (...) {
        if (product->release != nullptr && adapter_owner != nullptr) {
            product->release(adapter_owner);
        }
        libusb_close(handle);
        libusb_exit(usb_context);
        return DTV_L3_FAILED;
    }
    if (created != DTV_L3_OK || adapter == nullptr || claim_count == 0U ||
        claim_count > claims.size()) {
        if (product->release != nullptr && adapter_owner != nullptr) {
            product->release(adapter_owner);
        }
        libusb_close(handle);
        libusb_exit(usb_context);
        return created == DTV_L3_OK ? DTV_L3_FAILED : created;
    }
    for (uint32_t index = 0U; index < claim_count; ++index) {
        if (claims[index].interface_number < 0 ||
            claims[index].interface_number > 255 ||
            claims[index].alternate_setting < -1 ||
            claims[index].alternate_setting > 255) {
            if (product->release != nullptr && adapter_owner != nullptr) {
                product->release(adapter_owner);
            }
            libusb_close(handle);
            libusb_exit(usb_context);
            return DTV_L3_INVALID;
        }
        for (uint32_t earlier = 0U; earlier < index; ++earlier) {
            if (claims[earlier].interface_number ==
                claims[index].interface_number) {
                if (product->release != nullptr && adapter_owner != nullptr) {
                    product->release(adapter_owner);
                }
                libusb_close(handle);
                libusb_exit(usb_context);
                return DTV_L3_INVALID;
            }
        }
    }

    std::shared_ptr<NativeUsbProvider> native;
    try {
        native = std::make_shared<NativeUsbProvider>(usb_context, handle,
                                                      to_internal_logger(logger_owner.get()));
        native->set_logger_owner(logger_owner);
    } catch (...) {
        if (product->release != nullptr && adapter_owner != nullptr) {
            product->release(adapter_owner);
        }
        libusb_close(handle);
        libusb_exit(usb_context);
        return DTV_L3_FAILED;
    }
    try {
        native->set_claims(std::vector<dtv_l3_native_claim>(
            claims.begin(), claims.begin() + claim_count));
    } catch (...) {
        if (product->release != nullptr && adapter_owner != nullptr) {
            product->release(adapter_owner);
        }
        return DTV_L3_FAILED;
    }
    const dtv_l3_usb_provider provider = native->provider();
    const dtv_l3_open_result opened =
        dtv_l3_open_ex(&provider, adapter, claim_count);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == nullptr ||
        opened.op == nullptr) {
        if (product->release != nullptr && adapter_owner != nullptr) {
            product->release(adapter_owner);
        }
        return opened.result;
    }
    l3_owner_prototype::attach_native_owner(opened.owner, native,
                                            product->release, adapter_owner,
                                            args->power_permission);
    *owner_out = opened.owner;
    *op_out = opened.op;
    return DTV_L3_ACCEPTED;
}

}  // namespace

struct dtv_l3_public_context {
    dtv_l3_context *owner = nullptr;
    dtv_l3_op *open_op = nullptr;
};

extern "C" dtv_l3_public_result dtv_l3_public_open(
    const dtv_l3_public_open_args *args,
    dtv_l3_public_context **context_out)
{
    if (context_out != nullptr) *context_out = nullptr;
    if (args == nullptr || context_out == nullptr ||
        args->struct_size < sizeof(dtv_l3_public_open_args)) {
        return DTV_L3_PUBLIC_INVALID;
    }
    auto *context = new (std::nothrow) dtv_l3_public_context;
    if (context == nullptr) return DTV_L3_PUBLIC_FAILED;
    dtv_l3_result opened = DTV_L3_FAILED;
    try {
        opened = create_native_context(args, &context->owner, &context->open_op);
    } catch (...) {
        delete context;
        return DTV_L3_PUBLIC_FAILED;
    }
    if (opened != DTV_L3_ACCEPTED) {
        delete context;
        return l3_owner_prototype::map_public_result(opened);
    }
    *context_out = context;
    return DTV_L3_PUBLIC_ACCEPTED;
}

extern "C" dtv_l3_public_result dtv_l3_public_wait_ready(
    dtv_l3_public_context *context, uint32_t timeout_ms)
{
    if (context == nullptr || context->owner == nullptr ||
        context->open_op == nullptr) {
        return DTV_L3_PUBLIC_INVALID;
    }
    const dtv_l3_result waited = dtv_l3_op_wait(context->open_op, timeout_ms);
    if (waited != DTV_L3_OK) return l3_owner_prototype::map_public_result(waited);
    dtv_l3_op_result result{};
    const dtv_l3_result polled = dtv_l3_op_poll(context->open_op, &result);
    if (polled != DTV_L3_OK) return l3_owner_prototype::map_public_result(polled);
    if (result.state == DTV_L3_OP_RUNNING || result.state == DTV_L3_OP_OPENING) {
        return DTV_L3_PUBLIC_TIMEOUT;
    }
    (void)dtv_l3_op_release(context->open_op);
    context->open_op = nullptr;
    return l3_owner_prototype::map_public_result(result.result);
}

extern "C" dtv_l3_public_result dtv_l3_public_close(
    dtv_l3_public_context *context, uint32_t timeout_ms)
{
    if (context == nullptr || context->owner == nullptr) {
        return DTV_L3_PUBLIC_INVALID;
    }
    (void)dtv_l3_close(context->owner, timeout_ms);
    if (context->open_op != nullptr) {
        dtv_l3_op_result operation{};
        if (dtv_l3_op_poll(context->open_op, &operation) != DTV_L3_OK ||
            operation.state == DTV_L3_OP_RUNNING ||
            operation.state == DTV_L3_OP_OPENING) {
            return DTV_L3_PUBLIC_TIMEOUT;
        }
        (void)dtv_l3_op_release(context->open_op);
        context->open_op = nullptr;
    }
    dtv_l3_close_status status{};
    if (dtv_l3_close_status_get(context->owner, &status) != DTV_L3_OK) {
        return DTV_L3_PUBLIC_FAILED;
    }
    if (status.cleanup_complete == 0U) {
        (void)dtv_l3_cleanup_retry(context->owner);
        if (dtv_l3_close_status_get(context->owner, &status) != DTV_L3_OK) {
            return DTV_L3_PUBLIC_FAILED;
        }
    }
    if (status.releasable == 0U) {
        return status.cleanup_complete == 0U ? DTV_L3_PUBLIC_TIMEOUT
                                             : DTV_L3_PUBLIC_BUSY;
    }
    const dtv_l3_result released = dtv_l3_context_release(context->owner);
    if (released != DTV_L3_OK) return l3_owner_prototype::map_public_result(released);
    context->owner = nullptr;
    delete context;
    return DTV_L3_PUBLIC_OK;
}
