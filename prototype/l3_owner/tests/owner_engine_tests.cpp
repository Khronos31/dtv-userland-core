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

    dtv_l3_result claim(Binding *, std::uint32_t index) override {
        if (claim_barrier && index == claim_barrier_index) {
            claim_barrier->arrive_and_wait();
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++claim_calls[index];
        const auto found = claim_results.find(index);
        return found == claim_results.end() ? DTV_L3_OK : found->second;
    }

    void release_claim(Binding *, std::uint32_t index) override {
        std::lock_guard<std::mutex> lock(mutex);
        released_claims.push_back(index);
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
        return DTV_L3_ACCEPTED;
    }

    dtv_l3_result cancel(Binding *binding, std::uint64_t id) override {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled.push_back(TransferKey{binding, id});
        return DTV_L3_ACCEPTED;
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

    std::size_t released_claim_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return released_claims.size();
    }

    std::mutex mutex;
    std::map<TransferKey, Pending> transfers;
    std::map<std::uint32_t, std::uint32_t> claim_calls;
    std::map<std::uint32_t, dtv_l3_result> claim_results;
    std::vector<std::uint32_t> released_claims;
    std::vector<TransferKey> cancelled;
    std::shared_ptr<TestBarrier> claim_barrier;
    std::shared_ptr<TestBarrier> submit_barrier;
    std::uint32_t claim_barrier_index = 0;
    std::optional<dtv_l3_result> immediate_result;
    std::vector<std::uint8_t> immediate_data;
    std::size_t submit_calls = 0;
};

dtv_l3_transfer_request request_for(const std::vector<std::uint8_t> &payload) {
    return dtv_l3_transfer_request{1, 0x81, 1000, payload.data(), payload.size(), 64};
}

void test_failed_open_and_partial_claim_cleanup() {
    ProviderMock provider;
    provider.claim_results[1] = DTV_L3_FAILED;
    OwnerEngine owner(provider, 10);

    OwnerEngine::Snapshot before_claims{};
    CHECK(owner.close_status(&before_claims) == DTV_L3_OK);
    CHECK(before_claims.open_state == OwnerEngine::OpenState::Opening);

    owner.initialize_claims(3);
    OwnerEngine::Snapshot snapshot{};
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.open_state == OwnerEngine::OpenState::Closed);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(provider.released_claim_count() == 1);
    CHECK(owner.cleanup_count() == 1);
    CHECK(owner.close(0) == DTV_L3_OK);
    CHECK(owner.close(0) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
    CHECK(owner.release_count() == 1);
    CHECK(owner.cleanup_count() == 1);
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
    CHECK(owner.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(owner.close(0) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_immediate_completion_and_cancel_success_race() {
    ProviderMock provider;
    provider.immediate_result = DTV_L3_OK;
    provider.immediate_data = {7, 8};
    OwnerEngine owner(provider, 12);
    owner.initialize_claims(0);
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
    CHECK(owner.close(0) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

void test_p1_borrow_survives_physical_quiescence_and_gate_orders() {
    ProviderMock provider;
    OwnerEngine owner(provider, 13);
    owner.initialize_claims(0);
    CHECK(owner.close(0) == DTV_L3_OK);
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
    CHECK(second.close(0) == DTV_L3_OK);
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
    owner_a.initialize_claims(0);
    owner_b.initialize_claims(0);
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
    CHECK(owner_a.close_status(&snapshot) == DTV_L3_OK);
    CHECK(snapshot.physical_quiesced);
    CHECK(snapshot.cleanup_complete);
    CHECK(owner_a.close(0) == DTV_L3_OK);
    CHECK(owner_b.close(0) == DTV_L3_OK);
    op_a.reset();
    binding_a.reset();
    CHECK(!old_binding.expired());
    CHECK(owner_a.release() == DTV_L3_OK);
    CHECK(owner_b.release() == DTV_L3_OK);
    CHECK(old_binding.expired());
}

void test_b2_control_and_status_progress_while_submit_is_blocked() {
    ProviderMock provider;
    auto barrier = std::make_shared<TestBarrier>();
    provider.submit_barrier = barrier;
    OwnerEngine owner(provider, 31);
    owner.initialize_claims(0);
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
    CHECK(owner.close(0) == DTV_L3_OK);
    CHECK(owner.release() == DTV_L3_OK);
}

}  // namespace

int main() {
    test_failed_open_and_partial_claim_cleanup();
    test_close_during_opening_defers_cleanup();
    test_immediate_completion_and_cancel_success_race();
    test_p1_borrow_survives_physical_quiescence_and_gate_orders();
    test_p2_binding_local_ids_and_b1_timeout_retention();
    test_b2_control_and_status_progress_while_submit_is_blocked();
    return failures == 0 ? 0 : 1;
}
