/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

#include <stdatomic.h>

struct fixture {
    unsigned int release_calls;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
    dtv_l3_provider_binding *binding;
    uint64_t transfer_id;
    atomic_bool retry_entered;
    atomic_bool allow_retry;
};

static dtv_l3_claim_result claim(void *context,
                                dtv_l3_provider_binding *binding,
                                uint32_t claim_index) {
    (void)context;
    (void)binding;
    (void)claim_index;
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_release_result release_claim(void *context,
                                           dtv_l3_provider_binding *binding,
                                           uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_calls;
    if (fixture->release_calls == 1) {
        return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                       DTV_L3_CLAIM_RETAINED,
                                       DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
    }
    atomic_store(&fixture->retry_entered, 1);
    while (!atomic_load(&fixture->allow_retry)) {
    }
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result submit(void *context,
                            dtv_l3_provider_binding *binding,
                            uint64_t transfer_id,
                            const dtv_l3_transfer_request *request,
                            dtv_l3_transfer_complete_fn complete,
                            void *callback_context) {
    struct fixture *fixture = context;
    (void)request;
    fixture->complete = complete;
    fixture->callback_context = callback_context;
    fixture->binding = binding;
    fixture->transfer_id = transfer_id;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result cancel(void *context,
                           dtv_l3_provider_binding *binding,
                           uint64_t transfer_id) {
    struct fixture *fixture = context;
    const uint8_t data[] = {0x90, 0x00};
    const dtv_l3_transfer_completion completion = {DTV_L3_OK, data, sizeof(data)};
    if (fixture->binding != binding || fixture->transfer_id != transfer_id ||
        fixture->complete == NULL) {
        return DTV_L3_INVALID;
    }
    fixture->complete(binding, transfer_id, &completion,
                      fixture->callback_context);
    return DTV_L3_ACCEPTED;
}

static int wait_terminal(dtv_l3_op *op, dtv_l3_op_result *result) {
    unsigned int attempt;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return 0;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    struct fixture fixture = {0, NULL, NULL, NULL, 0,
                              ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0)};
    const dtv_l3_provider provider = {
        &fixture, claim, release_claim, submit, cancel};
    dtv_l3_open_result opened = dtv_l3_open(&provider, 1);
    dtv_l3_op_result op_result = {0};
    dtv_l3_transfer_request request = {0};
    dtv_l3_close_status close_status = {0};
    dtv_l3_op *transfer = NULL;
    unsigned int attempt;

    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL ||
        opened.op == NULL || !wait_terminal(opened.op, &op_result) ||
        op_result.kind != DTV_L3_OP_KIND_OPEN || op_result.result != DTV_L3_OK ||
        dtv_l3_op_poll(opened.op, &op_result) != DTV_L3_OK ||
        op_result.kind != DTV_L3_OP_KIND_OPEN || op_result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 1;
    }

    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.timeout_ms = 20;
    request.claim_index = 0;
    if (dtv_l3_transfer_start(opened.owner, &request, &transfer) !=
            DTV_L3_ACCEPTED ||
        dtv_l3_op_cancel(transfer) != DTV_L3_ACCEPTED ||
        !wait_terminal(transfer, &op_result) ||
        op_result.kind != DTV_L3_OP_KIND_TRANSFER ||
        op_result.result != DTV_L3_OK ||
        op_result.data_size != 2 || op_result.data[0] != 0x90 ||
        op_result.data[1] != 0x00 ||
        dtv_l3_op_poll(transfer, &op_result) != DTV_L3_OK ||
        op_result.result != DTV_L3_OK || op_result.data_size != 2 ||
        op_result.data[0] != 0x90 || op_result.data[1] != 0x00 ||
        dtv_l3_op_release(transfer) != DTV_L3_OK) {
        return 3;
    }
    if (dtv_l3_close(opened.owner, 1000) != DTV_L3_OK) {
        return 4;
    }
    if (dtv_l3_close_status_get(opened.owner, &close_status) != DTV_L3_OK ||
        close_status.cleanup_epoch != 1 || !close_status.physical_quiesced ||
        close_status.cleanup_complete) {
        return 7;
    }
    if (dtv_l3_context_release(opened.owner) != DTV_L3_BUSY) {
        return 8;
    }
    if (dtv_l3_cleanup_retry(opened.owner) != DTV_L3_ACCEPTED) {
        return 9;
    }
    while (!atomic_load(&fixture.retry_entered)) {
    }
    if (dtv_l3_close_status_get(opened.owner, &close_status) != DTV_L3_OK ||
        close_status.cleanup_epoch != 2 || close_status.physical_quiesced ||
        dtv_l3_context_release(opened.owner) != DTV_L3_BUSY) {
        return 10;
    }
    atomic_store(&fixture.allow_retry, 1);
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &close_status) != DTV_L3_OK) {
            return 5;
        }
        if (close_status.cleanup_epoch == 2 && close_status.cleanup_complete) {
            break;
        }
    }
    if (close_status.cleanup_epoch != 2 || !close_status.physical_quiesced ||
        dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        return 6;
    }
    return 0;
}
