/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef L3_OWNER_NATIVE_USB_H
#define L3_OWNER_NATIVE_USB_H

#include <dtv/l3_owner.h>

#include <libusb.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "native_product.h"

namespace l3_owner_prototype {

class NativeUsbProvider {
public:
    NativeUsbProvider(libusb_context *context,
                      libusb_device_handle *handle,
                      const dtv_l3_logger &logger);
    ~NativeUsbProvider();

    NativeUsbProvider(const NativeUsbProvider &) = delete;
    NativeUsbProvider &operator=(const NativeUsbProvider &) = delete;

    void set_claims(const std::vector<dtv_l3_native_claim> &claims);
    void set_logger_owner(std::shared_ptr<void> logger_owner);
    dtv_l3_usb_provider provider();

private:
    struct Transfer;
    static dtv_l3_claim_result claim(void *, dtv_l3_provider_binding *, uint32_t);
    static dtv_l3_release_result release_claim(void *, dtv_l3_provider_binding *, uint32_t);
    static dtv_l3_result submit(void *, dtv_l3_provider_binding *, uint64_t,
                                const dtv_l3_transfer_request *,
                                dtv_l3_transfer_complete_fn, void *);
    static dtv_l3_result cancel(void *, dtv_l3_provider_binding *, uint64_t);
    static void LIBUSB_CALL complete(libusb_transfer *transfer);
    void event_loop();

    libusb_context *context_ = nullptr;
    libusb_device_handle *handle_ = nullptr;
    dtv_l3_logger logger_{};
    std::vector<dtv_l3_native_claim> claims_;
    std::vector<bool> claimed_;
    std::shared_ptr<void> logger_owner_;
    std::mutex mutex_;
    std::map<uint64_t, std::shared_ptr<Transfer>> transfers_;
    std::atomic<bool> stopping_{false};
    std::thread event_thread_;
};

dtv_l3_public_result map_public_result(dtv_l3_result result);

}  // namespace l3_owner_prototype

#endif
