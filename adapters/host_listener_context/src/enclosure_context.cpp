// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/host_listener_context/enclosure_context.h"

#include "detail/listener_io.h"
#include "dtv/userland/host_daemon/card_service.h"
#include "dtv/userland/host_daemon/host_authority_bridge.h"
#if !defined(_WIN32)
#include "px4/posix_ipc.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <thread>

#if defined(_WIN32)
#include "../../host_daemon/src/windows_pthread_compat.h"
#else
#include <pthread.h>
#endif

namespace dtv::userland::host_listener_context {
namespace {

using px4::userland::CardService;
using px4::userland::TunerService;
using px4::userland::ipc::posix::ControlWorkerLanes;
using px4::userland::ipc::posix::HostAuthorityBridge;
using detail::ListenerIo;

constexpr std::size_t kControlCapacity = 32U;
constexpr std::size_t kDataCapacity = 32U;
constexpr std::size_t kDataBufferBytes =
    wire::kMaxTsPacketBytes + wire::kMaxFrameSize;
static_assert(kDataCapacity * kDataBufferBytes == 67104000U);
std::atomic<std::size_t> ts_buffer_current{0U};
std::atomic<std::size_t> ts_buffer_high_water{0U};

host_runtime::EndpointConfig endpoint_config(const ContextConfig& config,
                                             std::string_view name) noexcept
{
    return host_runtime::EndpointConfig{config.runtime_directory, config.product,
                                        config.instance, config.access, -1, name};
}

void sleep_briefly() noexcept
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

struct EnclosureContext::Impl final {
    struct ControlSlot final : wire::FrameHandler {
        struct Impl* owner = nullptr;
        std::size_t index = 0U;
        detail::ConnectionToken connection{};
        std::unique_ptr<std::uint8_t[]> payload{};
        std::unique_ptr<wire::FrameReceiver> receiver{};
        ByteView output{nullptr, 0U};
        std::size_t output_offset = 0U;
        std::array<std::uint64_t, 32U> leases{};
        std::size_t lease_count = 0U;
        std::uint8_t bridge_slot = 0U;
        std::uint64_t pending_lease = 0U;
        bool pending_lease_change = false;
        bool pending_lease_remove = false;
        bool used = false;
        bool waiting_response = false;
        bool close_after_write = false;
        bool response_pending = false;
        Result<void> on_frame(const wire::Frame& frame) noexcept override
        {
            return owner->on_control_frame(index, frame);
        }
    };

    struct DataSlot final : wire::FrameHandler {
        struct Impl* owner = nullptr;
        std::size_t index = 0U;
        detail::ConnectionToken connection{};
        px4::userland::ipc::posix::HostAuthorityDataConnection bridge{};
        std::unique_ptr<std::uint8_t[]> payload{};
        std::unique_ptr<wire::FrameReceiver> receiver{};
        std::unique_ptr<std::uint8_t[]> read_buffer{};
        std::unique_ptr<std::uint8_t[]> frame_buffer{};
        ByteView output{nullptr, 0U};
        std::uint8_t control_slot = 0U;
        std::uint32_t request_id = 0U;
        std::size_t output_offset = 0U;
        std::size_t output_size = 0U;
        bool used = false;
        bool attach_pending = false;
        bool attach_prepared = false;
        bool attached = false;
        bool stream_drained = false;
        bool output_is_attach = false;
        bool output_is_stream_end = false;
        bool close_after_write = false;
        bool buffer_counted = false;
        Result<void> on_frame(const wire::Frame& frame) noexcept override
        {
            return owner->on_data_frame(index, frame);
        }
    };

    Impl(std::unique_ptr<host_runtime::EnclosureFamily> family_value,
         std::unique_ptr<host_runtime::TunerNonceSource> nonce_value,
         const ContextConfig& config_value) noexcept
        : family(std::move(family_value)), nonce_source(std::move(nonce_value)),
          config(config_value)
    {
    }

    ~Impl() noexcept
    {
        if (thread_started) {
            request_close();
            while (!cleanup_complete.load(std::memory_order_acquire)) sleep_briefly();
            join_cleanup();
        } else if (workers) {
            (void)workers->stop_and_join();
        }
        if (condition_ready) (void)pthread_cond_destroy(&cleanup_condition);
        if (mutex_ready) (void)pthread_mutex_destroy(&cleanup_mutex);
    }

    static void* cleanup_entry(void* value) noexcept
    {
        static_cast<Impl*>(value)->cleanup_loop();
        return nullptr;
    }

