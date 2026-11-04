/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_PENDING 8

struct pending {
    int used;
    dtv_l3_provider_binding *binding;
    uint64_t id;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
};

struct fixture {
    unsigned int claim_calls;
    unsigned int release_claim_calls;
    unsigned int submit_calls;
    unsigned int cancel_calls;
    unsigned int tune_calls;
    unsigned int bridge_start_calls;
    unsigned int bridge_stop_calls;
    unsigned int attach_calls;
    unsigned int stream_stop_calls;
    unsigned int reader_calls;
    unsigned int lnb_calls;
    unsigned int factory_create_calls;
    unsigned int factory_release_calls;
    uint32_t lnb_mode;
    dtv_l3_open_config seen_config;
    const uint8_t *seen_firmware;
    size_t seen_firmware_size;
    dtv_l3_ts_deliver_fn deliver;
    void *deliver_context;
    dtv_l3_product_adapter factory_adapter;
    struct pending pending[MAX_PENDING];
};

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
}

static struct pending *find_pending(struct fixture *fixture, uint64_t id) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used &&
            fixture->pending[index].id == id) {
            return &fixture->pending[index];
        }
    }
    return NULL;
}

static struct pending *free_pending(struct fixture *fixture) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (!fixture->pending[index].used) {
            return &fixture->pending[index];
        }
    }
    return NULL;
}

