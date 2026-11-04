/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <thread>
#include <vector>

using namespace l3_owner_prototype;

namespace {

int failures = 0;

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #expression      \
                      << " failed\n";                                           \
            ++failures;                                                        \
        }                                                                       \
    } while (false)

class ProviderMock final : public Provider {
public:
    struct Pending {
        dtv_l3_transfer_complete_fn complete;
        void *callback_context;
        std::vector<std::uint8_t> input;
    };

    dtv_l3_claim_result claim(Binding *, std::uint32_t index) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++claim_calls[index];
            claims_in_flight[index] = true;
        }
        if (claim_barrier && index == claim_barrier_index) {
            claim_barrier->arrive_and_wait();
        }
        std::lock_guard<std::mutex> lock(mutex);
        claims_in_flight[index] = false;
        const auto found = claim_results.find(index);
        return found == claim_results.end()
                   ? dtv_l3_claim_result{DTV_L3_OK, DTV_L3_CLAIM_RETAINED}
                   : found->second;
    }

    dtv_l3_release_result release_claim(Binding *, std::uint32_t index) override {
        if (release_claim_barrier) {
            release_claim_barrier->arrive_and_wait();
        }
        std::lock_guard<std::mutex> lock(mutex);
        released_claims.push_back(index);
        release_raced_claim = release_raced_claim || claims_in_flight[index];
        const auto found = release_results.find(index);
        if (found == release_results.end() || found->second.empty()) {
            return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                    DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
        }
        auto result = found->second.front();
        if (found->second.size() > 1) {
            found->second.erase(found->second.begin());
        }
        return result;
    }

    dtv_l3_result submit(Binding *binding,
                        std::uint64_t id,
                        const dtv_l3_transfer_request &request,
                        dtv_l3_transfer_complete_fn complete,
                        void *callback_context) override {
        if (submit_barrier) {
            submit_barrier->arrive_and_wait();
        }
        Pending pending{complete, callback_context, {}};
        if (request.out_size != 0 && request.out_data != nullptr) {
            pending.input.assign(request.out_data,
                                 request.out_data + request.out_size);
        }
        bool complete_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++provider_entry_calls;
            const auto route = route_endpoints.find(request.claim_index);
            if (request.kind == DTV_L3_TRANSFER_BULK &&
                (route == route_endpoints.end() ||
                 route->second != request.endpoint)) {
                return DTV_L3_INVALID;
            }
            const auto function = route_functions.find(request.claim_index);
            if (function == route_functions.end()) {
                return DTV_L3_INVALID;
            }
            if (request.kind == DTV_L3_TRANSFER_CONTROL) {
                last_control_slot = request.claim_index;
                last_control_setup = request.control;
            }
            ++usb_submit_calls;
            usb_destinations.emplace_back(request.claim_index, request.endpoint);
            usb_functions.push_back(function->second);
            ++submit_calls;
            if (immediate_result) {
                complete_now = true;
            } else {
                transfers.emplace(TransferKey{binding, id}, pending);
            }
        }
        if (complete_now) {
            const dtv_l3_transfer_completion completion{
                *immediate_result,
                immediate_data.empty() ? nullptr : immediate_data.data(),
                immediate_data.size()};
            complete(binding, id, &completion, callback_context);
        }
        if (submit_return_barrier) {
            submit_return_barrier->arrive_and_wait();
        }
        return DTV_L3_ACCEPTED;
    }

    dtv_l3_result cancel(Binding *binding, std::uint64_t id) override {
        if (cancel_barrier) {
            cancel_barrier->arrive_and_wait();
        }
        Pending pending{};
        bool complete_now = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancelled.push_back(TransferKey{binding, id});
            cancel_condition.notify_all();
            const auto found = transfers.find(TransferKey{binding, id});
            if (require_registered_cancel && found == transfers.end()) {
                return DTV_L3_INVALID;
            }
            if (cancel_completes_transfer && found != transfers.end()) {
                pending = found->second;
                transfers.erase(found);
                complete_now = true;
            }
        }
        if (complete_now) {
            const dtv_l3_transfer_completion completion{
                DTV_L3_CANCELLED, nullptr, 0};
            pending.complete(binding, id, &completion,
                             pending.callback_context);
        }
        return DTV_L3_ACCEPTED;
    }

    dtv_l3_result scope_query(std::uint32_t receiver_id,
                              dtv_l3_scope_info *scope) override {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = scopes.find(receiver_id);
        if (found == scopes.end()) {
            *scope = dtv_l3_scope_info{receiver_id,
                                       DTV_L3_LEASE_SHAREABLE};
        } else {
            *scope = found->second;
        }
        return DTV_L3_OK;
    }

    dtv_l3_result bridge_start(std::uint32_t bridge_id,
                               std::uint32_t receiver_id) override {
        (void)receiver_id;
        if (bridge_start_barrier) {
            bridge_start_barrier->arrive_and_wait();
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++bridge_start_calls[bridge_id];
        return bridge_start_fail ? DTV_L3_FAILED : DTV_L3_OK;
    }

    dtv_l3_result bridge_stop(std::uint32_t bridge_id) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++bridge_stop_calls[bridge_id];
        return bridge_stop_fail ? DTV_L3_FAILED : DTV_L3_OK;
    }

    dtv_l3_result stream_attach(std::uint64_t stream_id,
                                dtv_l3_ts_deliver_fn deliver,
                                void *callback_context) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++stream_attach_calls;
        if (stream_attach_result != DTV_L3_OK) {
            return stream_attach_result;
        }
        stream_delivers[stream_id] = deliver;
        stream_contexts[stream_id] = callback_context;
        return DTV_L3_OK;
    }

    dtv_l3_result stream_stop(std::uint64_t stream_id) override {
        if (stream_stop_barrier) {
            stream_stop_barrier->arrive_and_wait();
        }
        dtv_l3_ts_deliver_fn deliver = nullptr;
        void *callback_context = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++stream_stop_calls;
            const auto found = stream_delivers.find(stream_id);
            if (found != stream_delivers.end()) {
                deliver = found->second;
                callback_context = stream_contexts[stream_id];
            }
        }
        if (deliver != nullptr) {
            deliver(callback_context, nullptr, 0);
        }
        return DTV_L3_OK;
    }

    dtv_l3_result tune(std::uint32_t receiver_id,
                       std::uint64_t requested_hz,
                       std::uint64_t *programmed_hz) override {
        (void)receiver_id;
        (void)requested_hz;
        std::lock_guard<std::mutex> lock(mutex);
        ++tune_calls;
        *programmed_hz = requested_hz + 1;
        return DTV_L3_OK;
    }

    bool feed(std::uint64_t stream_id,
              const std::vector<std::uint8_t> &data) {
        dtv_l3_ts_deliver_fn deliver = nullptr;
        void *callback_context = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = stream_delivers.find(stream_id);
            if (found == stream_delivers.end()) {
                return false;
            }
            deliver = found->second;
            callback_context = stream_contexts[stream_id];
        }
        deliver(callback_context, data.data(), data.size());
        return true;
    }

    std::size_t bridge_start_count(std::uint32_t bridge_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return bridge_start_calls[bridge_id];
    }

    std::size_t bridge_stop_count(std::uint32_t bridge_id) {
        std::lock_guard<std::mutex> lock(mutex);
        return bridge_stop_calls[bridge_id];
    }

    bool fire(Binding *binding,
              std::uint64_t id,
              dtv_l3_result result = DTV_L3_OK,
              const std::vector<std::uint8_t> &data = {}) {
        Pending pending{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = transfers.find(TransferKey{binding, id});
            if (found == transfers.end()) {
                return false;
            }
            pending = found->second;
            transfers.erase(found);
        }
        const dtv_l3_transfer_completion completion{
            result, data.empty() ? nullptr : data.data(), data.size()};
        pending.complete(binding, id, &completion, pending.callback_context);
        return true;
    }

    std::size_t cancel_count(Binding *binding, std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (const auto &key : cancelled) {
            if (key.binding == binding && key.id == id) {
                ++count;
            }
        }
        return count;
    }

    void wait_for_cancel_count(Binding *binding,
                               std::uint64_t id,
                               std::size_t expected) {
        std::unique_lock<std::mutex> lock(mutex);
        cancel_condition.wait(lock, [&] {
            std::size_t count = 0;
            for (const auto &key : cancelled) {
                if (key.binding == binding && key.id == id) {
                    ++count;
                }
            }
            return count >= expected;
        });
    }

    std::size_t released_claim_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return released_claims.size();
    }

    std::mutex mutex;
    std::condition_variable cancel_condition;
    std::map<TransferKey, Pending> transfers;
    std::map<std::uint32_t, std::uint32_t> claim_calls;
    std::map<std::uint32_t, dtv_l3_claim_result> claim_results;
    std::map<std::uint32_t, bool> claims_in_flight;
    std::map<std::uint32_t, std::vector<dtv_l3_release_result>> release_results;
    std::map<std::uint32_t, std::uint32_t> route_endpoints{{0, 0x81}, {1, 0x81}};
    std::map<std::uint32_t, std::uint32_t> route_functions{{0, 10}, {1, 20}};
    std::vector<std::uint32_t> released_claims;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> usb_destinations;
    std::vector<std::uint32_t> usb_functions;
    std::vector<TransferKey> cancelled;
    std::shared_ptr<TestBarrier> claim_barrier;
    std::shared_ptr<TestBarrier> submit_barrier;
    std::shared_ptr<TestBarrier> submit_return_barrier;
    std::shared_ptr<TestBarrier> cancel_barrier;
    std::shared_ptr<TestBarrier> release_claim_barrier;
    std::shared_ptr<TestBarrier> bridge_start_barrier;
    std::shared_ptr<TestBarrier> stream_stop_barrier;
    std::map<std::uint32_t, dtv_l3_scope_info> scopes;
    std::map<std::uint32_t, std::size_t> bridge_start_calls;
    std::map<std::uint32_t, std::size_t> bridge_stop_calls;
    std::map<std::uint64_t, dtv_l3_ts_deliver_fn> stream_delivers;
    std::map<std::uint64_t, void *> stream_contexts;
    std::uint32_t claim_barrier_index = 0;
    std::optional<dtv_l3_result> immediate_result;
    std::vector<std::uint8_t> immediate_data;
    std::size_t submit_calls = 0;
    std::size_t provider_entry_calls = 0;
    std::size_t usb_submit_calls = 0;
    std::uint32_t last_control_slot = 0;
    decltype(dtv_l3_transfer_request::control) last_control_setup{};
    bool release_raced_claim = false;
    bool require_registered_cancel = false;
    bool cancel_completes_transfer = false;
    bool bridge_start_fail = false;
    bool bridge_stop_fail = false;
    dtv_l3_result stream_attach_result = DTV_L3_OK;
    std::size_t stream_attach_calls = 0;
    std::size_t stream_stop_calls = 0;
    std::size_t tune_calls = 0;
};