    Result<void> initialize() noexcept
    {
        if (!family || !nonce_source || config.runtime_directory.empty() ||
            config.product.empty() || config.instance.empty() ||
            config.stream_engine_epoch == 0U || config.stream_arm_budget_ms == 0U)
            return Result<void>::failure(Error::INVALID_ARGUMENT);

        card.reset(new (std::nothrow) CardService(
            family->card_backend(), family->card_protocol()));
        tuner.reset(new (std::nothrow) TunerService(
            family->tuner_backend(), *nonce_source, family->time(), nullptr,
            nullptr, family->stream_control()));
        if (!card || !tuner) return Result<void>::failure(Error::RESOURCE_LIMIT);
        const auto strict = tuner->configure_strict_capabilities();
        if (!strict) return Result<void>::failure(strict.error());
        const auto stream = tuner->configure_stream_contract(
            px4::userland::detail::StreamContract::v1,
            config.stream_engine_epoch);
        if (!stream) return Result<void>::failure(stream.error());

        auto created_workers = ControlWorkerLanes::create(
            *card, *tuner, config.runtime_directory.c_str());
        if (!created_workers)
            return Result<void>::failure(created_workers.error());
        workers = std::move(created_workers.value());
        bridge.reset(new (std::nothrow) HostAuthorityBridge(
            *tuner, *workers, config.server, config.canonical_list,
            config.validate_tune));
        if (!bridge) return Result<void>::failure(Error::RESOURCE_LIMIT);
        const Error stream_configured = bridge->configure_stream_contract(
            config.stream_engine_epoch, config.stream_arm_budget_ms);
        if (stream_configured != Error::OK)
            return Result<void>::failure(stream_configured);
        io = detail::create_listener_io();
        if (!io) return Result<void>::failure(Error::RESOURCE_LIMIT);

        if (pthread_mutex_init(&cleanup_mutex, nullptr) != 0)
            return Result<void>::failure(Error::INTERNAL);
        mutex_ready = true;
        if (pthread_cond_init(&cleanup_condition, nullptr) != 0)
            return Result<void>::failure(Error::INTERNAL);
        condition_ready = true;
        if (pthread_create(&cleanup_thread, nullptr, &Impl::cleanup_entry, this) != 0)
            return Result<void>::failure(Error::INTERNAL);
        thread_started = true;
        return Result<void>::success();
    }

    void request_close() noexcept
    {
        if (mutex_ready) (void)pthread_mutex_lock(&cleanup_mutex);
        close_requested = true;
        if (condition_ready) (void)pthread_cond_broadcast(&cleanup_condition);
        if (mutex_ready) (void)pthread_mutex_unlock(&cleanup_mutex);
    }

    void cleanup_loop() noexcept
    {
        (void)pthread_mutex_lock(&cleanup_mutex);
        while (!close_requested || poll_active)
            (void)pthread_cond_wait(&cleanup_condition, &cleanup_mutex);
        (void)pthread_mutex_unlock(&cleanup_mutex);

        cleanup_error.store(perform_cleanup(), std::memory_order_release);
        cleanup_complete.store(true, std::memory_order_release);
    }

    Error perform_cleanup() noexcept
    {
        Error first_error = Error::OK;
        const auto retain_error = [&first_error](Error error) noexcept {
            if (first_error == Error::OK && error != Error::OK)
                first_error = error;
        };
        bool owner_quiesced = true;
        if (published) {
            retain_error(io->unpublish_pair(listeners));
            published = false;
        }
        if (io) {
            for (ControlSlot& slot : controls) {
                if (!slot.used) continue;
                if (slot.response_pending)
                    (void)bridge->response_sent(slot.bridge_slot, false);
                (void)io->close_connection(slot.connection);
                (void)bridge->close_session(slot.bridge_slot);
                slot.used = false;
            }
            for (DataSlot& slot : data_slots) {
                if (!slot.used) continue;
                (void)io->close_connection(slot.connection);
                (void)bridge->close_data_session(slot.bridge);
                slot.used = false;
                if (slot.buffer_counted) {
                    ts_buffer_current.fetch_sub(kDataBufferBytes,
                                                std::memory_order_relaxed);
                    slot.buffer_counted = false;
                }
                slot.read_buffer.reset();
                slot.frame_buffer.reset();
            }
        }
        const Error shutdown_started = bridge->begin_shutdown();
        retain_error(shutdown_started);
        if (shutdown_started != Error::OK && !bridge->stopped())
            owner_quiesced = false;
        std::array<detail::ReadyEvent, detail::kListenerIoEventCapacity> events{};
        while (!bridge->stopped()) {
            std::size_t count = 0U;
            (void)io->poll(workers->wake_fd(), Timeout{100U}, events.data(),
                           events.size(), count);
            const Error advanced = bridge->poll();
            if (advanced != Error::OK && !bridge->stopped()) sleep_briefly();
        }
        const Error bridge_shutdown = bridge->shutdown_result();
        retain_error(bridge_shutdown);
        if (bridge_shutdown != Error::OK) owner_quiesced = false;
        const auto joined = workers->join();
        if (!joined) {
            retain_error(joined.error());
            owner_quiesced = false;
        }
        if (namespace_reserved) {
            retain_error(io->release_namespace());
            namespace_reserved = false;
        }
        if (owner_quiesced) {
            const auto shutdown = family->shutdown();
            if (!shutdown) {
                retain_error(shutdown.error());
                owner_quiesced = false;
            }
        }
        if (owner_quiesced) {
            bridge.reset();
            workers.reset();
            tuner.reset();
            card.reset();
            io.reset();
            nonce_source.reset();
            family.reset();
        }
        cleanup_quiesced.store(owner_quiesced, std::memory_order_release);
        return first_error;
    }

