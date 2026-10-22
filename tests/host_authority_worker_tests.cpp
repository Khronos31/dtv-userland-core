// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_daemon/control_workers.h"
#include "px4/host_authority_owner.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::detail;
using namespace px4::userland;
using namespace px4::userland::ipc::posix;

class Backend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t, std::uint64_t, std::uint32_t) noexcept override
    {
        ++hz_calls;
        return Result<TunerHzProgramming>::success({});
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        TunerStrictProfile profile;
        if (receiver == 2U && system == ipc::System::ISDB_T) {
            profile.native_hz = true;
            profile.min_hz = 400000000U;
            profile.max_hz = 800000000U;
        }
        return profile;
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        if (receiver < closed.size()) closed[receiver] = true;
        return Result<void>::success();
    }
    unsigned hz_calls = 0U;
    std::array<bool, 8U> closed{};
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    { return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success({{1U}}); }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::success(0U); }
    bool initialized() const noexcept override { return false; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtrBytes atr_{};
};

class Executor final : public ControlAuthorityExecutor {
public:
    bool submit(const AuthorityJob& value) noexcept override
    {
        if (has_job) return false;
        job = value;
        has_job = true;
        return true;
    }
    AuthorityJob take() noexcept
    {
        has_job = false;
        return job;
    }
    AuthorityJob job{};
    bool has_job = false;
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
    list.producer.product = text<32U>("authority-worker-test");
    list.producer.version = text<64U>("1");
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
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 2U;
    for (std::size_t i = 0U; i < 2U; ++i) {
        auto& receiver = list.receivers.value.items[i];
        receiver.id = i == 0U ? 2U : 4U;
        receiver.systems.mask = i == 0U ? 1U : 2U;
        receiver.binding.present = true;
        receiver.binding.value.function_id = 1U;
        receiver.binding.value.local_id = receiver.id;
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
    const auto size = w::encode_hello_request({{1U, 0U}, {1U, 0U}, 0U, 0U},
        {payload.data(), payload.size()});
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

ControlWorkerTask task_for(const AuthorityJob& job, ControlWorkerTaskKind kind);

bool wait_completion(ControlWorkerLanes& workers, ControlWorkerLane lane,
                     ControlWorkerCompletion& completion);

std::uint64_t acquire(ControlAuthority& authority, ControlServiceSession& session,
                      Executor& executor, ControlWorkerLanes& workers,
                      ControlWorkerLane lane, std::uint32_t request_id,
                      std::uint16_t receiver)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({authority.server().epoch, receiver},
        {payload.data(), payload.size()});
    if (!size) return 0U;
    w::Context context;
    w::Snapshot snapshot;
    auto reply = authority.accept(session,
        request(w::kOpcodeAcquire, request_id, {payload.data(), size.value()}),
        context, snapshot, 0U, executor);
    if (!reply.queued || !executor.has_job)
        return 0U;
    const AuthorityJob job = executor.take();
    const auto task = task_for(job, ControlWorkerTaskKind::authority_request);
    if (workers.submit(lane, task).error() != Error::OK) return 0U;
    ControlWorkerCompletion completion{};
    if (!wait_completion(workers, lane, completion) || completion.error != Error::OK)
        return 0U;
    reply = authority.complete(completion.authority_completion);
    AuthorityJob rejected;
    if (reply.action != w::InboundAction::respond ||
        authority.response_sent(session, true, executor, &rejected) != Error::OK)
        return 0U;
    const auto decoded = w::decode_acquire_response(
        {reply.frame.data + w::kHeaderSize, w::kAcquireResponseSize});
    return decoded ? decoded.value().lease_id : 0U;
}

bool wait_completion(ControlWorkerLanes& workers, ControlWorkerLane lane,
                     ControlWorkerCompletion& completion)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        if (workers.try_pop(lane, completion)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

ControlWorkerTask task_for(const AuthorityJob& job, ControlWorkerTaskKind kind)
{
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::tuner_authority_job;
    task.kind = kind;
    task.authority_job = job;
    task.client_id = job.key.client_id;
    task.request_id = static_cast<std::uint32_t>(job.key.request_id);
    if (job.kind == AuthorityJobKind::tune) {
        task.receiver = job.tune.receiver;
        task.lease_id = job.tune.lease_id;
    } else {
        task.receiver = job.receiver;
        task.lease_id = job.lease_id;
    }
    return task;
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
    TunerService tuner(backend, nonce, time);
    if (!tuner.configure_strict_capabilities())
        return fail("strict tune admission setup failed");
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    auto created = ControlWorkerLanes::create(card, tuner);
    if (!created) return fail("worker create failed");
    ControlWorkerLanes& workers = *created.value();

    const auto list = make_list();
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    HostAuthorityOwner owner(tuner, server, list);
    auto& authority = owner.authority();
    ControlServiceSession session(authority);
    struct SessionGuard final {
        ControlAuthority& authority;
        ControlServiceSession& session;
        ~SessionGuard() { if (session.connected()) (void)authority.close(session); }
    } session_guard{authority, session};
    Executor executor;
    if (authority.open(session) != Error::OK || !hello(authority, session, executor))
        return fail("authority session setup failed");
    const std::uint64_t lease2 = acquire(authority, session, executor, workers,
        ControlWorkerLane::tuner_dev1, 2U, 2U);
    const std::uint64_t lease3 = acquire(authority, session, executor, workers,
        ControlWorkerLane::tuner_dev2, 3U, 4U);
    if (lease2 == 0U || lease3 == 0U)
        return fail("authority leases were not acquired");

    std::array<std::uint8_t, 128U> payload{};
    w::TuneRequest tune;
    tune.epoch = server.epoch;
    tune.lease_id = lease2;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = 1000U;
    const auto encoded = w::encode_tune_request(tune, {payload.data(), payload.size()});
    if (!encoded) return fail("authority tune encoding failed");
    w::Context context;
    w::Snapshot snapshot;
    context.state = w::ContextState::ready;
    const auto tune_reply = authority.accept(session,
        request(w::kOpcodeTune, 4U, {payload.data(), encoded.value()}), context,
        snapshot, 0U, executor);
    if (!tune_reply.queued || !executor.has_job || backend.hz_calls != 0U) {
        if (tune_reply.frame.size >= w::kHeaderSize) {
            const auto header = w::decode_header({tune_reply.frame.data, w::kHeaderSize});
            if (header) {
                const auto error = w::decode_error_response(
                    {tune_reply.frame.data + w::kHeaderSize,
                     header.value().payload_length});
                if (error) std::cerr << "tune error=" << error.value().error.code << '\n';
            }
        }
        (void)authority.close(session);
        return fail("authority tune was not admitted once before worker submit");
    }
    const AuthorityJob tune_job = executor.take();
    if (tune_job.key.client_id != session.client_id() ||
        tune_job.key.request_id != 4U || tune_job.tune.receiver != 2U)
        return fail("authority tune job lost its origin values");
    const auto tune_task = task_for(tune_job, ControlWorkerTaskKind::authority_request);
    if (workers.submit(ControlWorkerLane::tuner_dev1, tune_task).error() != Error::OK)
        return fail("authority tune task was rejected on its receiver lane");
    ControlWorkerCompletion completion{};
    if (!wait_completion(workers, ControlWorkerLane::tuner_dev1, completion) ||
        completion.error != Error::OK || completion.authority_key.client_id !=
            tune_job.key.client_id || completion.authority_key.request_id != 4U ||
        completion.authority_job_kind != AuthorityJobKind::tune ||
        completion.receiver != 2U || completion.lease_id != lease2 ||
        completion.authority_completion.key.client_id != tune_job.key.client_id ||
        completion.authority_completion.kind != AuthorityJobKind::tune ||
        completion.authority_completion.lease_id != lease2 ||
        backend.hz_calls != 1U)
        return fail("worker did not execute the admitted Authority tune exactly once");
    (void)authority.complete(completion.authority_completion);
    AuthorityJob rejected_cleanup;
    if (authority.response_sent(session, true, executor, &rejected_cleanup) != Error::OK)
        return fail("Authority TUNE response completion failed");

    std::array<std::uint8_t, w::kEpochSize + 8U> release_payload{};
    const auto release_size = w::encode_release_request({server.epoch, lease3},
        {release_payload.data(), release_payload.size()});
    if (!release_size) return fail("Authority RELEASE encoding failed");
    const auto release_reply = authority.accept(session,
        request(w::kOpcodeRelease, 5U,
                {release_payload.data(), release_size.value()}),
        context, snapshot, 0U, executor);
    if (!release_reply.queued || !executor.has_job) {
        if (release_reply.frame.size >= w::kHeaderSize) {
            const auto header = w::decode_header(
                {release_reply.frame.data, w::kHeaderSize});
            if (header) {
                const auto error = w::decode_error_response(
                    {release_reply.frame.data + w::kHeaderSize,
                     header.value().payload_length});
                if (error) std::cerr << "release error=" << error.value().error.code << '\n';
            }
        }
        (void)authority.close(session);
        return fail("Authority RELEASE was not dispatched");
    }
    const AuthorityJob release_job = executor.take();
    if (release_job.receiver != 4U)
        return fail("Authority RELEASE omitted its receiver route");
    const auto release_task = task_for(release_job,
                                       ControlWorkerTaskKind::authority_request);

    auto submit_rejected_without_effect = [&](ControlWorkerLane lane,
                                              const ControlWorkerTask& task,
                                              const char* label) {
        const auto before = tuner.engine_status();
        const unsigned hz_calls = backend.hz_calls;
        if (!before || before.value().states[2U] == ReceiverState::free ||
            before.value().states[4U] == ReceiverState::free) {
            std::cerr << label << " fixture leases are missing\n";
            return false;
        }
        if (workers.submit(lane, task).error() != Error::INVALID_ARGUMENT) {
            std::cerr << label << " was not rejected at submit\n";
            return false;
        }
        ControlWorkerCompletion unexpected{};
        if (workers.try_pop(lane, unexpected)) {
            std::cerr << label << " reached the worker completion queue\n";
            return false;
        }
        const auto after = tuner.engine_status();
        if (!after || after.value().states[2U] != before.value().states[2U] ||
            after.value().states[4U] != before.value().states[4U] ||
            backend.hz_calls != hz_calls || backend.closed[2U] || backend.closed[4U]) {
            std::cerr << label << " changed Engine/backend state\n";
            return false;
        }
        return true;
    };

    ControlWorkerTask bad_client_task = release_task;
    bad_client_task.authority_job.key.client_id = 1U;
    bad_client_task.authority_job.client_id = 1U;
    bad_client_task.client_id = 1U;
    if (!submit_rejected_without_effect(ControlWorkerLane::tuner_dev2,
                                        bad_client_task,
                                        "out-of-range Authority client ID"))
        return fail("out-of-range Authority client ID submit guard failed");

    AuthorityJob oversized_request_job = release_job;
    oversized_request_job.key.request_id =
        static_cast<std::uint64_t>(UINT32_MAX) + 1U;
    const auto oversized_request_task = task_for(
        oversized_request_job, ControlWorkerTaskKind::authority_request);
    if (!submit_rejected_without_effect(ControlWorkerLane::tuner_dev2,
                                        oversized_request_task,
                                        "u32-overflow Authority request ID"))
        return fail("u32-overflow Authority request ID submit guard failed");

    AuthorityJob receiver_lane_disconnect{};
    receiver_lane_disconnect.kind = AuthorityJobKind::disconnect;
    const std::uint64_t disconnected_client = kAuthorityClientIdFirst + 100U;
    receiver_lane_disconnect.key = SessionKey{disconnected_client, 6U};
    receiver_lane_disconnect.client_id = disconnected_client;
    receiver_lane_disconnect.receiver = 4U;
    const auto receiver_lane_disconnect_task = task_for(
        receiver_lane_disconnect, ControlWorkerTaskKind::authority_request);
    if (!submit_rejected_without_effect(ControlWorkerLane::tuner_dev2,
                                        receiver_lane_disconnect_task,
                                        "DISCONNECT on receiver lane"))
        return fail("DISCONNECT receiver-lane submit guard failed");

    AuthorityJob deferred_disconnect = receiver_lane_disconnect;
    deferred_disconnect.deferred = true;
    const auto deferred_disconnect_task = task_for(
        deferred_disconnect, ControlWorkerTaskKind::authority_cleanup);
    if (!submit_rejected_without_effect(ControlWorkerLane::tuner_dev1,
                                        deferred_disconnect_task,
                                        "deferred DISCONNECT before ACQUIRE completion"))
        return fail("deferred DISCONNECT submit guard failed");

    ControlWorkerTask mismatched_request_task = release_task;
    ++mismatched_request_task.request_id;
    if (!submit_rejected_without_effect(ControlWorkerLane::tuner_dev2,
                                        mismatched_request_task,
                                        "task/job request ID mismatch"))
        return fail("task/job request ID submit guard failed");

    if (workers.submit(ControlWorkerLane::tuner_dev1, release_task).error() !=
            Error::INVALID_ARGUMENT ||
        !workers.submit(ControlWorkerLane::tuner_dev2, release_task))
        return fail("Authority RELEASE did not enforce receiver lane routing");
    if (!wait_completion(workers, ControlWorkerLane::tuner_dev2, completion) ||
        completion.error != Error::OK || completion.authority_key.request_id != 5U ||
        completion.authority_job_kind != AuthorityJobKind::release ||
        completion.receiver != 4U || completion.lease_id != lease3)
        return fail("Authority RELEASE completion lost routed values");
    (void)authority.complete(completion.authority_completion);
    if (authority.response_sent(session, true, executor, &rejected_cleanup) != Error::OK)
        return fail("Authority RELEASE response completion failed");

    const AuthorityJob cleanup = authority.close(session);
    const auto cleanup_task = task_for(cleanup, ControlWorkerTaskKind::authority_cleanup);
    if (cleanup.kind != AuthorityJobKind::disconnect ||
        !workers.submit(ControlWorkerLane::tuner_dev1, cleanup_task) ||
        !workers.request_stop() ||
        workers.submit(ControlWorkerLane::tuner_dev1, cleanup_task).error() !=
            Error::NOT_READY ||
        !wait_completion(workers, ControlWorkerLane::tuner_dev1, completion) ||
        completion.error != Error::OK ||
        completion.authority_job_kind != AuthorityJobKind::disconnect ||
        completion.authority_key.client_id != cleanup.key.client_id)
        return fail("Authority DISCONNECT cleanup was not delivered through worker");
    const auto status = tuner.engine_status();
    if (!status || status.value().states[2U] != ReceiverState::free ||
        status.value().states[4U] != ReceiverState::free)
        return fail("Authority DISCONNECT did not clean the shared Engine leases");
    if (!workers.join()) return fail("worker stop and drain failed");
    std::cout << "host authority worker value tests passed\n";
    return 0;
}
