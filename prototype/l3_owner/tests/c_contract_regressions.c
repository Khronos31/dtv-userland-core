/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dtv/l3_owner.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct fixture {
    dtv_l3_claim_result claim_result;
    int reject_submit;
    int retain_first_release;
    int complete_on_cancel;
    unsigned int release_calls;
    unsigned int cancel_calls;
    dtv_l3_transfer_complete_fn complete;
    void *callback_context;
    dtv_l3_provider_binding *binding;
    uint64_t transfer_id;
    atomic_int hold_release;
    atomic_int release_entered;
    atomic_int allow_release;
    atomic_int hold_claim;
    atomic_int claim_entered;
    atomic_int allow_claim;
    atomic_int hold_release_call;
};

extern void l3_test_hold_initialize(void);
extern void l3_test_hold_initialize_after_setup(void);
extern int l3_test_wait_initialize(void);
extern void l3_test_release_initialize(void);

static dtv_l3_claim_result provider_claim(void *context,
                                          dtv_l3_provider_binding *binding,
                                          uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    if (atomic_load(&fixture->hold_claim)) {
        atomic_store(&fixture->claim_entered, 1);
        while (!atomic_load(&fixture->allow_claim)) {
        }
    }
    return fixture->claim_result;
}

static dtv_l3_release_result provider_release(
    void *context,
    dtv_l3_provider_binding *binding,
    uint32_t claim_index) {
    struct fixture *fixture = context;
    (void)binding;
    (void)claim_index;
    ++fixture->release_calls;
    if (atomic_load(&fixture->hold_release) &&
        fixture->release_calls ==
            (unsigned int)atomic_load(&fixture->hold_release_call)) {
        atomic_store(&fixture->release_entered, 1);
        while (!atomic_load(&fixture->allow_release)) {
        }
    }
    if (fixture->release_calls == 1 && fixture->retain_first_release) {
        return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                       DTV_L3_CLAIM_RETAINED,
                                       DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
    }
    return (dtv_l3_release_result){DTV_L3_RELEASE_QUIESCED,
                                   DTV_L3_CLAIM_NONE,
                                   DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
}

static dtv_l3_result provider_submit(
    void *context,
    dtv_l3_provider_binding *binding,
    uint64_t transfer_id,
    const dtv_l3_transfer_request *request,
    dtv_l3_transfer_complete_fn complete,
    void *callback_context) {
    struct fixture *fixture = context;
    (void)request;
    if (fixture->reject_submit) {
        return DTV_L3_INVALID;
    }
    fixture->complete = complete;
    fixture->callback_context = callback_context;
    fixture->binding = binding;
    fixture->transfer_id = transfer_id;
    return DTV_L3_ACCEPTED;
}

static dtv_l3_result provider_cancel(void *context,
                                     dtv_l3_provider_binding *binding,
                                     uint64_t transfer_id) {
    struct fixture *fixture = context;
    ++fixture->cancel_calls;
    if (fixture->complete_on_cancel) {
        const uint8_t data[] = {0x90, 0x00};
        const dtv_l3_transfer_completion completion = {
            .result = DTV_L3_OK,
            .data = data,
            .size = sizeof(data),
            .actual_length = sizeof(data),
            .actual_length_known = 1};
        if (fixture->complete == NULL || fixture->binding != binding ||
            fixture->transfer_id != transfer_id) {
            return DTV_L3_INVALID;
        }
        fixture->complete(binding, transfer_id, &completion,
                          fixture->callback_context);
    }
    return DTV_L3_ACCEPTED;
}

static struct fixture fixture_new(void) {
    struct fixture fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.claim_result =
        (dtv_l3_claim_result){DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    atomic_init(&fixture.hold_release, 0);
    atomic_init(&fixture.release_entered, 0);
    atomic_init(&fixture.allow_release, 0);
    atomic_init(&fixture.hold_claim, 0);
    atomic_init(&fixture.claim_entered, 0);
    atomic_init(&fixture.allow_claim, 0);
    atomic_init(&fixture.hold_release_call, 1);
    return fixture;
}

static dtv_l3_provider provider_for(struct fixture *fixture) {
    return (dtv_l3_provider){.provider_context = fixture,
                             .claim = provider_claim,
                             .release_claim = provider_release,
                             .submit = provider_submit,
                             .cancel = provider_cancel};
}

static int wait_open(dtv_l3_op *op, dtv_l3_op_result *result) {
    unsigned int attempt;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return 0;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return 1;
        }
        sched_yield();
    }
    return 0;
}