    Result<void> on_control_frame(std::size_t index,
                                  const wire::Frame& frame) noexcept
    {
        ControlSlot& slot = controls[index];
        if (!slot.used || slot.response_pending || slot.waiting_response)
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        const std::uint64_t now_ms = family->time().monotonic_ms();
        if (snapshot_sequence == (std::numeric_limits<std::uint64_t>::max)() ||
            now_ms > (std::numeric_limits<std::uint64_t>::max)() / 1000000U)
            return Result<void>::failure(Error::RESOURCE_LIMIT);
        ++snapshot_sequence;
        const wire::Snapshot snapshot{snapshot_sequence, now_ms * 1000000U};
        const auto reply = bridge->accept(slot.bridge_slot, frame, config.context,
                                           snapshot, now_ms);
        if (reply.action == wire::InboundAction::respond && reply.frame.size != 0U) {
            return queue_control_reply(slot, reply);
        }
        if (reply.action == wire::InboundAction::dispatch || reply.queued) {
            slot.waiting_response = true;
            return refresh_control_interest(slot);
        }
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    }

    Result<void> queue_control_reply(ControlSlot& slot,
                                     const px4::userland::detail::AuthorityReply& reply) noexcept
    {
        if (reply.frame.data == nullptr || reply.frame.size < wire::kHeaderSize)
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        slot.output = reply.frame;
        slot.output_offset = 0U;
        slot.response_pending = true;
        slot.waiting_response = false;
        slot.close_after_write = reply.close_after_reply;
        slot.pending_lease_change = false;
        slot.pending_lease_remove = false;
        const auto header = wire::decode_header(
            ByteView{reply.frame.data, wire::kHeaderSize});
        if (header && header.value().kind == wire::Kind::response) {
            const ByteView payload{reply.frame.data + wire::kHeaderSize,
                                   reply.frame.size - wire::kHeaderSize};
            if (header.value().opcode == wire::kOpcodeAcquire) {
                const auto acquired = wire::decode_acquire_response(payload);
                if (acquired) {
                    slot.pending_lease = acquired.value().lease_id;
                    slot.pending_lease_change = true;
                }
            } else if (header.value().opcode == wire::kOpcodeRelease) {
                const auto released = wire::decode_release_response(payload);
                if (released && released.value().released) {
                    slot.pending_lease = released.value().lease_id;
                    slot.pending_lease_remove = true;
                }
            }
        }
        return refresh_control_interest(slot);
    }

    Result<void> refresh_control_interest(ControlSlot& slot) noexcept
    {
        std::uint8_t flags = 0U;
        if (!slot.waiting_response && !slot.response_pending) flags |= detail::ready_read;
        if (slot.response_pending) flags |= detail::ready_write;
        const Error error = io->set_connection_interest(slot.connection, flags);
        return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
    }

    void apply_lease_change(ControlSlot& slot) noexcept
    {
        if (slot.pending_lease_change) {
            for (std::size_t i = 0U; i < slot.lease_count; ++i)
                if (slot.leases[i] == slot.pending_lease) {
                    slot.pending_lease_change = false;
                    slot.pending_lease = 0U;
                    return;
                }
            if (slot.lease_count < slot.leases.size())
                slot.leases[slot.lease_count++] = slot.pending_lease;
        } else if (slot.pending_lease_remove) {
            for (std::size_t i = 0U; i < slot.lease_count; ++i) {
                if (slot.leases[i] != slot.pending_lease) continue;
                slot.leases[i] = slot.leases[--slot.lease_count];
                slot.leases[slot.lease_count] = 0U;
                break;
            }
        }
        slot.pending_lease_change = false;
        slot.pending_lease_remove = false;
        slot.pending_lease = 0U;
    }

    void close_control(std::size_t index) noexcept
    {
        ControlSlot& slot = controls[index];
        if (!slot.used) return;
        if (slot.response_pending)
            (void)bridge->response_sent(slot.bridge_slot, false);
        (void)io->close_connection(slot.connection);
        (void)bridge->close_session(slot.bridge_slot);
        slot = ControlSlot{};
    }

    void accept_control(detail::ListenerToken listener) noexcept
    {
        std::size_t index = kControlCapacity;
        for (std::size_t i = 0U; i < controls.size(); ++i)
            if (!controls[i].used) { index = i; break; }
        const detail::AcceptResult accepted = io->accept(listener);
        if (accepted.status != detail::IoStatus::progress) return;
        if (index == kControlCapacity) {
            (void)io->close_connection(accepted.connection);
            return;
        }
        auto bridge_slot = bridge->open_session();
        if (!bridge_slot) {
            (void)io->close_connection(accepted.connection);
            return;
        }
        ControlSlot& slot = controls[index];
        slot.payload.reset(new (std::nothrow) std::uint8_t[wire::kMaxControlPayload]);
        if (!slot.payload) {
            (void)bridge->close_session(bridge_slot.value());
            (void)io->close_connection(accepted.connection);
            return;
        }
        slot.owner = this;
        slot.index = index;
        slot.connection = accepted.connection;
        slot.bridge_slot = bridge_slot.value();
        slot.receiver.reset(new (std::nothrow) wire::FrameReceiver(
            MutableByteView{slot.payload.get(), wire::kMaxControlPayload}));
        if (!slot.receiver) {
            (void)bridge->close_session(slot.bridge_slot);
            (void)io->close_connection(slot.connection);
            slot = ControlSlot{};
            return;
        }
        slot.used = true;
        if (!refresh_control_interest(slot)) close_control(index);
    }

