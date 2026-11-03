/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <pthread.h>

namespace {

enum class InitializeHold { None, Entry, AfterSetupUnlock };

std::mutex initialize_mutex;
std::condition_variable initialize_condition;
InitializeHold initialize_hold = InitializeHold::None;
bool initialize_entered = false;
bool initialize_released = false;
thread_local bool hold_after_setup_unlock = false;

}  // namespace

extern "C" void l3_test_hold_initialize(void) {
    std::lock_guard<std::mutex> lock(initialize_mutex);
    initialize_hold = InitializeHold::Entry;
    initialize_entered = false;
    initialize_released = false;
}

extern "C" void l3_test_hold_initialize_after_setup(void) {
    std::lock_guard<std::mutex> lock(initialize_mutex);
    initialize_hold = InitializeHold::AfterSetupUnlock;
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
    InitializeHold hold;
    {
        std::unique_lock<std::mutex> lock(initialize_mutex);
        hold = initialize_hold;
        initialize_hold = InitializeHold::None;
        if (hold == InitializeHold::Entry) {
            initialize_entered = true;
            initialize_condition.notify_all();
            initialize_condition.wait(
                lock, [] { return initialize_released; });
        }
    }
    hold_after_setup_unlock = hold == InitializeHold::AfterSetupUnlock;
    real_initialize_claims(owner, count);
}

extern "C" int real_pthread_mutex_unlock(pthread_mutex_t *mutex)
    __asm__("__real_pthread_mutex_unlock");

extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    const int result = real_pthread_mutex_unlock(mutex);
    if (result == 0 && hold_after_setup_unlock) {
        hold_after_setup_unlock = false;
        std::unique_lock<std::mutex> lock(initialize_mutex);
        initialize_entered = true;
        initialize_condition.notify_all();
        initialize_condition.wait(lock, [] { return initialize_released; });
    }
    return result;
}