static int wait_transfer(dtv_l3_op *op, dtv_l3_op_result *result) {
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

static dtv_l3_transfer_request bulk_request(void) {
    dtv_l3_transfer_request request;
    memset(&request, 0, sizeof(request));
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.timeout_ms = 20;
    request.claim_index = 0;
    return request;
}

static int finish_context(dtv_l3_context *owner) {
    dtv_l3_close_status status;
    unsigned int attempt;
    (void)dtv_l3_close(owner, 1000);
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(owner, &status) != DTV_L3_OK) {
            return 0;
        }
        if (status.cleanup_complete && status.all_ops_recovered) {
            return dtv_l3_context_release(owner) == DTV_L3_OK;
        }
    }
    return 0;
}

static dtv_l3_result release_context_after_open(dtv_l3_context *owner) {
    dtv_l3_result result = DTV_L3_BUSY;
    unsigned int attempt;
    for (attempt = 0; attempt < 1000000 && result == DTV_L3_BUSY; ++attempt) {
        result = dtv_l3_context_release(owner);
        sched_yield();
    }
    return result;
}

static int test_rejected_submit_recovered(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened = dtv_l3_open(&provider, 1);
    dtv_l3_op_result result;
    dtv_l3_transfer_request request = bulk_request();
    dtv_l3_op *transfer = NULL;
    dtv_l3_close_status status;
    int observed = 0;

    if (opened.result != DTV_L3_ACCEPTED || !wait_open(opened.op, &result) ||
        result.result != DTV_L3_OK ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        return 1;
    }
    fixture.reject_submit = 1;
    observed = dtv_l3_transfer_start(opened.owner, &request, &transfer) ==
                   DTV_L3_INVALID &&
               transfer == NULL &&
               dtv_l3_close(opened.owner, 1000) == DTV_L3_OK &&
               dtv_l3_close_status_get(opened.owner, &status) == DTV_L3_OK &&
               status.all_ops_recovered && status.cleanup_complete;
    return finish_context(opened.owner) && observed ? 0 : 2;
}

static int test_open_cancel_is_local(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened = dtv_l3_open(&provider, 1);
    dtv_l3_op_result result;
    dtv_l3_transfer_request request = bulk_request();
    dtv_l3_op *transfer = NULL;
    dtv_l3_open_status open_status;
    int observed;

    if (opened.result != DTV_L3_ACCEPTED || !wait_open(opened.op, &result) ||
        result.result != DTV_L3_OK ||
        dtv_l3_transfer_start(opened.owner, &request, &transfer) !=
            DTV_L3_ACCEPTED) {
        return 1;
    }
    fixture.complete_on_cancel = 1;
    observed = dtv_l3_op_cancel(opened.op) == DTV_L3_OK &&
               fixture.cancel_calls == 0 && fixture.release_calls == 0 &&
               dtv_l3_open_status_get(opened.owner, &open_status) == DTV_L3_OK &&
               open_status.state == DTV_L3_OPEN_READY &&
               dtv_l3_op_poll(opened.op, &result) == DTV_L3_OK &&
               result.state == DTV_L3_OP_DONE && result.result == DTV_L3_OK;
    {
        const dtv_l3_result cancel_result = dtv_l3_op_cancel(transfer);
        if ((cancel_result != DTV_L3_ACCEPTED && cancel_result != DTV_L3_OK) ||
            !wait_transfer(transfer, &result) || result.result != DTV_L3_OK ||
            dtv_l3_op_release(transfer) != DTV_L3_OK ||
            dtv_l3_op_release(opened.op) != DTV_L3_OK) {
            return 2;
        }
    }
    return finish_context(opened.owner) && observed ? 0 : 3;
}

struct op_releaser {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    dtv_l3_op *op;
    dtv_l3_result result;
    int done;
};

static void *release_open_op(void *argument) {
    struct op_releaser *releaser = argument;
    const dtv_l3_result result = dtv_l3_op_release(releaser->op);
    pthread_mutex_lock(&releaser->mutex);
    releaser->result = result;
    releaser->done = 1;
    pthread_cond_signal(&releaser->condition);
    pthread_mutex_unlock(&releaser->mutex);
    return NULL;
}