    void read_control(std::size_t index) noexcept
    {
        ControlSlot& slot = controls[index];
        std::array<std::uint8_t, 4096U> input{};
        const auto read = io->read_some(slot.connection,
            MutableByteView{input.data(), input.size()});
        if (read.status == detail::IoStatus::would_block) return;
        if (read.status == detail::IoStatus::eof) {
            (void)slot.receiver->finish();
            close_control(index);
            return;
        }
        if (read.status != detail::IoStatus::progress || read.transferred == 0U ||
            read.transferred > input.size()) {
            close_control(index);
            return;
        }
        const auto fed = slot.receiver->feed(
            ByteView{input.data(), read.transferred}, slot);
        if (!fed) close_control(index);
    }

    void write_control(std::size_t index) noexcept
    {
        ControlSlot& slot = controls[index];
        if (!slot.used || !slot.response_pending || slot.output.data == nullptr) return;
        const std::size_t remaining = slot.output.size - slot.output_offset;
        const auto written = io->write_some(slot.connection,
            ByteView{slot.output.data + slot.output_offset, remaining});
        if (written.status == detail::IoStatus::would_block) return;
        if (written.status != detail::IoStatus::progress || written.transferred == 0U ||
            written.transferred > remaining) {
            close_control(index);
            return;
        }
        slot.output_offset += written.transferred;
        if (slot.output_offset != slot.output.size) return;
        const bool close_after = slot.close_after_write;
        const Error completed = bridge->response_sent(slot.bridge_slot, true);
        apply_lease_change(slot);
        slot.output = ByteView{nullptr, 0U};
        slot.output_offset = 0U;
        slot.response_pending = false;
        slot.close_after_write = false;
        if (completed != Error::OK || close_after) {
            close_control(index);
            return;
        }
        if (!refresh_control_interest(slot)) close_control(index);
    }

    void complete_control_responses() noexcept
    {
        for (std::size_t i = 0U; i < controls.size(); ++i) {
            ControlSlot& slot = controls[i];
            if (!slot.used || !slot.waiting_response) continue;
            const auto reply = bridge->take_response(slot.bridge_slot);
            if (reply.action == wire::InboundAction::respond && reply.frame.size != 0U) {
                if (!queue_control_reply(slot, reply)) close_control(i);
            } else if (bridge->counts().fatal_unresolved != 0U) {
                close_control(i);
            }
        }
    }

    void process_events(const detail::ReadyEvent* events, std::size_t count) noexcept
    {
        for (std::size_t i = 0U; i < count; ++i) {
            const detail::ReadyEvent& event = events[i];
            if (event.kind == detail::ReadyKind::listener) {
                if (event.listener_kind == detail::ListenerKind::control)
                    accept_control(event.listener);
                else
                    accept_data(event.listener);
            } else if (event.kind == detail::ReadyKind::connection) {
                if ((event.flags & detail::ready_error) != 0U) {
                    const std::size_t control = find_control(event.connection);
                    if (control != kControlCapacity) close_control(control);
                    else {
                        const std::size_t data = find_data(event.connection);
                        if (data != kDataCapacity) close_data(data);
                    }
                    continue;
                }
                const std::size_t control = find_control(event.connection);
                if (control != kControlCapacity) {
                    if ((event.flags & (detail::ready_read | detail::ready_hangup)) != 0U)
                        read_control(control);
                    if (control < controls.size() && controls[control].used &&
                        (event.flags & detail::ready_write) != 0U)
                        write_control(control);
                } else {
                    const std::size_t data = find_data(event.connection);
                    if (data != kDataCapacity) handle_data_event(data, event.flags);
                }
            }
        }
    }

    std::size_t find_lease_owner(std::uint64_t lease_id) const noexcept
    {
        for (std::size_t i = 0U; i < controls.size(); ++i) {
            const ControlSlot& slot = controls[i];
            if (!slot.used) continue;
            for (std::size_t j = 0U; j < slot.lease_count; ++j)
                if (slot.leases[j] == lease_id) return i;
        }
        return kControlCapacity;
    }

