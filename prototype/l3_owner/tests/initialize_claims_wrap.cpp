/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace {

std::mutex initialize_mutex;
std::condition_variable initialize_condition;
bool initialize_held = false;
bool initialize_entered = false;
bool initialize_released = false;

}  // namespace

extern "C" void l3_test_hold_initialize(void) {
    std::lock_guard<std::mutex> lock(initialize_mutex);
    initialize_held = true;
    initialize_entered = false;
    initialize_released = false;
}

extern "C" int l3_test_wait_initialize(void) {
    std::unique_lock<std::mutex> lock(initialize_mutex);
    return initialize_condition.wait_for(
               lock, std::chrono::seconds(5),
               [] { return initialize_entered; })
               ? 1
               : 0;
}

extern "C" void l3_test_release_initialize(void) {
    std::lock_guard<std::mutex> lock(initialize_mutex);
    initialize_released = true;
    initialize_condition.notify_all();
}

extern "C" void real_initialize_claims(
    l3_owner_prototype::OwnerEngine *owner,
    std::uint32_t count) __asm__("__real__ZN18l3_owner_prototype11OwnerEngine17initialize_claimsEj");

extern "C" void __wrap__ZN18l3_owner_prototype11OwnerEngine17initialize_claimsEj(
    l3_owner_prototype::OwnerEngine *owner,
    std::uint32_t count) {
    {
        std::unique_lock<std::mutex> lock(initialize_mutex);
        if (initialize_held) {
            initialize_entered = true;
            initialize_condition.notify_all();
            initialize_condition.wait(
                lock, [] { return initialize_released; });
            initialize_held = false;
        }
    }
    real_initialize_claims(owner, count);
}