dtv_l3_transfer_request request_for(const std::vector<std::uint8_t> &payload) {
    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81;
    request.timeout_ms = 1000;
    request.out_data = payload.data();
    request.out_size = payload.size();
    request.in_capacity = 64;
    request.claim_index = 0;
    return request;
}

bool wait_for_physical_quiescence(OwnerEngine &owner) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    OwnerEngine::Snapshot snapshot{};
    do {
        if (owner.close_status(&snapshot) != DTV_L3_OK) {
            return false;
        }
        if (snapshot.physical_quiesced && snapshot.cleanup_complete) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool wait_for_cleanup_pass(OwnerEngine &owner, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    do {
        if (owner.cleanup_count() >= expected) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void test_failed_open_and_partial_claim_cleanup() {
    ProviderMock provider;
    provider.claim_results[1] =
        {DTV_L3_FAILED, DTV_L3_CLAIM_NONE};
    OwnerEngine owner(provider, 10);

    OwnerEngine::Snapshot before_claims{};
    CHECK(owner.close_status(&before_claims) == DTV_L3_OK);
    CHECK(before_claims.open_state == OwnerEngine::OpenState::Opening);

    owner.initialize_claims(3);
    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.open_state == OwnerEngine::OpenState::Failed);
    CHECK(snapshot.open_result == DTV_L3_FAILED);
    CHECK(snapshot.cleanup_result == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(provider.released_claim_count() == 1);
    CHECK(owner.cleanup_count() == 1);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
    CHECK(owner.release_count() == 1);
    CHECK(owner.cleanup_count() == 1);
}

void test_failed_open_result_survives_cleanup_and_close() {
    ProviderMock failed_provider;
    failed_provider.claim_results[0] = {DTV_L3_FAILED, DTV_L3_CLAIM_NONE};
    OwnerEngine failed_owner(failed_provider, 101);
    failed_owner.initialize_claims(1);
    OwnerEngine::Snapshot failed{};
    CHECK(failed_owner.close_status(&failed) == DTV_L3_OK);
    CHECK(failed.open_state == OwnerEngine::OpenState::Failed);
    CHECK(failed.open_result == DTV_L3_FAILED);
    CHECK(failed.cleanup_result == DTV_L3_OK);
    CHECK(failed.cleanup_complete);
    CHECK(failed_provider.released_claim_count() == 0);
    CHECK(failed_owner.close(0) == DTV_L3_OK);
    CHECK(failed_owner.close_status(&failed) == DTV_L3_OK);
    CHECK(failed.open_result == DTV_L3_FAILED);
    CHECK(failed_owner.release() == DTV_L3_OK);

    ProviderMock successful_provider;
    OwnerEngine successful_owner(successful_provider, 102);
    successful_owner.initialize_claims(1);
    CHECK(successful_owner.close(2000) == DTV_L3_OK);
    OwnerEngine::Snapshot successful{};
    CHECK(successful_owner.close_status(&successful) == DTV_L3_OK);
    CHECK(successful.open_result == DTV_L3_OK);
    CHECK(failed.open_state != successful.open_state);
    CHECK(failed.open_result != successful.open_result);
    CHECK(failed.physical_quiesced == successful.physical_quiesced);
    CHECK(failed.cleanup_complete == successful.cleanup_complete);
    CHECK(failed.all_ops_recovered == successful.all_ops_recovered);
    CHECK(failed.active_borrows == successful.active_borrows);
    CHECK(failed.claims_attempted == successful.claims_attempted);
    CHECK(failed.transfer_ready_claims == successful.transfer_ready_claims);
    CHECK(failed.cleanup_claims_remaining == successful.cleanup_claims_remaining);
    CHECK(failed.cleanup_result == successful.cleanup_result);
    CHECK(failed.last_cleanup_error == successful.last_cleanup_error);
    CHECK(failed.restoration_failed == successful.restoration_failed);
    CHECK(successful_owner.release() == DTV_L3_OK);
}

void test_close_during_opening_defers_cleanup() {
    ProviderMock provider;
    auto barrier = std::make_shared<TestBarrier>();
    provider.claim_barrier = barrier;
    OwnerEngine owner(provider, 11);
    std::thread opener([&] { owner.initialize_claims(2); });
    barrier->wait_until_arrived();

    CHECK(owner.close(0) == DTV_L3_TIMEOUT);
    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.open_state == OwnerEngine::OpenState::Opening);
    CHECK(!snapshot.physical_quiesced);
    CHECK(provider.released_claim_count() == 0);

    barrier->release();
    opener.join();
    CHECK(provider.released_claim_count() == 1);
    CHECK(!provider.release_raced_claim);
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_immediate_completion_and_cancel_success_race() {
    ProviderMock provider;
    provider.immediate_result = DTV_L3_OK;
    provider.immediate_data = {7, 8};
    OwnerEngine owner(provider, 12);
    owner.initialize_claims(1);
    std::vector<std::uint8_t> payload{1, 2, 3};
    std::shared_ptr<OwnerEngine::Operation> immediate;
    CHECK(owner.start(request_for(payload), &immediate) == DTV_L3_ACCEPTED);
    CHECK(immediate->state == DTV_L3_OP_DONE);
    CHECK(immediate->response == provider.immediate_data);
    CHECK(owner.release_operation(immediate) == DTV_L3_OK);

    provider.immediate_result.reset();
    std::shared_ptr<OwnerEngine::Operation> delayed;
    CHECK(owner.start(request_for(payload), &delayed) == DTV_L3_ACCEPTED);
    const auto binding = owner.binding();
    CHECK(delayed->id == 2);
    CHECK(owner.cancel(delayed) == DTV_L3_ACCEPTED);
    CHECK(provider.cancel_count(binding.get(), delayed->id) == 1);
    CHECK(provider.fire(binding.get(), delayed->id, DTV_L3_OK, {9}));
    CHECK(delayed->state == DTV_L3_OP_DONE);
    CHECK(delayed->result == DTV_L3_OK);
    CHECK(owner.release_operation(delayed) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_p1_borrow_survives_physical_quiescence_and_gate_orders() {
    ProviderMock provider;
    OwnerEngine owner(provider, 13);
    owner.initialize_claims(0);
    CHECK(owner.close(2000) == DTV_L3_OK);
    const auto stream = owner.stream_lifetime();
    TestBarrier read_barrier;
    dtv_l3_result read_result = DTV_L3_FAILED;
    std::thread reader([&] {
        read_result = owner.read_ts([&] { read_barrier.arrive_and_wait(); });
    });
    read_barrier.wait_until_arrived();

    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.all_ops_recovered);
    CHECK(snapshot.cleanup_complete);
    CHECK(owner.release() == DTV_L3_BUSY);
    CHECK(!stream.expired());
    CHECK(owner.read_ts() == DTV_L3_BUSY);
    read_barrier.release();
    reader.join();
    CHECK(read_result == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
    CHECK(owner.cleanup_count() == 1);
    CHECK(owner.release_count() == 1);
    CHECK(stream.expired());

    ProviderMock second_provider;
    OwnerEngine second(second_provider, 14);
    second.initialize_claims(0);
    CHECK(second.close(2000) == DTV_L3_OK);
    dtv_l3_result late_read_result = DTV_L3_FAILED;
    CHECK(second.release([&] {
              std::thread late_reader([&] { late_read_result = second.read_ts(); });
              late_reader.join();
          }) == DTV_L3_OK);
    CHECK(late_read_result == DTV_L3_BUSY);
    CHECK(second.release_count() == 1);
}

void test_p2_binding_local_ids_and_b1_timeout_retention() {
    ProviderMock provider;
    OwnerEngine owner_a(provider, 21);
    OwnerEngine owner_b(provider, 22);
    owner_a.initialize_claims(1);
    owner_b.initialize_claims(1);
    std::vector<std::uint8_t> payload{4, 5, 6, 7};
    std::shared_ptr<OwnerEngine::Operation> op_a;
    std::shared_ptr<OwnerEngine::Operation> op_b;
    CHECK(owner_a.start(request_for(payload), &op_a) == DTV_L3_ACCEPTED);
    CHECK(owner_b.start(request_for(payload), &op_b) == DTV_L3_ACCEPTED);
    CHECK(op_a->id == 1);
    CHECK(op_b->id == 1);
    auto binding_a = owner_a.binding();
    const auto binding_b = owner_b.binding();
    CHECK(binding_a.get() != binding_b.get());
    const std::weak_ptr<Binding> old_binding = binding_a;

    CHECK(owner_a.cancel(op_a) == DTV_L3_ACCEPTED);
    CHECK(provider.cancel_count(binding_a.get(), 1) == 1);
    CHECK(provider.cancel_count(binding_b.get(), 1) == 0);
    CHECK(owner_a.close(0) == DTV_L3_TIMEOUT);
    OwnerEngine::Snapshot snapshot{};
    CHECK(owner_a.close_status(&snapshot) == DTV_L3_OK);
    CHECK(!snapshot.physical_quiesced);
    CHECK(!snapshot.all_ops_recovered);
    CHECK(!snapshot.cleanup_complete);
    CHECK(op_a->retained_payload == payload);
    CHECK(!old_binding.expired());
    CHECK(owner_a.release() == DTV_L3_BUSY);

    CHECK(provider.fire(binding_b.get(), 1, DTV_L3_OK, {2}));
    CHECK(op_b->state == DTV_L3_OP_DONE);
    CHECK(op_a->state == DTV_L3_OP_OPENING);
    CHECK(provider.fire(binding_a.get(), 1, DTV_L3_OK, {1}));
    CHECK(op_a->state == DTV_L3_OP_DONE);
    CHECK(owner_a.release_operation(op_a) == DTV_L3_OK);
    CHECK(owner_b.release_operation(op_b) == DTV_L3_OK);
    CHECK(owner_a.close(2000) == DTV_L3_OK);
    CHECK(owner_b.close(2000) == DTV_L3_OK);
    CHECK(owner_a.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(owner_a.close(2000) == DTV_L3_OK);
    CHECK(owner_b.close(2000) == DTV_L3_OK);
    op_a.reset();
    binding_a.reset();
    CHECK(!old_binding.expired());
    CHECK(owner_a.release() == DTV_L3_OK);
    CHECK(owner_b.release() == DTV_L3_OK);
    CHECK(old_binding.expired());
}

void test_r1_completion_does_not_drain_blocked_cancel_or_submit() {
    {
        ProviderMock provider;
        auto cancel_barrier = std::make_shared<TestBarrier>();
        provider.cancel_barrier = cancel_barrier;
        OwnerEngine owner(provider, 41);
        owner.initialize_claims(1);
        std::vector<std::uint8_t> payload{1};
        std::shared_ptr<OwnerEngine::Operation> operation;
        CHECK(owner.start(request_for(payload), &operation) == DTV_L3_ACCEPTED);
        const auto binding = owner.binding();

        dtv_l3_result cancel_result = DTV_L3_FAILED;
        std::thread canceller([&] { cancel_result = owner.cancel(operation); });
        cancel_barrier->wait_until_arrived();
        CHECK(provider.fire(binding.get(), operation->id, DTV_L3_OK));
        auto close_future = std::async(std::launch::async, [&] {
            return owner.close(0);
        });
        CHECK(close_future.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(!snapshot.physical_quiesced);
        CHECK(provider.released_claim_count() == 0);

        cancel_barrier->release();
        CHECK(close_future.get() == DTV_L3_TIMEOUT);
        canceller.join();
        CHECK(cancel_result == DTV_L3_ACCEPTED);
        CHECK(wait_for_physical_quiescence(owner));
        CHECK(provider.released_claim_count() == 1);
        CHECK(owner.cleanup_count() == 1);
    }

    {
        ProviderMock provider;
        provider.immediate_result = DTV_L3_OK;
        auto submit_return_barrier = std::make_shared<TestBarrier>();
        provider.submit_return_barrier = submit_return_barrier;
        OwnerEngine owner(provider, 42);
        owner.initialize_claims(1);
        std::vector<std::uint8_t> payload{2};
        std::shared_ptr<OwnerEngine::Operation> operation;
        dtv_l3_result start_result = DTV_L3_FAILED;
        std::thread submitter([&] {
            start_result = owner.start(request_for(payload), &operation);
        });
        submit_return_barrier->wait_until_arrived();

        auto close_future = std::async(std::launch::async, [&] {
            return owner.close(0);
        });
        const auto close_ready = close_future.wait_for(std::chrono::seconds(2)) ==
                                 std::future_status::ready;
        CHECK(close_ready);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(!snapshot.physical_quiesced);
        CHECK(provider.released_claim_count() == 0);

        submit_return_barrier->release();
        CHECK(close_future.get() == DTV_L3_TIMEOUT);
        submitter.join();
        CHECK(start_result == DTV_L3_ACCEPTED);
        CHECK(operation->state == DTV_L3_OP_DONE);
        CHECK(wait_for_physical_quiescence(owner));
        CHECK(provider.released_claim_count() == 1);
        CHECK(owner.cleanup_count() == 1);
    }
}

void test_r2_close_deadline_and_status_outlive_blocking_provider_calls() {
    {
        ProviderMock provider;
        auto cancel_barrier = std::make_shared<TestBarrier>();
        provider.cancel_barrier = cancel_barrier;
        OwnerEngine owner(provider, 43);
        owner.initialize_claims(1);
        std::vector<std::uint8_t> payload{3};
        std::shared_ptr<OwnerEngine::Operation> operation;
        CHECK(owner.start(request_for(payload), &operation) == DTV_L3_ACCEPTED);
        const auto binding = owner.binding();

        auto close_future = std::async(std::launch::async, [&] {
            return owner.close(0);
        });
        const auto close_ready = close_future.wait_for(std::chrono::seconds(2)) ==
                                 std::future_status::ready;
        CHECK(close_ready);
        cancel_barrier->wait_until_arrived();
        auto status_future = std::async(std::launch::async, [&] {
            OwnerEngine::Snapshot snapshot{};
            const auto result = owner.close_status(&snapshot);
            return std::pair<dtv_l3_result, OwnerEngine::Snapshot>{result,
                                                                  snapshot};
        });
        CHECK(status_future.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        CHECK(status_future.get().first == DTV_L3_OK);
        CHECK(provider.released_claim_count() == 0);
        cancel_barrier->release();
        CHECK(close_future.get() == DTV_L3_TIMEOUT);
        CHECK(provider.fire(binding.get(), operation->id, DTV_L3_OK));
        CHECK(wait_for_physical_quiescence(owner));
        CHECK(provider.released_claim_count() == 1);
    }

    {
        ProviderMock provider;
        auto cleanup_barrier = std::make_shared<TestBarrier>();
        provider.release_claim_barrier = cleanup_barrier;
        OwnerEngine owner(provider, 44);
        owner.initialize_claims(1);
        auto close_future = std::async(std::launch::async, [&] {
            return owner.close(0);
        });
        const auto close_ready = close_future.wait_for(std::chrono::seconds(2)) ==
                                 std::future_status::ready;
        CHECK(close_ready);
        cleanup_barrier->wait_until_arrived();
        auto status_future = std::async(std::launch::async, [&] {
            OwnerEngine::Snapshot snapshot{};
            const auto result = owner.close_status(&snapshot);
            return std::pair<dtv_l3_result, OwnerEngine::Snapshot>{result,
                                                                  snapshot};
        });
        CHECK(status_future.wait_for(std::chrono::seconds(2)) ==
              std::future_status::ready);
        const auto status = status_future.get();
        CHECK(status.first == DTV_L3_OK);
        CHECK(!status.second.physical_quiesced);
        CHECK(!status.second.cleanup_complete);
        cleanup_barrier->release();
        CHECK(close_future.get() == DTV_L3_TIMEOUT);
        CHECK(wait_for_physical_quiescence(owner));
        CHECK(owner.cleanup_count() == 1);
    }
}

void test_r3_cancel_before_registration_is_replayed_after_acceptance() {
    ProviderMock provider;
    auto submit_barrier = std::make_shared<TestBarrier>();
    provider.submit_barrier = submit_barrier;
    provider.require_registered_cancel = true;
    provider.cancel_completes_transfer = true;
    OwnerEngine owner(provider, 45);
    owner.initialize_claims(1);
    std::vector<std::uint8_t> payload{4};
    std::shared_ptr<OwnerEngine::Operation> operation;
    dtv_l3_result start_result = DTV_L3_FAILED;
    std::thread submitter([&] {
        start_result = owner.start(request_for(payload), &operation);
    });
    submit_barrier->wait_until_arrived();

    CHECK(owner.close(0) == DTV_L3_TIMEOUT);
    const auto binding = owner.binding();
    provider.wait_for_cancel_count(binding.get(), 1, 1);
    CHECK(provider.cancel_count(binding.get(), 1) == 1);
    submit_barrier->release();
    submitter.join();
    CHECK(start_result == DTV_L3_ACCEPTED);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    OwnerEngine::Snapshot snapshot{};
    do {
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        if (snapshot.physical_quiesced) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK(snapshot.physical_quiesced);
    CHECK(provider.cancel_count(binding.get(), operation->id) == 2);
    CHECK(operation->state == DTV_L3_OP_FAILED);
    CHECK(owner.cleanup_count() == 1);
    CHECK(provider.released_claim_count() == 1);
    CHECK(owner.release_operation(operation) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_b2_control_and_status_progress_while_submit_is_blocked() {
    ProviderMock provider;
    auto barrier = std::make_shared<TestBarrier>();
    provider.submit_barrier = barrier;
    OwnerEngine owner(provider, 31);
    owner.initialize_claims(1);
    std::vector<std::uint8_t> payload{1};
    std::shared_ptr<OwnerEngine::Operation> operation;
    dtv_l3_result start_result = DTV_L3_FAILED;
    std::thread worker([&] {
        start_result = owner.start(request_for(payload), &operation);
    });
    barrier->wait_until_arrived();

    CHECK(owner.close(0) == DTV_L3_TIMEOUT);
    auto status_future = std::async(std::launch::async, [&] {
        OwnerEngine::Snapshot snapshot{};
        const auto result = owner.close_status(&snapshot);
        return std::pair<dtv_l3_result, OwnerEngine::Snapshot>{result, snapshot};
    });
    CHECK(status_future.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
    const auto status = status_future.get();
    CHECK(status.first == DTV_L3_OK);
    CHECK(!status.second.physical_quiesced);
    CHECK(!status.second.cleanup_complete);

    barrier->release();
    worker.join();
    CHECK(start_result == DTV_L3_ACCEPTED);
    const auto binding = owner.binding();
    CHECK(provider.fire(binding.get(), operation->id, DTV_L3_OK));
    CHECK(owner.release_operation(operation) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_provider_route_slots_control_and_layered_rejection() {
    ProviderMock provider;
    provider.immediate_result = DTV_L3_OK;
    OwnerEngine owner(provider, 51);
    owner.initialize_claims(2);
    std::vector<std::uint8_t> payload{5};
    auto slot_zero = request_for(payload);
    auto slot_one = request_for(payload);
    slot_one.claim_index = 1;
    std::shared_ptr<OwnerEngine::Operation> op_zero;
    std::shared_ptr<OwnerEngine::Operation> op_one;
    CHECK(owner.start(slot_zero, &op_zero) == DTV_L3_ACCEPTED);
    CHECK(owner.start(slot_one, &op_one) == DTV_L3_ACCEPTED);
    CHECK(provider.usb_destinations.size() == 2);
    CHECK(provider.usb_destinations[0] == std::make_pair(0u, 0x81u));
    CHECK(provider.usb_destinations[1] == std::make_pair(1u, 0x81u));
    CHECK(provider.usb_functions[0] == 10);
    CHECK(provider.usb_functions[1] == 20);

    auto control = request_for(payload);
    control.kind = DTV_L3_TRANSFER_CONTROL;
    control.endpoint = 0;
    control.claim_index = 1;
    control.control = {0x40, 0x55, 0x1234, 0x0044};
    std::shared_ptr<OwnerEngine::Operation> control_op;
    CHECK(owner.start(control, &control_op) == DTV_L3_ACCEPTED);
    CHECK(provider.last_control_slot == 1);
    CHECK(provider.last_control_setup.request_type == 0x40);
    CHECK(provider.last_control_setup.request == 0x55);
    CHECK(provider.last_control_setup.value == 0x1234);
    CHECK(provider.last_control_setup.index == 0x0044);
    CHECK(provider.usb_functions[2] == 20);

    const auto entries_before_invalid = provider.provider_entry_calls;
    auto invalid = slot_zero;
    invalid.claim_index = 2;
    std::shared_ptr<OwnerEngine::Operation> invalid_op;
    CHECK(owner.start(invalid, &invalid_op) == DTV_L3_INVALID);
    auto invalid_format = slot_zero;
    invalid_format.control.index = 1;
    CHECK(owner.start(invalid_format, &invalid_op) == DTV_L3_INVALID);
    CHECK(provider.provider_entry_calls == entries_before_invalid);
    CHECK(provider.usb_submit_calls == 3);
    CHECK(owner.release_operation(op_zero) == DTV_L3_OK);
    CHECK(owner.release_operation(op_one) == DTV_L3_OK);
    CHECK(owner.release_operation(control_op) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);

    ProviderMock provider_x;
    ProviderMock provider_y;
    provider_x.immediate_result = DTV_L3_OK;
    provider_y.immediate_result = DTV_L3_OK;
    provider_y.route_endpoints[1] = 0x82;
    OwnerEngine owner_x(provider_x, 52);
    OwnerEngine owner_y(provider_y, 53);
    owner_x.initialize_claims(2);
    owner_y.initialize_claims(2);
    auto same_input = request_for(payload);
    same_input.claim_index = 1;
    std::shared_ptr<OwnerEngine::Operation> accepted;
    std::shared_ptr<OwnerEngine::Operation> rejected;
    CHECK(owner_x.start(same_input, &accepted) == DTV_L3_ACCEPTED);
    CHECK(owner_y.start(same_input, &rejected) == DTV_L3_INVALID);
    CHECK(provider_x.provider_entry_calls == 1);
    CHECK(provider_x.usb_submit_calls == 1);
    CHECK(provider_y.provider_entry_calls == 1);
    CHECK(provider_y.usb_submit_calls == 0);
    CHECK(owner_x.release_operation(accepted) == DTV_L3_OK);
    CHECK(owner_y.release_operation(rejected) == DTV_L3_OK);
    CHECK(owner_x.close(2000) == DTV_L3_OK);
    CHECK(owner_y.close(2000) == DTV_L3_OK);
    CHECK(owner_x.release() == DTV_L3_OK);
    CHECK(owner_y.release() == DTV_L3_OK);

    ProviderMock control_x;
    ProviderMock control_y;
    control_x.immediate_result = DTV_L3_OK;
    control_y.immediate_result = DTV_L3_OK;
    control_x.route_functions.erase(1);
    OwnerEngine control_owner_x(control_x, 57);
    OwnerEngine control_owner_y(control_y, 58);
    control_owner_x.initialize_claims(2);
    control_owner_y.initialize_claims(2);
    auto same_control = request_for(payload);
    same_control.kind = DTV_L3_TRANSFER_CONTROL;
    same_control.endpoint = 0;
    same_control.claim_index = 1;
    same_control.control = {0x40, 0x55, 0x1234, 0x0044};
    std::shared_ptr<OwnerEngine::Operation> control_x_op;
    std::shared_ptr<OwnerEngine::Operation> control_y_op;
    CHECK(control_owner_x.start(same_control, &control_x_op) == DTV_L3_INVALID);
    CHECK(control_owner_y.start(same_control, &control_y_op) == DTV_L3_ACCEPTED);
    CHECK(control_x.provider_entry_calls == 1);
    CHECK(control_y.provider_entry_calls == 1);
    CHECK(control_x.usb_submit_calls == 0);
    CHECK(control_y.usb_submit_calls == 1);
    CHECK(control_y.usb_functions[0] == 20);
    CHECK(control_owner_x.release_operation(control_x_op) == DTV_L3_OK);
    CHECK(control_owner_y.release_operation(control_y_op) == DTV_L3_OK);
    CHECK(control_owner_x.close(2000) == DTV_L3_OK);
    CHECK(control_owner_y.close(2000) == DTV_L3_OK);
    CHECK(control_owner_x.release() == DTV_L3_OK);
    CHECK(control_owner_y.release() == DTV_L3_OK);
}

void test_failed_claim_residual_recovery_and_release_result_visibility() {
    {
        ProviderMock provider;
        provider.release_results[0] = {
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_RETAINED,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_FAILED},
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK}};
        OwnerEngine owner(provider, 103);
        owner.initialize_claims(1);
        CHECK(owner.close(2000) == DTV_L3_OK);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.physical_quiesced);
        CHECK(!snapshot.cleanup_complete);
        CHECK(snapshot.cleanup_claims_remaining == 1);
        CHECK(snapshot.cleanup_result == DTV_L3_FAILED);
        CHECK(owner.release() == DTV_L3_BUSY);
        CHECK(provider.released_claim_count() == 1);
        CHECK(owner.retry_cleanup() == DTV_L3_ACCEPTED);
        CHECK(wait_for_cleanup_pass(owner, 2));
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.physical_quiesced && snapshot.cleanup_complete);
        CHECK(snapshot.cleanup_result == DTV_L3_OK);
        CHECK(owner.release() == DTV_L3_OK);
    }

    {
        ProviderMock provider;
        provider.release_results[1] = {
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_RETAINED,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_FAILED},
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK}};
        provider.release_results[0] = {
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_FAILED, DTV_L3_UNSUPPORTED}};
        OwnerEngine owner(provider, 59);
        owner.initialize_claims(2);
        CHECK(owner.close(2000) == DTV_L3_OK);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.last_cleanup_error == DTV_L3_UNSUPPORTED);
        CHECK(snapshot.cleanup_result == DTV_L3_UNSUPPORTED);
        CHECK(snapshot.restoration_failed);
        CHECK(snapshot.cleanup_claims_remaining == 1);
        CHECK(owner.retry_cleanup() == DTV_L3_ACCEPTED);
        CHECK(wait_for_cleanup_pass(owner, 2));
        CHECK(provider.released_claims.size() == 3);
        CHECK(provider.released_claims[0] == 1);
        CHECK(provider.released_claims[1] == 0);
        CHECK(provider.released_claims[2] == 1);
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.last_cleanup_error == DTV_L3_UNSUPPORTED);
        CHECK(snapshot.cleanup_result == DTV_L3_UNSUPPORTED);
        CHECK(snapshot.restoration_failed);
        CHECK(snapshot.cleanup_complete);
        CHECK(snapshot.physical_quiesced);
        CHECK(owner.release() == DTV_L3_OK);
    }

    {
        ProviderMock provider;
        provider.release_results[0] = {
            {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_TIMEOUT}};
        OwnerEngine owner(provider, 104);
        owner.initialize_claims(1);
        CHECK(owner.close(0) == DTV_L3_TIMEOUT);
        CHECK(wait_for_cleanup_pass(owner, 1));
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(!snapshot.physical_quiesced);
        CHECK(snapshot.cleanup_complete);
        CHECK(snapshot.cleanup_claims_remaining == 0);
        CHECK(snapshot.cleanup_result == DTV_L3_TIMEOUT);
        CHECK(owner.retry_cleanup() == DTV_L3_BUSY);
        CHECK(provider.released_claim_count() == 1);
        CHECK(owner.release() == DTV_L3_BUSY);
    }

    {
        ProviderMock provider;
        provider.claim_results[0] =
            {DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
        provider.release_results[0] = {
            {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_FAILED},
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK}};
        OwnerEngine owner(provider, 54);
        owner.initialize_claims(1);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.claims_attempted == 1);
        CHECK(snapshot.transfer_ready_claims == 0);
        CHECK(snapshot.cleanup_claims_remaining == 1);
        CHECK(!snapshot.cleanup_complete);
        CHECK(owner.release() == DTV_L3_BUSY);
        CHECK(provider.released_claim_count() == 1);
        CHECK(owner.retry_cleanup() == DTV_L3_ACCEPTED);
        CHECK(wait_for_cleanup_pass(owner, 2));
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.cleanup_complete);
        CHECK(snapshot.cleanup_claims_remaining == 0);
        CHECK(owner.release() == DTV_L3_OK);
    }

    {
        ProviderMock provider;
        provider.claim_results[1] =
            {DTV_L3_FAILED, DTV_L3_CLAIM_RETAINED};
        provider.release_results[1] = {
            {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
             DTV_L3_RESTORE_UNKNOWN, DTV_L3_FAILED},
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK}};
        OwnerEngine owner(provider, 57);
        owner.initialize_claims(2);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.claims_attempted == 2);
        CHECK(snapshot.transfer_ready_claims == 0);
        CHECK(snapshot.cleanup_claims_remaining == 1);
        auto failed_slot_request = request_for({1});
        failed_slot_request.claim_index = 1;
        std::shared_ptr<OwnerEngine::Operation> failed_slot_op;
        CHECK(owner.start(failed_slot_request, &failed_slot_op) == DTV_L3_BUSY);
        CHECK(provider.provider_entry_calls == 0);
        CHECK(provider.released_claims.size() == 2);
        CHECK(provider.released_claims[0] == 1);
        CHECK(provider.released_claims[1] == 0);
        CHECK(owner.retry_cleanup() == DTV_L3_ACCEPTED);
        CHECK(wait_for_cleanup_pass(owner, 2));
        CHECK(provider.released_claims.size() == 3);
        CHECK(provider.released_claims[2] == 1);
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.cleanup_complete);
        CHECK(owner.release() == DTV_L3_OK);
    }

    {
        ProviderMock provider;
        provider.claim_results[0] =
            {DTV_L3_FAILED, DTV_L3_CLAIM_UNKNOWN};
        OwnerEngine owner(provider, 55);
        owner.initialize_claims(1);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.claims_attempted == 1);
        CHECK(snapshot.cleanup_claims_remaining == 1);
        CHECK(!snapshot.cleanup_complete);
        CHECK(owner.retry_cleanup() == DTV_L3_BUSY);
        CHECK(owner.release() == DTV_L3_BUSY);
        CHECK(provider.released_claim_count() == 0);
    }

    {
        ProviderMock provider;
        provider.release_results[0] = {
            {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
             DTV_L3_RESTORE_FAILED, DTV_L3_FAILED}};
        OwnerEngine owner(provider, 56);
        owner.initialize_claims(1);
        CHECK(owner.close(2000) == DTV_L3_OK);
        OwnerEngine::Snapshot snapshot{};
        CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
        CHECK(snapshot.cleanup_complete);
        CHECK(snapshot.physical_quiesced);
        CHECK(snapshot.restoration_failed);
        CHECK(snapshot.last_cleanup_error == DTV_L3_FAILED);
        CHECK(owner.release() == DTV_L3_OK);
    }
}

