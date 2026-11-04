/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_usb.h"

#include <dtv/l3.h>

#include "owner_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

namespace l3_owner_prototype {

struct NativeUsbProvider::Transfer {
    NativeUsbProvider *owner = nullptr;
    dtv_l3_provider_binding *binding = nullptr;
    uint64_t id = 0;
    dtv_l3_transfer_complete_fn callback = nullptr;
    void *callback_context = nullptr;
    libusb_transfer *transfer = nullptr;
    std::vector<unsigned char> buffer;
    bool control_in = false;
    bool input = false;

    ~Transfer()
    {
        if (transfer != nullptr) {
            libusb_free_transfer(transfer);
        }
    }
};

NativeUsbProvider::NativeUsbProvider(libusb_context *context,
                                     libusb_device_handle *handle,
                                     const dtv_l3_logger &logger)
    : context_(context), handle_(handle), logger_(logger)
{
    event_thread_ = std::thread(&NativeUsbProvider::event_loop, this);
}

NativeUsbProvider::~NativeUsbProvider()
{
    stopping_.store(true, std::memory_order_release);
    for (;;) {
        std::shared_ptr<Transfer> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (transfers_.empty()) break;
            pending = transfers_.begin()->second;
        }
        (void)libusb_cancel_transfer(pending->transfer);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (event_thread_.joinable()) {
        event_thread_.join();
    }
    for (std::size_t index = claimed_.size(); index > 0U; --index) {
        if (claimed_[index - 1U]) {
            (void)libusb_release_interface(
                handle_, claims_[index - 1U].interface_number);
        }
    }
    if (handle_ != nullptr) {
        libusb_close(handle_);
    }
    if (context_ != nullptr) {
        libusb_exit(context_);
    }
}

void NativeUsbProvider::set_claims(
    const std::vector<dtv_l3_native_claim> &claims)
{
    claims_ = claims;
    claimed_.assign(claims.size(), false);
}

void NativeUsbProvider::set_logger_owner(std::shared_ptr<void> logger_owner)
{
    logger_owner_ = std::move(logger_owner);
}

dtv_l3_usb_provider NativeUsbProvider::provider()
{
    dtv_l3_usb_provider result{};
    result.provider_context = this;
    result.claim = claim;
    result.release_claim = release_claim;
    result.submit = submit;
    result.cancel = cancel;
    result.claim_cancel = nullptr;
    result.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    result.call_flags = DTV_L3_CALL_FLAG_CANCEL_IN_FRAME |
                        DTV_L3_CALL_FLAG_SUBMIT_IN_FRAME;
    result.logger = logger_;
    return result;
}

dtv_l3_claim_result NativeUsbProvider::claim(
    void *context, dtv_l3_provider_binding *, uint32_t index)
{
    auto *self = static_cast<NativeUsbProvider *>(context);
    if (self == nullptr || index >= self->claims_.size()) {
        return {DTV_L3_INVALID, DTV_L3_CLAIM_NONE};
    }
    const auto &spec = self->claims_[index];
    if (libusb_claim_interface(self->handle_, spec.interface_number) != 0) {
        return {DTV_L3_FAILED, DTV_L3_CLAIM_NONE};
    }
    if (spec.alternate_setting >= 0 &&
        libusb_set_interface_alt_setting(self->handle_, spec.interface_number,
                                         spec.alternate_setting) != 0) {
        const int released =
            libusb_release_interface(self->handle_, spec.interface_number);
        if (released == 0) {
            return {DTV_L3_FAILED, DTV_L3_CLAIM_NONE};
        }
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->claimed_[index] = true;
        return {DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
    }
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->claimed_[index] = true;
    return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result NativeUsbProvider::release_claim(
    void *context, dtv_l3_provider_binding *, uint32_t index)
{
    auto *self = static_cast<NativeUsbProvider *>(context);
    if (self == nullptr || index >= self->claims_.size()) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_UNKNOWN,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_INVALID};
    }
    bool claimed = false;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        claimed = self->claimed_[index];
    }
    if (!claimed) {
        return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
    }
    const int released = libusb_release_interface(
        self->handle_, self->claims_[index].interface_number);
    if (released != 0) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN,
                DTV_L3_CLAIM_RETAINED,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_FAILED};
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->claimed_[index] = false;
    }
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
            DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result NativeUsbProvider::submit(
    void *context, dtv_l3_provider_binding *binding, uint64_t id,
    const dtv_l3_transfer_request *request,
    dtv_l3_transfer_complete_fn callback, void *callback_context)
{
    auto *self = static_cast<NativeUsbProvider *>(context);
    if (self == nullptr || binding == nullptr || request == nullptr ||
        callback == nullptr || request->claim_index >= self->claims_.size()) {
        return DTV_L3_INVALID;
    }
    const bool control = request->kind == DTV_L3_TRANSFER_CONTROL;
    if (!control && request->kind != DTV_L3_TRANSFER_BULK) {
        return DTV_L3_UNSUPPORTED;
    }
    const bool input = control
        ? (request->control.request_type & LIBUSB_ENDPOINT_DIR_MASK) ==
              LIBUSB_ENDPOINT_IN
        : (request->endpoint & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
    if ((input && request->out_size != 0U) ||
        (!input && request->in_capacity != 0U) ||
        (!control && (request->endpoint == 0U || request->endpoint > 255U))) {
        return DTV_L3_INVALID;
    }
    const std::size_t data_size = input ? request->in_capacity : request->out_size;
    if ((!input && data_size != 0U && request->out_data == nullptr) ||
        data_size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        (control && data_size > std::numeric_limits<uint16_t>::max())) {
        return DTV_L3_INVALID;
    }
    std::shared_ptr<Transfer> operation;
    try {
        operation = std::make_shared<Transfer>();
        const std::size_t prefix = control ? LIBUSB_CONTROL_SETUP_SIZE : 0U;
        operation->buffer.resize(prefix + data_size);
    } catch (...) {
        return DTV_L3_FAILED;
    }
    operation->owner = self;
    operation->binding = binding;
    operation->id = id;
    operation->callback = callback;
    operation->callback_context = callback_context;
    operation->control_in = control && input;
    operation->input = input;
    const std::size_t prefix = control ? LIBUSB_CONTROL_SETUP_SIZE : 0U;
    if (!input && data_size != 0U) {
        std::memcpy(operation->buffer.data() + prefix, request->out_data,
                    data_size);
    }
    operation->transfer = libusb_alloc_transfer(0);
    if (operation->transfer == nullptr) {
        return DTV_L3_FAILED;
    }
    if (control) {
        libusb_fill_control_setup(operation->buffer.data(),
                                  request->control.request_type,
                                  request->control.request,
                                  request->control.value,
                                  request->control.index,
                                  static_cast<uint16_t>(data_size));
        libusb_fill_control_transfer(
            operation->transfer, self->handle_, operation->buffer.data(),
            complete, operation.get(), request->timeout_ms);
    } else {
        libusb_fill_bulk_transfer(
            operation->transfer, self->handle_,
            static_cast<unsigned char>(request->endpoint),
            operation->buffer.data(), static_cast<int>(data_size), complete,
            operation.get(), request->timeout_ms);
    }
    try {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->stopping_.load(std::memory_order_acquire) ||
            self->transfers_.find(id) != self->transfers_.end()) {
            return DTV_L3_CLOSING;
        }
        self->transfers_.emplace(id, operation);
    } catch (...) {
        return DTV_L3_FAILED;
    }
    const int submitted = libusb_submit_transfer(operation->transfer);
    if (submitted != 0) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->transfers_.erase(id);
        return submitted == LIBUSB_ERROR_NO_DEVICE ? DTV_L3_DISCONNECTED
                                                   : DTV_L3_FAILED;
    }
    return DTV_L3_ACCEPTED;
}