static int test_failed_open_result_release_does_not_join_cleanup(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider;
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    struct op_releaser releaser;
    pthread_t release_thread;
    struct timespec deadline;
    dtv_l3_close_status status;
    int prompt_release;
    int prompt_owner_release;
    unsigned int attempt;

    fixture.claim_result =
        (dtv_l3_claim_result){DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
    fixture.retain_first_release = 1;
    atomic_store(&fixture.hold_release, 1);
    provider = provider_for(&fixture);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || !wait_open(opened.op, &result) ||
        result.state != DTV_L3_OP_FAILED || result.result != DTV_L3_FAILED) {
        return 1;
    }
    pthread_mutex_init(&releaser.mutex, NULL);
    pthread_cond_init(&releaser.condition, NULL);
    releaser.op = opened.op;
    releaser.result = DTV_L3_FAILED;
    releaser.done = 0;
    pthread_create(&release_thread, NULL, release_open_op, &releaser);
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_nsec += 100000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&releaser.mutex);
    while (!releaser.done) {
        if (pthread_cond_timedwait(&releaser.condition, &releaser.mutex,
                                   &deadline) != 0) {
            break;
        }
    }
    prompt_release = releaser.done && releaser.result == DTV_L3_OK;
    pthread_mutex_unlock(&releaser.mutex);
    prompt_owner_release = prompt_release &&
                          dtv_l3_context_release(opened.owner) == DTV_L3_BUSY;
    if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
        atomic_store(&fixture.allow_release, 1);
        pthread_join(release_thread, NULL);
        return 2;
    }
    atomic_store(&fixture.allow_release, 1);
    pthread_join(release_thread, NULL);
    pthread_cond_destroy(&releaser.condition);
    pthread_mutex_destroy(&releaser.mutex);
    if (!prompt_release || releaser.result != DTV_L3_OK) {
        return 3;
    }
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_cleanup_retry(opened.owner) == DTV_L3_ACCEPTED) {
            break;
        }
    }
    if (attempt == 1000000) {
        return 4;
    }
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            return 5;
        }
        if (status.cleanup_complete) {
            break;
        }
    }
    if (attempt == 1000000) {
        return 6;
    }
    {
        dtv_l3_result release_result = DTV_L3_BUSY;
        for (attempt = 0; attempt < 1000000 &&
                           release_result == DTV_L3_BUSY;
             ++attempt) {
            release_result = dtv_l3_context_release(opened.owner);
        }
        if (release_result != DTV_L3_OK) {
            return 6;
        }
    }
    return prompt_release && releaser.result == DTV_L3_OK &&
                   prompt_owner_release
               ? 0
               : 3;
}

static int test_open_terminal_poll_stable_during_close(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result initial;
    dtv_l3_op_result observed;
    dtv_l3_close_status status;
    unsigned int attempt;
    int stable_during_close;
    int stable_after_close;
    int handle_released;
    int context_released;

    atomic_store(&fixture.hold_release, 1);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || !wait_open(opened.op, &initial) ||
        initial.state != DTV_L3_OP_DONE || initial.result != DTV_L3_OK ||
        dtv_l3_close(opened.owner, 0) != DTV_L3_TIMEOUT) {
        return 1;
    }
    while (!atomic_load(&fixture.release_entered)) {
    }
    stable_during_close = dtv_l3_op_poll(opened.op, &observed) == DTV_L3_OK &&
                          observed.state == initial.state &&
                          observed.result == initial.result;
    atomic_store(&fixture.allow_release, 1);
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            return 3;
        }
        if (status.cleanup_complete) {
            break;
        }
    }
    if (attempt == 1000000) {
        return 4;
    }
    stable_after_close = dtv_l3_op_poll(opened.op, &observed) == DTV_L3_OK &&
                         observed.state == initial.state &&
                         observed.result == initial.result;
    handle_released = dtv_l3_op_release(opened.op) == DTV_L3_OK;
    context_released = dtv_l3_context_release(opened.owner) == DTV_L3_OK;
    return stable_during_close && stable_after_close && handle_released &&
                   context_released
               ? 0
               : 5;
}

static int test_open_cancel_before_initialize(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_close_status status;
    unsigned int attempt;
    int observed;
    int cleanup_before_initialize = 0;

    l3_test_hold_initialize();
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED ||
        !l3_test_wait_initialize()) {
        l3_test_release_initialize();
        return 1;
    }
    observed = dtv_l3_op_cancel(opened.op) == DTV_L3_ACCEPTED &&
               dtv_l3_close_status_get(opened.owner, &status) == DTV_L3_OK &&
               dtv_l3_op_poll(opened.op, &result) == DTV_L3_OK &&
               result.state == DTV_L3_OP_RUNNING;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            cleanup_before_initialize = 1;
            break;
        }
        if (status.cleanup_complete) {
            cleanup_before_initialize = status.cleanup_complete;
            break;
        }
        sched_yield();
    }
    l3_test_release_initialize();
    if (!wait_open(opened.op, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED ||
        dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        !status.cleanup_complete || !status.all_ops_recovered ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK ||
        release_context_after_open(opened.owner) != DTV_L3_OK) {
        return 2;
    }
    return observed && !cleanup_before_initialize ? 0 : 3;
}

