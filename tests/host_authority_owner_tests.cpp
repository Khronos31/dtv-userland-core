// SPDX-License-Identifier: GPL-2.0-only
#include "px4/host_authority_owner.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::detail;
using namespace px4::userland;

class Backend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        ++opens;
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
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
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }

    unsigned opens = 0U;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success({{1U}});
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class Executor final : public ControlAuthorityExecutor {
public:
    bool submit(const AuthorityJob&) noexcept override { return true; }
};

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> result;
    while (value[result.length] != '\0') {
        result.bytes[result.length] = value[result.length];
        ++result.length;
    }
    return result;
}

w::ListResponse make_list()
{
    w::ListResponse list;
    for (std::size_t i = 0U; i < list.epoch.size(); ++i)
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    list.producer.product = text<32>("owner-test");
    list.producer.version = text<64>("1");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 2U;
    for (std::size_t i = 0U; i < 2U; ++i) {
        auto& receiver = list.receivers.value.items[i];
        receiver.id = static_cast<std::uint16_t>(i + 2U);
        receiver.systems.mask = 1U;
        receiver.binding.present = true;
        receiver.binding.value.function_id = 1U;
        receiver.binding.value.local_id = static_cast<std::uint16_t>(i);
    }
    list.readers.present = true;
    return list;
}

w::Frame request(std::uint16_t opcode, std::uint32_t id, ByteView payload)
{
    w::Frame result;
    result.header.kind = w::Kind::request;
    result.header.opcode = opcode;
    result.header.request_id = id;
    result.header.payload_length = static_cast<std::uint32_t>(payload.size);
    result.payload = payload;
    return result;
}

bool hello(ControlAuthority& authority, ControlServiceSession& session,
           Executor& executor)
{
    std::array<std::uint8_t, w::kHelloRequestSize> payload{};
    const w::HelloRequest value{{1U, 0U}, {1U, 0U}, 0U, 0U};
    const auto size = w::encode_hello_request(value, {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = authority.accept(session,
        request(w::kOpcodeHello, 1U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    AuthorityJob rejected;
    return reply.action == w::InboundAction::respond &&
           authority.response_sent(session, true, executor, &rejected) == Error::OK;
}

std::uint64_t authority_acquire(ControlAuthority& authority,
                                ControlServiceSession& session,
                                Executor& executor, std::uint32_t id,
                                std::uint16_t receiver)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({authority.server().epoch, receiver},
                                                 {payload.data(), payload.size()});
    if (!size) return 0U;
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = authority.accept(session,
        request(w::kOpcodeAcquire, id, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    AuthorityJob rejected;
    if (reply.action != w::InboundAction::respond ||
        authority.response_sent(session, true, executor, &rejected) != Error::OK ||
        reply.frame.size < w::kHeaderSize + w::kAcquireResponseSize)
        return 0U;
    const auto response = w::decode_acquire_response(
        {reply.frame.data + w::kHeaderSize, w::kAcquireResponseSize});
    return response ? response.value().lease_id : 0U;
}

int fail(const char* message)
{
    std::cerr << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService service(backend, nonce, time);
    const auto invalid = service.acquire(kAuthorityClientIdFirst, 2U);
    if (invalid || invalid.error() != Error::INVALID_ARGUMENT || backend.opens != 0U)
        return fail("legacy high-bit ACQUIRE was not rejected before backend access");

    const auto rejected = [](const auto& result) {
        return !result && result.error() == Error::INVALID_ARGUMENT;
    };
    ipc::TuneRequestPayload tune_request{};
    TunerService::TunerAdmittedTune admitted{};
    admitted.client_id = kAuthorityClientIdFirst;
    TunerService::TunerStreamTicket stream_ticket{};
    stream_ticket.client_id = kAuthorityClientIdFirst;
    TunerAttachment attachment{};
    attachment.owner_client_id = kAuthorityClientIdFirst;
    const std::array<std::uint8_t, ipc::kNonceLength> stream_nonce{};
    if (!rejected(service.release(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.tune(kAuthorityClientIdFirst, tune_request)) ||
        !rejected(service.start_stream(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.stop_stream(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.stats(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.detach_stream(attachment)) ||
        !rejected(service.revoke_stream(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.revoke_stream(kAuthorityClientIdFirst, 1U, stream_nonce)) ||
        !rejected(service.disconnect_client(kAuthorityClientIdFirst)) ||
        !rejected(service.try_admit_tune(kAuthorityClientIdFirst, tune_request, 0U)) ||
        !rejected(service.try_admit_prepared_tune(kAuthorityClientIdFirst,
                                                   TunerPreparedTune{}, 0U)) ||
        !rejected(service.execute_admitted_tune(admitted)) ||
        !rejected(service.abort_unstarted_tune(admitted, Error::INTERNAL)) ||
        !rejected(service.revoke_tune_ticket(kAuthorityClientIdFirst, 1U)) ||
        !rejected(service.start_stream_v1(kAuthorityClientIdFirst, 1U, 1U, 0U, 1U)) ||
        !rejected(service.reserve_attach_v1(kAuthorityClientIdFirst, 1U, 1U, 1U,
                                             detail::ReceiverToken{}, 0U)) ||
        !rejected(service.prepare_attach_v1(stream_ticket)) ||
        !rejected(service.commit_attach_v1(stream_ticket)) ||
        !rejected(service.abort_attach_v1(stream_ticket, Error::INTERNAL)) ||
        !rejected(service.stop_stream_v1(kAuthorityClientIdFirst, 1U, 1U)) ||
        !rejected(service.expire_armed_v1(kAuthorityClientIdFirst, 1U, 1U, 0U)) ||
        !rejected(service.run_stream_cleanup_v1(stream_ticket)) ||
        !rejected(service.complete_stream_cleanup_v1(stream_ticket, Error::OK, true)) ||
        !rejected(service.stream_last_terminal_v1(kAuthorityClientIdFirst, 1U)) ||
        backend.opens != 0U)
        return fail("a legacy client-ID entrance accepted high-bit identity");

    const auto legacy = service.acquire(7U, 3U);
    if (!legacy) return fail("legacy low ID ACQUIRE failed");

    const auto list = make_list();
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    HostAuthorityOwner owner(service, server, list);
    auto& authority = owner.authority();
    ControlServiceSession session(authority);
    Executor executor;
    if (authority.open(session) != Error::OK ||
        session.client_id() != kAuthorityClientIdFirst ||
        !hello(authority, session, executor))
        return fail("high-range Authority session did not open on the host owner");
    const std::uint64_t authority_lease = authority_acquire(
        authority, session, executor, 2U, 2U);
    if (authority_lease == 0U) return fail("Authority ACQUIRE failed");

    auto status = service.engine_status();
    if (!status || status.value().receiver_count < 4U ||
        status.value().states[2U] != ReceiverState::leased ||
        status.value().states[3U] != ReceiverState::leased)
        return fail("legacy and Authority leases did not coexist in one Engine");

    if (!service.release(7U, legacy.value().lease_id))
        return fail("legacy RELEASE failed");
    status = service.engine_status();
    if (!status || status.value().states[2U] != ReceiverState::leased ||
        status.value().states[3U] != ReceiverState::free)
        return fail("legacy RELEASE disturbed the Authority receiver lease");

    const auto rejected_disconnect = service.disconnect_client(session.client_id());
    if (rejected_disconnect || rejected_disconnect.error() != Error::INVALID_ARGUMENT)
        return fail("legacy DISCONNECT accepted a high-range Authority ID");
    authority.close(session);
    std::cout << "host authority owner identity tests passed\n";
    return 0;
}