    void accept_data(detail::ListenerToken listener) noexcept
    {
        std::size_t index = kDataCapacity;
        for (std::size_t i = 0U; i < data_slots.size(); ++i)
            if (!data_slots[i].used) { index = i; break; }
        const auto accepted = io->accept(listener);
        if (accepted.status != detail::IoStatus::progress) return;
        if (index == kDataCapacity) {
            (void)io->close_connection(accepted.connection);
            return;
        }
        auto bridge_connection = bridge->open_data_session();
        if (!bridge_connection) {
            (void)io->close_connection(accepted.connection);
            return;
        }
        DataSlot& slot = data_slots[index];
        slot.owner = this;
        slot.index = index;
        slot.connection = accepted.connection;
        slot.bridge = bridge_connection.value();
        slot.payload.reset(new (std::nothrow) std::uint8_t[wire::kMaxControlPayload]);
        slot.read_buffer.reset(new (std::nothrow) std::uint8_t[wire::kMaxTsPacketBytes]);
        slot.frame_buffer.reset(new (std::nothrow) std::uint8_t[wire::kMaxFrameSize]);
        if (!slot.payload || !slot.read_buffer || !slot.frame_buffer) {
            (void)bridge->close_data_session(slot.bridge);
            (void)io->close_connection(slot.connection);
            slot = DataSlot{};
            return;
        }
        slot.receiver.reset(new (std::nothrow) wire::FrameReceiver(
            MutableByteView{slot.payload.get(), wire::kMaxControlPayload}));
        if (!slot.receiver) {
            (void)bridge->close_data_session(slot.bridge);
            (void)io->close_connection(slot.connection);
            slot = DataSlot{};
            return;
        }
        slot.buffer_counted = true;
        const std::size_t current = ts_buffer_current.fetch_add(
            kDataBufferBytes, std::memory_order_relaxed) + kDataBufferBytes;
        std::size_t high = ts_buffer_high_water.load(std::memory_order_relaxed);
        while (current > high && !ts_buffer_high_water.compare_exchange_weak(
                   high, current, std::memory_order_relaxed)) {}
        slot.used = true;
        if (io->set_connection_interest(slot.connection, detail::ready_read) != Error::OK)
            close_data(index);
    }

    Result<void> on_data_frame(std::size_t index, const wire::Frame& frame) noexcept
    {
        DataSlot& slot = data_slots[index];
        if (!slot.used || slot.attach_pending || slot.attached ||
            slot.output_size != 0U || frame.header.kind != wire::Kind::request ||
            frame.header.opcode != wire::kOpcodeAttachStream)
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        const auto request = wire::decode_attach_request(frame.payload);
        if (!request) return Result<void>::failure(request.error());
        const std::size_t control = find_lease_owner(request.value().lease_id);
        if (control == kControlCapacity) {
            const auto staged = stage_attach_error(slot, frame.header.request_id,
                                                    Error::NOT_FOUND);
            if (!staged) return staged;
            return refresh_data_interest(slot);
        }
        slot.control_slot = controls[control].bridge_slot;
        slot.request_id = frame.header.request_id;
        const Error prepared = bridge->prepare_data_attach(
            slot.bridge, slot.control_slot, request.value(), family->time().monotonic_ms());
        if (prepared != Error::OK) {
            const auto staged = stage_attach_error(slot, frame.header.request_id,
                                                    prepared);
            if (!staged) return staged;
            return refresh_data_interest(slot);
        }
        slot.attach_pending = true;
        return refresh_data_interest(slot);
    }

    Result<void> stage_attach_error(DataSlot& slot, std::uint32_t request_id,
                                    Error error) noexcept
    {
        const auto encoded = wire::encode_error_frame(
            wire::kOpcodeAttachStream, request_id, config.server.epoch,
            static_cast<std::uint32_t>(error), "attach", wire::Scope{},
            MutableByteView{slot.frame_buffer.get(), wire::kMaxFrameSize});
        if (!encoded) return Result<void>::failure(encoded.error());
        slot.output = ByteView{slot.frame_buffer.get(), encoded.value()};
        slot.output_size = encoded.value();
        slot.output_offset = 0U;
        slot.close_after_write = true;
        slot.output_is_attach = false;
        return Result<void>::success();
    }

    void complete_data_attaches() noexcept
    {
        for (std::size_t i = 0U; i < data_slots.size(); ++i) {
            DataSlot& slot = data_slots[i];
            if (!slot.used || !slot.attach_pending) continue;
            const auto attached = bridge->take_data_attach_result(slot.bridge);
            if (!attached.ready) continue;
            slot.attach_pending = false;
            if (attached.error != Error::OK) {
                const auto staged = stage_attach_error(slot, slot.request_id,
                                                       attached.error);
                if (!staged) {
                    close_data(i);
                    continue;
                }
                slot.attach_prepared = false;
            } else {
                const auto encoded = wire::encode_record_frame(
                    wire::Kind::response, wire::kOpcodeAttachStream,
                    slot.request_id, attached.response, &wire::encode_attach_response,
                    MutableByteView{slot.frame_buffer.get(), wire::kMaxFrameSize});
                if (!encoded) {
                    close_data(i);
                    continue;
                }
                slot.output = ByteView{slot.frame_buffer.get(), encoded.value()};
                slot.output_size = encoded.value();
                slot.output_offset = 0U;
                slot.output_is_attach = true;
                slot.attach_prepared = true;
            }
            if (!refresh_data_interest(slot)) close_data(i);
        }
    }