static dtv_l3_claim_result usb_claim(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->claim_calls;
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_release_result usb_release_claim(void *context,
                                               dtv_l3_provider_binding *binding,
                                               uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_claim_calls;
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result usb_submit(void *context,
                                dtv_l3_provider_binding *binding,
                                uint64_t transfer_id,
                                const dtv_l3_transfer_request *request,
                                dtv_l3_transfer_complete_fn complete,
                                void *callback_context) {
    struct fixture *fixture = context;
    struct pending *slot;
    (void)request;
    ++fixture->submit_calls;
    slot = free_pending(fixture);
    if (slot == NULL) {
        return DTV_L3_BUSY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->binding = binding;
    slot->id = transfer_id;
    slot->complete = complete;
    slot->callback_context = callback_context;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result usb_cancel(void *context,
                                dtv_l3_provider_binding *binding,
                                uint64_t transfer_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)transfer_id;
    ++fixture->cancel_calls;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result product_scope(void *context,
                                   uint32_t receiver_id,
                                   dtv_l3_scope_info *scope) {
    (void)context;
    (void)receiver_id;
    scope->bridge_id = 3;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
    return DTV_L3_OK;
}

static dtv_l3_result product_tune(void *context,
                                  dtv_l3_provider_binding *binding,
                                  uint32_t receiver_id,
                                  uint64_t requested_hz,
                                  uint64_t *programmed_hz) {
    struct fixture *fixture = context;
    (void)binding;
    (void)receiver_id;
    ++fixture->tune_calls;
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

static dtv_l3_result product_bridge_start(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t bridge_id,
                                          uint32_t receiver_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    (void)receiver_id;
    ++fixture->bridge_start_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_bridge_stop(void *context,
                                         dtv_l3_provider_binding *binding,
                                         uint32_t bridge_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)bridge_id;
    ++fixture->bridge_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_attach(void *context,
                                    dtv_l3_provider_binding *binding,
                                    uint64_t stream_id,
                                    dtv_l3_ts_deliver_fn deliver,
                                    void *callback_context) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->attach_calls;
    fixture->deliver = deliver;
    fixture->deliver_context = callback_context;
    return DTV_L3_OK;
}

static dtv_l3_result product_stream_stop(void *context,
                                         dtv_l3_provider_binding *binding,
                                         uint64_t stream_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)stream_id;
    ++fixture->stream_stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader(void *context,
                                    uint32_t reader_id,
                                    dtv_l3_reader **reader_out) {
    struct fixture *fixture = context;
    (void)reader_id;
    ++fixture->reader_calls;
    *reader_out = (dtv_l3_reader *)fixture;
    return DTV_L3_OK;
}

static dtv_l3_result product_lnb(void *context,
                                 uint32_t receiver_id,
                                 uint32_t mode) {
    struct fixture *fixture = context;
    (void)receiver_id;
    ++fixture->lnb_calls;
    fixture->lnb_mode = mode;
    return DTV_L3_OK;
}

static dtv_l3_usb_provider usb_provider_for(struct fixture *fixture) {
    return (dtv_l3_usb_provider){
        .provider_context = fixture,
        .claim = usb_claim,
        .release_claim = usb_release_claim,
        .submit = usb_submit,
        .cancel = usb_cancel,
        .cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE};
}

static dtv_l3_product_adapter adapter_for(struct fixture *fixture,
                                          const dtv_l3_firmware *firmware) {
    return (dtv_l3_product_adapter){
        .product_context = fixture,
        .capabilities = {.receiver_count = 2,
                         .reader_count = 1,
                         .lnb_power = 1,
                         .serial = 0,
                         .cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE},
        .firmware = firmware,
        .scope_query = product_scope,
        .tune = product_tune,
        .bridge_start = product_bridge_start,
        .bridge_stop = product_bridge_stop,
        .stream_attach = product_attach,
        .stream_stop = product_stream_stop,
        .reader_acquire = product_reader,
        .lnb_set = product_lnb};
}

static int wait_op_terminal(dtv_l3_op *op, dtv_l3_op_result *result) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return 0;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int open_ready_ex(dtv_l3_usb_provider *usb,
                         dtv_l3_product_adapter *adapter,
                         dtv_l3_context **owner_out) {
    dtv_l3_open_result opened = dtv_l3_open_ex(usb, adapter, 1);
    dtv_l3_op_result result;
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 0;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 0;
    }
    *owner_out = opened.owner;
    return 1;
}

static int wait_releasable(dtv_l3_context *owner) {
    unsigned int attempt;
    dtv_l3_close_status status;
    for (attempt = 0; attempt < 4000; ++attempt) {
        if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK) {
            return 0;
        }
        if (status.releasable) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static int close_and_release(dtv_l3_context *owner) {
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner)) {
        return 0;
    }
    return dtv_l3_context_release(owner) == DTV_L3_OK;
}

static int fire_pending(struct fixture *fixture, uint64_t id) {
    struct pending slot;
    struct pending *found = find_pending(fixture, id);
    uint8_t data[2] = {0xaa, 0xbb};
    if (found == NULL) {
        return 0;
    }
    slot = *found;
    memset(found, 0, sizeof(*found));
    const dtv_l3_transfer_completion completion = {
        .result = DTV_L3_OK,
        .data = data,
        .size = sizeof(data),
        .actual_length = sizeof(data),
        .actual_length_known = 1};
    slot.complete(slot.binding, slot.id, &completion, slot.callback_context);
    return 1;
}

static uint64_t only_pending_id(struct fixture *fixture) {
    unsigned int index;
    for (index = 0; index < MAX_PENDING; ++index) {
        if (fixture->pending[index].used) {
            return fixture->pending[index].id;
        }
    }
    return 0;
}

static int exercise_product(dtv_l3_context *owner, struct fixture *fixture) {
    dtv_l3_transfer_request request;
    dtv_l3_op *op = NULL;
    dtv_l3_op_result op_result;
    dtv_l3_lease *lease = NULL;
    dtv_l3_stream *stream = NULL;
    dtv_l3_lease_status lease_status;
    dtv_l3_stream_status stream_status;
    dtv_l3_reader *reader = NULL;
    uint64_t programmed = 0;
    uint64_t revision = 0;
    uint64_t id;
    uint8_t ts_data[376];
    uint8_t out[188];
    size_t bytes = 0;
    dtv_l3_read_state read_state;

    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.timeout_ms = 0;
    request.in_capacity = 64;
    request.claim_index = 0;
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == NULL) {
        return 10;
    }
    id = only_pending_id(fixture);
    if (!fire_pending(fixture, id) || !wait_op_terminal(op, &op_result) ||
        op_result.state != DTV_L3_OP_DONE || op_result.data_size != 2 ||
        dtv_l3_op_release(op) != DTV_L3_OK) {
        return 11;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_OK ||
        dtv_l3_lease_tune(lease, 500000000, &programmed, &revision) !=
            DTV_L3_OK ||
        dtv_l3_lease_start(lease) != DTV_L3_OK ||
        dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK ||
        stream == NULL || fixture->deliver == NULL) {
        return 12;
    }
    memset(ts_data, 0x47, sizeof(ts_data));
    fixture->deliver(fixture->deliver_context, ts_data, sizeof(ts_data));
    if (dtv_l3_stream_read_ts(stream, out, sizeof(out), &bytes,
                              &read_state) != DTV_L3_OK ||
        read_state != DTV_L3_READ_DATA || bytes != sizeof(out)) {
        return 13;
    }
    if (dtv_l3_stream_status_get(stream, &stream_status) != DTV_L3_OK ||
        dtv_l3_lease_status_get(lease, &lease_status) != DTV_L3_OK) {
        return 14;
    }
    if (dtv_l3_stream_stop(stream, 0) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 15;
    }
    if (dtv_l3_reader_acquire(owner, 0, &reader) != DTV_L3_OK ||
        reader == NULL || fixture->reader_calls != 1) {
        return 16;
    }
    if (dtv_l3_lnb_set(owner, 0, 1) != DTV_L3_OK || fixture->lnb_calls != 1 ||
        fixture->lnb_mode != 1) {
        return 17;
    }
    if (fixture->tune_calls != 1 || fixture->bridge_start_calls != 1 ||
        fixture->attach_calls != 1 || fixture->stream_stop_calls != 1) {
        return 18;
    }
    return 0;
}

static int test_separated_open(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_capabilities capabilities;
    dtv_l3_context *owner = NULL;
    const uint8_t firmware[3] = {0x01, 0x02, 0x03};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .bytes = firmware,
        .size = sizeof(firmware)};
    int rc;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    adapter = adapter_for(&fixture, &firmware_view);
    if (!open_ready_ex(&usb, &adapter, &owner)) {
        return 1;
    }
    if (dtv_l3_capabilities_get(owner, &capabilities) != DTV_L3_OK ||
        capabilities.receiver_count != 2 || capabilities.reader_count != 1 ||
        capabilities.lnb_power != 1 || capabilities.serial != 0 ||
        capabilities.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE) {
        return 2;
    }
    rc = exercise_product(owner, &fixture);
    if (rc != 0) {
        return rc;
    }
    if (!close_and_release(owner)) {
        return 19;
    }
    if (fixture.release_claim_calls != 1 || fixture.cancel_calls != 0) {
        return 20;
    }
    return 0;
}

static dtv_l3_result factory_create_ok(void *context,
                                       const dtv_l3_open_config *config,
                                       const dtv_l3_product_adapter **out,
                                       void **owner_out) {
    struct fixture *fixture = context;
    ++fixture->factory_create_calls;
    if (config != NULL) {
        fixture->seen_config = *config;
        if (config->firmware != NULL &&
            config->firmware->source == DTV_L3_FIRMWARE_BYTES) {
            fixture->seen_firmware = config->firmware->bytes;
            fixture->seen_firmware_size = config->firmware->size;
        }
    }
    *out = &fixture->factory_adapter;
    *owner_out = fixture;
    return DTV_L3_OK;
}

static dtv_l3_result factory_create_fail(void *context,
                                         const dtv_l3_open_config *config,
                                         const dtv_l3_product_adapter **out,
                                         void **owner_out) {
    struct fixture *fixture = context;
    (void)config;
    ++fixture->factory_create_calls;
    *out = NULL;
    *owner_out = fixture;
    return DTV_L3_FAILED;
}

static void factory_release(void *owner) {
    struct fixture *fixture = owner;
    ++fixture->factory_release_calls;
}

static dtv_l3_adapter_factory factory_for(struct fixture *fixture) {
    return (dtv_l3_adapter_factory){.factory_context = fixture,
                                    .create = factory_create_ok,
                                    .release = factory_release};
}

static int test_factory_open(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_adapter_factory factory;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    const uint8_t firmware[2] = {0x10, 0x20};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .bytes = firmware,
        .size = sizeof(firmware)};
    int rc;

    memset(&fixture, 0, sizeof(fixture));
    fixture.factory_adapter = adapter_for(&fixture, &firmware_view);
    usb = usb_provider_for(&fixture);
    factory = factory_for(&fixture);
    memset(&config, 0, sizeof(config));
    config.model = 7;
    config.selector = 0x1234;
    config.firmware = &firmware_view;
    opened = dtv_l3_open_factory(&usb, &factory, &config, 1);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL) {
        return 1;
    }
    if (fixture.factory_create_calls != 1 ||
        fixture.seen_config.model != 7 ||
        fixture.seen_config.selector != 0x1234 ||
        fixture.seen_firmware != firmware ||
        fixture.seen_firmware_size != sizeof(firmware)) {
        return 2;
    }
    if (dtv_l3_op_wait(opened.op, 2000) != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 3;
    }
    rc = exercise_product(opened.owner, &fixture);
    if (rc != 0) {
        return rc;
    }
    if (!close_and_release(opened.owner)) {
        return 4;
    }
    if (fixture.factory_release_calls != 1) {
        return 5;
    }
    return 0;
}