void test_lease_borrow_gate_and_stale_generation() {
    ProviderMock provider;
    OwnerEngine owner(provider, 41);
    owner.initialize_claims(0);
    std::uint32_t lease = 0;
    std::uint64_t programmed = 0;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    CHECK(owner.acquire_lease(0, &lease) == DTV_L3_OK);
    CHECK(owner.tune_lease(lease, 500000000, &programmed, &revision) ==
          DTV_L3_OK);
    CHECK(owner.start_lease(lease) == DTV_L3_OK);
    CHECK(owner.attach_lease(lease, &generation) == DTV_L3_OK);

    std::vector<std::uint8_t> first(188, 0x11);
    std::vector<std::uint8_t> second(188, 0x22);
    CHECK(provider.feed(generation, first));
    CHECK(provider.feed(generation, second));
    CHECK(owner.stop_stream(generation, 0) == DTV_L3_OK);

    TestBarrier borrow_barrier;
    dtv_l3_result read_result = DTV_L3_FAILED;
    dtv_l3_read_state read_state = DTV_L3_READ_WAIT;
    std::size_t bytes_read = 0;
    std::thread reader([&] {
        std::uint8_t buffer[2 * 188];
        read_result = owner.read_stream(
            generation, buffer, sizeof(buffer), &bytes_read, &read_state,
            [&] { borrow_barrier.arrive_and_wait(); });
    });
    borrow_barrier.wait_until_arrived();

    CHECK(owner.release_stream(generation,
                               DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) ==
          DTV_L3_ACCEPTED);
    dtv_l3_stream_status stream_status{};
    CHECK(owner.stream_status(generation, &stream_status) == DTV_L3_OK);
    CHECK(stream_status.phase == DTV_L3_STREAM_RETIRING);
    CHECK(stream_status.queued_packets == 2);
    CHECK(stream_status.read_borrows == 1);
    dtv_l3_lease_status lease_status{};
    CHECK(owner.lease_status(lease, &lease_status) == DTV_L3_OK);
    CHECK(lease_status.state != DTV_L3_LEASE_FREE);

    borrow_barrier.release();
    reader.join();
    CHECK(read_result == DTV_L3_OK);
    CHECK(read_state == DTV_L3_READ_DATA);
    CHECK(bytes_read == 2 * 188);
    CHECK(owner.stream_status(generation, &stream_status) == DTV_L3_OK);
    CHECK(stream_status.phase == DTV_L3_STREAM_RETIRED);
    CHECK(stream_status.queued_packets == 0);
    CHECK(stream_status.discarded_unread_packets == 0);
    CHECK(owner.lease_status(lease, &lease_status) == DTV_L3_OK);
    CHECK(lease_status.state == DTV_L3_LEASE_FREE);

    std::uint8_t packet[188];
    CHECK(owner.read_stream(generation, packet, sizeof(packet), &bytes_read,
                            &read_state) == DTV_L3_STALE);
    CHECK(owner.destroy_stream(generation) == DTV_L3_OK);
    CHECK(owner.live_stream_count() == 0);

    CHECK(owner.tune_lease(lease, 600000000, &programmed, &revision) ==
          DTV_L3_OK);
    CHECK(owner.start_lease(lease) == DTV_L3_OK);
    std::uint64_t next_generation = 0;
    CHECK(owner.attach_lease(lease, &next_generation) == DTV_L3_OK);
    CHECK(next_generation > generation);
    CHECK(provider.feed(next_generation, first));
    CHECK(owner.read_stream(next_generation, packet, sizeof(packet), &bytes_read,
                            &read_state) == DTV_L3_OK);
    CHECK(read_state == DTV_L3_READ_DATA);
    CHECK(packet[0] == 0x11);
    CHECK(owner.read_stream(generation, packet, sizeof(packet), &bytes_read,
                            &read_state) == DTV_L3_STALE);

    CHECK(owner.stop_stream(next_generation, 0) == DTV_L3_OK);
    CHECK(owner.read_stream(next_generation, packet, sizeof(packet), &bytes_read,
                            &read_state) == DTV_L3_OK);
    CHECK(read_state == DTV_L3_READ_EOF);
    CHECK(owner.release_stream(next_generation,
                               DTV_L3_STREAM_RELEASE_DRAINED) == DTV_L3_OK);
    CHECK(owner.destroy_stream(next_generation) == DTV_L3_OK);
    CHECK(owner.release_lease(lease) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_lease_close_waits_for_read_borrow() {
    ProviderMock provider;
    OwnerEngine owner(provider, 42);
    owner.initialize_claims(0);
    std::uint32_t lease = 0;
    std::uint64_t programmed = 0;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    CHECK(owner.acquire_lease(0, &lease) == DTV_L3_OK);
    CHECK(owner.tune_lease(lease, 500000000, &programmed, &revision) ==
          DTV_L3_OK);
    CHECK(owner.start_lease(lease) == DTV_L3_OK);
    CHECK(owner.attach_lease(lease, &generation) == DTV_L3_OK);
    CHECK(provider.feed(generation, std::vector<std::uint8_t>(188, 0x33)));
    CHECK(owner.stop_stream(generation, 0) == DTV_L3_OK);

    TestBarrier borrow_barrier;
    dtv_l3_result read_result = DTV_L3_FAILED;
    std::size_t bytes_read = 0;
    dtv_l3_read_state read_state = DTV_L3_READ_WAIT;
    std::thread reader([&] {
        std::uint8_t buffer[188];
        read_result = owner.read_stream(
            generation, buffer, sizeof(buffer), &bytes_read, &read_state,
            [&] { borrow_barrier.arrive_and_wait(); });
    });
    borrow_barrier.wait_until_arrived();

    CHECK(owner.close(0) == DTV_L3_TIMEOUT);
    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(!snapshot.all_ops_recovered);
    CHECK(owner.release() == DTV_L3_BUSY);

    borrow_barrier.release();
    reader.join();
    CHECK(read_result == DTV_L3_OK);
    CHECK(read_state == DTV_L3_READ_DATA);
    CHECK(owner.destroy_stream(generation) == DTV_L3_OK);
    CHECK(owner.release_lease(lease) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_stream_close_progresses_while_stop_blocked() {
    ProviderMock provider;
    OwnerEngine owner(provider, 43);
    owner.initialize_claims(0);
    provider.stream_stop_barrier = std::make_shared<TestBarrier>();
    std::uint32_t lease = 0;
    std::uint64_t programmed = 0;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    CHECK(owner.acquire_lease(0, &lease) == DTV_L3_OK);
    CHECK(owner.tune_lease(lease, 500000000, &programmed, &revision) ==
          DTV_L3_OK);
    CHECK(owner.start_lease(lease) == DTV_L3_OK);
    CHECK(owner.attach_lease(lease, &generation) == DTV_L3_OK);
    CHECK(provider.feed(generation, std::vector<std::uint8_t>(188, 0x44)));

    dtv_l3_result close_result = DTV_L3_FAILED;
    std::thread closer([&] { close_result = owner.close(0); });
    provider.stream_stop_barrier->wait_until_arrived();

    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(!snapshot.all_ops_recovered);
    dtv_l3_lease_status lease_status{};
    CHECK(owner.lease_status(lease, &lease_status) == DTV_L3_OK);
    dtv_l3_stream_status stream_status{};
    CHECK(owner.stream_status(generation, &stream_status) == DTV_L3_OK);
    CHECK(stream_status.phase == DTV_L3_STREAM_RETIRING);

    provider.stream_stop_barrier->release();
    closer.join();
    CHECK(close_result == DTV_L3_TIMEOUT);
    CHECK(wait_for_physical_quiescence(owner));
    CHECK(owner.destroy_stream(generation) == DTV_L3_OK);
    CHECK(owner.release_lease(lease) == DTV_L3_OK);
    CHECK(owner.close(2000) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

}  // namespace

int main() {
    test_failed_open_and_partial_claim_cleanup();
    test_failed_open_result_survives_cleanup_and_close();
    test_close_during_opening_defers_cleanup();
    test_immediate_completion_and_cancel_success_race();
    test_p1_borrow_survives_physical_quiescence_and_gate_orders();
    test_p2_binding_local_ids_and_b1_timeout_retention();
    test_r1_completion_does_not_drain_blocked_cancel_or_submit();
    test_r2_close_deadline_and_status_outlive_blocking_provider_calls();
    test_r3_cancel_before_registration_is_replayed_after_acceptance();
    test_b2_control_and_status_progress_while_submit_is_blocked();
    test_provider_route_slots_control_and_layered_rejection();
    test_failed_claim_residual_recovery_and_release_result_visibility();
    test_lease_borrow_gate_and_stale_generation();
    test_lease_close_waits_for_read_borrow();
    test_stream_close_progresses_while_stop_blocked();
    return failures == 0 ? 0 : 1;
}