    Result<void> refresh_data_interest(DataSlot& slot) noexcept
    {
        std::uint8_t flags = 0U;
        if (!slot.attach_pending && !slot.output_size) flags |= detail::ready_read;
        if (slot.output_size != 0U) flags |= detail::ready_write;
        const Error error = io->set_connection_interest(slot.connection, flags);
        return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
    }

    void close_data(std::size_t index) noexcept
    {
        DataSlot& slot = data_slots[index];
        if (!slot.used) return;
        if (slot.output_is_attach && slot.attach_prepared)
            (void)bridge->data_attach_response_sent(slot.bridge, false);
        if (slot.output_is_stream_end)
            (void)bridge->data_stream_end_sent(slot.bridge, false);
        (void)io->close_connection(slot.connection);
        (void)bridge->close_data_session(slot.bridge);
        if (slot.buffer_counted) {
            ts_buffer_current.fetch_sub(kDataBufferBytes, std::memory_order_relaxed);
            slot.buffer_counted = false;
        }
        slot = DataSlot{};
    }

    void read_data_control(std::size_t index) noexcept
    {
        DataSlot& slot = data_slots[index];
        std::array<std::uint8_t, 4096U> input{};
        const auto read = io->read_some(slot.connection,
            MutableByteView{input.data(), input.size()});
        if (read.status == detail::IoStatus::would_block) return;
        if (read.status == detail::IoStatus::eof) {
            (void)slot.receiver->finish();
            close_data(index);
            return;
        }
        if (read.status != detail::IoStatus::progress || read.transferred == 0U ||
            read.transferred > input.size()) {
            close_data(index);
            return;
        }
        const auto fed = slot.receiver->feed(
            ByteView{input.data(), read.transferred}, slot);
        if (!fed) close_data(index);
    }

    void write_data(std::size_t index) noexcept
    {
        DataSlot& slot = data_slots[index];
        if (!slot.used || slot.output_size == 0U) return;
        const std::size_t remaining = slot.output_size - slot.output_offset;
        const auto written = io->write_some(slot.connection,
            ByteView{slot.output.data + slot.output_offset, remaining});
        if (written.status == detail::IoStatus::would_block) return;
        if (written.status != detail::IoStatus::progress || written.transferred == 0U ||
            written.transferred > remaining) {
            close_data(index);
            return;
        }
        slot.output_offset += written.transferred;
        if (slot.output_offset != slot.output_size) return;
        const bool attach = slot.output_is_attach;
        const bool stream_end = slot.output_is_stream_end;
        const bool close_after = slot.close_after_write || stream_end;
        slot.output = ByteView{nullptr, 0U};
        slot.output_offset = 0U;
        slot.output_size = 0U;
        slot.output_is_attach = false;
        slot.output_is_stream_end = false;
        slot.close_after_write = false;
        if (attach) {
            if (bridge->data_attach_response_sent(slot.bridge, true) != Error::OK) {
                close_data(index);
                return;
            }
            slot.attach_prepared = false;
            slot.attached = true;
        }
        if (stream_end) {
            const Error ended = bridge->data_stream_end_sent(slot.bridge, true);
            if (ended != Error::OK) {
                close_data(index);
                return;
            }
        }
        if (close_after) {
            close_data(index);
            return;
        }
        if (!refresh_data_interest(slot)) close_data(index);
    }

    void queue_data_frame(DataSlot& slot, std::size_t size, bool stream_end) noexcept
    {
        slot.output = ByteView{slot.frame_buffer.get(), size};
        slot.output_size = size;
        slot.output_offset = 0U;
        slot.output_is_stream_end = stream_end;
    }

    void pump_data() noexcept
    {
        complete_data_attaches();
        for (std::size_t i = 0U; i < data_slots.size(); ++i) {
            DataSlot& slot = data_slots[i];
            if (!slot.used || !slot.attached || slot.output_size != 0U) continue;
            if (!slot.stream_drained) {
                const auto read = bridge->read_data(slot.bridge,
                    MutableByteView{slot.read_buffer.get(), wire::kMaxTsPacketBytes},
                    Timeout{0U});
                if (read.error != Error::OK) {
                    close_data(i);
                    continue;
                }
                if (!read.eof && !read.timed_out) {
                    const auto encoded = wire::encode_record_frame(
                        wire::Kind::event, wire::kOpcodeTsDataEvent, 0U, read.data,
                        &wire::encode_ts_data,
                        MutableByteView{slot.frame_buffer.get(), wire::kMaxFrameSize});
                    if (!encoded) {
                        close_data(i);
                        continue;
                    }
                    queue_data_frame(slot, encoded.value(), false);
                    if (!refresh_data_interest(slot)) close_data(i);
                    continue;
                }
                slot.stream_drained = read.eof;
            }
            if (slot.stream_drained) {
                const auto end = bridge->take_data_stream_end(slot.bridge);
                if (!end.ready) continue;
                const auto encoded = wire::encode_record_frame(
                    wire::Kind::event, wire::kOpcodeStreamEndEvent, 0U,
                    end.event, &wire::encode_stream_end_event,
                    MutableByteView{slot.frame_buffer.get(), wire::kMaxFrameSize});
                if (!encoded) {
                    close_data(i);
                    continue;
                }
                queue_data_frame(slot, encoded.value(), true);
                if (!refresh_data_interest(slot)) close_data(i);
            }
        }
    }

