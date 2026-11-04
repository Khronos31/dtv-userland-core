/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <dtv/l3_owner.h>

#include <pthread.h>
#include <stdint.h>
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
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int cancel_block;
    int cancel_entered;
    int cancel_release;
    int release_block;
    int release_entered;
    int release_release;
    int submit_block_endpoint;
    int submit_entered;
    int submit_release;
    unsigned int claim_calls;
    unsigned int submit_calls;
    unsigned int cancel_calls;
    unsigned int release_claim_calls;
    pthread_t submit_thread;
    pthread_t cancel_thread;
    struct pending pending[MAX_PENDING];
};

static uint64_t monotonic_now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

static void sleep_ms(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000u);
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
    nanosleep(&delay, NULL);
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

static dtv_l3_claim_result provider_claim(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    pthread_mutex_lock(&fixture->mutex);
    ++fixture->claim_calls;
    pthread_mutex_unlock(&fixture->mutex);
    return (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

static dtv_l3_release_result provider_release_claim(
    void *context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    pthread_mutex_lock(&fixture->mutex);
    ++fixture->release_claim_calls;
    if (fixture->release_block) {
        fixture->release_entered = 1;
        pthread_cond_broadcast(&fixture->condition);
        while (!fixture->release_release) {
            pthread_cond_wait(&fixture->condition, &fixture->mutex);
        }
    }
    pthread_mutex_unlock(&fixture->mutex);
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result provider_submit(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id,
                                     const dtv_l3_transfer_request *request,
                                     dtv_l3_transfer_complete_fn complete,
                                     void *callback_context) {
    struct fixture *fixture = context;
    struct pending *slot;
    pthread_mutex_lock(&fixture->mutex);
    ++fixture->submit_calls;
    fixture->submit_thread = pthread_self();
    if (fixture->submit_block_endpoint != 0 &&
        (int)request->endpoint == fixture->submit_block_endpoint) {
        fixture->submit_entered = 1;
        pthread_cond_broadcast(&fixture->condition);
        while (!fixture->submit_release) {
            pthread_cond_wait(&fixture->condition, &fixture->mutex);
        }
    }
    slot = free_pending(fixture);
    if (slot == NULL) {
        pthread_mutex_unlock(&fixture->mutex);
        return DTV_L3_BUSY;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->binding = binding;
    slot->id = transfer_id;
    slot->complete = complete;
    slot->callback_context = callback_context;
    pthread_mutex_unlock(&fixture->mutex);
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    struct fixture *fixture = context;
    (void)binding;
    (void)transfer_id;
    pthread_mutex_lock(&fixture->mutex);
    ++fixture->cancel_calls;
    fixture->cancel_thread = pthread_self();
    if (fixture->cancel_block) {
        fixture->cancel_entered = 1;
        pthread_cond_broadcast(&fixture->condition);
        while (!fixture->cancel_release) {
            pthread_cond_wait(&fixture->condition, &fixture->mutex);
        }
    }
    pthread_mutex_unlock(&fixture->mutex);
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_claim_cancel(void *context,
                                           dtv_l3_provider_binding *binding,
                                           uint32_t claim_index) {
    (void)context;
    (void)binding;
    (void)claim_index;
    return DTV_L3_ACCEPTED;
}

static void fixture_init(struct fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    pthread_mutex_init(&fixture->mutex, NULL);
    pthread_cond_init(&fixture->condition, NULL);
}

static void fixture_destroy(struct fixture *fixture) {
    pthread_cond_destroy(&fixture->condition);
    pthread_mutex_destroy(&fixture->mutex);
}

static dtv_l3_usb_provider provider_for(struct fixture *fixture,
                                        dtv_l3_submit_mode mode) {
    return (dtv_l3_usb_provider){
        .provider_context = fixture,
        .claim = provider_claim,
        .release_claim = provider_release_claim,
        .submit = provider_submit,
        .cancel = provider_cancel,
        .claim_cancel = provider_claim_cancel,
        .cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER,
        .submit_mode = mode};
}

static dtv_l3_product_adapter dummy_adapter(void) {
    dtv_l3_product_adapter adapter;
    memset(&adapter, 0, sizeof(adapter));
    adapter.capabilities.receiver_count = 1;
    return adapter;
}

static int wait_flag(struct fixture *fixture, int *flag) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        pthread_mutex_lock(&fixture->mutex);
        const int value = *flag;
        pthread_mutex_unlock(&fixture->mutex);
        if (value) {
            return 1;
        }
        sleep_ms(1);
    }
    return 0;
}

static void set_flag(struct fixture *fixture, int *flag) {
    pthread_mutex_lock(&fixture->mutex);
    *flag = 1;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->mutex);
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

static int open_ready(dtv_l3_usb_provider *usb,
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

static int release_owner(dtv_l3_context *owner) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4000; ++attempt) {
        const dtv_l3_result result = dtv_l3_context_release(owner);
        if (result != DTV_L3_BUSY) {
            return result == DTV_L3_OK;
        }
        sleep_ms(1);
    }
    return 0;
}

static int fire_pending(struct fixture *fixture,
                        uint64_t id,
                        dtv_l3_result result) {
    struct pending slot;
    struct pending *found = find_pending(fixture, id);
    if (found == NULL) {
        return 0;
    }
    slot = *found;
    memset(found, 0, sizeof(*found));
    const dtv_l3_transfer_completion completion = {
        .result = result,
        .data = NULL,
        .size = 0,
        .actual_length = 0,
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

static dtv_l3_transfer_request request_for(uint32_t endpoint,
                                           uint32_t timeout_ms,
                                           uint64_t deadline_ns) {
    dtv_l3_transfer_request request;
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = endpoint;
    request.timeout_ms = timeout_ms;
    request.in_capacity = 64;
    request.claim_index = 0;
    request.deadline_ns = deadline_ns;
    return request;
}

static int test_cancel_thread_before_close(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter = dummy_adapter();
    dtv_l3_context *owner = NULL;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_transfer_request request;
    pthread_t caller = pthread_self();

    fixture_init(&fixture);
    usb = provider_for(&fixture, DTV_L3_SUBMIT_NONBLOCKING);
    if (!open_ready(&usb, &adapter, &owner)) {
        fixture_destroy(&fixture);
        return 1;
    }
    request = request_for(0x81, 0, 0);
    if (dtv_l3_transfer_start(owner, &request, &op_a) != DTV_L3_ACCEPTED ||
        dtv_l3_transfer_start(owner, &request, &op_b) != DTV_L3_ACCEPTED) {
        fixture_destroy(&fixture);
        return 2;
    }
    if (dtv_l3_op_cancel(op_a) != DTV_L3_ACCEPTED ||
        !pthread_equal(fixture.cancel_thread, caller) ||
        !pthread_equal(fixture.submit_thread, caller)) {
        fixture_destroy(&fixture);
        return 3;
    }
    /* The T2 lane may cancel only after close. */
    if (dtv_l3_close(owner, 0) != DTV_L3_TIMEOUT) {
        fixture_destroy(&fixture);
        return 4;
    }
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_CANCELLED);
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_OK);
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_op_release(op_b) != DTV_L3_OK) {
        fixture_destroy(&fixture);
        return 5;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        !release_owner(owner)) {
        fixture_destroy(&fixture);
        return 6;
    }
    fixture_destroy(&fixture);
    return 0;
}

struct cancel_thread_arg {
    struct fixture *fixture;
    dtv_l3_op *op;
    dtv_l3_result result;
};

static void *cancel_thread_main(void *argument) {
    struct cancel_thread_arg *arg = argument;
    arg->result = dtv_l3_op_cancel(arg->op);
    return NULL;
}

static int test_deadline_while_cancel_blocks(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter = dummy_adapter();
    dtv_l3_context *owner = NULL;
    dtv_l3_op *op_a = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    struct cancel_thread_arg arg;
    pthread_t cancel_thread;

    fixture_init(&fixture);
    fixture.cancel_block = 1;
    usb = provider_for(&fixture, DTV_L3_SUBMIT_NONBLOCKING);
    if (!open_ready(&usb, &adapter, &owner)) {
        fixture_destroy(&fixture);
        return 1;
    }
    request = request_for(0x81, 0, 0);
    if (dtv_l3_transfer_start(owner, &request, &op_a) != DTV_L3_ACCEPTED) {
        fixture_destroy(&fixture);
        return 2;
    }
    request = request_for(0x82, 0, monotonic_now_ns() + 50000000ull);
    if (dtv_l3_transfer_start(owner, &request, &op_b) != DTV_L3_ACCEPTED) {
        fixture_destroy(&fixture);
        return 3;
    }
    arg.fixture = &fixture;
    arg.op = op_a;
    arg.result = DTV_L3_BUSY;
    if (pthread_create(&cancel_thread, NULL, cancel_thread_main, &arg) != 0) {
        fixture_destroy(&fixture);
        return 4;
    }
    if (!wait_flag(&fixture, &fixture.cancel_entered)) {
        fixture_destroy(&fixture);
        return 5;
    }
    /* No host poll or wait on op_b: T2 must own the deadline. */
    sleep_ms(150);
    if (dtv_l3_op_poll(op_b, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_TIMEOUT) {
        fixture_destroy(&fixture);
        return 6;
    }
    if (dtv_l3_op_release(op_b) != DTV_L3_OK) {
        fixture_destroy(&fixture);
        return 7;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_BUSY) {
        fixture_destroy(&fixture);
        return 8;
    }
    set_flag(&fixture, &fixture.cancel_release);
    pthread_join(cancel_thread, NULL);
    if (arg.result != DTV_L3_ACCEPTED) {
        fixture_destroy(&fixture);
        return 9;
    }
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_CANCELLED);
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_OK);
    if (dtv_l3_op_release(op_a) != DTV_L3_OK ||
        dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        !release_owner(owner)) {
        fixture_destroy(&fixture);
        return 10;
    }
    fixture_destroy(&fixture);
    return 0;
}

