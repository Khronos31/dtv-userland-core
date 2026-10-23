// SPDX-License-Identifier: GPL-2.0-only
#include "host_listener_context_fake_io.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>

using namespace dtv::userland;
using namespace dtv::userland::host_listener_context::detail;

struct FakeListenerIoControl::Impl final {
    struct Connection final {
        ListenerKind listener = ListenerKind::control;
        std::vector<std::uint8_t> input;
        std::vector<std::uint8_t> output;
        std::size_t input_offset = 0U;
        std::uint8_t interest = 0U;
        bool write_blocked = false;
        bool eof = false;
        bool closed = false;
    };
    struct Pending final {
        ListenerKind listener = ListenerKind::control;
        ConnectionToken connection{};
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Connection> connections;
    std::vector<Pending> pending;
    std::size_t control_accepted = 0U;
    std::size_t data_accepted = 0U;
    std::size_t read_chunk = 4096U;
    std::size_t write_chunk = 4096U;
    bool reserved = false;
    bool published = false;
    bool block_unpublish = false;
    bool unpublish_entered = false;
    bool fail_unpublish_after_cleanup = false;
    bool poll_entered = false;
    std::size_t unpublish_count = 0U;
    std::size_t namespace_release_count = 0U;
};

namespace {
FakeListenerIoControl* active_control = nullptr;
}

class FakeListenerIo final : public ListenerIo {
public:
    explicit FakeListenerIo(FakeListenerIoControl::Impl& state) noexcept : state_(state) {}

    Error reserve_pair(const host_runtime::EndpointConfig&,
                       const host_runtime::EndpointConfig&) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (state_.reserved) return Error::BUSY;
        state_.reserved = true;
        return Error::OK;
    }
    Error publish_pair(ListenerPair& pair) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (!state_.reserved || state_.published) return Error::NOT_READY;
        state_.published = true;
        pair = ListenerPair{ListenerToken{1U}, ListenerToken{2U}};
        state_.changed.notify_all();
        return Error::OK;
    }
    Error poll(const ::px4::userland::ipc::posix::WakeHandle&, Timeout timeout,
               ReadyEvent* events, std::size_t capacity,
               std::size_t& count) noexcept override
    {
        std::unique_lock<std::mutex> lock(state_.mutex);
        state_.poll_entered = true;
        state_.changed.notify_all();
        auto ready = [this] {
            if (!state_.pending.empty()) return true;
            for (const auto& connection : state_.connections) {
                if (connection.closed) continue;
                if ((connection.interest & ready_write) != 0U &&
                    !connection.write_blocked) return true;
                if ((connection.interest & ready_read) != 0U &&
                    (connection.input_offset < connection.input.size() || connection.eof))
                    return true;
            }
            return false;
        };
        if (!ready() && timeout.milliseconds != 0U)
            (void)state_.changed.wait_for(lock,
                std::chrono::milliseconds(timeout.milliseconds), ready);
        count = 0U;
        for (const auto& pending : state_.pending) {
            if (count == capacity) break;
            events[count++] = ReadyEvent{ReadyKind::listener, pending.listener,
                ListenerToken{pending.listener == ListenerKind::control ? 1U : 2U},
                ConnectionToken{}, ready_read};
        }
        for (std::size_t i = 0U; i < state_.connections.size() && count < capacity; ++i) {
            const auto& connection = state_.connections[i];
            if (connection.closed) continue;
            std::uint8_t flags = 0U;
            if ((connection.interest & ready_read) != 0U &&
                (connection.input_offset < connection.input.size() || connection.eof))
                flags |= ready_read;
            if ((connection.interest & ready_write) != 0U &&
                !connection.write_blocked) flags |= ready_write;
            if (flags != 0U)
                events[count++] = ReadyEvent{ReadyKind::connection,
                    ListenerKind::control, ListenerToken{},
                    ConnectionToken{static_cast<std::uint64_t>(i + 3U)}, flags};
        }
        return Error::OK;
    }
    AcceptResult accept(ListenerToken listener) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        const ListenerKind kind = listener.value == 1U ? ListenerKind::control
                                                        : ListenerKind::data;
        for (auto it = state_.pending.begin(); it != state_.pending.end(); ++it) {
            if (it->listener != kind) continue;
            const ConnectionToken connection = it->connection;
            state_.pending.erase(it);
            if (kind == ListenerKind::control) ++state_.control_accepted;
            else ++state_.data_accepted;
            return AcceptResult{IoStatus::progress, connection, Error::OK};
        }
        return {};
    }
    Error set_connection_interest(ConnectionToken connection,
                                  std::uint8_t flags) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        auto* found = lookup(connection);
        if (found == nullptr) return Error::NOT_FOUND;
        found->interest = flags;
        state_.changed.notify_all();
        return Error::OK;
    }
    IoResult read_some(ConnectionToken connection, MutableByteView output) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        auto* found = lookup(connection);
        if (found == nullptr || found->closed)
            return IoResult{IoStatus::failure, 0U, Error::DISCONNECTED};
        if (found->input_offset == found->input.size())
            return found->eof ? IoResult{IoStatus::eof, 0U, Error::OK} : IoResult{};
        const std::size_t size = std::min({output.size,
            state_.read_chunk, found->input.size() - found->input_offset});
        std::copy_n(found->input.data() + found->input_offset, size, output.data);
        found->input_offset += size;
        return IoResult{IoStatus::progress, size, Error::OK};
    }
    IoResult write_some(ConnectionToken connection, ByteView input) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        auto* found = lookup(connection);
        if (found == nullptr || found->closed)
            return IoResult{IoStatus::failure, 0U, Error::DISCONNECTED};
        if (found->write_blocked) return IoResult{};
        const std::size_t size = std::min(input.size, state_.write_chunk);
        found->output.insert(found->output.end(), input.data, input.data + size);
        state_.changed.notify_all();
        return IoResult{IoStatus::progress, size, Error::OK};
    }
    Error close_connection(ConnectionToken connection) noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        auto* found = lookup(connection);
        if (found == nullptr) return Error::NOT_FOUND;
        found->closed = true;
        state_.changed.notify_all();
        return Error::OK;
    }
    Error unpublish_pair(ListenerPair) noexcept override
    {
        std::unique_lock<std::mutex> lock(state_.mutex);
        ++state_.unpublish_count;
        if (state_.block_unpublish) {
            state_.unpublish_entered = true;
            state_.changed.notify_all();
            state_.changed.wait(lock, [this] { return !state_.block_unpublish; });
        }
        state_.published = false;
        if (state_.fail_unpublish_after_cleanup) {
            state_.fail_unpublish_after_cleanup = false;
            return Error::INTERNAL;
        }
        return Error::OK;
    }
    Error release_namespace() noexcept override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        ++state_.namespace_release_count;
        state_.reserved = false;
        return Error::OK;
    }