dtv_l3_result NativeUsbProvider::cancel(
    void *context, dtv_l3_provider_binding *, uint64_t id)
{
    auto *self = static_cast<NativeUsbProvider *>(context);
    if (self == nullptr) {
        return DTV_L3_INVALID;
    }
    std::shared_ptr<Transfer> operation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        const auto found = self->transfers_.find(id);
        if (found == self->transfers_.end()) {
            return DTV_L3_ACCEPTED;
        }
        operation = found->second;
    }
    const int result = libusb_cancel_transfer(operation->transfer);
    return result == 0 || result == LIBUSB_ERROR_NOT_FOUND
               ? DTV_L3_ACCEPTED
               : result == LIBUSB_ERROR_NO_DEVICE ? DTV_L3_DISCONNECTED
                                                   : DTV_L3_FAILED;
}

void LIBUSB_CALL NativeUsbProvider::complete(libusb_transfer *transfer)
{
    auto *raw = static_cast<Transfer *>(transfer->user_data);
    if (raw == nullptr || raw->owner == nullptr) {
        return;
    }
    NativeUsbProvider *self = raw->owner;
    std::shared_ptr<Transfer> operation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        const auto found = self->transfers_.find(raw->id);
        if (found == self->transfers_.end()) {
            return;
        }
        operation = found->second;
        self->transfers_.erase(found);
    }
    dtv_l3_transfer_completion completion{};
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        completion.result = DTV_L3_OK;
        break;
    case LIBUSB_TRANSFER_TIMED_OUT:
        completion.result = DTV_L3_TIMEOUT;
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        completion.result = DTV_L3_CANCELLED;
        break;
    case LIBUSB_TRANSFER_NO_DEVICE:
        completion.result = DTV_L3_DISCONNECTED;
        break;
    default:
        completion.result = DTV_L3_FAILED;
        break;
    }
    const std::size_t prefix = operation->control_in ? LIBUSB_CONTROL_SETUP_SIZE : 0U;
    completion.data = operation->input &&
                              transfer->status == LIBUSB_TRANSFER_COMPLETED &&
                              transfer->actual_length > 0
                          ? operation->buffer.data() + prefix
                          : nullptr;
    completion.size = operation->input &&
                              transfer->status == LIBUSB_TRANSFER_COMPLETED &&
                              transfer->actual_length > 0
                          ? static_cast<std::size_t>(transfer->actual_length)
                          : 0U;
    completion.actual_length = transfer->status == LIBUSB_TRANSFER_COMPLETED
                                   ? static_cast<std::size_t>(transfer->actual_length)
                                   : 0U;
    completion.actual_length_known = 1U;
    operation->callback(operation->binding, operation->id, &completion,
                        operation->callback_context);
}

void NativeUsbProvider::event_loop()
{
    while (true) {
        if (stopping_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (transfers_.empty()) {
                return;
            }
        }
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        (void)libusb_handle_events_timeout_completed(context_, &timeout,
                                                     nullptr);
    }
}

dtv_l3_public_result map_public_result(dtv_l3_result result)
{
    switch (result) {
    case DTV_L3_OK: return DTV_L3_PUBLIC_OK;
    case DTV_L3_ACCEPTED: return DTV_L3_PUBLIC_ACCEPTED;
    case DTV_L3_TIMEOUT:
    case DTV_L3_WAIT_TIMEOUT: return DTV_L3_PUBLIC_TIMEOUT;
    case DTV_L3_UNSUPPORTED: return DTV_L3_PUBLIC_UNSUPPORTED;
    case DTV_L3_INVALID: return DTV_L3_PUBLIC_INVALID;
    case DTV_L3_BUSY:
    case DTV_L3_CLOSING: return DTV_L3_PUBLIC_BUSY;
    default: return DTV_L3_PUBLIC_FAILED;
    }
}

}  // namespace l3_owner_prototype