    void handle_data_event(std::size_t index, std::uint8_t flags) noexcept
    {
        if ((flags & (detail::ready_read | detail::ready_hangup)) != 0U)
            read_data_control(index);
        if (index < data_slots.size() && data_slots[index].used &&
            (flags & detail::ready_write) != 0U) write_data(index);
    }

    std::size_t find_control(detail::ConnectionToken connection) const noexcept
    {
        for (std::size_t i = 0U; i < controls.size(); ++i)
            if (controls[i].used && controls[i].connection.value == connection.value)
                return i;
        return kControlCapacity;
    }

    std::size_t find_data(detail::ConnectionToken connection) const noexcept
    {
        for (std::size_t i = 0U; i < data_slots.size(); ++i)
            if (data_slots[i].used && data_slots[i].connection.value == connection.value)
                return i;
        return kDataCapacity;
    }

    Result<void> run_poll(Timeout timeout) noexcept
    {
        std::array<detail::ReadyEvent, detail::kListenerIoEventCapacity> events{};
        std::size_t count = 0U;
        const Timeout bounded{std::min<std::uint32_t>(timeout.milliseconds, 100U)};
        const Error waited = io->poll(workers->wake_fd(), bounded, events.data(),
                                      events.size(), count);
        if (waited != Error::OK) return Result<void>::failure(waited);
        if (count > events.size()) return Result<void>::failure(Error::PROTOCOL_ERROR);
        process_events(events.data(), count);
        const Error advanced = bridge->poll();
        complete_control_responses();
        pump_data();
        return advanced == Error::OK ? Result<void>::success()
                                     : Result<void>::failure(advanced);
    }

    Result<void> poll_once(Timeout timeout) noexcept
    {
        (void)pthread_mutex_lock(&cleanup_mutex);
        if (close_requested || !published || poll_active) {
            const Error error = poll_active ? Error::BUSY : Error::NOT_READY;
            (void)pthread_mutex_unlock(&cleanup_mutex);
            return Result<void>::failure(error);
        }
        poll_active = true;
        (void)pthread_mutex_unlock(&cleanup_mutex);
        const auto result = run_poll(timeout);
        (void)pthread_mutex_lock(&cleanup_mutex);
        poll_active = false;
        (void)pthread_cond_broadcast(&cleanup_condition);
        (void)pthread_mutex_unlock(&cleanup_mutex);
        return result;
    }

    void join_cleanup() noexcept
    {
        if (!thread_started) return;
        (void)pthread_mutex_lock(&cleanup_mutex);
        if (thread_joined) {
            (void)pthread_mutex_unlock(&cleanup_mutex);
            return;
        }
        if (thread_joining) {
            while (!thread_joined)
                (void)pthread_cond_wait(&cleanup_condition, &cleanup_mutex);
            (void)pthread_mutex_unlock(&cleanup_mutex);
            return;
        }
        thread_joining = true;
        (void)pthread_mutex_unlock(&cleanup_mutex);
        (void)pthread_join(cleanup_thread, nullptr);
        (void)pthread_mutex_lock(&cleanup_mutex);
        thread_joined = true;
        thread_joining = false;
        (void)pthread_cond_broadcast(&cleanup_condition);
        (void)pthread_mutex_unlock(&cleanup_mutex);
    }