static int test_factory_failure(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_adapter_factory factory;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    factory = factory_for(&fixture);
    factory.create = factory_create_fail;
    memset(&config, 0, sizeof(config));
    opened = dtv_l3_open_factory(&usb, &factory, &config, 1);
    if (opened.result != DTV_L3_FAILED || opened.owner != NULL ||
        opened.op != NULL || fixture.factory_create_calls != 1 ||
        fixture.factory_release_calls != 1 || fixture.claim_calls != 0) {
        return 1;
    }
    return 0;
}

static int test_missing_product_callbacks(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter;
    dtv_l3_context *owner = NULL;
    dtv_l3_lease *lease = NULL;

    memset(&fixture, 0, sizeof(fixture));
    usb = usb_provider_for(&fixture);
    memset(&adapter, 0, sizeof(adapter));
    adapter.product_context = &fixture;
    adapter.capabilities.receiver_count = 1;
    if (!open_ready_ex(&usb, &adapter, &owner)) {
        return 1;
    }
    if (dtv_l3_lease_acquire(owner, 0, &lease) != DTV_L3_UNSUPPORTED ||
        lease != NULL) {
        return 2;
    }
    if (dtv_l3_reader_acquire(owner, 0, NULL) != DTV_L3_INVALID) {
        return 3;
    }
    if (!close_and_release(owner)) {
        return 4;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "separated") == 0) {
        return test_separated_open();
    }
    if (strcmp(argv[1], "factory") == 0) {
        return test_factory_open();
    }
    if (strcmp(argv[1], "factory-fail") == 0) {
        return test_factory_failure();
    }
    if (strcmp(argv[1], "missing") == 0) {
        return test_missing_product_callbacks();
    }
    return 65;
}