struct submit_thread_arg {
    struct fixture *fixture;
    dtv_l3_context *owner;
    dtv_l3_op *op;
    dtv_l3_result result;
};

static void *submit_thread_main(void *argument) {
    struct submit_thread_arg *arg = argument;
    dtv_l3_transfer_request request =
        request_for(0x02, 0, monotonic_now_ns() + 20000000ull);
    arg->result = dtv_l3_transfer_start(arg->owner, &request, &arg->op);
    return NULL;
}

static int test_may_block_submit_and_deadline(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter = dummy_adapter();
    dtv_l3_context *owner = NULL;
    dtv_l3_op *op_b = NULL;
    dtv_l3_op_result result;
    dtv_l3_transfer_request request;
    struct submit_thread_arg arg;
    pthread_t submit_thread;

    fixture_init(&fixture);
    fixture.submit_block_endpoint = 0x02;
    usb = provider_for(&fixture, DTV_L3_SUBMIT_MAY_BLOCK);
    if (!open_ready(&usb, &adapter, &owner)) {
        fixture_destroy(&fixture);
        return 1;
    }
    request = request_for(0x81, 0, monotonic_now_ns() + 40000000ull);
    if (dtv_l3_transfer_start(owner, &request, &op_b) != DTV_L3_ACCEPTED) {
        fixture_destroy(&fixture);
        return 2;
    }
    arg.fixture = &fixture;
    arg.owner = owner;
    arg.op = NULL;
    arg.result = DTV_L3_BUSY;
    if (pthread_create(&submit_thread, NULL, submit_thread_main, &arg) != 0) {
        fixture_destroy(&fixture);
        return 3;
    }
    if (!wait_flag(&fixture, &fixture.submit_entered)) {
        fixture_destroy(&fixture);
        return 4;
    }
    sleep_ms(100);
    if (dtv_l3_op_poll(op_b, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_TIMEOUT) {
        fixture_destroy(&fixture);
        return 5;
    }
    set_flag(&fixture, &fixture.submit_release);
    pthread_join(submit_thread, NULL);
    if (arg.result != DTV_L3_ACCEPTED || arg.op == NULL) {
        fixture_destroy(&fixture);
        return 6;
    }
    if (!wait_op_terminal(arg.op, &result) ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_TIMEOUT) {
        fixture_destroy(&fixture);
        return 7;
    }
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_OK);
    fire_pending(&fixture, only_pending_id(&fixture), DTV_L3_OK);
    if (dtv_l3_op_release(op_b) != DTV_L3_OK ||
        dtv_l3_op_release(arg.op) != DTV_L3_OK ||
        dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        !release_owner(owner)) {
        fixture_destroy(&fixture);
        return 8;
    }
    fixture_destroy(&fixture);
    return 0;
}

