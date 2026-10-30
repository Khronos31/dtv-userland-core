#include "dtv/userland/host_daemon/tuner_service.h"
#include "px4/tuner_service.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using namespace px4::userland;

class FakeBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        opened = receiver;
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t frequency_khz,
                                  std::uint32_t) noexcept override
    {
        tuned = receiver == opened && frequency_khz == 473142U;
        return tuned ? Result<void>::success() : Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        closed = receiver;
        return Result<void>::success();
    }

    std::uint8_t opened = 0xffU;
    std::uint8_t closed = 0xffU;
    bool tuned = false;
};

class FakeNonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> value{};
        value[0] = 1U;
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(value);
    }
};

class FakeTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now += milliseconds; }

private:
    std::uint64_t now = 0U;
};

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    FakeBackend backend;
    FakeNonce nonce;
    FakeTime time;
    TunerService service(backend, nonce, time);

    const auto acquired = service.acquire(1U, 2U);
    if (!acquired) return fail("ACQUIRE failed");
    auto status = service.status();
    if (!status || status.value().receiver_states[2U] != ipc::ReceiverState::leased)
        return fail("ACQUIRE did not update the service owner state");

    const ipc::TuneRequestPayload request{
        acquired.value().lease_id, ipc::System::ISDB_T, 473142U, 0xffffU, 0xffffU,
        6000000U, 0U, 1000U};
    const auto tuned = service.tune(1U, request);
    if (!tuned || !backend.tuned || tuned.value().locked != 1U)
        return fail("TUNE failed through the service owner");
    status = service.status();
    if (!status || status.value().receiver_states[2U] != ipc::ReceiverState::tuned)
        return fail("TUNE did not update the service owner state");

    const auto released = service.release(1U, acquired.value().lease_id);
    if (!released || backend.closed != 2U) return fail("RELEASE cleanup failed");
    status = service.status();
    if (!status || status.value().receiver_states[2U] != ipc::ReceiverState::free)
        return fail("RELEASE did not clean up the service owner state");

    std::cout << "TunerService owner lifecycle passed\n";
    return 0;
}
