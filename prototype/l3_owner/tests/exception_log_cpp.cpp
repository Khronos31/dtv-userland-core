/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "owner_engine.h"

#include <chrono>
#include <cstdint>
#include <new>
#include <thread>

using namespace l3_owner_prototype;

namespace {

class QuiescentProvider final : public Provider {
public:
    dtv_l3_claim_result claim(Binding *, std::uint32_t) override {
        return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    }

    dtv_l3_release_result release_claim(Binding *, std::uint32_t) override {
        return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                DTV_L3_RESTORE_SUCCEEDED, DTV_L3_OK};
    }

    dtv_l3_result submit(Binding *,
                         std::uint64_t,
                         const dtv_l3_transfer_request &,
                         dtv_l3_transfer_complete_fn,
                         void *) override {
        return DTV_L3_ACCEPTED;
    }

    dtv_l3_result cancel(Binding *, std::uint64_t) override {
        return DTV_L3_ACCEPTED;
    }
};

void throwing_log(void *,
                  dtv_l3_log_level,
                  const char *,
                  const char *) {
    throw std::bad_alloc();
}

}  // namespace

int main() {
    QuiescentProvider provider;
    OwnerEngine engine(provider, 7,
                       dtv_l3_logger{throwing_log, nullptr,
                                     DTV_L3_LOG_DEBUG});
    set_test_exception_mask(kTestInjectClaim);
    engine.initialize_claims(1);
    set_test_exception_mask(0);

    OwnerEngine::Snapshot snapshot{};
    if (engine.close_status(&snapshot) != DTV_L3_OK ||
        snapshot.open_state != OwnerEngine::OpenState::Failed) {
        return 1;
    }
    if (engine.close(2000) != DTV_L3_OK) {
        return 2;
    }
    for (unsigned int attempt = 0; attempt < 4000; ++attempt) {
        if (engine.close_status(&snapshot) != DTV_L3_OK) {
            return 3;
        }
        if (snapshot.releasable) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!snapshot.releasable) {
        return 4;
    }
    if (engine.release() != DTV_L3_OK) {
        return 5;
    }
    return 0;
}
