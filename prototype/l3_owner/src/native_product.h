/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef L3_OWNER_NATIVE_PRODUCT_H
#define L3_OWNER_NATIVE_PRODUCT_H

#include <dtv/l3_owner.h>
#include <dtv/l3.h>

#include <memory>

struct libusb_device_handle;

struct dtv_l3_native_claim {
    int interface_number;
    /* -1 keeps the device's current alternate setting. */
    int alternate_setting;
};

struct dtv_l3_product {
    uint16_t vendor_id;
    uint16_t product_id;
    void *product_context;
    dtv_l3_result (*create)(
        void *product_context,
        libusb_device_handle *handle,
        const dtv_l3_firmware *firmware,
        const dtv_l3_product_adapter **adapter_out,
        struct dtv_l3_native_claim *claims,
        uint32_t claim_capacity,
        uint32_t *claim_count_out,
        void **adapter_owner_out);
    dtv_l3_adapter_release_fn release;
};

namespace l3_owner_prototype {
void attach_native_owner(dtv_l3_context *owner,
                         std::shared_ptr<void> native_owner,
                         dtv_l3_adapter_release_fn release,
                         void *adapter_owner,
                         uint32_t power_permission);
}

#endif
