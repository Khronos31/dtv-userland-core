// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/control_authority.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::detail;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "control authority check failed at line %d: %s\n", \
                         __LINE__, #condition); \
            return false; \
        } \
    } while (0)

class Backend final : public ReceiverBackend {
public:
    bool supports(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial;
    }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override { return 0U; }
    Result<void> open(ReceiverId) noexcept override
    {
        ++open_calls;
        open_thread = std::this_thread::get_id();
        return Result<void>::success();
    }
    Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t,
                      std::uint32_t) noexcept override
    {
        ++khz_calls;
        return Result<void>::success();
    }
    Result<TuneProgramming> program_hz(ReceiverId, ReceiverSystem,
                                       std::uint64_t requested_hz,
                                       std::uint32_t) noexcept override
    {
        ++hz_calls;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (block_tune) {
                tune_started = true;
                condition.notify_all();
                condition.wait(lock, [&] { return !block_tune; });
            }
        }
        if (tune_error != Error::OK)
            return Result<TuneProgramming>::failure(tune_error);
        return Result<TuneProgramming>::success(TuneProgramming{requested_hz + 9U});
    }
    Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(ReceiverId, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_tsid(ReceiverId, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> close(ReceiverId) noexcept override
    {
        ++close_calls;
        close_thread = std::this_thread::get_id();
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (block_close) {
                close_started = true;
                condition.notify_all();
                condition.wait(lock, [&] { return !block_close; });
            }
        }
        return close_error == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(close_error);
    }
    Result<void> begin_tune_power(ReceiverId, ReceiverSystem, std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> commit_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> rollback_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    void mark_disconnected(ReceiverId) noexcept override {}
    Result<void> shutdown() noexcept override { return Result<void>::success(); }
    Result<void> start_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }

    void wait_for_tune()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return tune_started; });
    }
    void unblock_tune()
    {
        std::lock_guard<std::mutex> lock(mutex);
        block_tune = false;
        condition.notify_all();
    }
    void wait_for_close()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return close_started; });
    }
    void unblock_close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        block_close = false;
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool block_tune = false;
    bool tune_started = false;
    bool block_close = false;
    bool close_started = false;
    std::thread::id open_thread{};
    std::thread::id close_thread{};
    std::atomic<int> open_calls{0};
    std::atomic<int> hz_calls{0};
    std::atomic<int> khz_calls{0};
    std::atomic<int> close_calls{0};
    Error close_error = Error::OK;
    Error tune_error = Error::OK;
};

class Tokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (block) {
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return !block; });
        }
        return Result<ReceiverToken>::success(ReceiverToken{17U});
    }
    void arm()
    {
        std::lock_guard<std::mutex> lock(mutex);
        entered = false;
        block = true;
    }
    void wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex);
        block = false;
        condition.notify_all();
    }
    std::mutex mutex;
    std::condition_variable condition;
    bool block = false;
    bool entered = false;
};

class Clock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t value) noexcept override { now += value; }
    std::uint64_t now = 100U;
};

class Executor final : public ControlAuthorityExecutor {
public:
    bool submit(const AuthorityJob& job) noexcept override
    {
        if (reject || count == jobs.size()) return false;
        jobs[count++] = job;
        return true;
    }
    w::Optional<bool> lnb_power_state() noexcept override { return lnb_state; }
    bool supports_internal_card_observation() const noexcept override
    {
        return internal_card_observation;
    }
    AuthorityJob pop()
    {
        AuthorityJob job = jobs[0U];
        for (std::size_t i = 1U; i < count; ++i) jobs[i - 1U] = jobs[i];
        --count;
        return job;
    }
    std::array<AuthorityJob, 16> jobs{};
    std::size_t count = 0U;
    bool reject = false;
    w::Optional<bool> lnb_state{};
    bool internal_card_observation = false;
};

Error send_full(ControlAuthority& authority, ControlServiceSession& session,
                Executor& executor)
{
    AuthorityJob rejected_cleanup;
    return authority.response_sent(session, true, executor, &rejected_cleanup);
}

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

w::ListResponse make_list(const ReceiverId* ids, std::size_t count)
{
    w::ListResponse list;
    for (std::size_t i = 0U; i < list.epoch.size(); ++i)
        list.epoch[i] = static_cast<std::uint8_t>(i + 1U);
    list.producer.product = text<32>("authority-test");
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
    list.receivers.value.count = static_cast<std::uint16_t>(count);
    for (std::size_t i = 0U; i < count; ++i) {
        auto& descriptor = list.receivers.value.items[i];
        descriptor.id = static_cast<std::uint16_t>(ids[i]);
        descriptor.systems.mask = 1U;
        descriptor.binding.present = true;
        descriptor.binding.value.function_id = 1U;
        descriptor.binding.value.local_id = static_cast<std::uint16_t>(i);
    }
    list.readers.present = true;
    return list;
}

w::Frame frame(std::uint16_t opcode, std::uint32_t id, ByteView payload)
{
    w::Frame result;
    result.header.kind = w::Kind::request;
    result.header.opcode = opcode;
    result.header.request_id = id;
    result.header.payload_length = static_cast<std::uint32_t>(payload.size);
    result.payload = payload;
    return result;
}

bool hello(ControlAuthority& authority, ControlServiceSession& session, Executor& executor)
{
    std::array<std::uint8_t, w::kHelloRequestSize> payload{};
    const w::HelloRequest request{{w::kVersionMajor, w::kVersionMinor},
                                  {w::kVersionMajor, w::kVersionMinor}, 0U, 0U};
    const auto size = w::encode_hello_request(request, {payload.data(), payload.size()});
    if (!size) return false;
    w::Context context;
    w::Snapshot snapshot;
    const auto result = authority.accept(session,
        frame(w::kOpcodeHello, 1U, {payload.data(), size.value()}), context, snapshot, 0U,
        executor);
    return result.action == w::InboundAction::respond &&
           send_full(authority, session, executor) == Error::OK;
}

AuthorityReply acquire(ControlAuthority& authority, ReceiverEngine& engine,
                       ControlServiceSession& session, Executor& executor,
                       std::uint32_t id, ReceiverId receiver)
{
    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    const auto size = w::encode_acquire_request({authority.server().epoch,
        static_cast<std::uint16_t>(receiver)}, {payload.data(), payload.size()});
    if (!size) return {};
    w::Context context;
    w::Snapshot snapshot;
    auto reply = authority.accept(session,
        frame(w::kOpcodeAcquire, id, {payload.data(), size.value()}), context, snapshot, 0U,
        executor);
    if (!reply.queued || executor.count != 1U) return reply;
    const AuthorityJob job = executor.pop();
    return authority.complete(ControlAuthority::execute_job_value(engine, job));
}

AuthorityCompletion execute_on_worker(ReceiverEngine& engine, const AuthorityJob& job)
{
    AuthorityCompletion completion;
    std::thread worker([&] {
        completion = ControlAuthority::execute_job_value(engine, job);
    });
    worker.join();
    return completion;
}