    std::unique_ptr<host_runtime::EnclosureFamily> family;
    std::unique_ptr<host_runtime::TunerNonceSource> nonce_source;
    ContextConfig config;
    std::unique_ptr<CardService> card{};
    std::unique_ptr<TunerService> tuner{};
    std::unique_ptr<ControlWorkerLanes> workers{};
    std::unique_ptr<HostAuthorityBridge> bridge{};
    std::unique_ptr<ListenerIo> io{};
    std::array<ControlSlot, kControlCapacity> controls{};
    std::array<DataSlot, kDataCapacity> data_slots{};
    detail::ListenerPair listeners{};
    pthread_mutex_t cleanup_mutex{};
    pthread_cond_t cleanup_condition{};
    pthread_t cleanup_thread{};
    std::atomic<bool> cleanup_complete{false};
    std::atomic<Error> cleanup_error{Error::OK};
    std::atomic<bool> cleanup_quiesced{false};
    bool mutex_ready = false;
    bool condition_ready = false;
    bool thread_started = false;
    bool thread_joined = false;
    bool close_requested = false;
    bool poll_active = false;
    bool thread_joining = false;
    bool namespace_reserved = false;
    bool published = false;
    std::uint64_t snapshot_sequence = 0U;
};

EnclosureContext::EnclosureContext(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

EnclosureContext::~EnclosureContext() noexcept
{
    if (!impl_) return;
    impl_->request_close();
    while (!impl_->cleanup_complete.load(std::memory_order_acquire)) sleep_briefly();
    if (!impl_->cleanup_quiesced.load(std::memory_order_acquire)) {
        impl_->join_cleanup();
        (void)impl_.release();
        return;
    }
    impl_->join_cleanup();
}

Result<std::unique_ptr<EnclosureContext>> EnclosureContext::assemble(
    std::unique_ptr<host_runtime::EnclosureFamily> family,
    std::unique_ptr<host_runtime::TunerNonceSource> nonce_source,
    const ContextConfig& config) noexcept
{
    ContextConfig effective_config = config;
#if !defined(_WIN32)
    if (effective_config.runtime_directory.empty()) {
        const auto runtime_root = px4::userland::ipc::posix::select_publish_runtime_root(
            effective_config.product.c_str(), effective_config.instance.c_str());
        if (!runtime_root)
            return Result<std::unique_ptr<EnclosureContext>>::failure(runtime_root.error());
        effective_config.runtime_directory = runtime_root.value();
    }
#endif
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl(
        std::move(family), std::move(nonce_source), effective_config));
    if (!impl) return Result<std::unique_ptr<EnclosureContext>>::failure(
        Error::RESOURCE_LIMIT);
    const auto initialized = impl->initialize();
    if (!initialized) {
        if (impl->workers) (void)impl->workers->stop_and_join();
        impl->bridge.reset();
        impl->workers.reset();
        impl->tuner.reset();
        impl->card.reset();
        if (impl->family) (void)impl->family->shutdown();
        return Result<std::unique_ptr<EnclosureContext>>::failure(initialized.error());
    }
    std::unique_ptr<EnclosureContext> context(new (std::nothrow) EnclosureContext(
        std::move(impl)));
    if (!context) return Result<std::unique_ptr<EnclosureContext>>::failure(
        Error::RESOURCE_LIMIT);
    return Result<std::unique_ptr<EnclosureContext>>::success(std::move(context));
}

Result<void> EnclosureContext::reserve_namespace() noexcept
{
    return reserve_namespace(nullptr);
}

Result<void> EnclosureContext::reserve_namespace(ListenerDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr) *diagnostic = ListenerDiagnostic{};
    if (!impl_) return Result<void>::failure(Error::NOT_READY);
    (void)pthread_mutex_lock(&impl_->cleanup_mutex);
    if (impl_->close_requested || impl_->namespace_reserved || impl_->poll_active) {
        (void)pthread_mutex_unlock(&impl_->cleanup_mutex);
        return Result<void>::failure(Error::BUSY);
    }
    const auto control = endpoint_config(impl_->config, "control.sock");
    const auto data = endpoint_config(impl_->config, "stream.sock");
    const Error error = impl_->io->reserve_pair(control, data, diagnostic);
    if (error == Error::OK) impl_->namespace_reserved = true;
    (void)pthread_mutex_unlock(&impl_->cleanup_mutex);
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}

Result<void> EnclosureContext::publish() noexcept
{
    if (!impl_) return Result<void>::failure(Error::NOT_READY);
    (void)pthread_mutex_lock(&impl_->cleanup_mutex);
    if (impl_->close_requested || !impl_->namespace_reserved || impl_->published ||
        impl_->poll_active) {
        (void)pthread_mutex_unlock(&impl_->cleanup_mutex);
        return Result<void>::failure(Error::NOT_READY);
    }
    const Error error = impl_->io->publish_pair(impl_->listeners);
    if (error == Error::OK) impl_->published = true;
    (void)pthread_mutex_unlock(&impl_->cleanup_mutex);
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}

Result<void> EnclosureContext::poll_once(Timeout timeout) noexcept
{
    if (!impl_) return Result<void>::failure(Error::NOT_READY);
    return impl_->poll_once(timeout);
}

Result<CloseState> EnclosureContext::close(Timeout timeout) noexcept
{
    if (!impl_) return Result<CloseState>::failure(Error::NOT_READY);
    impl_->request_close();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout.milliseconds);
    while (!impl_->cleanup_complete.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return Result<CloseState>::success(CloseState::pending);
        sleep_briefly();
    }
    const Error error = impl_->cleanup_error.load(std::memory_order_acquire);
    impl_->join_cleanup();
    if (error != Error::OK) return Result<CloseState>::failure(error);
    return Result<CloseState>::success(CloseState::quiesced);
}

}  // namespace dtv::userland::host_listener_context

std::size_t host_listener_context_ts_buffer_current_for_test() noexcept
{
    return dtv::userland::host_listener_context::ts_buffer_current.load(
        std::memory_order_relaxed);
}

std::size_t host_listener_context_ts_buffer_high_water_for_test() noexcept
{
    return dtv::userland::host_listener_context::ts_buffer_high_water.load(
        std::memory_order_relaxed);
}