private:
    FakeListenerIoControl::Impl::Connection* lookup(ConnectionToken token) noexcept
    {
        if (token.value < 3U || token.value - 3U >= state_.connections.size())
            return nullptr;
        return &state_.connections[static_cast<std::size_t>(token.value - 3U)];
    }
    FakeListenerIoControl::Impl& state_;
};

FakeListenerIoControl::FakeListenerIoControl() : impl_(new Impl()) {}
FakeListenerIoControl::~FakeListenerIoControl() = default;

ConnectionToken FakeListenerIoControl::connect(ListenerKind listener, ByteView input)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Connection connection;
    connection.listener = listener;
    if (input.data != nullptr) connection.input.assign(input.data, input.data + input.size);
    impl_->connections.push_back(std::move(connection));
    const ConnectionToken token{impl_->connections.size() + 2U};
    impl_->pending.push_back(Impl::Pending{listener, token});
    impl_->changed.notify_all();
    return token;
}

std::vector<std::uint8_t> FakeListenerIoControl::output(ConnectionToken connection) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (connection.value < 3U || connection.value - 3U >= impl_->connections.size()) return {};
    return impl_->connections[static_cast<std::size_t>(connection.value - 3U)].output;
}

void FakeListenerIoControl::send(ConnectionToken connection, ByteView input)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (connection.value < 3U || connection.value - 3U >= impl_->connections.size() ||
        input.data == nullptr || input.size == 0U) return;
    auto& found = impl_->connections[static_cast<std::size_t>(connection.value - 3U)];
    found.input.insert(found.input.end(), input.data, input.data + input.size);
    impl_->changed.notify_all();
}

bool FakeListenerIoControl::closed(ConnectionToken connection) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (connection.value < 3U || connection.value - 3U >= impl_->connections.size()) return true;
    return impl_->connections[static_cast<std::size_t>(connection.value - 3U)].closed;
}

std::size_t FakeListenerIoControl::accepted(ListenerKind listener) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return listener == ListenerKind::control ? impl_->control_accepted : impl_->data_accepted;
}

void FakeListenerIoControl::set_chunks(std::size_t read_bytes, std::size_t write_bytes)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->read_chunk = read_bytes;
    impl_->write_chunk = write_bytes;
}

void FakeListenerIoControl::block_writes(ConnectionToken connection, bool blocked)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (connection.value < 3U || connection.value - 3U >= impl_->connections.size()) return;
    impl_->connections[static_cast<std::size_t>(connection.value - 3U)].write_blocked = blocked;
    impl_->changed.notify_all();
}

void FakeListenerIoControl::block_unpublish()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->block_unpublish = true;
    impl_->unpublish_entered = false;
}

void FakeListenerIoControl::fail_unpublish_after_cleanup()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->fail_unpublish_after_cleanup = true;
}

bool FakeListenerIoControl::wait_unpublish()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->changed.wait_for(lock, std::chrono::seconds(5),
        [this] { return impl_->unpublish_entered; });
}

bool FakeListenerIoControl::wait_poll()
{
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->changed.wait_for(lock, std::chrono::seconds(5),
        [this] { return impl_->poll_entered; });
}

void FakeListenerIoControl::release_unpublish()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->block_unpublish = false;
    impl_->changed.notify_all();
}

bool FakeListenerIoControl::reserved() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reserved;
}

bool FakeListenerIoControl::published() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->published;
}

std::size_t FakeListenerIoControl::unpublish_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->unpublish_count;
}

std::size_t FakeListenerIoControl::namespace_release_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->namespace_release_count;
}

void set_fake_listener_io_control(FakeListenerIoControl* control) noexcept
{
    active_control = control;
}

namespace dtv::userland::host_listener_context::detail {

std::unique_ptr<ListenerIo> create_listener_io() noexcept
{
    if (active_control == nullptr || !active_control->impl_) return {};
    return std::unique_ptr<ListenerIo>(
        new (std::nothrow) FakeListenerIo(*active_control->impl_));
}

}  // namespace dtv::userland::host_listener_context::detail