static int test_may_block_rejects_resubmit(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter = dummy_adapter();
    dtv_l3_context *owner = NULL;
    dtv_l3_op *op = NULL;
    dtv_l3_transfer_request request;

    fixture_init(&fixture);
    usb = provider_for(&fixture, DTV_L3_SUBMIT_MAY_BLOCK);
    if (!open_ready(&usb, &adapter, &owner)) {
        fixture_destroy(&fixture);
        return 1;
    }
    request = request_for(0x81, 0, 0);
    request.resubmit_on_provider_abort = 1;
    if (dtv_l3_transfer_start(owner, &request, &op) != DTV_L3_INVALID ||
        op != NULL || fixture.submit_calls != 0) {
        fixture_destroy(&fixture);
        return 2;
    }
    if (dtv_l3_close(owner, 2000) != DTV_L3_OK || !wait_releasable(owner) ||
        !release_owner(owner)) {
        fixture_destroy(&fixture);
        return 3;
    }
    fixture_destroy(&fixture);
    return 0;
}

static int test_close_timeout_while_release_blocks(void) {
    struct fixture fixture;
    dtv_l3_usb_provider usb;
    dtv_l3_product_adapter adapter = dummy_adapter();
    dtv_l3_context *owner = NULL;
    dtv_l3_close_status status;

    fixture_init(&fixture);
    fixture.release_block = 1;
    usb = provider_for(&fixture, DTV_L3_SUBMIT_NONBLOCKING);
    if (!open_ready(&usb, &adapter, &owner)) {
        fixture_destroy(&fixture);
        return 1;
    }
    if (dtv_l3_close(owner, 50) != DTV_L3_TIMEOUT) {
        fixture_destroy(&fixture);
        return 2;
    }
    if (!wait_flag(&fixture, &fixture.release_entered)) {
        fixture_destroy(&fixture);
        return 3;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_BUSY) {
        fixture_destroy(&fixture);
        return 4;
    }
    if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK ||
        status.releasable) {
        fixture_destroy(&fixture);
        return 5;
    }
    set_flag(&fixture, &fixture.release_release);
    if (!wait_releasable(owner)) {
        fixture_destroy(&fixture);
        return 6;
    }
    if (dtv_l3_context_release(owner) != DTV_L3_OK) {
        fixture_destroy(&fixture);
        return 7;
    }
    fixture_destroy(&fixture);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "caller-cancel") == 0) {
        return test_cancel_thread_before_close();
    }
    if (strcmp(argv[1], "blocked-cancel-deadline") == 0) {
        return test_deadline_while_cancel_blocks();
    }
    if (strcmp(argv[1], "may-block-submit") == 0) {
        return test_may_block_submit_and_deadline();
    }
    if (strcmp(argv[1], "may-block-resubmit") == 0) {
        return test_may_block_rejects_resubmit();
    }
    if (strcmp(argv[1], "release-block-close") == 0) {
        return test_close_timeout_while_release_blocks();
    }
    return 65;
}