std::uint64_t acquired_lease(const AuthorityReply& reply)
{
    if (reply.frame.data == nullptr || reply.frame.size < w::kHeaderSize + w::kAcquireResponseSize)
        return 0U;
    const auto response = w::decode_acquire_response(
        {reply.frame.data + w::kHeaderSize, w::kAcquireResponseSize});
    return response ? response.value().lease_id : 0U;
}

AuthorityReply tune(ControlAuthority& authority, ControlServiceSession& session,
                    Executor& executor, const w::ServerHello& server,
                    const w::Context& context, const w::Snapshot& snapshot,
                    std::uint32_t request_id, std::uint64_t lease_id,
                    std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                    std::uint64_t accepted_at_ms)
{
    w::TuneRequest request;
    request.epoch = server.epoch;
    request.lease_id = lease_id;
    request.frequency_hz = frequency_hz;
    request.timeout_ms = timeout_ms;
    std::array<std::uint8_t, 128U> payload{};
    const auto size = w::encode_tune_request(request, {payload.data(), payload.size()});
    if (!size) return {};
    return authority.accept(session,
        frame(w::kOpcodeTune, request_id, {payload.data(), size.value()}),
        context, snapshot, accepted_at_ms, executor);
}

bool test_projection_lease_tune_and_gate()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> ids{{17U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> caps{{ReceiverCapability{17U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession first(authority);
    ControlServiceSession second(authority);
    Executor executor;
    CHECK(authority.open(first) == Error::OK && authority.open(second) == Error::OK);
    CHECK(first.client_id() != second.client_id() && hello(authority, first, executor));
    CHECK(hello(authority, second, executor));

    std::array<std::uint8_t, 128> list_payload{};
    auto list_size = w::encode_list_request({server.epoch},
        {list_payload.data(), list_payload.size()});
    CHECK(list_size);
    w::Context context;
    context.epoch = server.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    snapshot.sequence = 3U;
    snapshot.observed_at_ns = 44U;
    const auto listed = authority.accept(first,
        frame(w::kOpcodeList, 2U, {list_payload.data(), list_size.value()}), context,
        snapshot, 0U, executor);
    CHECK(listed.action == w::InboundAction::respond);
    const auto list_header = w::decode_header({listed.frame.data, w::kHeaderSize});
    CHECK(list_header && w::decode_list_response(
        {listed.frame.data + w::kHeaderSize, list_header.value().payload_length}));
    CHECK(send_full(authority, first, executor) == Error::OK);

    std::array<std::uint8_t, 128> status_payload{};
    w::StatusRequest status_request;
    status_request.epoch = server.epoch;
    status_request.scope.kind = w::ScopeKind::receiver;
    status_request.scope.id = 17U;
    auto status_size = w::encode_status_request(status_request,
        {status_payload.data(), status_payload.size()});
    CHECK(status_size);
    const auto status = authority.accept(first,
        frame(w::kOpcodeStatus, 3U, {status_payload.data(), status_size.value()}), context,
        snapshot, 0U, executor);
    CHECK(status.action == w::InboundAction::respond);
    const auto status_header = w::decode_header({status.frame.data, w::kHeaderSize});
    CHECK(status_header);
    const auto status_value = w::decode_status_response(
        {status.frame.data + w::kHeaderSize, status_header.value().payload_length});
    CHECK(status_value && status_value.value().receivers.present &&
          status_value.value().receivers.value.items[0U].descriptor.id == 17U &&
          !status_value.value().lnb_15v_enabled.present);
    CHECK(send_full(authority, first, executor) == Error::OK);

    const auto lease_reply = acquire(authority, engine, first, executor, 4U, 17U);
    CHECK(lease_reply.action == w::InboundAction::respond && acquired_lease(lease_reply) != 0U);
    const std::uint64_t lease = acquired_lease(lease_reply);
    CHECK(send_full(authority, first, executor) == Error::OK);
    const auto busy = acquire(authority, engine, second, executor, 2U, 17U);
    CHECK(busy.action == w::InboundAction::respond);
    const auto busy_header = w::decode_header({busy.frame.data, w::kHeaderSize});
    CHECK(busy_header && busy_header.value().kind == w::Kind::error_response);
    AuthorityJob rejected_cleanup;
    CHECK(authority.response_sent(second, false, executor, &rejected_cleanup) == Error::OK);
    CHECK(!second.connected() && executor.count == 1U);
    const AuthorityJob send_failure_cleanup = executor.pop();
    CHECK(send_failure_cleanup.kind == AuthorityJobKind::disconnect);
    CHECK(ControlAuthority::execute_job(engine, send_failure_cleanup) == Error::OK);

    std::array<std::uint8_t, 128> payload{};
    w::TuneRequest tune;
    tune.epoch = server.epoch;
    tune.lease_id = lease;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = 1000U;
    auto encoded = w::encode_tune_request(tune, {payload.data(), payload.size()});
    CHECK(encoded);
    auto accepted = authority.accept(first,
        frame(w::kOpcodeTune, 5U, {payload.data(), encoded.value()}), context, snapshot,
        clock.now, executor);
    CHECK(accepted.queued && executor.count == 1U && backend.hz_calls == 0);
    const AuthorityJob job = executor.pop();
    CHECK(job.kind == AuthorityJobKind::tune && job.key.client_id == first.client_id());
    CHECK(ControlAuthority::execute_job(engine, job) == Error::OK);
    CHECK(backend.hz_calls == 1 && engine.status().value().tune_metadata[0U].current.requested_hz ==
          tune.frequency_hz);
    const auto completed = authority.complete(job.key, Error::OK);
    const auto tune_header = w::decode_header({completed.frame.data, w::kHeaderSize});
    CHECK(tune_header && tune_header.value().kind == w::Kind::response);
    const auto result = w::decode_tune_result(
        {completed.frame.data + w::kHeaderSize, tune_header.value().payload_length});
    CHECK(result && result.value().requested_hz == tune.frequency_hz &&
          result.value().programmed_hz.present &&
          result.value().programmed_hz.value == tune.frequency_hz + 9U &&
          result.value().tune_revision == 1U &&
          result.value().verification == w::Verification::lock &&
          result.value().lock.state == w::MeasureState::valid &&
          result.value().lock.value);
    CHECK(send_full(authority, first, executor) == Error::OK);

    std::array<std::uint8_t, 128> release_payload{};
    const auto release_size = w::encode_release_request({server.epoch, lease},
        {release_payload.data(), release_payload.size()});
    CHECK(release_size);
    const auto releasing = authority.accept(first,
        frame(w::kOpcodeRelease, 6U, {release_payload.data(), release_size.value()}),
        context, snapshot, clock.now, executor);
    CHECK(releasing.queued && executor.count == 1U);
    const AuthorityJob release_job = executor.pop();
    CHECK(ControlAuthority::execute_job(engine, release_job) == Error::OK);
    const auto released = authority.complete(release_job.key, Error::OK);
    const auto release_header = w::decode_header({released.frame.data, w::kHeaderSize});
    CHECK(release_header && release_header.value().kind == w::Kind::response);
    const auto release_result = w::decode_release_response(
        {released.frame.data + w::kHeaderSize, release_header.value().payload_length});
    CHECK(release_result && release_result.value().released && release_result.value().quiesced);
    CHECK(send_full(authority, first, executor) == Error::OK);
    (void)authority.close(first);
    (void)authority.close(second);
    return true;
}

bool test_authority_release_preserves_legacy_engine_lease()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 2> ids{{17U, 18U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const auto legacy = engine.acquire(7U, 17U);
    CHECK(legacy);
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));
    const auto acquired = acquire(authority, engine, session, executor, 2U, 18U);
    const std::uint64_t authority_lease = acquired_lease(acquired);
    CHECK(acquired.action == w::InboundAction::respond && authority_lease != 0U);
    CHECK(send_full(authority, session, executor) == Error::OK);
    auto status = engine.status();
    CHECK(status && status.value().states[0U] == ReceiverState::leased &&
          status.value().states[1U] == ReceiverState::leased);

    std::array<std::uint8_t, 128> payload{};
    const auto size = w::encode_release_request({server.epoch, authority_lease},
        {payload.data(), payload.size()});
    CHECK(size);
    w::Context context;
    w::Snapshot snapshot;
    const auto reply = authority.accept(session,
        frame(w::kOpcodeRelease, 3U, {payload.data(), size.value()}), context, snapshot,
        0U, executor);
    CHECK(reply.queued && executor.count == 1U);
    const AuthorityJob release = executor.pop();
    CHECK(release.kind == AuthorityJobKind::release &&
          ControlAuthority::execute_job(engine, release) == Error::OK);
    const auto completed = authority.complete(release.key, Error::OK);
    const auto header = w::decode_header({completed.frame.data, w::kHeaderSize});
    CHECK(header && header.value().kind == w::Kind::response);
    const auto response = w::decode_release_response(
        {completed.frame.data + w::kHeaderSize, header.value().payload_length});
    CHECK(response && response.value().released && response.value().quiesced);
    status = engine.status();
    CHECK(status && status.value().states[0U] == ReceiverState::leased &&
          status.value().states[1U] == ReceiverState::free);

    CHECK(engine.release(7U, legacy.value().lease_id));
    status = engine.status();
    CHECK(status && status.value().states[0U] == ReceiverState::free &&
          status.value().states[1U] == ReceiverState::free);
    const AuthorityJob cleanup = authority.close(session);
    CHECK(cleanup.kind == AuthorityJobKind::disconnect &&
          ControlAuthority::execute_job(engine, cleanup) == Error::OK);
    return true;
}

bool test_rejected_queue_aborts_and_late_completion_drops()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> ids{{17U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> caps{{ReceiverCapability{17U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));
    const auto acquired = acquire(authority, engine, session, executor, 2U, 17U);
    const std::uint64_t lease = acquired_lease(acquired);
    CHECK(lease != 0U && send_full(authority, session, executor) == Error::OK);

    std::array<std::uint8_t, 128> payload{};
    w::TuneRequest tune;
    tune.epoch = server.epoch;
    tune.lease_id = lease;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = 1000U;
    auto encoded = w::encode_tune_request(tune, {payload.data(), payload.size()});
    CHECK(encoded);
    w::Context context;
    context.epoch = server.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    tune.system = w::System::isdb_s;
    encoded = w::encode_tune_request(tune, {payload.data(), payload.size()});
    CHECK(encoded);
    const auto unsupported = authority.accept(session,
        frame(w::kOpcodeTune, 3U, {payload.data(), encoded.value()}), context, snapshot,
        clock.now, executor);
    const auto unsupported_header = w::decode_header(
        {unsupported.frame.data, w::kHeaderSize});
    CHECK(unsupported_header && unsupported_header.value().kind == w::Kind::error_response &&
          backend.hz_calls == 0);
    CHECK(send_full(authority, session, executor) == Error::OK);
    tune.system = w::System::isdb_t;
    encoded = w::encode_tune_request(tune, {payload.data(), payload.size()});
    CHECK(encoded);
    executor.reject = true;
    const auto rejected = authority.accept(session,
        frame(w::kOpcodeTune, 4U, {payload.data(), encoded.value()}), context, snapshot,
        clock.now, executor);
    CHECK(rejected.action == w::InboundAction::respond && executor.count == 0U);
    const auto after_reject = engine.status();
    CHECK(backend.hz_calls == 0 && after_reject &&
          after_reject.value().operations[0U] == ReceiverTuneOperation::idle &&
          after_reject.value().tune_metadata[0U].revision == 1U);
    CHECK(send_full(authority, session, executor) == Error::OK);

    executor.reject = false;
    const auto queued = authority.accept(session,
        frame(w::kOpcodeTune, 5U, {payload.data(), encoded.value()}), context, snapshot,
        clock.now, executor);
    CHECK(queued.queued && executor.count == 1U);
    const AuthorityJob job = executor.pop();
    CHECK(job.tune.tune_revision == 2U);
    const AuthorityJob cleanup = authority.close(session);
    CHECK(cleanup.kind == AuthorityJobKind::disconnect &&
          cleanup.client_id == job.key.client_id);
    CHECK(ControlAuthority::execute_job(engine, job) == Error::NOT_FOUND);
    CHECK(backend.hz_calls == 0);
    CHECK(authority.complete(job.key, Error::OK).action == w::InboundAction::close);
    CHECK(ControlAuthority::execute_job(engine, cleanup) == Error::OK);
    return true;
}

bool test_running_tune_completion_after_close_and_cleanup()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> ids{{17U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> caps{{ReceiverCapability{17U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));
    const auto acquired = acquire(authority, engine, session, executor, 2U, 17U);
    const auto lease = acquired_lease(acquired);
    CHECK(lease != 0U && send_full(authority, session, executor) == Error::OK);
    std::array<std::uint8_t, 128> payload{};
    w::TuneRequest request;
    request.epoch = server.epoch;
    request.lease_id = lease;
    request.frequency_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto size = w::encode_tune_request(request, {payload.data(), payload.size()});
    CHECK(size);
    w::Context context;
    context.epoch = server.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto accepted = authority.accept(session,
        frame(w::kOpcodeTune, 3U, {payload.data(), size.value()}), context, snapshot,
        clock.now, executor);
    CHECK(accepted.queued && executor.count == 1U);
    const AuthorityJob tune_job = executor.pop();
    backend.block_tune = true;
    std::atomic<Error> tune_result{Error::INTERNAL};
    std::thread tune_worker([&] {
        tune_result.store(ControlAuthority::execute_job(engine, tune_job));
    });
    backend.wait_for_tune();
    const AuthorityJob cleanup = authority.close(session);
    CHECK(cleanup.kind == AuthorityJobKind::disconnect);
    std::atomic<Error> cleanup_result{Error::INTERNAL};
    std::thread cleanup_worker([&] {
        cleanup_result.store(ControlAuthority::execute_job(engine, cleanup));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    backend.unblock_tune();
    tune_worker.join();
    cleanup_worker.join();
    CHECK(tune_result.load() == Error::NOT_FOUND && cleanup_result.load() == Error::OK);
    CHECK(authority.complete(tune_job.key, Error::OK).action == w::InboundAction::close);
    CHECK(engine.status().value().states[0U] == ReceiverState::free);
    return true;
}

bool test_failed_release_is_not_quiesced()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> ids{{17U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 1> caps{{ReceiverCapability{17U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));
    const auto acquired = acquire(authority, engine, session, executor, 2U, 17U);
    const auto lease = acquired_lease(acquired);
    CHECK(lease != 0U && send_full(authority, session, executor) == Error::OK);
    std::array<std::uint8_t, 128> payload{};
    const auto payload_size = w::encode_release_request({server.epoch, lease},
        {payload.data(), payload.size()});
    CHECK(payload_size);
    w::Context context;
    context.epoch = server.epoch;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;
    const auto pending = authority.accept(session,
        frame(w::kOpcodeRelease, 3U, {payload.data(), payload_size.value()}), context,
        snapshot, clock.now, executor);
    CHECK(pending.queued && executor.count == 1U);
    const auto job = executor.pop();
    backend.close_error = Error::USB_IO;
    CHECK(ControlAuthority::execute_job(engine, job) == Error::USB_IO);
    const auto reply = authority.complete(job.key, Error::USB_IO);
    const auto header = w::decode_header({reply.frame.data, w::kHeaderSize});
    CHECK(header);
    const auto response = w::decode_release_response(
        {reply.frame.data + w::kHeaderSize, header.value().payload_length});
    CHECK(response && !response.value().released && !response.value().quiesced &&
          response.value().error.present && response.value().error.value.code ==
              static_cast<std::uint32_t>(Error::USB_IO));
    CHECK(send_full(authority, session, executor) == Error::OK);
    (void)authority.close(session);
    return true;
}

bool test_async_acquire_rollback_and_close_ownership()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 2> ids{{17U, 18U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 2> caps{{
        ReceiverCapability{17U, true, false, true},
        ReceiverCapability{18U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession first(authority);
    ControlServiceSession second(authority);
    Executor executor;
    CHECK(authority.open(first) == Error::OK && authority.open(second) == Error::OK);
    CHECK(hello(authority, first, executor) && hello(authority, second, executor));

    std::array<std::uint8_t, w::kAcquireRequestSize> payload{};
    auto size = w::encode_acquire_request({server.epoch, 17U},
        {payload.data(), payload.size()});
    CHECK(size);
    w::Context context;
    w::Snapshot snapshot;
    executor.reject = true;
    const auto rejected = authority.accept(first,
        frame(w::kOpcodeAcquire, 2U, {payload.data(), size.value()}), context, snapshot,
        0U, executor);
    CHECK(rejected.action == w::InboundAction::respond && !rejected.queued &&
          backend.open_calls == 0 && executor.count == 0U);
    CHECK(send_full(authority, first, executor) == Error::OK);
    executor.reject = false;

    const auto pending = authority.accept(first,
        frame(w::kOpcodeAcquire, 3U, {payload.data(), size.value()}), context, snapshot,
        0U, executor);
    CHECK(pending.queued && executor.count == 1U && backend.open_calls == 0);
    const std::thread::id poll_thread = std::this_thread::get_id();
    const AuthorityJob acquire_job = executor.pop();
    CHECK(acquire_job.kind == AuthorityJobKind::acquire &&
          acquire_job.key.client_id == first.client_id() && acquire_job.receiver == 17U);
    const AuthorityCompletion acquired = execute_on_worker(engine, acquire_job);
    CHECK(acquired.error == Error::OK && acquired.lease_id != 0U &&
          acquired.kind == AuthorityJobKind::acquire && backend.open_calls == 1 &&
          backend.open_thread != poll_thread);
    const auto scalar_acquire = authority.complete(acquire_job.key, Error::OK);
    CHECK(scalar_acquire.action == w::InboundAction::ignored &&
          !scalar_acquire.cleanup_required && !scalar_acquire.queued &&
          first.connected() && backend.close_calls == 0 && executor.count == 0U);
    AuthorityCompletion wrong_kind = acquired;
    wrong_kind.kind = AuthorityJobKind::tune;
    CHECK(authority.complete(wrong_kind).action == w::InboundAction::close);
    AuthorityCompletion malformed = acquired;
    malformed.receiver = 18U;
    const auto rollback = authority.complete(malformed);
    CHECK(rollback.cleanup_required && rollback.queued &&
          rollback.cleanup.kind == AuthorityJobKind::rollback_release &&
          rollback.cleanup.lease_id == acquired.lease_id &&
          rollback.cleanup.receiver == 17U && backend.close_calls == 0);
    const auto scalar_rollback = authority.complete(rollback.cleanup.key, Error::OK);
    CHECK(scalar_rollback.action == w::InboundAction::ignored &&
          !scalar_rollback.cleanup_required && !scalar_rollback.queued &&
          first.connected() && backend.close_calls == 0 && executor.count == 0U);
    CHECK(authority.complete(acquired).action == w::InboundAction::close);

    executor.reject = true;
    CHECK(!executor.submit(rollback.cleanup));
    executor.reject = false;
    CHECK(executor.submit(rollback.cleanup) && executor.count == 1U);
    const AuthorityJob rollback_job = executor.pop();
    backend.block_close = true;
    AuthorityCompletion rolled_back;
    std::thread worker([&] {
        rolled_back = ControlAuthority::execute_job_value(engine, rollback_job);
    });
    backend.wait_for_close();
    CHECK(backend.close_calls == 1);
    size = w::encode_acquire_request({server.epoch, 18U},
        {payload.data(), payload.size()});
    CHECK(size);
    const auto other_pending = authority.accept(second,
        frame(w::kOpcodeAcquire, 2U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    CHECK(other_pending.queued && executor.count == 1U);
    const AuthorityJob other_job = executor.pop();
    const AuthorityCompletion other_failure{other_job.key, other_job.kind,
                                             Error::USB_IO, 0U, other_job.receiver};
    const auto other_completed = authority.complete(other_failure);
    const auto other_header = w::decode_header(
        {other_completed.frame.data, w::kHeaderSize});
    CHECK(other_completed.action == w::InboundAction::respond && other_header &&
          other_header.value().kind == w::Kind::error_response &&
          send_full(authority, second, executor) == Error::OK);
    const auto other_list = w::encode_list_request({server.epoch},
        {payload.data(), payload.size()});
    CHECK(other_list);
    const auto listed = authority.accept(second,
        frame(w::kOpcodeList, 3U, {payload.data(), other_list.value()}), context,
        snapshot, 0U, executor);
    CHECK(listed.action == w::InboundAction::respond && send_full(authority, second,
          executor) == Error::OK);
    backend.unblock_close();
    worker.join();
    CHECK(rolled_back.error == Error::OK &&
          rolled_back.kind == AuthorityJobKind::rollback_release &&
          backend.close_thread != poll_thread);
    const auto rollback_done = authority.complete(rolled_back);
    const auto rollback_header = w::decode_header(
        {rollback_done.frame.data, w::kHeaderSize});
    CHECK(rollback_done.action == w::InboundAction::respond && rollback_header &&
          rollback_header.value().kind == w::Kind::error_response && backend.close_calls == 1);
    CHECK(send_full(authority, first, executor) == Error::OK);

    size = w::encode_acquire_request({server.epoch, 17U},
        {payload.data(), payload.size()});
    CHECK(size);
    const auto typed_pending = authority.accept(first,
        frame(w::kOpcodeAcquire, 4U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    CHECK(typed_pending.queued && executor.count == 1U);
    const AuthorityJob typed_job = executor.pop();
    const auto typed_reply = authority.complete(execute_on_worker(engine, typed_job));
    CHECK(typed_reply.action == w::InboundAction::respond &&
          acquired_lease(typed_reply) != 0U);
    CHECK(send_full(authority, first, executor) == Error::OK);
    const AuthorityJob typed_cleanup = authority.close(first);
    CHECK(typed_cleanup.kind == AuthorityJobKind::disconnect &&
          !typed_cleanup.deferred);
    const auto typed_cleanup_completion = ControlAuthority::execute_job_value(
        engine, typed_cleanup);
    CHECK(typed_cleanup_completion.error == Error::OK &&
          authority.complete(typed_cleanup_completion).action == w::InboundAction::close &&
          backend.close_calls == 2 &&
          engine.status().value().states[0U] == ReceiverState::free);

    size = w::encode_acquire_request({server.epoch, 18U},
        {payload.data(), payload.size()});
    CHECK(size);
    const auto late_pending = authority.accept(second,
        frame(w::kOpcodeAcquire, 4U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    CHECK(late_pending.queued && backend.open_calls == 2);
    const AuthorityJob late_job = executor.pop();
    const auto late_completion = execute_on_worker(engine, late_job);
    CHECK(late_completion.error == Error::OK && backend.open_calls == 3 &&
          backend.open_thread != poll_thread);
    const AuthorityJob deferred = authority.close(second);
    CHECK(deferred.kind == AuthorityJobKind::disconnect && deferred.deferred &&
          backend.close_calls == 2);
    const auto late_reply = authority.complete(late_completion);
    CHECK(late_reply.action == w::InboundAction::close && late_reply.cleanup_required &&
          late_reply.cleanup.kind == AuthorityJobKind::disconnect &&
          !late_reply.cleanup.deferred && backend.close_calls == 2);
    executor.reject = true;
    CHECK(!executor.submit(late_reply.cleanup));
    CHECK(authority.cleanup_submitted(late_reply.cleanup, false) == Error::OK);
    const AuthorityCompletion premature_disconnect{late_reply.cleanup.key,
        AuthorityJobKind::disconnect, Error::OK, 0U, late_reply.cleanup.receiver};
    CHECK(authority.complete(premature_disconnect).action == w::InboundAction::close);
    executor.reject = false;
    CHECK(executor.submit(late_reply.cleanup));
    CHECK(authority.cleanup_submitted(late_reply.cleanup, true) == Error::OK);
    CHECK(backend.close_calls == 2);
    const AuthorityCompletion disconnected = execute_on_worker(engine, executor.pop());
    CHECK(disconnected.kind == AuthorityJobKind::disconnect &&
          disconnected.error == Error::OK && backend.close_calls == 3 &&
          backend.close_thread != poll_thread);
    CHECK(authority.complete(disconnected).action == w::InboundAction::close);
    (void)authority.close(first);
    return true;
}

bool test_other_receiver_cleanup_while_tune_blocks()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 2> ids{{17U, 18U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 2> caps{{
        ReceiverCapability{17U, true, false, true},
        ReceiverCapability{18U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(caps.data(), caps.size()));
    const auto first = engine.acquire(101U, 17U);
    const auto second = engine.acquire(202U, 18U);
    CHECK(first && second);
    HzTuneRequest request;
    request.lease_id = first.value().lease_id;
    request.requested_hz = 473142857U;
    request.timeout_ms = 1000U;
    const auto ticket = engine.try_admit_tune(101U, request, clock.now);
    CHECK(ticket);
    AuthorityJob tune;
    tune.kind = AuthorityJobKind::tune;
    tune.tune = ticket.value();
    AuthorityJob cleanup;
    cleanup.kind = AuthorityJobKind::release;
    cleanup.client_id = 202U;
    cleanup.lease_id = second.value().lease_id;
    backend.block_tune = true;
    std::atomic<Error> tune_result{Error::INTERNAL};
    std::atomic<Error> cleanup_result{Error::INTERNAL};
    std::thread tune_worker([&] {
        tune_result.store(ControlAuthority::execute_job(engine, tune));
    });
    backend.wait_for_tune();
    std::thread cleanup_worker([&] {
        cleanup_result.store(ControlAuthority::execute_job(engine, cleanup));
    });
    for (unsigned i = 0U; i < 100U && cleanup_result.load() == Error::INTERNAL; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool independent = cleanup_result.load() == Error::OK;
    backend.unblock_tune();
    cleanup_worker.join();
    tune_worker.join();
    CHECK(independent && tune_result.load() == Error::OK);
    return true;
}

bool test_client_id_domain_and_authority_exhaustion()
{
    CHECK(!is_legacy_client_id(0U));
    CHECK(is_legacy_client_id(1U));
    CHECK(is_legacy_client_id(kLegacyClientIdMax));
    CHECK(!is_legacy_client_id(kAuthorityClientIdFirst));

    ClientIdAllocator legacy_ids(kLegacyClientIdMax - 1U, kLegacyClientIdMax);
    CHECK(legacy_ids.next().value() == kLegacyClientIdMax - 1U);
    CHECK(legacy_ids.next().value() == kLegacyClientIdMax);
    CHECK(!legacy_ids.next());
    ClientIdAllocator authority_ids(std::numeric_limits<std::uint64_t>::max() - 1U,
                                    std::numeric_limits<std::uint64_t>::max());
    CHECK(authority_ids.next().value() == std::numeric_limits<std::uint64_t>::max() - 1U);
    CHECK(authority_ids.next().value() == std::numeric_limits<std::uint64_t>::max());
    CHECK(!authority_ids.next());
    return true;
}

bool test_card_dispatch_rejects_unadvertised_reader()
{
    const ReceiverId ids[] = {2U};
    auto list = make_list(ids, 1U);
    list.readers.present = true;
    list.readers.value.count = 0U;
    w::ServerHello server{list.epoch, list.producer, w::kFeatureCardService};
    Backend backend;
    Tokens tokens;
    Clock clock;
    const ReceiverId receiver_ids[] = {2U};
    ReceiverEngine engine(backend, receiver_ids, 1U, tokens, clock);
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK);
    std::array<std::uint8_t, w::kHelloRequestSize> hello_payload{};
    const w::HelloRequest hello_request{{w::kVersionMajor, w::kVersionMinor},
                                        {w::kVersionMajor, w::kVersionMinor},
                                        w::kFeatureCardService, 0U};
    const auto hello_size = w::encode_hello_request(
        hello_request, {hello_payload.data(), hello_payload.size()});
    CHECK(hello_size);
    w::Context context;
    w::Snapshot snapshot;
    auto reply = authority.accept(session,
        frame(w::kOpcodeHello, 1U, {hello_payload.data(), hello_size.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.action == w::InboundAction::respond);
    CHECK(send_full(authority, session, executor) == Error::OK);

    w::CardStatusRequest request{};
    request.key.epoch = list.epoch;
    request.key.reader_id = 7U;
    std::array<std::uint8_t, 64U> payload{};
    const auto request_size = w::encode_card_status_request(request,
        {payload.data(), payload.size()});
    CHECK(request_size);
    reply = authority.accept(session,
        frame(w::kOpcodeCardStatus, 2U, {payload.data(), request_size.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.action == w::InboundAction::respond && !reply.queued);
    CHECK(executor.count == 0U);
    CHECK(reply.frame.size >= w::kHeaderSize);
    const auto header = w::decode_header({reply.frame.data, w::kHeaderSize});
    CHECK(header && header.value().opcode == w::kOpcodeCardStatus);
    const auto error = w::decode_error_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(error && error.value().error.code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(authority.close(session).kind == AuthorityJobKind::disconnect);
    return true;
}

bool test_lnb_power_dispatch_and_status()
{
    const ReceiverId ids[] = {2U};
    auto list = make_list(ids, 1U);
    auto& capabilities = list.receivers.value.items[0U].capabilities;
    capabilities.controls |= w::kControlBitLnb;
    capabilities.lnb.values.count = 2U;
    capabilities.lnb.values.items[0U] = 0U;
    capabilities.lnb.values.items[1U] = 15U;
    w::ServerHello server{list.epoch, list.producer, 0U};
    Backend backend;
    Tokens tokens;
    Clock clock;
    ReceiverEngine engine(backend, ids, 1U, tokens, clock);
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));

    w::LnbPowerRequest request{list.epoch, true};
    std::array<std::uint8_t, 64U> payload{};
    const auto encoded = w::encode_lnb_power_request(
        request, {payload.data(), payload.size()});
    CHECK(encoded);
    w::Context context;
    context.epoch = list.epoch;
    w::Snapshot snapshot;
    auto reply = authority.accept(session,
        frame(w::kOpcodeLnbPower, 2U, {payload.data(), encoded.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.queued && executor.count == 1U);
    const AuthorityJob job = executor.pop();
    CHECK(job.kind == AuthorityJobKind::lnb_power && job.client_id == session.client_id() &&
          job.lnb_enabled);
    AuthorityCompletion completion;
    completion.key = job.key;
    completion.kind = job.kind;
    completion.lnb_enabled = job.lnb_enabled;
    reply = authority.complete(completion);
    CHECK(reply.action == w::InboundAction::respond);
    const auto response = w::decode_lnb_power_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(response && response.value().epoch == list.epoch && response.value().enabled);
    CHECK(send_full(authority, session, executor) == Error::OK);

    executor.lnb_state = w::Optional<bool>{true, true};
    w::StatusRequest status_request{list.epoch, w::Scope{}};
    std::array<std::uint8_t, 64U> status_payload{};
    const auto status_size = w::encode_status_request(
        status_request, {status_payload.data(), status_payload.size()});
    CHECK(status_size);
    reply = authority.accept(session,
        frame(w::kOpcodeStatus, 3U, {status_payload.data(), status_size.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.action == w::InboundAction::respond);
    const auto status = w::decode_status_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(status && status.value().lnb_15v_enabled.present &&
          status.value().lnb_15v_enabled.value);
    CHECK(send_full(authority, session, executor) == Error::OK);

    executor.lnb_state.present = false;
    reply = authority.accept(session,
        frame(w::kOpcodeStatus, 4U, {status_payload.data(), status_size.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.action == w::InboundAction::respond);
    const auto unknown = w::decode_status_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(unknown && !unknown.value().lnb_15v_enabled.present);
    CHECK(send_full(authority, session, executor) == Error::OK);

    w::LnbPowerRequest off{list.epoch, false};
    const auto off_size = w::encode_lnb_power_request(
        off, {payload.data(), payload.size()});
    CHECK(off_size);
    reply = authority.accept(session,
        frame(w::kOpcodeLnbPower, 5U, {payload.data(), off_size.value()}),
        context, snapshot, 0U, executor);
    CHECK(reply.queued && executor.count == 1U);
    const AuthorityJob failed_job = executor.pop();
    completion = AuthorityCompletion{};
    completion.key = failed_job.key;
    completion.kind = failed_job.kind;
    completion.error = Error::USB_IO;
    reply = authority.complete(completion);
    CHECK(reply.action == w::InboundAction::respond);
    const auto failure = w::decode_error_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(failure && failure.value().error.code == static_cast<std::uint32_t>(Error::USB_IO));
    CHECK(send_full(authority, session, executor) == Error::OK);
    CHECK(authority.close(session).kind == AuthorityJobKind::disconnect);

    auto unsupported_list = make_list(ids, 1U);
    w::ServerHello unsupported_server{unsupported_list.epoch,
                                       unsupported_list.producer, 0U};
    Backend unsupported_backend;
    Tokens unsupported_tokens;
    Clock unsupported_clock;
    ReceiverEngine unsupported_engine(unsupported_backend, ids, 1U,
                                      unsupported_tokens, unsupported_clock);
    ControlAuthority unsupported_authority(unsupported_server, unsupported_list,
                                           unsupported_engine);
    ControlServiceSession unsupported_session(unsupported_authority);
    Executor unsupported_executor;
    CHECK(unsupported_authority.open(unsupported_session) == Error::OK &&
          hello(unsupported_authority, unsupported_session, unsupported_executor));
    reply = unsupported_authority.accept(unsupported_session,
        frame(w::kOpcodeLnbPower, 2U, {payload.data(), encoded.value()}),
        context, snapshot, 0U, unsupported_executor);
    CHECK(reply.action == w::InboundAction::respond && unsupported_executor.count == 0U);
    const auto unsupported = w::decode_error_response(
        {reply.frame.data + w::kHeaderSize, reply.frame.size - w::kHeaderSize});
    CHECK(unsupported && unsupported.value().error.code ==
          static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(unsupported_authority.close(unsupported_session).kind ==
          AuthorityJobKind::disconnect);
    return true;
}

bool test_internal_card_status_queue_and_completion()
{
    const ReceiverId ids[] = {2U};
    auto list = make_list(ids, 1U);
    w::ServerHello server{list.epoch, list.producer, 0U};
    Backend backend;
    Tokens tokens;
    Clock clock;
    ReceiverEngine engine(backend, ids, 1U, tokens, clock);
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    executor.internal_card_observation = true;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));

    w::StatusRequest request{list.epoch, w::Scope{}};
    std::array<std::uint8_t, 64U> payload{};
    const auto size = w::encode_status_request(request, {payload.data(), payload.size()});
    CHECK(size);
    w::Context context;
    context.epoch = list.epoch;
    w::Snapshot snapshot;

    executor.reject = true;
    auto reply = authority.accept(session,
        frame(w::kOpcodeStatus, 2U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    CHECK(reply.action == w::InboundAction::respond && !reply.queued);
    const auto refused_header = w::decode_header({reply.frame.data, w::kHeaderSize});
    CHECK(refused_header && refused_header.value().kind == w::Kind::error_response &&
          refused_header.value().request_id == 2U);
    const auto refused = w::decode_error_response(
        {reply.frame.data + w::kHeaderSize, refused_header.value().payload_length});
    CHECK(refused && refused.value().error.code ==
          static_cast<std::uint32_t>(Error::RESOURCE_LIMIT));
    CHECK(send_full(authority, session, executor) == Error::OK);

    executor.reject = false;
    reply = authority.accept(session,
        frame(w::kOpcodeStatus, 3U, {payload.data(), size.value()}), context,
        snapshot, 0U, executor);
    CHECK(reply.queued && executor.count == 1U);
    const AuthorityJob job = executor.pop();
    CHECK(job.kind == AuthorityJobKind::internal_card_status &&
          job.key.client_id == session.client_id() && job.key.request_id == 3U);
    AuthorityCompletion completion;
    completion.key = job.key;
    completion.kind = job.kind;
    completion.internal_card_inserted.present = true;
    completion.internal_card_inserted.value = true;
    reply = authority.complete(completion);
    CHECK(reply.action == w::InboundAction::respond && reply.frame.size >= w::kHeaderSize);
    const auto header = w::decode_header({reply.frame.data, w::kHeaderSize});
    CHECK(header && header.value().kind == w::Kind::response &&
          header.value().opcode == w::kOpcodeStatus && header.value().request_id == 3U);
    const auto status = w::decode_status_response(
        {reply.frame.data + w::kHeaderSize, header.value().payload_length});
    CHECK(status && status.value().card_inserted.present &&
          status.value().card_inserted.value);
    CHECK(send_full(authority, session, executor) == Error::OK);
    CHECK(authority.close(session).kind == AuthorityJobKind::disconnect);
    return true;
}

bool test_deferred_admission_fairness_and_session_order()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 4U> ids{{17U, 18U, 19U, 20U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 4U> capabilities{{
        {17U, true, false, true}, {18U, true, false, true},
        {19U, true, false, true}, {20U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession first(authority);
    ControlServiceSession second(authority);
    ControlServiceSession third(authority);
    Executor executor;
    CHECK(authority.open(first) == Error::OK && hello(authority, first, executor));
    CHECK(authority.open(second) == Error::OK && hello(authority, second, executor));
    CHECK(authority.open(third) == Error::OK && hello(authority, third, executor));
    const std::uint64_t first_lease = acquired_lease(
        acquire(authority, engine, first, executor, 2U, 17U));
    const std::uint64_t second_lease = acquired_lease(
        acquire(authority, engine, second, executor, 2U, 18U));
    const std::uint64_t third_lease = acquired_lease(
        acquire(authority, engine, third, executor, 2U, 19U));
    CHECK(first_lease != 0U && second_lease != 0U && third_lease != 0U);
    CHECK(send_full(authority, first, executor) == Error::OK &&
          send_full(authority, second, executor) == Error::OK &&
          send_full(authority, third, executor) == Error::OK);

    tokens.arm();
    Result<AcquireResult> gated_acquire = Result<AcquireResult>::failure(Error::INTERNAL);
    std::thread holder([&] { gated_acquire = engine.acquire(99U, 20U); });
    struct GateGuard final {
        Tokens& tokens;
        std::thread& holder;
        ~GateGuard() { tokens.open(); if (holder.joinable()) holder.join(); }
    } gate_guard{tokens, holder};
    tokens.wait_entered();
    w::Context context;
    w::Snapshot snapshot;
    context.state = w::ContextState::ready;
    CHECK(tune(authority, first, executor, server, context, snapshot,
               3U, first_lease, 473142857U, 1000U, clock.now).queued);
    CHECK(tune(authority, second, executor, server, context, snapshot,
               3U, second_lease, 473142858U, 1000U, clock.now).queued);
    CHECK(tune(authority, third, executor, server, context, snapshot,
               3U, third_lease, 473142859U, 1000U, clock.now).queued);
    CHECK(executor.count == 0U);

    for (ControlServiceSession* expected : {&first, &second, &third}) {
        const auto poll = authority.retry_pending_admission(clock.now, executor);
        CHECK(poll.attempted && !poll.reply_ready && poll.session == expected &&
              executor.count == 0U);
    }
    tokens.open();
    holder.join();
    CHECK(gated_acquire);
    int expected_hz_calls = 0;
    for (ControlServiceSession* expected : {&first, &second, &third}) {
        const auto poll = authority.retry_pending_admission(clock.now, executor);
        CHECK(poll.attempted && !poll.reply_ready && poll.session == expected &&
              poll.reply.queued && executor.count == 1U);
        const AuthorityJob job = executor.pop();
        CHECK(job.kind == AuthorityJobKind::tune && job.key.client_id == expected->client_id() &&
              job.key.request_id == 3U);
        const auto completion = ControlAuthority::execute_job_value(engine, job);
        CHECK(completion.error == Error::OK &&
              backend.hz_calls.load() == ++expected_hz_calls);
        const auto reply = authority.complete(completion);
        const auto header = w::decode_header({reply.frame.data, w::kHeaderSize});
        CHECK(header && header.value().kind == w::Kind::response &&
              header.value().request_id == 3U);
        CHECK(send_full(authority, *expected, executor) == Error::OK);
    }

    CHECK(tune(authority, first, executor, server, context, snapshot,
               4U, first_lease, 473142860U, 1000U, clock.now).queued &&
          executor.count == 1U);
    const AuthorityJob next = executor.pop();
    CHECK(next.key.request_id == 4U && next.tune.tune_revision == 2U);
    const auto next_completion = ControlAuthority::execute_job_value(engine, next);
    CHECK(next_completion.error == Error::OK && backend.hz_calls == 4);
    const auto next_reply = authority.complete(next_completion);
    const auto next_header = w::decode_header({next_reply.frame.data, w::kHeaderSize});
    CHECK(next_header && next_header.value().request_id == 4U);
    CHECK(send_full(authority, first, executor) == Error::OK);

    const auto busy_ticket = engine.try_admit_tune(first.client_id(),
        HzTuneRequest{first_lease, ReceiverSystem::terrestrial, 473142861U}, clock.now);
    CHECK(busy_ticket);
    const auto receiver_busy = tune(authority, first, executor, server, context, snapshot,
        5U, first_lease, 473142862U, 1000U, clock.now);
    CHECK(receiver_busy.action == w::InboundAction::respond && executor.count == 0U);
    const auto receiver_error = w::decode_error_response(
        {receiver_busy.frame.data + w::kHeaderSize,
         receiver_busy.frame.size - w::kHeaderSize});
    CHECK(receiver_error && receiver_error.value().error.code ==
          static_cast<std::uint32_t>(Error::BUSY));
    CHECK(!authority.retry_pending_admission(clock.now, executor).attempted);
    CHECK(engine.abort_unstarted_tune(busy_ticket.value(), Error::BUSY));
    CHECK(send_full(authority, first, executor) == Error::OK);

    backend.tune_error = Error::BUSY;
    CHECK(tune(authority, first, executor, server, context, snapshot,
               6U, first_lease, 473142863U, 1000U, clock.now).queued &&
          executor.count == 1U);
    const auto backend_busy = ControlAuthority::execute_job_value(engine, executor.pop());
    CHECK(backend_busy.error == Error::BUSY && backend.hz_calls == 5);
    const auto backend_reply = authority.complete(backend_busy);
    const auto backend_error = w::decode_error_response(
        {backend_reply.frame.data + w::kHeaderSize,
         backend_reply.frame.size - w::kHeaderSize});
    CHECK(backend_error && backend_error.value().error.code ==
          static_cast<std::uint32_t>(Error::BUSY));
    CHECK(!authority.retry_pending_admission(clock.now, executor).attempted &&
          executor.count == 0U);
    CHECK(send_full(authority, first, executor) == Error::OK);

    for (ControlServiceSession* session : {&first, &second, &third}) {
        const AuthorityJob cleanup = authority.close(*session);
        CHECK(ControlAuthority::execute_job(engine, cleanup) == Error::OK);
    }
    CHECK(engine.disconnect_client(99U));
    return true;
}

bool test_deferred_admission_deadline_and_observed_close()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 2U> ids{{17U, 18U}};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    const std::array<ReceiverCapability, 2U> capabilities{{
        {17U, true, false, true}, {18U, true, false, true}}};
    CHECK(engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
    const auto list = make_list(ids.data(), ids.size());
    w::ServerHello server;
    server.epoch = list.epoch;
    server.producer = list.producer;
    ControlAuthority authority(server, list, engine);
    ControlServiceSession session(authority);
    Executor executor;
    CHECK(authority.open(session) == Error::OK && hello(authority, session, executor));
    const std::uint64_t lease = acquired_lease(
        acquire(authority, engine, session, executor, 2U, 17U));
    CHECK(lease != 0U && send_full(authority, session, executor) == Error::OK);
    w::Context context;
    context.state = w::ContextState::ready;
    w::Snapshot snapshot;

    tokens.arm();
    Result<AcquireResult> gated_acquire = Result<AcquireResult>::failure(Error::INTERNAL);
    std::thread holder([&] { gated_acquire = engine.acquire(99U, 18U); });
    struct GateGuard final {
        Tokens& tokens;
        std::thread& holder;
        ~GateGuard() { tokens.open(); if (holder.joinable()) holder.join(); }
    } gate_guard{tokens, holder};
    tokens.wait_entered();
    CHECK(tune(authority, session, executor, server, context, snapshot,
               3U, lease, 473142857U, 20U, 100U).queued);
    const auto expired = authority.retry_pending_admission(120U, executor);
    CHECK(expired.attempted && expired.reply_ready && expired.session == &session &&
          executor.count == 0U && backend.hz_calls == 0);
    const auto expired_error = w::decode_error_response(
        {expired.reply.frame.data + w::kHeaderSize,
         expired.reply.frame.size - w::kHeaderSize});
    CHECK(expired_error && expired_error.value().error.code ==
          static_cast<std::uint32_t>(Error::TIMEOUT));
    CHECK(send_full(authority, session, executor) == Error::OK);

    CHECK(tune(authority, session, executor, server, context, snapshot,
               4U, lease, 473142858U, 1000U, 200U).queued);
    // A short independent client deadline has elapsed; its close is now
    // observed by the server before the next admission poll.
    tokens.open();
    holder.join();
    CHECK(gated_acquire);
    const AuthorityJob cleanup = authority.close(session);
    CHECK(cleanup.kind == AuthorityJobKind::disconnect && !session.connected());
    const auto after_timeout = engine.status();
    CHECK(after_timeout && after_timeout.value().tune_metadata[0U].revision == 0U &&
          !after_timeout.value().tune_metadata[0U].current_valid);
    CHECK(!authority.retry_pending_admission(201U, executor).attempted &&
          executor.count == 0U && backend.hz_calls == 0);
    CHECK(ControlAuthority::execute_job(engine, cleanup) == Error::OK);
    const auto after_close = engine.status();
    CHECK(after_close && after_close.value().tune_metadata[0U].revision == 0U &&
          after_close.value().states[0U] == ReceiverState::free);
    CHECK(engine.disconnect_client(99U));
    return true;
}

}  // namespace

int main()
{
    if (!test_projection_lease_tune_and_gate()) return 1;
    if (!test_authority_release_preserves_legacy_engine_lease()) return 1;
    if (!test_rejected_queue_aborts_and_late_completion_drops()) return 1;
    if (!test_running_tune_completion_after_close_and_cleanup()) return 1;
    if (!test_failed_release_is_not_quiesced()) return 1;
    if (!test_async_acquire_rollback_and_close_ownership()) return 1;
    if (!test_other_receiver_cleanup_while_tune_blocks()) return 1;
    if (!test_client_id_domain_and_authority_exhaustion()) return 1;
    if (!test_card_dispatch_rejects_unadvertised_reader()) return 1;
    if (!test_lnb_power_dispatch_and_status()) return 1;
    if (!test_internal_card_status_queue_and_completion()) return 1;
    if (!test_deferred_admission_fairness_and_session_order()) return 1;
    if (!test_deferred_admission_deadline_and_observed_close()) return 1;
    std::puts("control authority tests passed");
    return 0;
}