static int test_open_cancel_after_setup_before_claim(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_close_status status;
    unsigned int attempt;
    int observed;
    int cleanup_before_claim = 0;

    l3_test_hold_initialize_after_setup();
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || !l3_test_wait_initialize()) {
        l3_test_release_initialize();
        return 1;
    }
    observed = dtv_l3_op_cancel(opened.op) == DTV_L3_ACCEPTED &&
               dtv_l3_op_poll(opened.op, &result) == DTV_L3_OK &&
               result.state == DTV_L3_OP_RUNNING;
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            cleanup_before_claim = 1;
            break;
        }
        if (status.cleanup_complete) {
            cleanup_before_claim = 1;
            break;
        }
        sched_yield();
    }
    l3_test_release_initialize();
    if (!wait_open(opened.op, &result) ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED ||
        dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        !status.cleanup_complete || !status.all_ops_recovered ||
        fixture.release_calls != 0 ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK ||
        release_context_after_open(opened.owner) != DTV_L3_OK) {
        return 2;
    }
    return observed && !cleanup_before_claim ? 0 : 3;
}

static int test_repeated_open_cancel_keeps_request(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result result;
    dtv_l3_close_status status;
    int observed;

    atomic_store(&fixture.hold_claim, 1);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED) {
        return 1;
    }
    while (!atomic_load(&fixture.claim_entered)) {
        sched_yield();
    }
    observed = dtv_l3_op_cancel(opened.op) == DTV_L3_ACCEPTED &&
               dtv_l3_op_cancel(opened.op) == DTV_L3_OK;
    atomic_store(&fixture.allow_claim, 1);
    if (!wait_open(opened.op, &result) ||
        result.result != DTV_L3_CANCELLED ||
        dtv_l3_op_release(opened.op) != DTV_L3_OK ||
        dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK ||
        !status.cleanup_complete || !status.all_ops_recovered ||
        release_context_after_open(opened.owner) != DTV_L3_OK) {
        return 2;
    }
    return observed ? 0 : 3;
}

static int test_open_terminal_epoch_is_stable_during_retry(void) {
    struct fixture fixture = fixture_new();
    dtv_l3_provider provider = provider_for(&fixture);
    dtv_l3_open_result opened;
    dtv_l3_op_result initial;
    dtv_l3_op_result observed;
    dtv_l3_close_status status;
    unsigned int attempt;
    int stable;
    int owner_busy;

    fixture.retain_first_release = 1;
    atomic_store(&fixture.hold_release, 1);
    atomic_store(&fixture.hold_release_call, 2);
    opened = dtv_l3_open(&provider, 1);
    if (opened.result != DTV_L3_ACCEPTED || !wait_open(opened.op, &initial) ||
        initial.state != DTV_L3_OP_DONE) {
        return 1;
    }
    {
        const dtv_l3_result close_result = dtv_l3_close(opened.owner, 0);
        if (close_result != DTV_L3_OK && close_result != DTV_L3_TIMEOUT) {
            return 2;
        }
    }
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            return 2;
        }
        if (status.physical_quiesced) {
            break;
        }
        sched_yield();
    }
    if (attempt == 1000000) {
        return 2;
    }
    if (dtv_l3_cleanup_retry(opened.owner) != DTV_L3_ACCEPTED) {
        return 3;
    }
    while (!atomic_load(&fixture.release_entered)) {
        sched_yield();
    }
    stable = dtv_l3_op_poll(opened.op, &observed) == DTV_L3_OK &&
             observed.state == initial.state &&
             observed.result == initial.result &&
             observed.cleanup_epoch == initial.cleanup_epoch;
    if (dtv_l3_op_release(opened.op) != DTV_L3_OK) {
        atomic_store(&fixture.allow_release, 1);
        return 2;
    }
    owner_busy = dtv_l3_context_release(opened.owner) == DTV_L3_BUSY;
    atomic_store(&fixture.allow_release, 1);
    for (attempt = 0; attempt < 1000000; ++attempt) {
        if (dtv_l3_close_status_get(opened.owner, &status) != DTV_L3_OK) {
            return 2;
        }
        if (status.cleanup_complete) {
            break;
        }
        sched_yield();
    }
    if (attempt == 1000000 || dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        return 3;
    }
    return stable && owner_busy ? 0 : 4;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 64;
    }
    if (strcmp(argv[1], "reject") == 0) {
        return test_rejected_submit_recovered();
    }
    if (strcmp(argv[1], "cancel") == 0) {
        return test_open_cancel_is_local();
    }
    if (strcmp(argv[1], "release") == 0) {
        return test_failed_open_result_release_does_not_join_cleanup();
    }
    if (strcmp(argv[1], "poll") == 0) {
        return test_open_terminal_poll_stable_during_close();
    }
    if (strcmp(argv[1], "preinit-cancel") == 0) {
        return test_open_cancel_before_initialize();
    }
    if (strcmp(argv[1], "repeat-cancel") == 0) {
        return test_repeated_open_cancel_keeps_request();
    }
    if (strcmp(argv[1], "epoch") == 0) {
        return test_open_terminal_epoch_is_stable_during_retry();
    }
    if (strcmp(argv[1], "after-setup") == 0) {
        return test_open_cancel_after_setup_before_claim();
    }
    return 64;
}
